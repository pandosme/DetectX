# Runtime Model Introspection Implementation Plan

## Branch: feature/runtime-model-introspection

This branch implements runtime model introspection to generate model.json dynamically, eliminating the need for prepare.py while maintaining 100% backward compatibility with the legacy implementation.

## Status: IN PROGRESS

### Completed
- ✅ Created branch from main
- ✅ Added labelparse.c and labelparse.h for runtime label parsing
- ✅ Updated Makefile to include labelparse.c

### Remaining Tasks

#### 1. Update Model.c includes

Add to includes section (after line 16):
```c
#include "labelparse.h"
```

Add static variables after line 79:
```c
static char** modelLabels = NULL;
static char* labelBuffer = NULL;
static size_t numLabels = 0;
static const char* MODEL_PATH = "model/model.tflite";
static const char* LABELS_PATH = "model/labels.txt";
```

#### 2. Replace Model_Setup model.json loading (lines 521-537)

Replace the `ACAP_FILE_Read("model/model.json")` section with runtime introspection:

```c
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

// Determine chip from platform
const char* chipString = "a9-dlpu-tflite";  // Default for ARTPEC-9
// TODO: Could auto-detect from platform if needed

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

// Get input dimensions
const larodTensorDims* inputDims = larodGetTensorDims(tempInputTensors[0], &error);
if (!inputDims || inputDims->numDims != 4) {
    LOG_WARN("%s: Invalid input tensor dimensions\n", __func__);
    larodDestroyTensors(conn, &tempInputTensors, inputs, NULL);
    larodDestroyTensors(conn, &tempOutputTensors, outputs, NULL);
    Model_Cleanup();
    return 0;
}

modelHeight = inputDims->dims[1];
modelWidth = inputDims->dims[2];
channels = inputDims->dims[3];

LOG("Model input: %ux%ux%u\n", modelWidth, modelHeight, channels);

// Get output dimensions and quantization
const larodTensorDims* outputDims = larodGetTensorDims(tempOutputTensors[0], &error);
if (!outputDims || outputDims->numDims < 2) {
    LOG_WARN("%s: Invalid output tensor dimensions\n", __func__);
    larodDestroyTensors(conn, &tempInputTensors, inputs, NULL);
    larodDestroyTensors(conn, &tempOutputTensors, outputs, NULL);
    Model_Cleanup();
    return 0;
}

boxes = outputDims->dims[1];
int stride = outputDims->dims[2];
classes = stride - 5;  // YOLOv5 format: x,y,w,h,objectness,class1...classN

LOG("Model output: %u boxes, %u classes, stride=%d\n", boxes, classes, stride);

// Get quantization parameters
larodTensorDataType dataType = larodGetTensorDataType(tempOutputTensors[0], &error);
if (dataType == LAROD_TENSOR_DATA_TYPE_INT8 || dataType == LAROD_TENSOR_DATA_TYPE_UINT8) {
    float quant_scale = 0;
    int quant_offset = 0;
    if (larodGetTensorQuantization(tempOutputTensors[0], &quant_scale, &quant_offset, &error)) {
        quant = quant_scale;
        quant_zero = quant_offset;
        LOG("Quantization: scale=%.6f, zero=%d\n", quant, (int)quant_zero);
    } else {
        LOG("Using default quantization (float model)\n");
        quant = 1.0;
        quant_zero = 0;
    }
} else {
    quant = 1.0;
    quant_zero = 0;
}

// Clean up temporary tensors
larodDestroyTensors(conn, &tempInputTensors, inputs, &error);
larodDestroyTensors(conn, &tempOutputTensors, outputs, &error);

// Step 4: Calculate optimal 4:3 video dimensions
// Use legacy prepare.py mapping for compatibility
if (modelHeight == 640) {
    videoWidth = 800;
    videoHeight = 600;
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
    // Fallback: calculate 4:3 from model size
    videoHeight = (modelHeight * 3) / 4;
    videoWidth = (videoHeight * 4) / 3;
    // Ensure even dimensions (VDO requirement)
    if (videoWidth % 2) videoWidth++;
    if (videoHeight % 2) videoHeight++;
}

LOG("Video dimensions: %ux%u (4:3 aspect ratio)\n", videoWidth, videoHeight);

// Step 5: Load labels
if (!labels_parse_file(LABELS_PATH, &modelLabels, &labelBuffer, &numLabels)) {
    LOG_WARN("%s: Failed to load labels from %s, using defaults\n", __func__, LABELS_PATH);
    numLabels = 0;
} else {
    LOG("Loaded %zu labels from %s\n", numLabels, LABELS_PATH);
}

// Step 6: Build model.json structure
modelConfig = cJSON_CreateObject();
cJSON_AddNumberToObject(modelConfig, "modelWidth", modelWidth);
cJSON_AddNumberToObject(modelConfig, "modelHeight", modelHeight);
cJSON_AddNumberToObject(modelConfig, "videoWidth", videoWidth);
cJSON_AddNumberToObject(modelConfig, "videoHeight", videoHeight);
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

LOG("Model config generated: %ux%u model, %ux%u video, %u boxes, %u classes\n",
    modelWidth, modelHeight, videoWidth, videoHeight, boxes, classes);

// ==== END RUNTIME INTROSPECTION ====
```

#### 3. Update Model_Inference to use parsed labels

Replace label lookup (around line 214-217):
```c
const char* label = "Undefined";
cJSON* labels = cJSON_GetObjectItem(modelConfig, "labels");
if (labels && classId >= 0 && cJSON_GetArrayItem(labels, classId))
    label = cJSON_GetArrayItem(labels, classId)->valuestring;
```

With:
```c
const char* label = labels_get(modelLabels, numLabels, classId);
```

#### 4. Update Model_Cleanup to free labels

Add to Model_Cleanup (around line 490):
```c
labels_free(modelLabels, labelBuffer);
modelLabels = NULL;
labelBuffer = NULL;
numLabels = 0;
```

#### 5. Remove Model Loading from setup section

DELETE lines 595-630 (model loading section that starts with "// Model (inference)") since we now load it during introspection.

#### 6. Update Dockerfile

Change line 26 from:
```dockerfile
    -a 'model/model.json'
```

To:
```dockerfile
    -a 'model/labels.txt'
```

#### 7. Delete legacy prepare.py

```bash
git rm prepare.py
```

## Testing Checklist

- [ ] Build succeeds
- [ ] App starts and loads model
- [ ] Logs show correct dimensions (800x600 for 640x640 model)
- [ ] Detections work (overlay and crops)
- [ ] Coordinates are correct (no left/right shift)
- [ ] Labels display correctly
- [ ] Model.json is NOT required in package

## Backward Compatibility

This implementation maintains 100% API compatibility:
- Same video dimensions as legacy (800x600 for 640 model)
- Same coordinate system (no transformations)
- Same preprocessing pipeline
- Same output format

The only difference is model.json is generated at runtime instead of being provided as a file.
