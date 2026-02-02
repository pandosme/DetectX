# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

DetectX is an ACAP (Axis Camera Application Platform) application that runs custom YOLOv5 object detection models directly on Axis network cameras with ARTPEC-8 and ARTPEC-9 chipsets. The application performs real-time inference on-camera and exports detections via MQTT, ONVIF, and HTTP.

This is a native C application that integrates with:
- Axis ACAP SDK (version 12.x, native SDK)
- larod (Axis ML inference framework for DLPU hardware acceleration)
- VDO (Video Device Object) for camera video capture
- Paho MQTT for messaging
- FastCGI for web UI backend

## Building and Development

### Build Commands

**Build the entire application:**
```bash
./build.sh
```
This creates `.eap` files (Axis application packages) in the root directory, ready for installation on cameras. Model parameters are automatically extracted during the build process.

**Clean build artifacts:**
```bash
cd app && make clean
```

### Build Process

1. `build.sh` uses Docker with the Axis ACAP SDK image (`axisecp/acap-native-sdk:12.5.0`)
2. The Dockerfile copies `app/` contents and runs `acap-build` to compile and package
3. Output is copied back as `.eap` files (one for ARTPEC-8, one for ARTPEC-9 if multi-architecture build)
4. The Makefile in `app/` compiles all C sources and links against ACAP SDK libraries

### Key Build Dependencies

The Makefile links against these packages (via pkg-config):
- `gio-2.0`, `gio-unix-2.0` - GLib I/O and event loop
- `liblarod` - Axis ML inference library
- `vdostream` - Video capture
- `fcgi` - FastCGI for HTTP endpoints
- `axevent` - Axis event system
- `libcurl` - HTTP client for posting crops

Also links statically: `libjpeg`, `libturbojpeg` (in `app/lib/`)

## Architecture

### Core Components

**main.c**
- Application entry point and main event loop
- `ImageProcess()`: Main inference loop that captures frames, runs inference, applies filters (AOI, confidence, size), and calls Output()
- Manages application configuration (settings.json, events.json) through ACAP wrapper
- Interfaces with all other modules

**ACAP.c/h**
- Wrapper around Axis ACAP SDK (version 3.7)
- Handles HTTP/FastCGI endpoints for web UI backend (registered via `ACAP_HTTP_Node()`)
- Manages persistent JSON configuration files
- Provides device information and status API
- Event system integration (ONVIF events for detection states)

**Model.c/h**
- Sets up and runs neural network inference using larod
- `Model_Setup()`: Initializes model, extracts parameters from TFLite file, allocates larod tensors
- `Model_Inference()`: Preprocesses YUV/RGB video frames, runs inference, performs NMS (non-maximum suppression), returns cJSON array of detections
- `Model_GetImageData()`: Provides JPEG-encoded crops of detections with configurable borders
- `Model_Reset()`: Clears crop cache between inference cycles

**Video.c/h**
- Abstracts VDO (Video Device Object) frame capture
- `Video_Start_YUV()` / `Video_Start_RGB()`: Initialize video streams
- `Video_Capture_YUV()` / `Video_Capture_RGB()`: Capture single frames
- Returns `VdoBuffer*` for processing by Model module

**Output.c/h** (split into multiple files)
- `Output.c`: Main entry point, processes filtered detections
- `Output_crop_cache.c`: Manages in-memory cache of recent detection crops for web UI
- `Output_http.c`: Posts detection crops to external HTTP endpoints
- `Output_helpers.c`: Event state management (transition debouncing, minimum duration)
- Publishes detections and events to MQTT
- Manages ONVIF event states per label

**MQTT.c/h**
- Paho MQTT client wrapper (version 2.0)
- Handles connection, reconnection, publishing
- `MQTT_Publish()`, `MQTT_Publish_JSON()`, `MQTT_Publish_Binary()`: Publish to topics
- Topics: `{pretopic}/detection/{serial}`, `{pretopic}/event/{serial}/{label}/{state}`, `{pretopic}/crop/{serial}`

**CERTS.c/h**
- Certificate management for MQTT TLS connections
- Stores CA certificates, client certificates, and keys

### Configuration Files

**app/manifest.json**
- ACAP package metadata: name, version, vendor
- HTTP endpoint definitions (FastCGI nodes: app, settings, status, device, model, mqtt, certs, crops)

**app/model/model.tflite**
- TFLite model file with int8 quantization
- Model parameters are automatically extracted at runtime by Model.c
- Supported formats: YOLOv5 with per-tensor quantization

**app/model/labels.txt**
- Text file with one label per line
- Loaded at runtime by labelparse.c
- Order must match model output classes

**app/settings/settings.json**
- Detection parameters: confidence threshold, AOI (area of interest), minimum size
- Event settings: stabilization time, minimum event duration, prioritize (accuracy vs responsiveness)
- Cropping configuration: active, throttle, output methods (MQTT/HTTP/SD), border adjustments

**app/settings/mqtt.json**
- MQTT broker connection: address, port, username, password
- TLS configuration
- Topic prefix (pretopic)
- Device metadata: name, location

**app/settings/events.json**
- Event state tracking (typically empty at startup)

### Data Flow

1. **Capture**: `Video_Capture_YUV()` gets a frame from camera
2. **Inference**: `Model_Inference(buffer)` runs YOLOv5 model via larod, returns detections array
3. **Filtering**: main.c applies AOI, confidence, size filters to detections
4. **Output**: `Output(processedDetections)` handles:
   - MQTT detection messages (bounding boxes)
   - Event state management (transition debouncing, per-label state)
   - MQTT event messages (label active/inactive)
   - Crop generation and caching (if enabled)
   - HTTP posting of crops (if configured)
5. **Reset**: `Model_Reset()` cleans up crop buffers
6. Loop repeats (triggered by GLib idle callback)

### Web UI

Located in `app/html/`:
- `index.html` - Main detection visualization page
- `mqtt.html` - MQTT configuration
- `advanced.html` - Event/label settings
- `cropping.html` - Detection export configuration
- `about.html` - Status and diagnostics
- `crops.html` - View cached detection crops
- Uses jQuery, Bootstrap, and custom media-stream-player for video overlay

Backend endpoints (FastCGI) are handled in ACAP.c callback functions that return JSON.

## Model Training and Customization

### Training Your Own Model

Follow the YOLOv5 training process documented in [docs/Train-Build.md](docs/Train-Build.md):

1. Clone YOLOv5 and apply Axis patch for ARTPEC compatibility
2. Train with desired dataset (use 640x640 or other multiple of 32)
3. Export to TFLite with int8 quantization and per-tensor quantization
4. Replace `app/model/model.tflite` and `app/model/labels.txt`
5. Run `./build.sh` - model parameters are automatically extracted during build

### Customizing Detection Output

The primary customization point is in **main.c** and **Output.c**:

- **main.c `ImageProcess()`**: Modify detection filtering logic (custom AOI shapes, multi-region detection, etc.)
- **Output.c `Output()`**: Add custom output destinations or modify payload formats
- **Output_http.c**: Customize HTTP POST payloads for detection crops

### Manifest Changes

To rename the application or change version, edit `app/manifest.json`:
- `friendlyName`: Display name in camera UI
- `appName`: Package identifier (must match executable name in Makefile PROG1)
- `version`: Semantic version

## Common Development Tasks

### Adding a New HTTP Endpoint

1. Add entry to `app/manifest.json` httpConfig array
2. Register handler in main.c or ACAP.c: `ACAP_HTTP_Node("endpoint", callback_function)`
3. Implement callback with signature: `void callback(ACAP_HTTP_Response response, const ACAP_HTTP_Request request)`

### Modifying Detection Logic

Key functions to modify:
- **main.c `ImageProcess()`**: Pre-output filtering and processing
- **Model.c `Model_Inference()`**: Post-processing (NMS, coordinate transforms)
- **Output.c `Output()`**: Output routing and event state logic

### Adding New Configuration Settings

1. Add fields to `app/settings/settings.json` (or create new JSON file)
2. Include file in Dockerfile `acap-build` command with `-a` flag
3. Access in code via: `cJSON* cfg = ACAP_Get_Config("settings")`
4. Update web UI to expose new settings

## Platform-Specific Notes

**ARTPEC-8 vs ARTPEC-9:**
- Model input size affects inference time significantly (see performance table in Train-Build.md)
- ARTPEC-9 supports larger models and faster inference
- Choose model size (nano/small/medium) based on target platform

**larod chips:**
- `axis-a8-dlpu-tflite` for ARTPEC-8
- `a9-dlpu-tflite` for ARTPEC-9
- Automatically detected at runtime based on device capabilities

## Debugging and Troubleshooting

**View application logs:**
On camera via SSH: `journalctl -f -u detectx` (assuming appName is "detectx")

**Check model status:**
Web UI "About" page shows model state, inference time, DLPU backend

**MQTT connection issues:**
Check broker connectivity from "MQTT" page in web UI. Verify firewall rules and credentials.

**Memory leaks:**
Recent fixes (3.5.3) addressed memory leaks in crop caching and video buffer handling. Always call `Model_Reset()` after processing detections.

## Recent Changes

Version 3.5.3 (Nov 29, 2025): Fixed memory leak
Version 3.5.2 (Aug 29, 2025): Fixed black-box video issue
Version 3.5.1 (Aug 29, 2025): Added Detection Export feature, MQTT improvements

See [README.md](README.md) for complete changelog.
