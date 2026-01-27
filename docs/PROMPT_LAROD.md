# LAROD (Lightweight Accelerated Runtime for Object Detection) Reference

Machine learning inference framework for Axis cameras with hardware acceleration support.

## Overview

Larod provides a unified API for running ML models on various hardware backends including DLPU (Deep Learning Processing Unit), CPU, and other accelerators. It handles model loading, tensor management, preprocessing, and inference execution.

## Headers

```c
#include "larod.h"

// Compile with
// -DLAROD_API_VERSION_3
```

## Connection Management

```c
larodConnection* conn = NULL;
larodError* error = NULL;

if (!larodConnect(&conn, &error)) {
    syslog(LOG_ERR, "Failed to connect to larod: %s", error->msg);
    larodClearError(&error);
    return false;
}

// ... use connection ...

larodDisconnect(&conn, NULL);
```

## Available Devices

### Device Names by Platform

| Platform | Device Name | Description |
|----------|-------------|-------------|
| ARTPEC-8 | `a8-dlpu-tflite` | DLPU with TFLite runtime |
| ARTPEC-9 | `a9-dlpu-tflite` | DLPU with TFLite runtime |
| CV25 | `ambarella-cvflow` | Ambarella CVflow accelerator |
| All | `cpu-proc` | CPU-based preprocessing (libyuv) |
| All | `cpu-tflite` | CPU TFLite (fallback, slower) |

### Device Detection

```c
// Auto-detect based on platform
const char* detect_device(larodConnection* conn) {
    larodError* error = NULL;

    // Try ARTPEC-9 first
    const larodDevice* dev = larodGetDevice(conn, "a9-dlpu-tflite", 0, &error);
    if (dev) return "a9-dlpu-tflite";
    larodClearError(&error);

    // Try ARTPEC-8
    dev = larodGetDevice(conn, "a8-dlpu-tflite", 0, &error);
    if (dev) return "a8-dlpu-tflite";
    larodClearError(&error);

    // Fallback to CPU
    return "cpu-tflite";
}
```

### Platform-Based Detection (Recommended)

```c
// Using ACAP_DEVICE_Prop (if available)
const char* platform = ACAP_DEVICE_Prop("platform");

if (strstr(platform, "artpec9")) {
    device_name = "a9-dlpu-tflite";
} else if (strstr(platform, "artpec8")) {
    device_name = "a8-dlpu-tflite";
} else {
    device_name = "cpu-tflite";
}
```

## Model Loading

### Load from File

```c
int model_fd = open("model/model.tflite", O_RDONLY);
if (model_fd < 0) {
    // Handle error
}

const larodDevice* device = larodGetDevice(conn, "a9-dlpu-tflite", 0, &error);

larodModel* model = larodLoadModel(conn,
                                    model_fd,
                                    device,
                                    LAROD_ACCESS_PRIVATE,
                                    "my_model",      // Name for debugging
                                    NULL,            // Optional params map
                                    &error);

if (!model) {
    syslog(LOG_ERR, "Failed to load model: %s", error->msg);
}

// Close fd after loading (larod keeps internal copy)
close(model_fd);
```

### Power Availability Retry

Models may fail to load if DLPU power is unavailable:

```c
larodModel* model = NULL;
int retries = 0;
const int MAX_RETRIES = 50;

while (!model && retries < MAX_RETRIES) {
    model = larodLoadModel(conn, model_fd, device, LAROD_ACCESS_PRIVATE,
                           "model", NULL, &error);

    if (!model && error->code == LAROD_ERROR_POWER_NOT_AVAILABLE) {
        larodClearError(&error);
        retries++;
        usleep(250 * 1000 * retries);  // Exponential backoff
    } else if (!model) {
        // Other error - fail
        break;
    }
}
```

## Tensor Management

### Allocate Input/Output Tensors

```c
size_t num_inputs, num_outputs;
larodTensor** input_tensors = NULL;
larodTensor** output_tensors = NULL;

// Allocate tensors based on model definition
input_tensors = larodAllocModelInputs(conn, model, 0, &num_inputs, NULL, &error);
output_tensors = larodAllocModelOutputs(conn, model, 0, &num_outputs, NULL, &error);
```

### Get Tensor Dimensions (Model Introspection)

```c
const larodTensorDims* dims = larodGetTensorDims(input_tensors[0], &error);

// For NHWC layout (typical for TFLite)
uint32_t batch = dims->dims[0];   // Usually 1
uint32_t height = dims->dims[1];
uint32_t width = dims->dims[2];
uint32_t channels = dims->dims[3];

// For NCHW layout
uint32_t batch = dims->dims[0];
uint32_t channels = dims->dims[1];
uint32_t height = dims->dims[2];
uint32_t width = dims->dims[3];
```

### Get Tensor Data Type

```c
larodTensorDataType dtype = larodGetTensorDataType(output_tensors[0], &error);

switch (dtype) {
    case LAROD_TENSOR_DATA_TYPE_FLOAT32:
        // 32-bit floating point
        break;
    case LAROD_TENSOR_DATA_TYPE_INT8:
        // Signed 8-bit integer (quantized)
        break;
    case LAROD_TENSOR_DATA_TYPE_UINT8:
        // Unsigned 8-bit integer (quantized)
        break;
}
```

### Memory-Map Tensor Buffers

```c
// Get file descriptor for tensor buffer
int fd = larodGetTensorFd(input_tensors[0], &error);

// Get buffer size
size_t buffer_size;
larodGetTensorFdSize(input_tensors[0], &buffer_size, &error);

// Memory map for read/write access
void* input_addr = mmap(NULL, buffer_size, PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd, 0);

// Copy input data
memcpy(input_addr, frame_data, buffer_size);
```

### Bind External Buffer to Tensor

```c
// Use your own buffer instead of larod-allocated
int my_buffer_fd = /* your fd */;
larodSetTensorFd(output_tensors[0], my_buffer_fd, &error);
```

## Preprocessing with cpu-proc

The `cpu-proc` device provides hardware-accelerated image preprocessing using libyuv.

### Create Preprocessing Model

```c
larodMap* pp_map = larodCreateMap(&error);

// Input configuration
larodMapSetStr(pp_map, "image.input.format", "nv12", &error);
larodMapSetIntArr2(pp_map, "image.input.size", input_width, input_height, &error);
larodMapSetInt(pp_map, "image.input.row-pitch", input_pitch, &error);

// Output configuration
larodMapSetStr(pp_map, "image.output.format", "rgb-interleaved", &error);
larodMapSetIntArr2(pp_map, "image.output.size", output_width, output_height, &error);
larodMapSetInt(pp_map, "image.output.row-pitch", output_pitch, &error);

// Get cpu-proc device
const larodDevice* pp_device = larodGetDevice(conn, "cpu-proc", 0, &error);

// Load preprocessing "model" (actually a processing pipeline)
larodModel* pp_model = larodLoadModel(conn, -1, pp_device,
                                       LAROD_ACCESS_PRIVATE, "preprocess",
                                       pp_map, &error);

larodDestroyMap(&pp_map);
```

### Preprocessing Format Strings

| Format String | VDO Equivalent | Description |
|---------------|----------------|-------------|
| `"nv12"` | VDO_FORMAT_YUV | Y plane + interleaved UV |
| `"rgb-interleaved"` | VDO_FORMAT_RGB | RGB packed |
| `"rgb-planar"` | VDO_FORMAT_PLANAR_RGB | Separate R, G, B planes |

### Center Cropping

```c
// Calculate crop region for aspect ratio matching
float dest_ratio = (float)model_width / model_height;
float crop_w = (float)input_width;
float crop_h = crop_w / dest_ratio;

if (crop_h > input_height) {
    crop_h = input_height;
    crop_w = crop_h * dest_ratio;
}

unsigned int clip_w = (unsigned int)crop_w;
unsigned int clip_h = (unsigned int)crop_h;
unsigned int clip_x = (input_width - clip_w) / 2;
unsigned int clip_y = (input_height - clip_h) / 2;

// Create crop map for job request
larodMap* crop_map = larodCreateMap(&error);
larodMapSetIntArr4(crop_map, "image.input.crop",
                   clip_x, clip_y, clip_w, clip_h, &error);
```

### Preprocessing Parameters Reference

| Parameter | Type | Description |
|-----------|------|-------------|
| `image.input.format` | string | Input pixel format |
| `image.input.size` | int[2] | Input width, height |
| `image.input.row-pitch` | int | Input row stride in bytes |
| `image.input.crop` | int[4] | Crop region: x, y, width, height |
| `image.output.format` | string | Output pixel format |
| `image.output.size` | int[2] | Output width, height |
| `image.output.row-pitch` | int | Output row stride in bytes |

## Job Execution

### Create Job Request

```c
// For inference
larodJobRequest* inf_request = larodCreateJobRequest(
    model,
    input_tensors, num_inputs,
    output_tensors, num_outputs,
    NULL,  // Optional params map (e.g., crop_map)
    &error
);

// For preprocessing with cropping
larodJobRequest* pp_request = larodCreateJobRequest(
    pp_model,
    pp_input_tensors, pp_num_inputs,
    pp_output_tensors, pp_num_outputs,
    crop_map,  // Crop parameters
    &error
);
```

### Run Job (Synchronous)

```c
if (!larodRunJob(conn, inf_request, &error)) {
    if (error->code == LAROD_ERROR_POWER_NOT_AVAILABLE) {
        // DLPU power unavailable - retry later
        larodClearError(&error);
        usleep(100000);
        return RETRY;
    }
    syslog(LOG_ERR, "Inference failed: %s", error->msg);
    return ERROR;
}
```

### Power Error Handling Pattern

```c
bool run_inference_with_retry(larodConnection* conn,
                              larodJobRequest* request) {
    larodError* error = NULL;
    static int power_retries = 0;

    if (!larodRunJob(conn, request, &error)) {
        if (error->code == LAROD_ERROR_POWER_NOT_AVAILABLE) {
            larodClearError(&error);
            power_retries++;

            if (power_retries > 50) {
                syslog(LOG_ERR, "Power unavailable after %d retries", power_retries);
                return false;
            }

            // Exponential backoff
            usleep(250 * 1000 * power_retries);
            return false;  // Caller should retry
        }

        syslog(LOG_ERR, "Job failed: %s", error->msg);
        larodClearError(&error);
        return false;
    }

    power_retries = 0;  // Reset on success
    return true;
}
```

## Quantization Parameters

For INT8 quantized models, you need scale and zero-point for dequantization:

```c
// Get quantization parameters from tensor metadata
larodTensorQuantizationParameters qparams;
if (larodGetTensorQuantizationParameters(output_tensors[0], &qparams, &error)) {
    float scale = qparams.scale;
    int32_t zero_point = qparams.zero_point;

    // Dequantize: real_value = (quantized_value - zero_point) * scale
}
```

### Manual Dequantization

```c
float dequantize_int8(int8_t quantized, float scale, int32_t zero_point) {
    return ((float)quantized - (float)zero_point) * scale;
}

// For output tensor processing
int8_t* output_data = (int8_t*)output_addr;
for (int i = 0; i < num_elements; i++) {
    float value = dequantize_int8(output_data[i], scale, zero_point);
    // Process value...
}
```

## Cleanup

```c
// Destroy job requests
larodDestroyJobRequest(&inf_request);
larodDestroyJobRequest(&pp_request);

// Destroy tensors
larodDestroyTensors(conn, &input_tensors, num_inputs, &error);
larodDestroyTensors(conn, &output_tensors, num_outputs, &error);

// Destroy models
larodDestroyModel(&model);
larodDestroyModel(&pp_model);

// Destroy maps
larodDestroyMap(&crop_map);

// Unmap memory
munmap(input_addr, buffer_size);
munmap(output_addr, output_size);

// Disconnect
larodDisconnect(&conn, NULL);

// Always clear errors
larodClearError(&error);
```

## Complete Inference Pipeline Example

```c
typedef struct {
    larodConnection* conn;
    larodModel* model;
    larodModel* pp_model;
    larodTensor** input_tensors;
    larodTensor** output_tensors;
    larodTensor** pp_input_tensors;
    larodTensor** pp_output_tensors;
    larodJobRequest* pp_request;
    larodJobRequest* inf_request;
    void* input_addr;
    void* output_addr;
    size_t input_size;
    size_t output_size;
} InferencePipeline;

bool pipeline_run(InferencePipeline* p, void* frame_data, size_t frame_size) {
    // Copy frame to preprocessing input
    memcpy(p->input_addr, frame_data, frame_size);

    // Run preprocessing (color conversion, scaling, cropping)
    if (!run_inference_with_retry(p->conn, p->pp_request)) {
        return false;
    }

    // Run inference
    if (!run_inference_with_retry(p->conn, p->inf_request)) {
        return false;
    }

    // Output is now in p->output_addr
    return true;
}
```

## Error Codes

| Error Code | Description |
|------------|-------------|
| `LAROD_ERROR_NONE` | Success |
| `LAROD_ERROR_POWER_NOT_AVAILABLE` | DLPU power unavailable (retry) |
| `LAROD_ERROR_INVALID_PARAM` | Invalid parameter |
| `LAROD_ERROR_DEVICE_NOT_FOUND` | Device not available on platform |
| `LAROD_ERROR_MODEL_LOAD_FAILED` | Model loading failed |
| `LAROD_ERROR_JOB_FAILED` | Job execution failed |

## Best Practices

1. **Power Handling**: Always handle `LAROD_ERROR_POWER_NOT_AVAILABLE` with exponential backoff
2. **Device Selection**: Auto-detect device based on platform rather than hardcoding
3. **Memory Management**: Memory-map tensors for zero-copy when possible
4. **Preprocessing**: Use `cpu-proc` for efficient color conversion and scaling
5. **Model Introspection**: Query tensor dimensions instead of hardcoding
6. **Quantization**: Get scale/zero_point from model metadata
7. **Cleanup**: Destroy all resources in reverse order of creation
8. **Error Handling**: Always check return values and clear errors

## Tensor Layouts

| Layout | Dimension Order | Typical Use |
|--------|-----------------|-------------|
| NHWC | [batch, height, width, channels] | TFLite default |
| NCHW | [batch, channels, height, width] | PyTorch/ONNX |
| 420SP | [Y plane, UV interleaved] | NV12 video |

## Related Documentation

- [VDO.md](VDO.md) - Video capture (provides frames for inference)
- ACAP Native SDK documentation
- TensorFlow Lite documentation
