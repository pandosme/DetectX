# AGENTS.md

Guidance for coding agents working in this repository. Read this file before
changing code, then inspect the files that own the behavior you are modifying.
The implementation and build scripts are authoritative when documentation
disagrees.

## Project at a glance

DetectX is a native C ACAP application that runs YOLOv8 object detection on Axis
cameras with ARTPEC-8 or ARTPEC-9. It captures camera frames through VDO, runs
inference through larod on the DLPU, filters detections, and publishes results
through MQTT, ONVIF events, HTTP, and the built-in web UI.

The application is currently version 5.0.1. DetectX 5.x accepts the repository's
split-output YOLOv8 TFLite format; YOLOv5/DetectX 4.x models are incompatible.
ARTPEC-9 requires Axis OS 13 or later.

Core dependencies are supplied by the Axis ACAP SDK: GLib/GIO, larod, VDO,
FastCGI, axevent, AXStorage, and libcurl. Paho MQTT headers and prebuilt JPEG
libraries are vendored under `app/`.

## Start here

Before editing:

1. Run `git status --short`. The worktree may contain user changes; never discard
   or rewrite unrelated work.
2. Read the owning `.c` and `.h` files together.
3. Check the related configuration JSON and HTML page when changing settings,
   endpoints, or user-visible behavior.
4. Prefer a small change followed by the narrowest available validation.

There is no unit-test, integration-test, lint, or formatter suite. The full
Docker build is the main compile and package check.

## Build and validation

Builds run in Docker with the Axis ACAP Native SDK. Do not expect the application
to compile directly on a normal Linux host.

```sh
./build.sh                 # build ARTPEC-8 and ARTPEC-9 packages
./build.sh --target a8     # build only ARTPEC-8
./build.sh --target a9     # build only ARTPEC-9
./build.sh --clean         # rebuild Docker layers without cache
CRUNTIME=podman ./build.sh --target a8
```

Packages are written to the repository root as
`DetectX_<version>_artpec8.eap` and `DetectX_<version>_artpec9.eap`.

Each requested target needs its ignored model binary:

- `app/model/model-a8.tflite`
- `app/model/model-a9.tflite`

The Dockerfile uses ACAP SDK 12.11.0 on Ubuntu 24.04 and TensorFlow 2.21.0. It
selects the target model as `model/model.tflite`, validates it with
`app/extract_model_params.py`, generates `model_params.h`, and invokes
`acap-build`. For ARTPEC-9 it also raises the packaged manifest's minimum Axis OS
to 13.0.

Do not run `make clean` from `app/` in the working tree. That target is intended
for the copied container build context and removes `app/lib/` and
`app/manifest.json` along with generated files.

Use checks proportional to the change:

```sh
sh -n build.sh benchmark_larod.sh benchmark_resolutions.sh export_resolutions.sh
PYTHONPYCACHEPREFIX=/tmp/detectx-pycache \
  python3 -m py_compile export_yolov8.py app/extract_model_params.py
python3 -m json.tool app/manifest.json >/dev/null
./build.sh --target a8     # definitive check for native C/build changes
```

The GitHub Actions workflow in `.github/workflows/build-eap.yml` builds both
targets, verifies both packages, uploads artifacts, and publishes matching tag
releases. It does not run a separate test suite.

## Model export contract

Use `export_yolov8.py`; Ultralytics' stock TFLite export does not produce the
format consumed by DetectX.

```sh
python3 export_yolov8.py \
  --target a8 \
  --weights /path/to/best.pt \
  --calibration-dir /path/to/images \
  --width 960 --height 544 \
  --output app/model/model-a8.tflite
```

Repeat with `--target a9` and the A9 output path. Both dimensions must be
multiples of 32. The model must have uint8 input and two uint8 outputs:
coordinates shaped `[1, 4, N]` and class scores shaped `[1, classes, N]`, with no
separate objectness value. ARTPEC-8 requires per-tensor quantization; ARTPEC-9
uses per-channel quantization. `app/model/labels.txt` must contain one label per
output class in model order.

See `Train-Build.md` for training, calibration, performance, and export details.
Do not hand-edit generated model parameters.

## Runtime architecture

The main path is:

1. `main.c` initializes ACAP configuration, HTTP nodes, AXStorage, MQTT, output
   state, and the GLib main loop.
2. Model startup is deferred because DLPU initialization may be slow. Failed
   setup retries rather than terminating the app.
3. `ImageProcess()` captures a YUV frame through `Video.c`, calls
   `Model_Inference()`, applies label/confidence/size/AOI/exclusion filters, and
   passes accepted detections to `Output()`.
4. `Output.c` publishes detection and event messages, updates ONVIF state,
   maintains crop output, and optionally saves full-frame training captures and
   YOLO labels to application-owned SD storage.
5. `Model_Reset()` releases per-frame crop data before the next iteration.

Important modules:

- `app/main.c`: lifecycle, filtering, model upload, HTTP callbacks, migrations.
- `app/Model.c`: larod setup, preprocessing, YOLOv8 decode, NMS, crop buffers.
- `app/Video.c` and `app/imgprovider.c`: VDO stream and frame ownership.
- `app/Output.c`: MQTT payloads, event transitions, crop routing, SD capture.
- `app/Output_helpers.c`: encoding, event helpers, SD files and archives.
- `app/Output_crop_cache.c`: in-memory crop gallery.
- `app/Output_http.c`: external HTTP posts.
- `app/Storage.c`: asynchronous AXStorage setup and app-owned storage path.
- `app/MQTT.c`: Paho client lifecycle, reconnects, settings, publishing.
- `app/ACAP.c`: configuration, FastCGI, status/device APIs, files, ONVIF events.
- `app/CERTS.c`: MQTT TLS certificate storage and HTTP handling.

## Configuration and HTTP

Persistent configuration lives in:

- `app/settings/settings.json`: detection, polygons, event, crop, and SD-capture
  defaults.
- `app/settings/mqtt.json`: broker, TLS, topic prefix, and device metadata.
- `app/settings/events.json`: persisted event state.
- `app/manifest.json`: package metadata, permissions, resources, and FastCGI
  endpoint declarations.

Coordinate regions are normalized to the `0..1000` range. Keep legacy migration
logic intact when changing their schema.

When adding an HTTP endpoint, update all three surfaces:

1. Declare it in `app/manifest.json` under `httpConfig`.
2. Register it with `ACAP_HTTP_Node()` during startup.
3. Implement the callback with the existing response helpers and request-method
   checks.

Current custom endpoints include `model`, `modelinput`, `crops`, `sd_download`,
and `sd_clear`. Runtime model upload writes application-local model data and
schedules an app restart.

## Web UI

The UI is static HTML/CSS/JavaScript under `app/html/`, using jQuery, Bootstrap,
and the Axis media stream player. There is no frontend build step.

- `index.html`: live video, overlay, confidence, AOI, and exclusion zones.
- `advanced.html`: labels, event behavior, and SD training capture.
- `cropping.html`: MQTT/HTTP detection crop export.
- `crops.html`: in-memory crop gallery.
- `model.html`: model metadata and upload.
- `mqtt.html`, `certificate.html`, `about.html`: connectivity and diagnostics.

Keep setting names synchronized across HTML/JavaScript, default JSON, and C
readers. Reuse the existing UI and endpoint patterns instead of adding a new
framework or asset pipeline.

## C ownership and runtime invariants

- Treat cJSON ownership explicitly. Determine whether a function borrows,
  duplicates, attaches, or deletes each object before changing cleanup code.
- Buffers returned by `Model_GetImageData()` are borrowed from `Model.c`. Do not
  free them or retain them after `Model_Reset()` or the next inference.
- Every completed inference/output cycle must reach `Model_Reset()`.
- VDO buffers must be returned through the matching video/imgprovider API.
- `Storage_GetPath()` may return `NULL` when writable storage is unavailable.
  This is a normal runtime state, not a reason to crash.
- Use AXStorage's application-owned path. Do not introduce hardcoded shared SD
  card paths.
- AXStorage callbacks and deferred model startup depend on the GLib main loop;
  avoid blocking callbacks or the inference loop.
- Preserve bounds such as `MAX_LABELS` and fixed event-state arrays when adding
  labels or state.
- Check allocations, file operations, cJSON lookups, and external API return
  values. Log failures through the project's existing logging style.
- Keep headers in sync with implementations and add new C sources to `OBJS1` in
  `app/Makefile`.

## Change checklist

For native C changes, build at least the affected target; build both targets when
touching model setup, manifest compatibility, packaging, or chip-specific logic.
For JSON-only changes, parse the edited files and verify all matching C/UI keys.
For shell or Python tooling, run syntax checks and exercise `--help` or the
narrowest non-destructive path. Camera-dependent behavior still requires a real
ARTPEC device; state clearly when it was not exercised.