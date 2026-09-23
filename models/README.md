# DetectX Model Checkpoints

This directory keeps only the source YOLOv8 checkpoints used to build DetectX packages:

| Checkpoint | Variant | Classes |
| --- | --- | --- |
| `detector_yolov8n_coco.pt` | YOLOv8 nano | COCO (80) |
| `detector_yolov8s_coco.pt` | YOLOv8 small | COCO (80) |
| `detector_yolov8m_coco.pt` | YOLOv8 medium | COCO (80) |

Generated ONNX files and SavedModel directories are intentionally not retained. The exporter regenerates them when needed.

## Exporting TFLite Models

Run these commands from the repository root using the environment that contains the exporter dependencies (`ultralytics`, `onnx`, `onnx2tf`, and TensorFlow). Use representative images from the deployment camera for calibration.

```bash
conda activate export

python export_yolov8.py \
  --target a8 \
  --weights models/detector_yolov8n_coco.pt \
  --calibration-dir /path/to/calibration/images \
  --width 640 --height 384 \
  --output app/model/model-a8.tflite

python export_yolov8.py \
  --target a9 \
  --weights models/detector_yolov8n_coco.pt \
  --calibration-dir /path/to/calibration/images \
  --width 640 --height 384 \
  --output app/model/model-a9.tflite
```

ARTPEC-8 exports use per-tensor quantization; ARTPEC-9 exports use per-channel quantization. The `--target` option selects the correct mode. Build packages only after exporting both files:

```bash
./build.sh
```

## Input Resolution

Set `--width` and `--height` to the desired model input. Both values must be multiples of 32. Use the same dimensions for A8 and A9 when the packages should produce equivalent boxes.

| Resolution | Aspect ratio | Use case |
| --- | --- | --- |
| `640x384` | 16:9 | Default, fast baseline |
| `960x544` | 16:9 | More detail at a higher inference cost |
| `1280x736` | near 16:9 | Highest detail; benchmark before deployment |

Larger inputs improve small or distant-object detection but reduce throughput. Use the smallest resolution that meets the required detection distance, and re-export both target variants whenever the input resolution changes.
