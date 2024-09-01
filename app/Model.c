#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <errno.h>

#include "larod.h"
#include "ACAP.h"
#include "Model.h"

#define LOG(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args);}
#define LOG_WARN(fmt, args...)    { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args);}
//#define LOG_TRACE(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }
#define LOG_TRACE(fmt, args...)    {}

static bool createAndMapTmpFile(char* fileName, size_t fileSize, void** mappedAddr, int* convFd);
float iou(float x1, float y1, float w1, float h1, float x2, float y2, float w2, float h2);
void Model_Close();
cJSON* non_maximum_suppression(cJSON* list);

unsigned int modelWidth = 640;
unsigned int modelHeight = 640;
size_t inputs = 1;
size_t outputs = 1;
size_t ppInputs = 1;
size_t ppOutputs = 1;
unsigned int videoWidth = 1280;
unsigned int videoHeight = 720;
unsigned int channels = 3;
unsigned int boxes = 0;
unsigned int classes = 0;
float quant = 1.0;
float quant_zero = 0;
float objectnessThreshold = 0;
float nms = 0.05;
int larodModelFd = -1;
larodConnection* conn = NULL;
larodModel* InfModel = NULL;
larodModel* ppModel = NULL;
larodJobRequest* ppReq = NULL;
larodMap* ppMap;
larodJobRequest* infReq;
void* ppInputAddr = MAP_FAILED;
void* larodInputAddr = MAP_FAILED;
void* larodOutput1Addr = MAP_FAILED;
int ppInputFd = -1;
int larodInputFd = -1;
int larodOutput1Fd = -1;
larodTensor** inputTensors = NULL;
larodTensor** outputTensors = NULL;
larodTensor** ppInputTensors = NULL;
larodTensor** ppOutputTensors = 0;
size_t yuyvBufferSize = 0;

cJSON* modelConfig = 0;

char PP_SD_INPUT_FILE_PATTERN[] = "/tmp/larod.pp.test-XXXXXX";
char OBJECT_DETECTOR_INPUT_FILE_PATTERN[] = "/tmp/larod.in.test-XXXXXX";
char OBJECT_DETECTOR_OUT1_FILE_PATTERN[]  = "/tmp/larod.out1.test-XXXXXX";

int inferenceErrors = 5;

cJSON*
Model_Inference(VdoBuffer* image) {
    larodError* error = NULL;

	if(!image) {
		LOG_TRACE("%s: No image\n",__func__);
		return 0;
	}

	if( !ACAP_STATUS_Bool( "model", "state" ) ) {  //The Model Was not Loaded
		LOG_TRACE("%s: Model not running\n",__func__);
		return 0;
	}

	if( inferenceErrors <= 0 ) {
		LOG_WARN("Too many inference errors.  Model stopped\n" );
		Model_Close();
		return 0;
	}


	uint8_t* nv12Data = (uint8_t*)vdo_buffer_get_data(image);
	memcpy(ppInputAddr, nv12Data, yuyvBufferSize);
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

    if (!larodRunJob(conn, infReq, &error)) {
		LOG_WARN("%s: Unable to run inference on model: %s (%d)\n", __func__, error->msg, error->code);
        larodClearError(&error);
		inferenceErrors--;
        return 0;
    }

	uint8_t* output_tensor = (uint8_t*)larodOutput1Addr;

	cJSON* list = cJSON_CreateArray();


	for (int i = 0; i < boxes; i++) {
		int box = i * (5 + classes);
		float objectness = (output_tensor[box + 4] - quant_zero) * quant;
        if (objectness >= objectnessThreshold) {
            float x = (output_tensor[box + 0] - quant_zero) * quant;
            float y = (output_tensor[box + 1] - quant_zero) * quant;
            float w = (output_tensor[box + 2] - quant_zero) * quant;
            float h = (output_tensor[box + 3] - quant_zero) * quant;
			int classId = -1;
			float maxConfidence = 0;
			for( int c = 0; c < classes; c++ ) {
				float confidence = (output_tensor[box + 5 + c] - quant_zero) * quant;
				if( confidence > maxConfidence ) {
					classId = c;
					maxConfidence = confidence;
				}
			}
			if( maxConfidence > objectness ) {
				const char* label = "Undefined";
				cJSON* labels = cJSON_GetObjectItem(modelConfig,"labels");
				if( labels && classId >= 0 && cJSON_GetArrayItem(labels, classId) )
					label = cJSON_GetArrayItem(labels, classId)->valuestring;
				cJSON* detection = cJSON_CreateObject();
				cJSON_AddStringToObject( detection,"label",label);
				cJSON_AddNumberToObject( detection,"c",maxConfidence);
				cJSON_AddNumberToObject( detection,"x",x - (w/2));
				cJSON_AddNumberToObject( detection,"y",y - (h/2));
				cJSON_AddNumberToObject( detection,"w",w);
				cJSON_AddNumberToObject( detection,"h",h);
				cJSON_AddItemToArray(list,detection);
			}
		}
	}
	return non_maximum_suppression( list );
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

cJSON*
non_maximum_suppression(cJSON* list) {
	
	int items = cJSON_GetArraySize(list);
	if( items < 2 )
		return list;
	LOG_TRACE("%s: In %d\n", __func__, items);
	
    int keep[items];
    memset(keep, 1, items * sizeof(int));
	
    for (int i = 0; i < items; i++) {
		if( keep[i] ) {
			cJSON* detection = cJSON_GetArrayItem(list,i);
			float x1 = cJSON_GetObjectItem(detection,"x")->valuedouble;
			float y1 = cJSON_GetObjectItem(detection,"y")->valuedouble;
			float w1 = cJSON_GetObjectItem(detection,"w")->valuedouble;
			float h1 = cJSON_GetObjectItem(detection,"h")->valuedouble;
			float c1 = cJSON_GetObjectItem(detection,"c")->valuedouble;

			for (int j = i + 1; j < items; j++) {
				if( keep[j] ) {
					cJSON* alternative = cJSON_GetArrayItem(list,i);
					float x2 = cJSON_GetObjectItem(alternative,"x")->valuedouble;
					float y2 = cJSON_GetObjectItem(alternative,"y")->valuedouble;
					float w2 = cJSON_GetObjectItem(alternative,"w")->valuedouble;
					float h2 = cJSON_GetObjectItem(alternative,"h")->valuedouble;
					float c2 = cJSON_GetObjectItem(alternative,"c")->valuedouble;

					if (iou(x1, y1, w1, h1, x2, y2, w2, h2) > nms) {
						if (c1 > c2) {
							keep[i] = 1;
							keep[j] = 0;
						} else {
							keep[j] = 1;
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
		cJSON *detection = cJSON_GetArrayItem(list,i);
		if( keep[i] )
			cJSON_AddItemToArray( result,  cJSON_Duplicate(detection, 1) );
	}
	LOG_TRACE("%s: Exit %d\n", __func__, cJSON_GetArraySize(result));
	cJSON_Delete( list );
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
Model_Close() {
    // Only the model handle is released here. We count on larod service to
    // release the privately loaded model when the session is disconnected in
    // larodDisconnect().
    larodError* error = NULL;
	
	if( ppMap ) larodDestroyMap(&ppMap);
    if( ppModel ) larodDestroyModel(&ppModel);
    larodDestroyModel(&InfModel);
    if (conn) larodDisconnect(&conn, NULL);
    if (larodModelFd >= 0) close(larodModelFd);
    if (larodInputAddr != MAP_FAILED) munmap(larodInputAddr, modelWidth * modelHeight * channels);
    if (larodInputFd >= 0) close(larodInputFd);
    if (ppInputAddr != MAP_FAILED) munmap(ppInputAddr, modelWidth * modelHeight * channels);
    if (ppInputFd >= 0) close(ppInputFd);
    if (larodOutput1Addr != MAP_FAILED) munmap(larodOutput1Addr, boxes * (classes + 5));
    larodDestroyJobRequest(&ppReq);
    larodDestroyJobRequest(&infReq);
    larodDestroyTensors(conn, &inputTensors, inputs, &error);
    larodDestroyTensors(conn, &outputTensors, outputs, &error);
    larodClearError(&error);
	ACAP_STATUS_SetString("model","status","Model stopped");
	ACAP_STATUS_SetBool("model","state", 0);	
}



cJSON*
Model_Setup() {

	LOG_TRACE("%s: Entry\n", __func__);

    larodError* error = NULL;

	ACAP_STATUS_SetString("model","status","Model initialization failed.  Check log file");
	ACAP_STATUS_SetBool("model","state", 0);	
	
	modelConfig = ACAP_FILE_Read( "html/config/model.json" );
	if( !modelConfig ) {
        LOG_WARN("%s: Unabel to read model.json\n", __func__);
		return 0;
	}

	LOG_TRACE("%s: Initializing\n", __func__);

	modelWidth = cJSON_GetObjectItem(modelConfig,"modelWidth")->valueint;
	modelHeight = cJSON_GetObjectItem(modelConfig,"modelHeight")->valueint;
	videoWidth = cJSON_GetObjectItem(modelConfig,"videoWidth")->valueint;
	videoHeight = cJSON_GetObjectItem(modelConfig,"videoHeight")->valueint;
	boxes = cJSON_GetObjectItem(modelConfig,"boxes")->valueint;
	classes = cJSON_GetObjectItem(modelConfig,"classes")->valueint;
	quant = cJSON_GetObjectItem(modelConfig,"quant")->valuedouble;
	quant_zero = cJSON_GetObjectItem(modelConfig,"zeroPoint")->valuedouble;
	objectnessThreshold = cJSON_GetObjectItem(modelConfig,"objectness")->valuedouble;
	nms = cJSON_GetObjectItem(modelConfig,"nms")->valuedouble;

	

    ppMap = larodCreateMap(&error);
    if (!ppMap) {
        LOG_WARN("%s: Could not create preprocessing larodMap %s\n",__func__, error->msg);
		Model_Close();
		return 0;
    }
    if (!larodMapSetStr(ppMap, "image.input.format", "nv12", &error)) {
        LOG_WARN("%s: Failed setting preprocessing parameters: %s\n", __func__, error->msg);
		Model_Close();
		return 0;
    }
    if (!larodMapSetIntArr2(ppMap, "image.input.size", videoWidth, videoHeight, &error)) {
        LOG_WARN("%s: Failed setting preprocessing parameters: %s\n", __func__, error->msg);
		Model_Close();
		return 0;
    }
    if (!larodMapSetStr(ppMap, "image.output.format", "rgb-interleaved", &error)) {
        LOG_WARN("%s: Failed setting preprocessing parameters: %s\n", __func__,error->msg);
		Model_Close();
		return 0;
    }

    if (!larodMapSetIntArr2(ppMap, "image.output.size", modelWidth, modelHeight, &error)) {
        LOG_WARN("%s: Failed setting preprocessing parameters: %s\n", __func__,error->msg);
		Model_Close();
		return 0;
    }

    // Model file
    const char* modelPath = cJSON_GetObjectItem(modelConfig,"path")?cJSON_GetObjectItem(modelConfig,"path")->valuestring:0;
	if(!modelPath) {
        LOG_WARN("%s: Could not find model path\n", __func__);
		Model_Close();
		return 0;
	}

	larodModelFd = open(modelPath, O_RDONLY,modelPath);
	if(larodModelFd < 0) {
        LOG_WARN("%s: Could not open model %s\n", __func__,modelPath);
		Model_Close();
		return 0;
	}

	//Connect to larod
    if (!larodConnect(&conn, &error)) {
        LOG_WARN("%s: Could not connect to larod: %s\n", __func__, error->msg);
		Model_Close();
		return 0;
    }

	const char* chipString = "cpu-tflite";
	cJSON* chip = cJSON_GetObjectItem(modelConfig,"chip");
	if( chip && chip->type == cJSON_String ) 
		chipString = chip->valuestring;
	
    const larodDevice* device = larodGetDevice(conn, chipString, 0, &error);
    if (!device) {
        LOG_WARN("%s: Could not get device %s: %s\n", __func__, chipString, error->msg);
        larodClearError(&error);
		Model_Close();
        return 0;
    }

    InfModel = larodLoadModel(conn, larodModelFd, device, LAROD_ACCESS_PRIVATE, "object_detection", NULL, &error);
    if (!InfModel) {
        LOG_WARN("%s: Unable to load model: %s\n", __func__, error->msg);
        larodClearError(&error);
        Model_Close();
        return 0;
    }

    // Use libyuv as image preprocessing backend
    const char* larodLibyuvPP = "cpu-proc";
    const larodDevice* device_prePros  = larodGetDevice(conn, larodLibyuvPP, 0, &error);
	if(!device_prePros) {
        LOG_WARN("%s: Could not get device %s: %s\n", __func__, larodLibyuvPP, error->msg);
        larodClearError(&error);
		Model_Close();
        return 0;
	}
    ppModel = larodLoadModel(conn, -1, device_prePros, LAROD_ACCESS_PRIVATE, "", ppMap, &error);
    if (!ppModel) {
        LOG_WARN("%s: Unable to load preprocessing model with chip %s: %s",__func__,larodLibyuvPP, error->msg);
        larodClearError(&error);
		Model_Close();
        return 0;
    } else {
		LOG("Loading preprocessing model with chip %s\n", larodLibyuvPP);
	}

    // Create input/output tensors
    ppInputTensors = larodCreateModelInputs(ppModel, &ppInputs, &error);
    if (!ppInputTensors) {
        LOG_WARN("%s: Failed retrieving input tensors: %s\n",__func__,error->msg);
        larodClearError(&error);
		Model_Close();
        return 0;
    }
    ppOutputTensors = larodCreateModelOutputs(ppModel, &ppOutputs, &error);
    if (!ppOutputTensors) {
        LOG_WARN("%s: Failed retrieving output tensors: %s\n",__func__, error->msg);
        larodClearError(&error);
		Model_Close();
        return 0;
    }

    inputTensors = larodCreateModelInputs(InfModel, &inputs, &error);
    if (!inputTensors) {
        LOG_WARN("%s: Failed retrieving input tensors: %s\n",__func__, error->msg);
        larodClearError(&error);
		Model_Close();
        return 0;
    }

    outputTensors = larodCreateModelOutputs(InfModel, &outputs, &error);
    if (!outputTensors) {
        LOG_WARN("%s: Failed retrieving output tensors: %s\n", __func__, error->msg);
        larodClearError(&error);
		Model_Close();
        return 0;
    }

    // Determine tensor buffer sizes
    const larodTensorPitches* ppInputPitches = larodGetTensorPitches(ppInputTensors[0], &error);
    if (!ppInputPitches) {
        LOG_WARN("%s: Could not get pitches of tensor: %s\n",__func__, error->msg);
        larodClearError(&error);
		Model_Close();
        return 0;
    }

    yuyvBufferSize = ppInputPitches->pitches[0];
    const larodTensorPitches* ppOutputPitches = larodGetTensorPitches(ppOutputTensors[0], &error);
    if (!ppOutputPitches) {
        LOG_WARN("%s: Could not get pitches of tensor: %s\n",__func__, error->msg);
        larodClearError(&error);
		Model_Close();
        return 0;
    }

    size_t rgbBufferSize = ppOutputPitches->pitches[0];
    size_t expectedSize  = modelWidth * modelHeight * channels;
    if (expectedSize != rgbBufferSize) {
        LOG_WARN("%s: Expected video output size %zu, actual %zu\n", __func__, expectedSize, rgbBufferSize);
		Model_Close();
        return 0;
    }
    const larodTensorPitches* outputPitches = larodGetTensorPitches(outputTensors[0], &error);
    if (!outputPitches) {
        LOG_WARN("%s: Could not get pitches of tensor: %s\n",__func__, error->msg);
		Model_Close();
        return 0;
    }
	
    // Allocate space for input tensor
    if (!createAndMapTmpFile(PP_SD_INPUT_FILE_PATTERN, yuyvBufferSize, &ppInputAddr, &ppInputFd)) {
        LOG_WARN("%s: Could not allocate pre-processor tensor\n",__func__);
		Model_Close();
        return 0;
    }
    if (!createAndMapTmpFile(OBJECT_DETECTOR_INPUT_FILE_PATTERN, modelWidth * modelHeight * channels, &larodInputAddr, &larodInputFd)) {
        LOG_WARN("%s: Could not allocate input tensor\n",__func__);
		Model_Close();
        return 0;
    }

    if (!createAndMapTmpFile(OBJECT_DETECTOR_OUT1_FILE_PATTERN, boxes * (classes + 5), &larodOutput1Addr, &larodOutput1Fd)) {
        LOG_WARN("%s: Could not allocate output tenso\n",__func__);
		Model_Close();
        return 0;
    }

    // Connect tensors to file descriptors
    if (!larodSetTensorFd(ppInputTensors[0], ppInputFd, &error)) {
        LOG_WARN("%s: Failed setting input tensor fd: %s\n",__func__, error->msg);
        larodClearError(&error);
		Model_Close();
        return 0;
    }
    if (!larodSetTensorFd(ppOutputTensors[0], larodInputFd, &error)) {
        LOG_WARN("%s: Failed setting input tensor fd: %s\n", __func__, error->msg);
        larodClearError(&error);
		Model_Close();
        return 0;
    }

    if (!larodSetTensorFd(inputTensors[0], larodInputFd, &error)) {
        LOG_WARN("%s: Failed setting input tensor fd: %s\n", __func__, error->msg);
        larodClearError(&error);
		Model_Close();
        return 0;
    }

    if (!larodSetTensorFd(outputTensors[0], larodOutput1Fd, &error)) {
        LOG_WARN("%s: Failed setting output tensor fd: %s\n", __func__, error->msg);
		Model_Close();
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
        LOG_WARN("%s: Failed creating preprocessing job request: %s\n", __func__,error->msg);
		Model_Close();
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
        LOG_WARN("%s: Failed creating inference request: %s\n", __func__,error->msg);
		Model_Close();
        return 0;
    }
	LOG("Success loding model to %s\n",chipString);
	LOG("%s: Objectness = %f\n",__func__, objectnessThreshold );
	
	ACAP_STATUS_SetString("model","status","Model OK.");
	ACAP_STATUS_SetBool("model","state", 1);
    return modelConfig;
}

