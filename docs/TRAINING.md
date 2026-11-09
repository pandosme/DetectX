# Training and exporting a model for DetectX

This guide goes from a labelled dataset to the two model files DetectX needs: one for
ARTPEC-8 and one for ARTPEC-9. Everything needed is in this repository:
`tools/export_yolov8.py` writes a model package, `tools/install-model.sh` puts it in
the build, and [BUILDING.md](BUILDING.md) builds the packages.

> **ARTPEC-9 requires AXIS OS 13 or later.** On earlier firmware a YOLOv8 model loads
> and runs at full speed on the A9 DLPU but returns high-confidence false detections.
> The same package is correct on ARTPEC-8.

For larger projects (dataset tooling, resumable training runs, two-stage
detector + classifier models), see
[acap-model-pipeline](https://github.com/pandosme/acap-model-pipeline). It produces the
same package format.

## 1. Environment

```bash
conda create -n yolov8 python=3.11 -y
conda activate yolov8
pip install ultralytics onnx onnxslim onnx2tf tensorflow
```

One environment handles training and export. A GPU is recommended for training; export
runs on the CPU.

Dataset in standard YOLO format:

```
dataset/
  data.yaml           # path, train, val, nc, names
  images/train/*.jpg
  labels/train/*.txt  # <class> <x_center> <y_center> <width> <height>, normalised
  images/val/*.jpg
  labels/val/*.txt
```

## 2. Choose the model and input size

DetectX runs **YOLOv8** detectors (anchor-free, two output tensors). YOLOv5 models do
not load. If you have a YOLOv5 dataset, retrain YOLOv8 with the same labels.

- **Both dimensions must be multiples of 32.** For a 1280x720 stream use 1280x736.
- **Prefer 16:9.** DetectX squeezes the camera view to the model input without
  letterboxing, so a square input distorts the scene. Common sizes: 640x352, 960x544,
  1280x736. If you run on a View Area with another shape, match that shape instead.
- **Pixels per object matter more than model size.** At equal compute, a smaller model
  at higher resolution usually beats a larger model at lower resolution, especially for
  small or densely packed objects. Start with **yolov8n** and increase the input size
  before the model size.
- Never set the input larger than the camera image you analyse.

Measured and estimated inference time (`yolov8m` at 1280x736 measured; others scaled by
GFLOPs, ARTPEC-8 about 3x slower than ARTPEC-9 on compute-bound models):

| Model | GFLOPs @1280x736 | ARTPEC-9 | ARTPEC-8 |
|---|---|---|---|
| yolov8n | 20 | ~25 ms | ~75 ms |
| yolov8s | 65 | ~76 ms | ~230 ms |
| yolov8m | 178 | 210 ms (measured) | ~630 ms |

`tools/benchmark_resolutions.sh` measures your own model on a camera.

## 3. Train

```bash
yolo detect train model=yolov8n.pt data=/path/to/dataset/data.yaml \
  imgsz=1280 rect=False epochs=100 batch=32 workers=4 device=0
```

- Train at (or close to) the export size. A model trained at 640 and exported at 1280
  sees objects at twice the size it learned.
- Objects small on camera: `scale=0.9` with `mosaic=1.0` widens the size range the
  model sees. Most datasets are shot closer than a mounted camera sees.
- Host RAM, not GPU memory, is usually the limit: keep `workers` at 4-8 on large
  datasets.

## 4. Export

```bash
python tools/export_yolov8.py \
  --weights runs/detect/train/weights/best.pt \
  --calibration-dir /path/to/calibration/frames \
  --size 1280x736
```

This writes a model package to `exports/best/`:

| File | |
|---|---|
| `best-a8-1280x736.tflite` | ARTPEC-8, int8 **per-tensor** weights |
| `best-a9-1280x736.tflite` | ARTPEC-9, int8 **per-channel** weights |
| `labels.txt` | class names in model order |
| `model.json` | what was exported, calibration, output scales |

Options:

| Option | |
|---|---|
| `--size 960x544 1280x736` | export several sizes in one run (compare them on the camera) |
| `--target a8` / `a9` | one chip only (default both) |
| `--output-dir DIR` / `--name NAME` | where and what to call the files |
| `--calibration-images N` | maximum calibration images (default 300) |
| `--no-clamp` | skip the class-logit clamp (see below) |

What the exporter does, and why Ultralytics' own `format=tflite` export does not work:

1. **uint8 input and output.** The DLPU runs full int8; the generic export leaves
   float32 I/O.
2. **Two output tensors.** YOLOv8 ends in a `Concat` of box coordinates (0 to input
   width, in pixels) and class scores (0-1). Quantized as one tensor they share one
   int8 scale, so scores lose almost all precision and boxes lose some too. The exporter
   cuts the graph before the `Concat` (`onnx2tf -onimc`) so each output gets its own
   scale.
3. **Per chip quantization.** ARTPEC-8's DLPU needs per-tensor weights. ARTPEC-9 uses
   per-channel weights, which keep more accuracy. The two files are not
   interchangeable, and the exporter checks each one.
4. **Class-logit clamp (multi-class models).** A class branch can push the logits of the
   classes a box is not to extreme values (seen: -133). The int8 range then becomes
   lopsided and the ARTPEC-9 DLPU returned false 100 % scores. The exporter adds
   `Hardtanh(-16, 16)` before the sigmoid; no prediction that matters changes.
5. **Verification.** Input shape and type, both output shapes, box count and per-tensor
   / per-channel weights are checked. A correct export prints:

   ```
   verified: input 1x736x1280x3 uint8, outputs [1,4,19320] + [1,3,19320] uint8, per-channel tensors 64
     coordinates scale=5.00848103 zero=0
     scores      scale=0.00390625 zero=0
   ```

## 5. Calibration images decide int8 accuracy

int8 quantization sets each tensor's range from representative images. Calibration
affects accuracy on small, low-contrast and densely packed objects more than anything
else in the export.

- **200-500 images.** Little gain past 300.
- **Frames from the deploying camera at its real mounting position**, not phone photos.
  The camera's sharpening, noise reduction and WDR shape what the network sees.
- **The same framing as inference.** If DetectX runs on a View Area or crop, calibrate
  on that crop, at the model's aspect ratio. The exporter warns about frames with
  another aspect ratio.
- **Cover what production looks like:** the full range of distances, full and sparse
  scenes, lighting variation, and about 20 % frames with no objects.
- **Do not mix day and IR/night frames.** Export two models if you need both.
- Leave out outliers; one blown-out frame widens ranges for every other frame.

Labels are not needed. Calibration images may be in subfolders.

## 6. Check int8 against the float model

Before going to the camera, compare the exported model with the `.pt` on the same
frames. Run the `.tflite` on a PC with `tf.lite.Interpreter`, decode both outputs and
run the same NMS. Measure what your application needs (for counting: the share of
frames with an exactly correct count), not only mAP. If int8 is clearly worse than
float:

1. Calibrate on more representative frames (section 5).
2. Increase the input size or crop to the area of interest.
3. Compare per-tensor and per-channel on ARTPEC-9 (`--target a8` gives a per-tensor
   file that also runs on ARTPEC-9, slower and usually less accurate).

## 7. Install and build

```bash
tools/install-model.sh exports/best        # add --size WxH if you exported several
./build.sh
```

Or, without building, upload the `.tflite` for the camera's chip and `labels.txt` on
DetectX's **Model** page. See [BUILDING.md](BUILDING.md) and
[MODEL_CONTRACT.md](MODEL_CONTRACT.md).

## 8. Post-processing cost

Decoding runs on the CPU and scales with boxes x classes, not with DLPU time. At
1280x736 a model has 19,320 boxes per frame. Classes you never use still cost time on
every frame, so train only the classes you need.

## 9. Troubleshooting

| Symptom | Cause |
|---|---|
| High-confidence false detections on ARTPEC-9, correct on ARTPEC-8 | Firmware older than AXIS OS 13, or a multi-class model exported with `--no-clamp` |
| All confidences near zero | Fused output tensor: export with `tools/export_yolov8.py` |
| Duplicate or stretched boxes on int8 but not on the float model | Calibration images not representative, or input too small for the objects |
| Model loads but detects nothing | `labels.txt` order does not match training |
| Very slow, long load time | Running on the CPU: the wrong chip's file, or an operation the DLPU does not support |
| Squeezed or stretched detections | Model aspect ratio differs from the analysed image |

Logs on the camera: `tools/camera.sh <camera> log`, or the app log in the camera's web
interface.
