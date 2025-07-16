#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <math.h>
#include "larod.h"
#include "cJSON.h"
#include "imgutils.h"
#include "ACAP.h"
#include "Model.h"

#define LOG(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }
#define LOG_WARN(fmt, args...)    { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args); }
#define LOG_TRACE(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }
//#define LOG_TRACE(fmt, args...)    {}

static unsigned int modelWidth = 640;
static unsigned int modelHeight = 640;
static unsigned int videoWidth = 640;
static unsigned int videoHeight = 480;
static unsigned int channels = 3;
static unsigned int boxes = 0;
static unsigned int classes = 0;
static float quant = 1.0f;
static float quant_zero = 0;
static float objectnessThreshold = 0.25f;
static float confidenceThreshold = 0.30f;
static float nms = 0.05f;
static int larodModelFd = -1;
static larodConnection* conn = NULL;
static larodModel* infModel = NULL;
static larodModel* ppModel = NULL;
static larodJobRequest* ppReq = NULL;
static larodJobRequest* infReq = NULL;
static larodMap* ppMap = NULL;
static void* ppInputAddr = MAP_FAILED;
static void* larodInputAddr = MAP_FAILED;
static void* larodOutput1Addr = MAP_FAILED;
static int ppInputFd = -1;
static int larodInputFd = -1;
static int larodOutput1Fd = -1;
static larodTensor** inputTensors = NULL;
static larodTensor** outputTensors = NULL;
static larodTensor** ppInputTensors = NULL;
static larodTensor** ppOutputTensors = NULL;
static size_t yuyvBufferSize = 0;
static cJSON* modelConfig = NULL;
static int inferenceErrors = 5;


static char PP_SD_INPUT_FILE_PATTERN[] = "/tmp/larod.pp.test-XXXXXX";
static char OBJECT_DETECTOR_INPUT_FILE_PATTERN[] = "/tmp/larod.in.test-XXXXXX";
static char OBJECT_DETECTOR_OUT1_FILE_PATTERN[]  = "/tmp/larod.out1.test-XXXXXX";

// Utility: Create & map temp file for processing
static bool createAndMapTmpFile(char* fileName, size_t fileSize, void** mappedAddr, int* convFd) {
//	LOG_TRACE("<%s\n",__func__);
//LOG("%s %ld",fileName,fileSize);	
    int fd = mkstemp(fileName);
    if (fd < 0) {
		LOG_TRACE("Error %s>\n",__func__);
		return false;
	}
    if (ftruncate(fd, (off_t)fileSize) < 0) {
		close(fd);
		LOG_TRACE("Error %s>\n",__func__);
		return false;
	}
    if (unlink(fileName)) {
		close(fd);
		LOG_TRACE("Error %s>\n",__func__);
		return false;
	}
    void* data = mmap(NULL, fileSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
		close(fd);
		LOG_TRACE("Error %s>\n",__func__);
		return false;
	}
    *mappedAddr = data;
	*convFd = fd;
//	LOG_TRACE("%s>\n",__func__);
	return true;
}

// IoU for NMS
static float iou(float x1, float y1, float w1, float h1, float x2, float y2, float w2, float h2) {
    float xx1 = fmaxf(x1 - w1/2, x2 - w2/2);
    float yy1 = fmaxf(y1 - h1/2, y2 - h2/2);
    float xx2 = fminf(x1 + w1/2, x2 + w2/2);
    float yy2 = fminf(y1 + h1/2, y2 + h2/2);
    float inter = fmaxf(0, xx2 - xx1) * fmaxf(0, yy2 - yy1);
    float union_ = w1 * h1 + w2 * h2 - inter;
    return union_ > 0 ? inter / union_ : 0;
}

// NMS implementation
static cJSON* non_maximum_suppression(cJSON* list) {
    if (!list) return NULL;
    int items = cJSON_GetArraySize(list); if (items < 2) return list;
    int* keep = (int*)malloc(items * sizeof(int)); memset(keep, 1, items * sizeof(int));
    for (int i = 0; i < items; i++) {
        if (keep[i]) {
            cJSON* det = cJSON_GetArrayItem(list, i);
            float x1 = cJSON_GetObjectItem(det, "x")->valuedouble;
            float y1 = cJSON_GetObjectItem(det, "y")->valuedouble;
            float w1 = cJSON_GetObjectItem(det, "w")->valuedouble;
            float h1 = cJSON_GetObjectItem(det, "h")->valuedouble;
            float c1 = cJSON_GetObjectItem(det, "c")->valuedouble;
            for (int j = i + 1; j < items; j++) {
                if (keep[j]) {
                    cJSON* alt = cJSON_GetArrayItem(list, j);
                    float x2 = cJSON_GetObjectItem(alt, "x")->valuedouble;
                    float y2 = cJSON_GetObjectItem(alt, "y")->valuedouble;
                    float w2 = cJSON_GetObjectItem(alt, "w")->valuedouble;
                    float h2 = cJSON_GetObjectItem(alt, "h")->valuedouble;
                    float c2 = cJSON_GetObjectItem(alt, "c")->valuedouble;
                    if (iou(x1, y1, w1, h1, x2, y2, w2, h2) > nms) {
                        if (c1 > c2) keep[j] = 0;
                        else { keep[i] = 0; break; }
                    }
                }
            }
        }
    }
    cJSON* out = cJSON_CreateArray();
    for (int i = 0; i < items; i++) {
        if (keep[i]) cJSON_AddItemToArray(out, cJSON_Duplicate(cJSON_GetArrayItem(list, i), 1));
    }
    cJSON_Delete(list); free(keep); return out;
}

static void export_detection_crop(
    unsigned char* rgb_full, unsigned int full_w, unsigned int full_h,
    int crop_x, int crop_y, int crop_w, int crop_h, int jpeg_quality,
    bool save_sd, bool send_mqtt, const char* label, double timestamp, int box_idx)
{
//	LOG_TRACE("<%s\n",__func__);

    unsigned char* crop = crop_interleaved(rgb_full, full_w, full_h, 3, crop_x, crop_y, crop_w, crop_h);
    struct jpeg_compress_struct comp_conf;
    unsigned long jpeg_size = 0;
    unsigned char* jpeg_buf = NULL;
    set_jpeg_configuration(crop_w, crop_h, 3, jpeg_quality, &comp_conf);
    buffer_to_jpeg(crop, &comp_conf, &jpeg_size, &jpeg_buf);

    char fname[128];
    if (save_sd) {
        snprintf(fname, sizeof(fname), "/var/spool/storage/SD_DISK/detectx/crop_%s_%.0f_%d.jpg", label, timestamp, box_idx);
        jpeg_to_file(fname, jpeg_buf, jpeg_size);
        LOG("Cropped detection written to SD: %s\n", fname);
    }
    if (send_mqtt) {
        // Write your MQTT transmit routine here.
        // Example: MQTT_PublishCrop(jpeg_buf, jpeg_size, label, timestamp);
        LOG("Cropped detection sent via MQTT (label: %s, ts: %.0f, sz: %lu)\n", label, timestamp, jpeg_size);
    }
    free(crop);
    free(jpeg_buf);
//	LOG_TRACE("%s>\n",__func__);	
}

cJSON* Model_Setup(void) {
    larodError* error = NULL;

	LOG_TRACE("<%s\n",__func__);
	
    ACAP_STATUS_SetString("model","status","Model initialization failed.  Check log file");
    ACAP_STATUS_SetBool("model","state", 0);	

    modelConfig = ACAP_FILE_Read("model/model.json");
    if (!modelConfig) {
        LOG_WARN("%s: Unable to read model.json\n", __func__);
        return NULL;
    }

    modelWidth = cJSON_GetObjectItem(modelConfig, "modelWidth")->valueint;
    modelHeight = cJSON_GetObjectItem(modelConfig, "modelHeight")->valueint;
    videoWidth = cJSON_GetObjectItem(modelConfig, "videoWidth")->valueint;
    videoHeight = cJSON_GetObjectItem(modelConfig, "videoHeight")->valueint;
    boxes = cJSON_GetObjectItem(modelConfig,"boxes")->valueint;
    classes = cJSON_GetObjectItem(modelConfig,"classes")->valueint;
    quant = cJSON_GetObjectItem(modelConfig,"quant")->valuedouble;
    quant_zero = cJSON_GetObjectItem(modelConfig,"zeroPoint")->valuedouble;
    objectnessThreshold = cJSON_GetObjectItem(modelConfig,"objectness")->valuedouble;
    nms = cJSON_GetObjectItem(modelConfig,"nms")->valuedouble;

    const char* modelPath = cJSON_GetObjectItem(modelConfig,"path") ? cJSON_GetObjectItem(modelConfig,"path")->valuestring : 0;
    if(!modelPath) {
        LOG_WARN("%s: Could not find model path\n", __func__);
        Model_Cleanup();
        return NULL;
    }
    larodModelFd = open(modelPath, O_RDONLY);
    if (larodModelFd < 0) {
        LOG_WARN("%s: Could not open model %s\n", __func__, modelPath);
        Model_Cleanup();
        return NULL;
    }
    if (!larodConnect(&conn, &error)) {
        LOG_WARN("%s: Could not connect to larod: %s\n", __func__, error->msg);
        Model_Cleanup();
        return NULL;
    }
    const char* chipString = cJSON_GetObjectItem(modelConfig,"chip") ?
        cJSON_GetObjectItem(modelConfig,"chip")->valuestring : "cpu-tflite";
    const larodDevice* device = larodGetDevice(conn, chipString, 0, &error);
    if (!device) {
        LOG_WARN("%s: Could not get device %s: %s\n", __func__, chipString, error->msg);
        larodClearError(&error);
        Model_Cleanup();
        return NULL;
    }
    infModel = larodLoadModel(conn, larodModelFd, device, LAROD_ACCESS_PRIVATE, "object_detection", NULL, &error);
    if (!infModel) {
        LOG_WARN("%s: Unable to load model: %s\n", __func__, error->msg);
        larodClearError(&error);
        Model_Cleanup();
        return NULL;
    }

    // Use cpu-proc as image preprocessing backend
    const larodDevice* device_prepros = larodGetDevice(conn, "cpu-proc", 0, &error);
    if (!device_prepros) {
        LOG_WARN("%s: Could not get device cpu-proc: %s\n", __func__, error->msg);
        larodClearError(&error);
        Model_Cleanup();
        return NULL;
    }
    ppMap = larodCreateMap(&error);
    larodMapSetStr(ppMap, "image.input.format", "nv12", &error);
    larodMapSetIntArr2(ppMap,"image.input.size", videoWidth, videoHeight, &error);
    larodMapSetStr(ppMap, "image.output.format", "rgb-interleaved", &error);
    larodMapSetIntArr2(ppMap,"image.output.size", modelWidth, modelHeight, &error);

    ppModel = larodLoadModel(conn, -1, device_prepros, LAROD_ACCESS_PRIVATE, "", ppMap, &error);
    if (!ppModel) {
        LOG_WARN("%s: Unable to load preprocessing model: %s\n", __func__, error->msg);
        larodClearError(&error);
        Model_Cleanup();
        return NULL;
    }
    ppInputTensors = larodCreateModelInputs(ppModel, NULL, &error);
    ppOutputTensors = larodCreateModelOutputs(ppModel, NULL, &error);

    inputTensors = larodCreateModelInputs(infModel, NULL, &error);
    outputTensors = larodCreateModelOutputs(infModel, NULL, &error);

    // Tensor buffer sizes
    const larodTensorPitches* ppInputPitches = larodGetTensorPitches(ppInputTensors[0], &error);
    yuyvBufferSize = ppInputPitches->pitches[0];

    // Output buffer size sanity-check
    const larodTensorPitches* ppOutputPitches = larodGetTensorPitches(ppOutputTensors[0], &error);
    size_t rgbBufferSize = ppOutputPitches->pitches[0];
    if (rgbBufferSize != modelWidth * modelHeight * channels) {
        LOG_WARN("%s: RGB buffer size mismatch\n", __func__);
        Model_Cleanup();
        return NULL;
    }

    // Map memory for all buffers
    if (!createAndMapTmpFile(PP_SD_INPUT_FILE_PATTERN, yuyvBufferSize, &ppInputAddr, &ppInputFd)) {
        LOG_WARN("%s: Could not allocate pre-processor tensor\n", __func__);
        Model_Cleanup();
        return NULL;
    }
    if (!createAndMapTmpFile(OBJECT_DETECTOR_INPUT_FILE_PATTERN, modelWidth * modelHeight * channels, &larodInputAddr, &larodInputFd)) {
        LOG_WARN("%s: Could not allocate input tensor\n", __func__);
        Model_Cleanup();
        return NULL;
    }
    if (!createAndMapTmpFile(OBJECT_DETECTOR_OUT1_FILE_PATTERN, boxes * (classes + 5), &larodOutput1Addr, &larodOutput1Fd)) {
        LOG_WARN("%s: Could not allocate output tensor\n", __func__);
        Model_Cleanup();
        return NULL;
    }

    larodSetTensorFd(ppInputTensors[0], ppInputFd, &error);
    larodSetTensorFd(ppOutputTensors[0], larodInputFd, &error);
    larodSetTensorFd(inputTensors[0], larodInputFd, &error);
    larodSetTensorFd(outputTensors[0], larodOutput1Fd, &error);

    ppReq = larodCreateJobRequest(ppModel,
                                  ppInputTensors, 1,
                                  ppOutputTensors, 1,
                                  NULL, &error);
    infReq = larodCreateJobRequest(infModel,
                                   inputTensors, 1,
                                   outputTensors, 1,
                                   NULL, &error);

    ACAP_STATUS_SetString("model","status","Model OK.");
    ACAP_STATUS_SetBool("model","state", 1);
	LOG_TRACE("%s>\n",__func__);
    return modelConfig;
}

void Model_Cleanup(void) {
    larodError* error = NULL;
    if(ppMap) larodDestroyMap(&ppMap);
    if(ppModel) larodDestroyModel(&ppModel);
    larodDestroyModel(&infModel);
    if(conn) larodDisconnect(&conn, NULL);
    if(larodModelFd >= 0) close(larodModelFd);
    if(larodInputAddr != MAP_FAILED) munmap(larodInputAddr, modelWidth * modelHeight * channels);
    if(larodInputFd >= 0) close(larodInputFd);
    if(ppInputAddr != MAP_FAILED) munmap(ppInputAddr, modelWidth * modelHeight * channels);
    if(ppInputFd >= 0) close(ppInputFd);
    if(larodOutput1Addr != MAP_FAILED) munmap(larodOutput1Addr, boxes * (classes+5));
    larodDestroyJobRequest(&ppReq);
    larodDestroyJobRequest(&infReq);
    larodDestroyTensors(conn, &inputTensors, 1, &error);
    larodDestroyTensors(conn, &outputTensors, 1, &error);
    larodClearError(&error);
    ACAP_STATUS_SetString("model","status","Model stopped");
    ACAP_STATUS_SetBool("model","state", 0);
}

cJSON* Model_Inference(VdoBuffer* image) {
    larodError* error = NULL;
    if(!image) return NULL;
    if(ACAP_STATUS_Bool("model", "state") == 0) return NULL;
    if(inferenceErrors <= 0) {
        LOG_WARN("Too many inference errors.  Model stopped\n");
        Model_Cleanup();
        return NULL;
    }

    cJSON* settings = ACAP_Get_Config("settings");
    cJSON* cropping = cJSON_GetObjectItem(settings,"cropping");
    bool cropping_active = cropping && cJSON_GetObjectItem(cropping,"active") && cJSON_IsTrue(cJSON_GetObjectItem(cropping,"active"));
    bool cropping_sdcard = cropping && cJSON_GetObjectItem(cropping,"sdcard") && cJSON_IsTrue(cJSON_GetObjectItem(cropping,"sdcard"));
    bool cropping_mqtt   = cropping && cJSON_GetObjectItem(cropping,"mqtt") && cJSON_IsTrue(cJSON_GetObjectItem(cropping,"mqtt"));
    int jpeg_quality = 90;

    uint8_t* nv12Data = (uint8_t*)vdo_buffer_get_data(image);
    memcpy(ppInputAddr, nv12Data, yuyvBufferSize);

    if (!larodRunJob(conn, ppReq, &error)) { larodClearError(&error); inferenceErrors--; return NULL; }
    if (lseek(larodOutput1Fd, 0, SEEK_SET) == -1) { inferenceErrors--; return NULL; }
    if (!larodRunJob(conn, infReq, &error)) { larodClearError(&error); inferenceErrors--; return NULL; }
    uint8_t* output_tensor = (uint8_t*)larodOutput1Addr;

    cJSON* detections = cJSON_CreateArray();
    for (int i = 0; i < boxes; i++) {
        int box = i * (5 + classes);
        float objectness = (float)(output_tensor[box + 4] - quant_zero) * quant;
        if (objectness < objectnessThreshold) continue;
        float x = (output_tensor[box + 0] - quant_zero) * quant;
        float y = (output_tensor[box + 1] - quant_zero) * quant;
        float w = (output_tensor[box + 2] - quant_zero) * quant;
        float h = (output_tensor[box + 3] - quant_zero) * quant;
        int classId = -1; float maxConf = 0;
        for (int c = 0; c < classes; c++) {
            float conf = (float)(output_tensor[box + 5 + c] - quant_zero) * quant * objectness;
            if(conf > maxConf) { classId = c; maxConf = conf; }
        }
        if (maxConf > confidenceThreshold) {
            const char* label = "Undefined";
            cJSON* labels = cJSON_GetObjectItem(modelConfig,"labels");
            if(labels && classId >= 0 && cJSON_GetArrayItem(labels, classId))
                label = cJSON_GetArrayItem(labels, classId)->valuestring;
            cJSON* det = cJSON_CreateObject();
            cJSON_AddStringToObject(det,"label",label);
            cJSON_AddNumberToObject(det,"c",maxConf);
            cJSON_AddNumberToObject(det,"x",x - w / 2);
            cJSON_AddNumberToObject(det,"y",y - h / 2);
            cJSON_AddNumberToObject(det,"w",w);
            cJSON_AddNumberToObject(det,"h",h);
            cJSON_AddItemToArray(detections, det);
        }
    }

    detections = non_maximum_suppression(detections);

    if (cropping_active && (cropping_sdcard || cropping_mqtt)) {
        unsigned char* rgb_full = (unsigned char*)larodInputAddr;
        int full_w = modelWidth, full_h = modelHeight;
        double timestamp = ACAP_DEVICE_Timestamp();

        int idx = 0;
        cJSON* detection = detections->child;
        while (detection) {
            double x = cJSON_GetObjectItem(detection,"x")->valuedouble * full_w;
            double y = cJSON_GetObjectItem(detection,"y")->valuedouble * full_h;
            double w = cJSON_GetObjectItem(detection,"w")->valuedouble * full_w;
            double h = cJSON_GetObjectItem(detection,"h")->valuedouble * full_h;
            int crop_x = (int)fmax(0, x);
            int crop_y = (int)fmax(0, y);
            int crop_w = (int)fmin(w, full_w - crop_x);
            int crop_h = (int)fmin(h, full_h - crop_y);
            const char* label = cJSON_GetObjectItem(detection,"label") ? cJSON_GetObjectItem(detection,"label")->valuestring : "unknown";
            export_detection_crop(
                rgb_full, full_w, full_h,
                crop_x, crop_y, crop_w, crop_h, jpeg_quality,
                cropping_sdcard, cropping_mqtt, label, timestamp, idx
            );
            idx++;
            detection = detection->next;
        }
    }

    return detections;
}
