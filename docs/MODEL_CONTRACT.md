# Model contract

What a model must look like for DetectX to load it. Anything that meets this contract
works without code changes: DetectX reads the input size, class count and quantization
from the model file itself.

Check a model against it with Docker only, no Python setup needed:

```bash
tools/check-model.sh my-model-a9.tflite labels.txt --target a9
```

The build runs the same check (`app/extract_model_params.py`), so a model that fails
here also fails `./build.sh`.

## The model file (`.tflite`)

| | Requirement |
|---|---|
| Architecture | YOLOv8 detector (anchor-free, no objectness). YOLOv5 models do not work. |
| Input | one tensor `[1, H, W, 3]`, **uint8**, RGB. H and W multiples of 32. |
| Outputs | exactly two **uint8** tensors: coordinates `[1, 4, N]` (cx, cy, w, h in input pixels) and class scores `[1, classes, N]` (after the sigmoid, 0-1). |
| Box count | `N = (H/8)(W/8) + (H/16)(W/16) + (H/32)(W/32)`, e.g. 10,710 at 960x544. |
| Quantization | full int8. **ARTPEC-8:** per-tensor weights. **ARTPEC-9:** per-channel weights. One file per chip; they are not interchangeable. |

Why two outputs: Ultralytics' own TFLite export produces one fused `[1, 4+classes, N]`
float tensor. Fused, coordinates (0 to the input width) and scores (0-1) share one int8
scale, and every score quantizes to zero. Separate tensors each get their own scale.
Use an exporter that cuts the graph before the final concat -- the training repository
[acap-model-pipeline](https://github.com/pandosme/acap-model-pipeline) does this.

## Input size

- Prefer a 16:9 shape: the camera image is squeezed to the model input without
  letterboxing, so a square input distorts the scene and wastes pixels.
  Common sizes: 640x352, 960x544, 1120x640, 1280x736.
- Both chip files must use the same input size.
- Larger inputs detect smaller and farther objects but run slower. See the README's
  performance tables before going above 960x544 on ARTPEC-8.

## Labels (`labels.txt`)

- One label per line, in the model's class order (line 1 = class 0).
- The number of lines must equal the model's class count.
- Labels are shown in the web UI and used in event names and MQTT topics. Spaces work
  (the bundled COCO labels have `traffic light`), but short lowercase labels without
  spaces are easier to use in integrations.

## Multi-class models: clamp the class logits

A model with many classes can push the score logits of the classes a box is *not* to
extreme values (seen: -133). After int8 quantization the ARTPEC-9 DLPU then returned
false 100 % scores at random positions while the same file was correct on the CPU.
Clamping the logits to +-16 before the sigmoid (`Hardtanh(-16, 16)` after each class
branch) prevents it without changing any prediction. acap-model-pipeline adds the clamp
automatically for models with more than one class.

## Calibration

Int8 export needs representative images: 200-500 frames from the deploying camera at
the export size, covering the distances you care about and including empty scenes.
Calibration decides accuracy on small objects more than anything else in the export.

## Packaging

The build expects, in `app/model/`:

| File | |
|---|---|
| `model-a8.tflite` | ARTPEC-8 model |
| `model-a9.tflite` | ARTPEC-9 model |
| `labels.txt` | labels for both |

`tools/install-model.sh` puts them there from a trained model package (the folder
training produces, e.g. `mymodel-a8-960x544.tflite`, `mymodel-a9-960x544.tflite`,
`labels.txt`) and records the source in `app/model/SOURCE.md`.

Without building, upload the `.tflite` for the camera's chip and `labels.txt` on the
**Model** page of the web UI, or with `tools/camera.sh <camera> upload-model`.
