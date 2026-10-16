# Bundled model

YOLOv8n, COCO (80 classes), input 960x544, int8 with split coordinate/score outputs.
Source weights: Ultralytics `yolov8n.pt` (v8.4.0 release; acap-model-pipeline
`tools/get_base_weights.sh n`), calibrated on camera frames at 960x544.

Replace it with `tools/install-model.sh` (see docs/BUILDING.md); the installer
rewrites this file.

| File | SHA-256 |
|---|---|
| model-a8.tflite | 2393d89e25813123... |
| model-a9.tflite | 9e381e7fc21b7361... |
| labels.txt | bd17f1ee35d5f3c8... |
