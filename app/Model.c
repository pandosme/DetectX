#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <errno.h>
#include <jpeglib.h>

#include "larod.h"
#include "ACAP.h"
#include "Model.h"
#include "imgutils.h"
#include "labelparse.h"
#include "preprocess.h"
#include "model_params.h"  // Generated at build time by extract_model_params.py

#define LOG(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args);}
#define LOG_WARN(fmt, args...)    { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args);}
//#define LOG_TRACE(fmt, args...)   { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }
#define LOG_TRACE(fmt, args...)   {}
#define MODEL_MAX_CACHED_CROPS 5

static bool createAndMapTmpFile(char* fileName, size_t fileSize, void** mappedAddr, int* convFd);
float iou(float x1, float y1, float w1, float h1, float x2, float y2, float w2, float h2);
void Model_Cleanup();
cJSON* non_maximum_suppression(cJSON* list);
static void clear_crop_cache(void);

// Model and video dimensions
static unsigned int modelWidth = 640;
static unsigned int modelHeight = 640;
static unsigned int videoWidth = 856;  // Default for 640 model: 4:3 aspect (856x640)
static unsigned int videoHeight = 640;
static unsigned int channels = 3;
static unsigned int boxes = 0;
static unsigned int classes = 0;
static size_t inputs = 1;
static size_t outputs = 1;
static size_t ppInputs = 1;
static size_t ppOutputs = 1;
static float quant = 1.0;
static float quant_zero = 0;
static float objectnessThreshold = 0.25;
static float confidenceThreshold = 0.30;
static float nms = 0.05;
static int larodModelFd = -1;
static larodConnection* conn = NULL;
static larodModel* InfModel = NULL;
static larodModel* ppModel = NULL;
static larodJobRequest* ppReq = NULL;
static larodMap* ppMap;
static larodJobRequest* infReq;
static void* ppInputAddr = MAP_FAILED;
static void* larodInputAddr = MAP_FAILED;
static void* larodOutput1Addr = MAP_FAILED;
static int ppInputFd = -1;
static int larodInputFd = -1;
static int larodOutput1Fd = -1;
static larodTensor** inputTensors = NULL;
static larodTensor** outputTensors = NULL;
static larodTensor** ppInputTensors = NULL;
static larodTensor** ppOutputTensors = 0;
static size_t yuyvBufferSize = 0;
static size_t outputBufferSize = 0;
//For cropping
static unsigned char* original_rgb_buffer = NULL;
larodMap* ppMapHD               = NULL;
larodModel* ppModelHD           = NULL;
larodTensor** ppInputTensorsHD  = NULL;
size_t ppNumInputsHD            = 1;
larodTensor** ppOutputTensorsHD = NULL;
size_t ppNumOutputsHD           = 1;
larodJobRequest* ppReqHD        = NULL;
void* ppInputAddrHD             = MAP_FAILED;
void* ppOutputAddrHD            = MAP_FAILED;
int ppInputFdHD                 = -1;
int ppOutputFdHD                = -1;

static cJSON* modelConfig = 0;

// Runtime label parsing
static char** modelLabels = NULL;
static char* labelBuffer = NULL;
static size_t numLabels = 0;
static const char* MODEL_PATH = "model/model.tflite";
static const char* LABELS_PATH = "model/labels.txt";

// Preprocessing scale mode
static PreprocessScaleMode scaleMode = SCALE_MODE_STRETCH;  // Default: balanced

static char PP_SD_INPUT_FILE_PATTERN[] = "/tmp/larod.pp.test-XXXXXX";
static char OBJECT_DETECTOR_INPUT_FILE_PATTERN[] = "/tmp/larod.in.test-XXXXXX";
static char OBJECT_DETECTOR_OUT1_FILE_PATTERN[]  = "/tmp/larod.out1.test-XXXXXX";
char PP_HD_INPUT_FILE_PATTERN[]  = "/tmp/larod.pp.hd.test-XXXXXX";
char PP_HD_OUTPUT_FILE_PATTERN[] = "/tmp/larod.pp.hd.out.test-XXXXXX";

int inferenceErrors = 5;
static int currentRefId = 1;

typedef struct {
    int refId;
    int crop_x;
    int crop_y;
    int crop_w;
    int crop_h;
	int img_w;
	int img_h;	
    unsigned char* jpeg_buf;
    unsigned jpeg_size;
} CropCacheEntry;

static CropCacheEntry cropCache[MODEL_MAX_CACHED_CROPS];
static int numCropCache = 0;

static void clear_crop_cache(void) {
    for (int i = 0; i < numCropCache; i++) {
        if (cropCache[i].jpeg_buf) {
            free(cropCache[i].jpeg_buf);
            cropCache[i].jpeg_buf = NULL;
        }
    }
    numCropCache = 0;
}


cJSON*
Model_Inference(VdoBuffer* image) {
    larodError* error = NULL;
    LOG_TRACE("%s: Called\n", __func__);
    if (!image) {
        LOG_TRACE("%s: No image\n", __func__);
        return 0;
    }
    if (ACAP_STATUS_Bool("model", "state") == 0) {
        LOG_TRACE("%s: Model not running\n", __func__);
        return 0;
    }
    if (inferenceErrors <= 0) {
        LOG_WARN("Too many inference errors.  Model stopped\n");
        Model_Cleanup();
        return 0;
    }

    // Get the captured NV12 frame
    uint8_t* nv12Data = (uint8_t*)vdo_buffer_get_data(image);
    
    // Copy NV12 data to BOTH preprocessing input buffers
    memcpy(ppInputAddr, nv12Data, yuyvBufferSize);      // For model inference (Aspect 1:1)


    // Cropping output config
    cJSON* settings = ACAP_Get_Config("settings");
    if (!settings) {
		LOG_TRACE("ERROR %s>\n",__func__);
		return 0;
	}
    cJSON* cropping = cJSON_GetObjectItem(settings, "cropping");
    int cropping_active = cropping && cJSON_IsTrue(cJSON_GetObjectItem(cropping, "active"));
	if( cropping_active ) {
		memcpy(ppInputAddrHD, nv12Data, yuyvBufferSize);    // For HD preprocessing (original res)

		// Run HD preprocessing job
		if (!larodRunJob(conn, ppReqHD, &error)) {
			LOG_WARN("%s: Unable to run HD pre-processing job: %s (%d)\n", __func__, error->msg, error->code);
			larodClearError(&error);
			inferenceErrors--;
			return 0;
		}
		original_rgb_buffer = (unsigned char*)ppOutputAddrHD;
	} else {
		original_rgb_buffer = 0;
	}

    // Run standard preprocessing for model inference
    if (!larodRunJob(conn, ppReq, &error)) {
        LOG_WARN("%s: Unable to run job to preprocess model: %s (%d)\n", __func__, error->msg, error->code);
        larodClearError(&error);
        inferenceErrors--;
        return 0;
    }
    
    if (lseek(larodOutput1Fd, 0, SEEK_SET) == -1) {
        LOG_WARN("%s: Unable to rewind output file position: %s\n", __func__, strerror(errno));
        inferenceErrors--;
        return 0;
    }
    
    // Run inference
    if (!larodRunJob(conn, infReq, &error)) {
        LOG_WARN("%s: Unable to run inference on model: %s (%d)\n", __func__, error->msg, error->code);
        larodClearError(&error);
        inferenceErrors--;
        return 0;
    }

    // Process inference results (uint8 quantized output)
    uint8_t* output_tensor = (uint8_t*)larodOutput1Addr;
    cJSON* list = cJSON_CreateArray();
    struct timeval tv;
    gettimeofday(&tv, NULL);
    long long timestamp = tv.tv_sec * 1000LL + tv.tv_usec / 1000;

    LOG_TRACE("%s: Processing %u boxes, quant=%.6f, quant_zero=%.1f, objectnessThreshold=%.2f, confidenceThreshold=%.2f\n",
              __func__, boxes, quant, quant_zero, objectnessThreshold, confidenceThreshold);

    // Check first few raw values for debugging
    LOG_TRACE("%s: First 10 raw output bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
              __func__, output_tensor[0], output_tensor[1], output_tensor[2], output_tensor[3], output_tensor[4],
              output_tensor[5], output_tensor[6], output_tensor[7], output_tensor[8], output_tensor[9]);

    int candidates = 0;
    int detections = 0;

    for (int i = 0; i < boxes; i++) {
        int box = i * (5 + classes);

        // Dequantize uint8 to float using quantization parameters
        float objectness = (float)(output_tensor[box + 4] - quant_zero) * quant;

        if (i < 3 && objectness > 0.01) {  // Log first few non-zero boxes
            LOG_TRACE("%s: Box %d: raw_obj=%u objectness=%.4f (threshold=%.2f)\n",
                      __func__, i, output_tensor[box + 4], objectness, objectnessThreshold);
        }

        if (objectness >= objectnessThreshold) {
            candidates++;

            float x = (float)(output_tensor[box + 0] - quant_zero) * quant;
            float y = (float)(output_tensor[box + 1] - quant_zero) * quant;
            float w = (float)(output_tensor[box + 2] - quant_zero) * quant;
            float h = (float)(output_tensor[box + 3] - quant_zero) * quant;
            int classId = -1;
            float maxConfidence = 0;
            for (int c = 0; c < classes; c++) {
                float confidence = (float)(output_tensor[box + 5 + c] - quant_zero) * quant * objectness;
                if (confidence > maxConfidence) {
                    classId = c;
                    maxConfidence = confidence;
                }
            }
            if (maxConfidence > confidenceThreshold) {
                detections++;
                cJSON* detection = cJSON_CreateObject();
                const char* label = labels_get(modelLabels, numLabels, classId);
                cJSON_AddStringToObject(detection, "label", label);
                cJSON_AddNumberToObject(detection, "c", maxConfidence);

                double x_norm = x - (w / 2);
                double y_norm = y - (h / 2);

                cJSON_AddNumberToObject(detection, "x", x_norm);
                cJSON_AddNumberToObject(detection, "y", y_norm);
                cJSON_AddNumberToObject(detection, "w", w);
                cJSON_AddNumberToObject(detection, "h", h);

                cJSON_AddNumberToObject(detection, "timestamp", timestamp);
                cJSON_AddNumberToObject(detection, "refId", currentRefId++);
                cJSON_AddItemToArray(list, detection);

                if (detections <= 3) {  // Log first few detections
                    LOG_TRACE("%s: Detection %d: %s conf=%.2f obj=%.2f x=%.3f y=%.3f w=%.3f h=%.3f\n",
                              __func__, detections, label, maxConfidence, objectness, x, y, w, h);
                }
            } else if (candidates <= 3) {  // Log why first few candidates were rejected
                LOG_TRACE("%s: Rejected box %d: obj=%.4f, maxConf=%.4f (threshold=%.2f)\n",
                          __func__, i, objectness, maxConfidence, confidenceThreshold);
            }
        }
    }

    LOG_TRACE("%s: Found %d candidates, %d detections after confidence filtering\n",
              __func__, candidates, detections);

    return non_maximum_suppression(list);
}

//The detection coordinates are in pixels relative to model input dimensions
const unsigned char*
Model_GetImageData(const cJSON* detection, unsigned* jpeg_size, int* out_x, int* out_y, int* out_w, int* out_h, int* img_w, int* img_h ) {
    if (jpeg_size) *jpeg_size = 0;
    if (!detection) {
        LOG_WARN("%s: detection is NULL\n", __func__);
        return NULL;
    }
    LOG_TRACE("<%s\n", __func__);

    char* json = cJSON_PrintUnformatted(detection);
    if (json) {
        LOG_TRACE("%s", json);
        free(json);
    }

    cJSON* settings = ACAP_Get_Config("settings");
    cJSON* cropping = settings ? cJSON_GetObjectItem(settings, "cropping") : NULL;
    int cropping_active = cropping && cJSON_IsTrue(cJSON_GetObjectItem(cropping, "active"));
    if (!cropping_active)
        return NULL;

    cJSON* refIdObj = cJSON_GetObjectItem(detection, "refId");
    if (!refIdObj || !cJSON_IsNumber(refIdObj)) {
        LOG_WARN("%s: detection missing valid 'refId'\n", __func__);
        return NULL;
    }
    int refId = refIdObj->valueint;

    for (int i = 0; i < numCropCache; ++i) {
        if (cropCache[i].refId == refId) {
            if (jpeg_size) *jpeg_size = cropCache[i].jpeg_size;
            if (out_x) *out_x = cropCache[i].crop_x;
            if (out_y) *out_y = cropCache[i].crop_y;
            if (out_w) *out_w = cropCache[i].crop_w;
            if (out_h) *out_h = cropCache[i].crop_h;
            if (img_w) *img_w = cropCache[i].img_w;
            if (img_h) *img_h = cropCache[i].img_h;
            return cropCache[i].jpeg_buf;
        }
    }

    int leftborder_px = cropping && cJSON_GetObjectItem(cropping, "leftborder") ? cJSON_GetObjectItem(cropping, "leftborder")->valueint : 0;
    int rightborder_px = cropping && cJSON_GetObjectItem(cropping, "rightborder") ? cJSON_GetObjectItem(cropping, "rightborder")->valueint : 0;
    int topborder_px = cropping && cJSON_GetObjectItem(cropping, "topborder") ? cJSON_GetObjectItem(cropping, "topborder")->valueint : 0;
    int bottomborder_px = cropping && cJSON_GetObjectItem(cropping, "bottomborder") ? cJSON_GetObjectItem(cropping, "bottomborder")->valueint : 0;

    cJSON* xObj = cJSON_GetObjectItem(detection, "x");
    cJSON* yObj = cJSON_GetObjectItem(detection, "y");
    cJSON* wObj = cJSON_GetObjectItem(detection, "w");
    cJSON* hObj = cJSON_GetObjectItem(detection, "h");
    if (!xObj || !cJSON_IsNumber(xObj) ||
        !yObj || !cJSON_IsNumber(yObj) ||
        !wObj || !cJSON_IsNumber(wObj) ||
        !hObj || !cJSON_IsNumber(hObj)) {
        LOG_WARN("%s: detection missing geometry\n", __func__);
        return NULL;
    }

	// Coordinates are in pixels relative to model input, scale to video frame pixels
	double scale_x = (double)videoWidth / (double)modelWidth;
	double scale_y = (double)videoHeight / (double)modelHeight;
	int det_pixel_x = (int)round(xObj->valuedouble * scale_x);
	int det_pixel_y = (int)round(yObj->valuedouble * scale_y);
	int det_pixel_w = (int)round(wObj->valuedouble * scale_x);
	int det_pixel_h = (int)round(hObj->valuedouble * scale_y);

    int crop_x = det_pixel_x - leftborder_px;
    int crop_y = det_pixel_y - topborder_px;
    int crop_w = det_pixel_w + leftborder_px + rightborder_px;
    int crop_h = det_pixel_h + topborder_px + bottomborder_px;

    if (crop_x < 0) { crop_w += crop_x; crop_x = 0; }
    if (crop_y < 0) { crop_h += crop_y; crop_y = 0; }
    if (crop_x + crop_w > (int)videoWidth) crop_w = videoWidth - crop_x;
    if (crop_y + crop_h > (int)videoHeight) crop_h = videoHeight - crop_y;
    if (crop_w < 1) crop_w = 1;
    if (crop_h < 1) crop_h = 1;

    int det_x = det_pixel_x - crop_x;
    int det_y = det_pixel_y - crop_y;
    int det_w = det_pixel_w;
    int det_h = det_pixel_h;
    if (det_x < 0) { det_w += det_x; det_x = 0; }
    if (det_y < 0) { det_h += det_y; det_y = 0; }
    if (det_x + det_w > crop_w) det_w = crop_w - det_x;
    if (det_y + det_h > crop_h) det_h = crop_h - det_y;
    if (det_w < 1) det_w = 1;
    if (det_h < 1) det_h = 1;

    if (!original_rgb_buffer) {
        LOG_WARN("%s: Original RGB image buffer is NULL\n", __func__);
        return NULL;
    }


    unsigned char* crop_buf = crop_interleaved(original_rgb_buffer, videoWidth, videoHeight, 3, crop_x, crop_y, crop_w, crop_h);
    if (!crop_buf) {
        LOG_WARN("%s: failed to crop interleaved RGB buffer\n", __func__);
        return NULL;
    }

    unsigned char* jpeg_buf = NULL;
    unsigned long jpeglen = 0;
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    cinfo.image_width = crop_w;
    cinfo.image_height = crop_h;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 90, TRUE);

    buffer_to_jpeg(crop_buf, &cinfo, &jpeglen, &jpeg_buf);

    jpeg_destroy_compress(&cinfo);
    free(crop_buf);

    if (!jpeg_buf || jpeglen == 0) {
        LOG_WARN("%s: JPEG encoding failed\n", __func__);
        return NULL;
    }


    if (numCropCache < MODEL_MAX_CACHED_CROPS) {
        cropCache[numCropCache].refId = refId;
        cropCache[numCropCache].crop_x = det_x;
        cropCache[numCropCache].crop_y = det_y;
        cropCache[numCropCache].crop_w = det_w;
        cropCache[numCropCache].crop_h = det_h;
        cropCache[numCropCache].img_w = crop_w;
        cropCache[numCropCache].img_h = crop_h;
        cropCache[numCropCache].jpeg_buf = jpeg_buf;
        cropCache[numCropCache].jpeg_size = jpeglen;
        numCropCache++;
    }

    if (jpeg_size) *jpeg_size = (unsigned)jpeglen;
    if (out_x) *out_x = det_x;
    if (out_y) *out_y = det_y;
    if (out_w) *out_w = det_w;
    if (out_h) *out_h = det_h;
	*img_w = crop_w;
	*img_h = crop_h;

    LOG_TRACE("%s>\n", __func__);
    return jpeg_buf;
}

void Model_Reset(void) {
    clear_crop_cache();
}

float iou(float x1, float y1, float w1, float h1, float x2, float y2, float w2, float h2) {
    float xx1 = fmax(x1 - (w1 / 2), x2 - (w2 / 2));
    float yy1 = fmax(y1 - (h1 / 2), y2 - (h2 / 2));
    float xx2 = fmin(x1 + (w1 / 2), x2 + (w2 / 2));
    float yy2 = fmin(y1 + (h1 / 2), y2 + (h2 / 2));

    float inter  = fmax(0, xx2 - xx1) * fmax(0, yy2 - yy1);
    float union_ = w1 * h1 + w2 * h2 - inter;

    return inter / union_;
}

cJSON* non_maximum_suppression(cJSON* list) {
    if(!list) {
        LOG_WARN("%s: Invalid list\n",__func__);
        return 0;
    }
    int items = cJSON_GetArraySize(list);
    if (items < 2) {
        return list;
    }
    int keep[items];
    memset(keep, 1, items * sizeof(int));
    for (int i = 0; i < items; i++) {
        if (keep[i]) {
            cJSON* detection = cJSON_GetArrayItem(list, i);
            float x1 = cJSON_GetObjectItem(detection, "x")->valuedouble;
            float y1 = cJSON_GetObjectItem(detection, "y")->valuedouble;
            float w1 = cJSON_GetObjectItem(detection, "w")->valuedouble;
            float h1 = cJSON_GetObjectItem(detection, "h")->valuedouble;
            float c1 = cJSON_GetObjectItem(detection, "c")->valuedouble;
            for (int j = i + 1; j < items; j++) {
                if (keep[j]) {
                    cJSON* alternative = cJSON_GetArrayItem(list, j);
                    float x2 = cJSON_GetObjectItem(alternative, "x")->valuedouble;
                    float y2 = cJSON_GetObjectItem(alternative, "y")->valuedouble;
                    float w2 = cJSON_GetObjectItem(alternative, "w")->valuedouble;
                    float h2 = cJSON_GetObjectItem(alternative, "h")->valuedouble;
                    float c2 = cJSON_GetObjectItem(alternative, "c")->valuedouble;
                    float iou_value = iou(x1, y1, w1, h1, x2, y2, w2, h2);
                    if (iou_value > nms) {
                        if (c1 > c2) {
                            keep[j] = 0;
                        } else {
                            keep[i] = 0;
                            break;
                        }
                    }
                }
            }
        }
    }
    cJSON* result = cJSON_CreateArray();
    for (int i = 0; i < items; i++) {
        if (keep[i]) {
            cJSON* detection = cJSON_GetArrayItem(list, i);
            cJSON_AddItemToArray(result, cJSON_Duplicate(detection, 1));
        }
    }
    cJSON_Delete(list);
    return result;
}

static bool 
createAndMapTmpFile(char* fileName, size_t fileSize, void** mappedAddr, int* convFd) {
	LOG_TRACE("%s: %s %zu\n", __func__,fileName, fileSize);
    int fd = mkstemp(fileName);
    if (fd < 0) {
        LOG_WARN("%s: Unable to open temp file %s: %s\n", __func__, fileName, strerror(errno));
        return false;
    }

    if (ftruncate(fd, (off_t)fileSize) < 0) {
        LOG_WARN("%s: Unable to truncate temp file %s: %s\n", __func__, fileName, strerror(errno));
        close(fd);
        return false;
    }

    if (unlink(fileName)) {
        LOG_WARN("%s: Unable to unlink from temp file %s: %s\n", __func__, fileName, strerror(errno));
        close(fd);
        return false;
    }

    void* data = mmap(NULL, fileSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        LOG_WARN("%s: Unable to mmap temp file %s: %s\n", __func__, fileName, strerror(errno));
        close(fd);
        return false;
    }

    *mappedAddr = data;
    *convFd = fd;
    return true;
}


void
Model_Cleanup() {
    // Only the model handle is released here. We count on larod service to
    // release the privately loaded model when the session is disconnected in
    // larodDisconnect().
    larodError* error = NULL;

	clear_crop_cache();

	// Free runtime-parsed labels
	labels_free(modelLabels, labelBuffer);
	modelLabels = NULL;
	labelBuffer = NULL;
	numLabels = 0;

	if( ppMap ) larodDestroyMap(&ppMap);
    if( ppModel ) larodDestroyModel(&ppModel);
    larodDestroyModel(&InfModel);
    if (conn) larodDisconnect(&conn, NULL);
    if (larodModelFd >= 0) close(larodModelFd);
    if (larodInputAddr != MAP_FAILED) munmap(larodInputAddr, modelWidth * modelHeight * channels);
    if (larodInputFd >= 0) close(larodInputFd);
    if (ppInputAddr != MAP_FAILED) munmap(ppInputAddr, modelWidth * modelHeight * channels);
    if (ppInputFd >= 0) close(ppInputFd);
    if (larodOutput1Addr != MAP_FAILED) munmap(larodOutput1Addr, outputBufferSize);
    larodDestroyJobRequest(&ppReq);
    larodDestroyJobRequest(&infReq);
    larodDestroyTensors(conn, &inputTensors, inputs, &error);
    larodDestroyTensors(conn, &outputTensors, outputs, &error);
    larodClearError(&error);
	ACAP_STATUS_SetString("model","status","Model stopped");
	ACAP_STATUS_SetBool("model","state", 0);	
}



cJSON* Model_Setup(void) {
    larodError* error = NULL;
    ACAP_STATUS_SetString("model", "status", "Model initialization failed. Check log file");
    ACAP_STATUS_SetBool("model", "state", 0);

    // ==== RUNTIME MODEL INTROSPECTION ====

    // Step 1: Connect to larod
    if (!larodConnect(&conn, &error)) {
        LOG_WARN("%s: Could not connect to larod: %s\n", __func__, error ? error->msg : "unknown");
        larodClearError(&error);
        return 0;
    }

    // Step 2: Load model to introspect
    larodModelFd = open(MODEL_PATH, O_RDONLY);
    if (larodModelFd < 0) {
        LOG_WARN("%s: Could not open model %s: %s\n", __func__, MODEL_PATH, strerror(errno));
        Model_Cleanup();
        return 0;
    }

    // Detect platform and select appropriate chip
    const char* platform = ACAP_DEVICE_Prop("platform");
    const char* chipString = "cpu-tflite";  // Default fallback

    if (platform) {
        if (strstr(platform, "Artpec-8")) {
            chipString = "axis-a8-dlpu-tflite";
            LOG("Detected ARTPEC-8 platform\n");
        } else if (strstr(platform, "Artpec-9")) {
            chipString = "a9-dlpu-tflite";
            LOG("Detected ARTPEC-9 platform\n");
        } else {
            LOG("Using CPU inference (platform: %s)\n", platform);
        }
    } else {
        LOG_WARN("Could not detect platform, using CPU fallback\n");
    }

    const larodDevice* device = larodGetDevice(conn, chipString, 0, &error);
    if (!device) {
        LOG_WARN("%s: Could not get device %s: %s\n", __func__, chipString, error->msg);
        larodClearError(&error);
        Model_Cleanup();
        return 0;
    }

    InfModel = larodLoadModel(conn, larodModelFd, device, LAROD_ACCESS_PRIVATE, "object_detection", NULL, &error);
    if (!InfModel) {
        LOG_WARN("%s: Unable to load model: %s\n", __func__, error->msg);
        larodClearError(&error);
        Model_Cleanup();
        return 0;
    }

    // Step 3: Introspect model tensors
    larodTensor** tempInputTensors = larodCreateModelInputs(InfModel, &inputs, &error);
    if (!tempInputTensors) {
        LOG_WARN("%s: Failed retrieving input tensors: %s\n", __func__, error->msg);
        larodClearError(&error);
        Model_Cleanup();
        return 0;
    }

    larodTensor** tempOutputTensors = larodCreateModelOutputs(InfModel, &outputs, &error);
    if (!tempOutputTensors) {
        LOG_WARN("%s: Failed retrieving output tensors: %s\n", __func__, error->msg);
        larodClearError(&error);
        larodDestroyTensors(conn, &tempInputTensors, inputs, NULL);
        Model_Cleanup();
        return 0;
    }

    // Get input dimensions (YOLOv5 input is always NHWC: batch, height, width, channels)
    const larodTensorDims* inputDims = larodGetTensorDims(tempInputTensors[0], &error);
    if (!inputDims) {
        LOG_WARN("%s: Failed to get input tensor dimensions\n", __func__);
        larodDestroyTensors(conn, &tempInputTensors, inputs, NULL);
        larodDestroyTensors(conn, &tempOutputTensors, outputs, NULL);
        Model_Cleanup();
        return 0;
    }

    modelHeight = inputDims->dims[1];
    modelWidth = inputDims->dims[2];
    channels = inputDims->dims[3];

    LOG("Model input: %ux%ux%u\n", modelWidth, modelHeight, channels);

    // Get output dimensions (YOLOv5 output is always [batch, boxes, stride])
    const larodTensorDims* outputDims = larodGetTensorDims(tempOutputTensors[0], &error);
    if (!outputDims) {
        LOG_WARN("%s: Failed to get output tensor dimensions\n", __func__);
        larodDestroyTensors(conn, &tempInputTensors, inputs, NULL);
        larodDestroyTensors(conn, &tempOutputTensors, outputs, NULL);
        Model_Cleanup();
        return 0;
    }

    boxes = outputDims->dims[1];
    int stride = outputDims->dims[2];
    classes = stride - 5;  // YOLOv5 format: x,y,w,h,objectness,class1...classN

    LOG("Model output: %u boxes, %u classes, stride=%d\n", boxes, classes, stride);

    // Get quantization parameters (extracted at build time from model)
    larodTensorDataType dataType = larodGetTensorDataType(tempOutputTensors[0], &error);
    if (dataType == LAROD_TENSOR_DATA_TYPE_INT8 || dataType == LAROD_TENSOR_DATA_TYPE_UINT8) {
        // Quantized model - use build-time extracted values
        quant = QUANTIZATION_SCALE;
        quant_zero = QUANTIZATION_ZERO_POINT;
        LOG("Quantized model: data_type=%d, scale=%.15f, zero_point=%d\n",
            dataType, quant, (int)quant_zero);
    } else {
        // Float model
        quant = 1.0;
        quant_zero = 0;
        LOG("Float model detected (data_type=%d), no quantization needed\n", dataType);
    }

    // Clean up temporary tensors
    larodDestroyTensors(conn, &tempInputTensors, inputs, &error);
    larodDestroyTensors(conn, &tempOutputTensors, outputs, &error);

    // Step 4: Read user settings (scaleMode, nms, objectness)
    cJSON* settings = ACAP_Get_Config("settings");
    if (settings) {
        // Read scale mode (default: "balanced")
        cJSON* scaleModeItem = cJSON_GetObjectItem(settings, "scaleMode");
        if (scaleModeItem && scaleModeItem->valuestring) {
            scaleMode = preprocess_mode_from_string(scaleModeItem->valuestring);
            LOG("Scale mode: %s\n", preprocess_mode_to_string(scaleMode));
        } else {
            scaleMode = SCALE_MODE_STRETCH;  // Default: balanced
            LOG("Scale mode: balanced (default)\n");
        }

        // Read NMS threshold
        cJSON* nmsItem = cJSON_GetObjectItem(settings, "nms");
        if (nmsItem) {
            nms = nmsItem->valuedouble;
        }

        // Read objectness threshold
        cJSON* objectnessItem = cJSON_GetObjectItem(settings, "objectness");
        if (objectnessItem) {
            objectnessThreshold = objectnessItem->valuedouble;
        }

        LOG("Detection thresholds: objectness=%.2f, nms=%.2f\n", objectnessThreshold, nms);
    }

    // Step 5: Calculate video dimensions based on model size and scale mode
    // All dimensions must be divisible by 8 (VDO requirement)
    if (scaleMode == SCALE_MODE_STRETCH) {
        // Balanced mode: 4:3 aspect ratio
        if (modelHeight == 640) {
            videoWidth = 856;  // 640 * 4/3 = 853.33, rounded to nearest /8 = 856
            videoHeight = 640;
        } else if (modelHeight == 480) {
            videoWidth = 640;
            videoHeight = 480;
        } else if (modelHeight == 768) {
            videoWidth = 1024;
            videoHeight = 768;
        } else if (modelHeight == 960) {
            videoWidth = 1280;
            videoHeight = 960;
        } else {
            // Calculate 4:3 from model size, ensure divisible by 8
            videoWidth = ((modelWidth * 4) / 3 + 7) & ~7;
            videoHeight = (modelWidth + 7) & ~7;
        }
        LOG("Video: %ux%u (4:3 balanced mode)\n", videoWidth, videoHeight);
    } else if (scaleMode == SCALE_MODE_CROP) {
        // Center-crop mode: 1:1 aspect ratio (same as model)
        videoWidth = (modelWidth + 7) & ~7;
        videoHeight = (modelHeight + 7) & ~7;
        LOG("Video: %ux%u (1:1 center-crop mode)\n", videoWidth, videoHeight);
    } else if (scaleMode == SCALE_MODE_LETTERBOX) {
        // Letterbox mode: 16:9 aspect ratio with padding
        if (modelHeight == 640) {
            videoWidth = 1136;  // 640 * 16/9 = 1136
            videoHeight = 640;
        } else if (modelHeight == 480) {
            videoWidth = 848;  // Closest 16:9 to 480
            videoHeight = 480;
        } else if (modelHeight == 768) {
            videoWidth = 1360;
            videoHeight = 768;
        } else {
            // Calculate 16:9, ensure divisible by 8
            videoWidth = ((modelHeight * 16) / 9 + 7) & ~7;
            videoHeight = (modelHeight + 7) & ~7;
        }
        LOG("Video: %ux%u (16:9 letterbox mode)\n", videoWidth, videoHeight);
    }

    // Step 6: Load labels
    if (!labels_parse_file(LABELS_PATH, &modelLabels, &labelBuffer, &numLabels)) {
        LOG_WARN("%s: Failed to load labels from %s, using defaults\n", __func__, LABELS_PATH);
        numLabels = 0;
    } else {
        LOG("Loaded %zu labels from %s\n", numLabels, LABELS_PATH);
    }

    // Step 7: Build model.json structure for frontend/API
    modelConfig = cJSON_CreateObject();
    cJSON_AddNumberToObject(modelConfig, "modelWidth", modelWidth);
    cJSON_AddNumberToObject(modelConfig, "modelHeight", modelHeight);
    cJSON_AddNumberToObject(modelConfig, "videoWidth", videoWidth);
    cJSON_AddNumberToObject(modelConfig, "videoHeight", videoHeight);

    // Video aspect and scale mode for frontend
    const char* videoAspect = "4:3";
    if (scaleMode == SCALE_MODE_CROP) {
        videoAspect = "1:1";
    } else if (scaleMode == SCALE_MODE_LETTERBOX) {
        videoAspect = "16:9";
    }
    cJSON_AddStringToObject(modelConfig, "videoAspect", videoAspect);
    cJSON_AddNumberToObject(modelConfig, "scaleMode", (int)scaleMode);
    cJSON_AddStringToObject(modelConfig, "scaleModeName", preprocess_mode_to_string(scaleMode));

    cJSON_AddNumberToObject(modelConfig, "boxes", boxes);
    cJSON_AddNumberToObject(modelConfig, "classes", classes);
    cJSON_AddNumberToObject(modelConfig, "quant", quant);
    cJSON_AddNumberToObject(modelConfig, "zeroPoint", quant_zero);
    cJSON_AddNumberToObject(modelConfig, "objectness", objectnessThreshold);
    cJSON_AddNumberToObject(modelConfig, "nms", nms);
    cJSON_AddStringToObject(modelConfig, "path", MODEL_PATH);
    cJSON_AddStringToObject(modelConfig, "chip", chipString);

    // Add labels array
    cJSON* labelsArray = cJSON_CreateArray();
    for (size_t i = 0; i < numLabels; i++) {
        cJSON_AddItemToArray(labelsArray, cJSON_CreateString(modelLabels[i]));
    }
    cJSON_AddItemToObject(modelConfig, "labels", labelsArray);

    // Store in ACAP config for other components
    ACAP_Set_Config("model", modelConfig);

    LOG("Model config: %ux%u model → %ux%u video (%s), %u boxes, %u classes\n",
        modelWidth, modelHeight, videoWidth, videoHeight, videoAspect, boxes, classes);

    // ==== END RUNTIME INTROSPECTION ====

    // Preprocessing (inference, 1:1 model)
    ppMap = larodCreateMap(&error);
    if (!ppMap) {
        LOG_WARN("%s: Could not create preprocessing larodMap %s\n", __func__, error->msg);
        Model_Cleanup();
        return 0;
    }
	
    if (!larodMapSetStr(ppMap, "image.input.format", "nv12", &error)) {
        LOG_WARN("%s: Failed setting preprocessing parameters: %s\n", __func__, error->msg);
        Model_Cleanup();
        return 0;
    }
    if (!larodMapSetIntArr2(ppMap, "image.input.size", videoWidth, videoHeight, &error)) {
        LOG_WARN("%s: Failed setting preprocessing parameters: %s\n", __func__, error->msg);
        Model_Cleanup();
        return 0;
    }
    if (!larodMapSetStr(ppMap, "image.output.format", "rgb-interleaved", &error)) {
        LOG_WARN("%s: Failed setting preprocessing parameters: %s\n", __func__, error->msg);
        Model_Cleanup();
        return 0;
    }
    if (!larodMapSetIntArr2(ppMap, "image.output.size", modelWidth, modelHeight, &error)) {
        LOG_WARN("%s: Failed setting preprocessing parameters: %s\n", __func__, error->msg);
        Model_Cleanup();
        return 0;
    }
    // --- HD Pre-processing for cropping from original resolution ----
    ppMapHD = larodCreateMap(&error);
    if (!ppMapHD) {
        LOG_WARN("%s: Could not create preprocessing (HD) larodMap %s\n", __func__, error->msg);
        Model_Cleanup();
        return 0;
    }
    if (!larodMapSetStr(ppMapHD, "image.input.format", "nv12", &error)) {
        LOG_WARN("%s: Failed setting HD preprocessing parameters: %s\n", __func__, error->msg);
        Model_Cleanup();
        return 0;
    }
    if (!larodMapSetIntArr2(ppMapHD, "image.input.size", videoWidth, videoHeight, &error)) {
        LOG_WARN("%s: Failed setting HD preprocessing parameters: %s\n", __func__, error->msg);
        Model_Cleanup();
        return 0;
    }
    if (!larodMapSetStr(ppMapHD, "image.output.format", "rgb-interleaved", &error)) {
        LOG_WARN("%s: Failed setting HD preprocessing parameters: %s\n", __func__, error->msg);
        Model_Cleanup();
        return 0;
    }
    if (!larodMapSetIntArr2(ppMapHD, "image.output.size", videoWidth, videoHeight, &error)) {
        LOG_WARN("%s: Failed setting HD preprocessing parameters: %s\n", __func__, error->msg);
        Model_Cleanup();
        return 0;
    }

    // Pre-processing model (1:1 model image) - model already loaded during introspection
    const char* larodLibyuvPP = "cpu-proc";
    const larodDevice* device_prePros = larodGetDevice(conn, larodLibyuvPP, 0, &error);
    if (!device_prePros) {
        LOG_WARN("%s: Could not get device %s: %s\n", __func__, larodLibyuvPP, error->msg);
        larodClearError(&error);
        Model_Cleanup();
        return 0;
    }
    ppModel = larodLoadModel(conn, -1, device_prePros, LAROD_ACCESS_PRIVATE, "", ppMap, &error);
    if (!ppModel) {
        LOG_WARN("%s: Unable to load preprocessing model with chip %s: %s", __func__, larodLibyuvPP, error->msg);
        larodClearError(&error);
        Model_Cleanup();
        return 0;
    }

    // HD preprocessing model
    ppModelHD = larodLoadModel(conn, -1, device_prePros, LAROD_ACCESS_PRIVATE, "", ppMapHD, &error);
    if (!ppModelHD) {
        LOG_WARN("%s: Unable to load HD preprocessing model: %s", __func__, error ? error->msg : "");
        larodClearError(&error);
        Model_Cleanup();
        return 0;
    }
    // Create input/output tensors
    ppInputTensors = larodCreateModelInputs(ppModel, &ppInputs, &error);
    if (!ppInputTensors) {
        LOG_WARN("%s: Failed retrieving input tensors: %s\n", __func__, error->msg);
        larodClearError(&error);
        Model_Cleanup();
        return 0;
    }
    ppOutputTensors = larodCreateModelOutputs(ppModel, &ppOutputs, &error);
    if (!ppOutputTensors) {
        LOG_WARN("%s: Failed retrieving output tensors: %s\n", __func__, error->msg);
        larodClearError(&error);
        Model_Cleanup();
        return 0;
    }
    // HD preprocessing tensors
    ppInputTensorsHD = larodCreateModelInputs(ppModelHD, &inputs, &error);
    if (!ppInputTensorsHD) {
        LOG_WARN("%s: Failed retrieving HD input tensors: %s\n", __func__, error->msg);
        larodClearError(&error);
        Model_Cleanup();
        return 0;
    }

    ppOutputTensorsHD = larodCreateModelOutputs(ppModelHD, &outputs, &error);
    if (!ppOutputTensorsHD) {
        LOG_WARN("%s: Failed retrieving HD output tensors: %s\n", __func__, error->msg);
        larodClearError(&error);
        Model_Cleanup();
        return 0;
    }
    inputTensors = larodCreateModelInputs(InfModel, &inputs, &error);
    if (!inputTensors) {
        LOG_WARN("%s: Failed retrieving input tensors: %s\n", __func__, error->msg);
        larodClearError(&error);
        Model_Cleanup();
        return 0;
    }
    outputTensors = larodCreateModelOutputs(InfModel, &outputs, &error);
    if (!outputTensors) {
        LOG_WARN("%s: Failed retrieving output tensors: %s\n", __func__, error->msg);
        larodClearError(&error);
        Model_Cleanup();
        return 0;
    }

    // Determine tensor buffer sizes
    const larodTensorPitches* ppInputPitches = larodGetTensorPitches(ppInputTensors[0], &error);
    if (!ppInputPitches) {
        LOG_WARN("%s: Could not get pitches of tensor: %s\n", __func__, error->msg);
        larodClearError(&error);
        Model_Cleanup();
        return 0;
    }

    yuyvBufferSize = ppInputPitches->pitches[0];
    LOG_TRACE("Buffer size: %zu\n", yuyvBufferSize);
    
    const larodTensorPitches* ppOutputPitches = larodGetTensorPitches(ppOutputTensors[0], &error);
    if (!ppOutputPitches) {
        LOG_WARN("%s: Could not get pitches of tensor: %s\n", __func__, error->msg);
        larodClearError(&error);
        Model_Cleanup();
        return 0;
    }

    size_t rgbBufferSize = ppOutputPitches->pitches[0];
    size_t expectedSize = modelWidth * modelHeight * channels;
    if (expectedSize != rgbBufferSize) {
        LOG_WARN("%s: Expected video output size %zu, actual %zu\n", __func__, expectedSize, rgbBufferSize);
        Model_Cleanup();
        return 0;
    }
    
    const larodTensorPitches* outputPitches = larodGetTensorPitches(outputTensors[0], &error);
    if (!outputPitches) {
        LOG_WARN("%s: Could not get pitches of tensor: %s\n", __func__, error->msg);
        Model_Cleanup();
        return 0;
    }

    outputBufferSize = outputPitches->pitches[0];
    LOG("Output tensor buffer size: %zu bytes (boxes=%u, classes=%u, stride=%d)\n",
        outputBufferSize, boxes, classes, classes + 5);

    // Allocate space for input tensors
    if (!createAndMapTmpFile(PP_SD_INPUT_FILE_PATTERN, yuyvBufferSize, &ppInputAddr, &ppInputFd)) {
        LOG_WARN("%s: Could not allocate pre-processor tensor\n", __func__);
        Model_Cleanup();
        return 0;
    }

    if (!createAndMapTmpFile(OBJECT_DETECTOR_INPUT_FILE_PATTERN, modelWidth * modelHeight * channels, &larodInputAddr, &larodInputFd)) {
        LOG_WARN("%s: Could not allocate input tensor\n", __func__);
        Model_Cleanup();
        return 0;
    }
    if (!createAndMapTmpFile(OBJECT_DETECTOR_OUT1_FILE_PATTERN, outputBufferSize, &larodOutput1Addr, &larodOutput1Fd)) {
        LOG_WARN("%s: Could not allocate output tensor\n", __func__);
        Model_Cleanup();
        return 0;
    }

    // Allocate space for HD preprocessing input/output
    size_t nv12BufferSizeHD = videoWidth * videoHeight * 3 / 2;
    size_t rgbBufferSizeHD = videoWidth * videoHeight * 3;
    if (!createAndMapTmpFile(PP_HD_INPUT_FILE_PATTERN,
                             nv12BufferSizeHD,
							 &ppInputAddrHD,
							 &ppInputFdHD)) {
        LOG_WARN("%s: Could not allocate HD preproc input tensor\n", __func__);
        Model_Cleanup();
        return 0;
    }
    if (!createAndMapTmpFile(PP_HD_OUTPUT_FILE_PATTERN,
	                         rgbBufferSizeHD,
							 &ppOutputAddrHD,
							 &ppOutputFdHD)) {
        LOG_WARN("%s: Could not allocate HD preproc output tensor\n", __func__);
        Model_Cleanup();
        return 0;
    }

    // Connect tensors to file descriptors
    if (!larodSetTensorFd(ppInputTensors[0], ppInputFd, &error)) {
        LOG_WARN("%s: Failed setting input tensor fd: %s\n", __func__, error->msg);
        larodClearError(&error);
        Model_Cleanup();
        return 0;
    }
    if (!larodSetTensorFd(ppOutputTensors[0], larodInputFd, &error)) {
        LOG_WARN("%s: Failed setting output tensor fd: %s\n", __func__, error->msg);
        larodClearError(&error);
        Model_Cleanup();
        return 0;
    }
    // HD preprocessing tensor connections
    if (!larodSetTensorFd(ppInputTensorsHD[0], ppInputFdHD, &error)) {
        LOG_WARN("%s: Failed setting HD input tensor fd: %s\n", __func__, error->msg);
        larodClearError(&error);
        Model_Cleanup();
        return 0;
    }
    if (!larodSetTensorFd(ppOutputTensorsHD[0], ppOutputFdHD, &error)) {
        LOG_WARN("%s: Failed setting HD output tensor fd: %s\n", __func__, error->msg);
        larodClearError(&error);
        Model_Cleanup();
        return 0;
    }

    if (!larodSetTensorFd(inputTensors[0], larodInputFd, &error)) {
        LOG_WARN("%s: Failed setting input tensor fd: %s\n", __func__, error->msg);
        larodClearError(&error);
        Model_Cleanup();
        return 0;
    }

    if (!larodSetTensorFd(outputTensors[0], larodOutput1Fd, &error)) {
        LOG_WARN("%s: Failed setting output tensor fd: %s\n", __func__, error->msg);
        Model_Cleanup();
        return 0;
    }

    // Create job requests
    ppReq = larodCreateJobRequest(ppModel,
                                  ppInputTensors,
                                  ppInputs,
                                  ppOutputTensors,
                                  ppOutputs,
                                  NULL,
                                  &error);
    if (!ppReq) {
        LOG_WARN("%s: Failed creating preprocessing job request: %s\n", __func__, error->msg);
        Model_Cleanup();
        return 0;
    }
    // HD preprocessing job request
    ppReqHD = larodCreateJobRequest(ppModelHD,
                                    ppInputTensorsHD,
                                    ppNumInputsHD,
                                    ppOutputTensorsHD,
                                    ppNumOutputsHD,
                                    NULL,
                                    &error);
    if (!ppReqHD) {
        LOG_WARN("%s: Failed creating HD preproc job request: %s\n", __func__, error ? error->msg : "");
        Model_Cleanup();
        return 0;
    }

    infReq = larodCreateJobRequest(InfModel,
                                   inputTensors,
                                   inputs,
                                   outputTensors,
                                   outputs,
                                   NULL,
                                   &error);
    if (!infReq) {
        LOG_WARN("%s: Failed creating inference request: %s\n", __func__, error->msg);
        Model_Cleanup();
        return 0;
    }

    clear_crop_cache();

    ACAP_STATUS_SetString("model", "status", "Model OK.");
    ACAP_STATUS_SetBool("model", "state", 1);

    return modelConfig;
}
