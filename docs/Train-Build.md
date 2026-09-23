# Training and Building DetectX with YOLOv8

DetectX 5.x runs **YOLOv8** models on ARTPEC-8 and ARTPEC-9 cameras. This guide
covers training a custom model, exporting it in the format the camera requires, and
building the ACAP.

> **ARTPEC-9 requires Axis OS 13 or later.** Earlier firmware miscomputes YOLOv8 on
> the A9 DLPU: the model loads and runs at normal speed but returns high-confidence
> garbage, while the identical package works correctly on ARTPEC-8. This was a
> firmware bug, not an export problem, and is fixed in OS 13.

---

## 1. What changed from YOLOv5 (DetectX 4.x)

YOLOv8 is **anchor-free**, and its output layout is not compatible with the 4.x
decoder. If you are migrating, note:

| | YOLOv5 (4.x) | YOLOv8 (5.x) |
|---|---|---|
| Output | one fused tensor `[1, N, 5+classes]` | two tensors: `[1, 4, N]` + `[1, classes, N]` |
| Objectness | separate column | **none** — class score is the confidence |
| Layout | box-major | channel-major (transposed) |
| Anchors | 3 per cell, predefined | anchor-free, 1 per cell |

`N` is the anchor count, which follows from the input size:

```
N = (W/8)·(H/8) + (W/16)·(H/16) + (W/32)·(H/32)
```

| Input | N |
|---|---|
| 640×640 | 8,400 |
| 1280×736 | 19,320 |
| 1920×1088 | 42,840 |
| 2688×1536 | 84,672 |

---

## 2. Prerequisites

```bash
conda create -n yolov8 python=3.11 -y
conda activate yolov8
pip install ultralytics onnx onnxslim onnx2tf tensorflow
```

A single environment handles both training and export. Unlike the YOLOv5 flow, no
separate export environment is needed.

Your dataset must be in standard YOLO format:

```
dataset/
  data.yaml           # path, train, val, nc, names
  images/train/*.jpg
  labels/train/*.txt  # <class> <x_center> <y_center> <width> <height>, all normalised
  images/val/*.jpg
  labels/val/*.txt
```

---

## 3. Choosing input size and model

**Both dimensions must be multiples of 32** (the stride-32 feature pyramid). The
height must be a multiple of 32 even when the camera streams 1280×720 — use
**1280×736** and let the app scale.

### Prefer a 16:9 input over a square one

A square input on a 16:9 scene wastes about 44% of its pixels on padding.
**1280×736 costs the same compute as 960×960** but covers the full scene at 1280
pixels of horizontal resolution instead of 960.

### Resolution matters more than model size for small objects

Object detection range is set by how many pixels the object occupies at the network
input. At equal compute, a smaller model at higher resolution beats a larger model at
lower resolution — often by a wide margin:

| Model | Input | GFLOPs | Relative range |
|---|---|---|---|
| yolov8m | 1280×736 | 178 | 1.0× |
| yolov8s | 1920×1088 | 147 | 1.5× |
| yolov8n | 2688×1536 | 90 | 2.1× |

Never set the input wider than the camera streams — beyond that you are upscaling
and gain nothing.

### Measured / estimated inference time

Anchored on a measured point: **yolov8m at 1280×720 = 210 ms on ARTPEC-9**,
including pre-processing, inference and post-processing. Other figures scale by
GFLOPs; ARTPEC-8 is roughly 3× slower than ARTPEC-9 on compute-bound models.

| Model | GFLOPs @1280×736 | ARTPEC-9 | ARTPEC-8 |
|---|---|---|---|
| yolov8n | 20 | ~25 ms | ~75 ms |
| yolov8s | 65 | ~76 ms | ~230 ms |
| yolov8m | 178 | **210 ms (measured)** | ~630 ms |

Start with **yolov8n** and move up only if accuracy requires it. Post-processing
cost grows with both the anchor count and the class count, so it is not included
in the GFLOPs scaling above — see §6.

---

## 4. Training

```bash
conda activate yolov8

yolo detect train \
  model=yolov8n.pt \
  data=/path/to/dataset/data.yaml \
  imgsz=640 \
  epochs=100 \
  batch=128 \
  workers=4 \
  device=0 \
  scale=0.9 \
  mosaic=1.0 \
  close_mosaic=10
```

### If your objects will be small on camera

`scale=0.9` (random 0.1–1.9×, versus the 0.5 default) combined with full mosaic
drags the training distribution down into the small-object range. Most datasets are
shot far closer than a mounted camera sees, and without this the model simply never
learns the sizes it will meet in production. This single setting matters more than
model size for long-range detection.

### Memory warnings

Host RAM, not VRAM, is usually the limit.

* `workers` — each dataloader worker gradually copies the whole label list
  (CPython refcounting defeats copy-on-write). On large datasets keep it at **4–8**;
  24 workers over 800k images will exhaust 60 GB and can take down your desktop.
* `batch` — CPU-side buffers scale as `workers × 2 × batch × ~5 MB` with mosaic.
* **Validation, not training, sets the peak.** An undertrained model returns
  near-maximum detections per image, and Ultralytics accumulates stats over the
  whole validation set. Use a few thousand validation images and `max_det=100`.

Run long jobs under a memory cap so a runaway cannot take the machine with it:

```bash
systemd-run --user --unit=train -p MemoryMax=32G -p MemorySwapMax=2G \
  --working-directory="$PWD" python train.py
```

---

## 5. Exporting for ARTPEC

**Ultralytics' built-in TFLite export does not produce a usable model for DetectX.**
It emits float32 input/output and one fused output tensor. DetectX requires uint8
input/output and two separate tensors.

This is not cosmetic. Under per-tensor INT8 quantization a fused tensor forces
coordinates (range 0…input width) and class scores (range 0…1) to share a single
scale. In practice the coordinate range wins and **every class confidence quantizes
to zero**. Split, each gets its own scale:

```
scores  scale = 0.00390625   (= 1/256, the full 8-bit range across 0..1)
coords  scale = 5.0…15.0     (its own, much coarser scale)
```

Use the supplied exporter:

```bash
conda activate export

python export_yolov8.py \
  --target a8 \
  --weights runs/detect/train/weights/best.pt \
  --calibration-dir /path/to/calibration/images \
  --calibration-images 300 \
  --width 1280 --height 736 \
  --output app/model/model-a8.tflite

python export_yolov8.py \
  --target a9 \
  --weights runs/detect/train/weights/best.pt \
  --calibration-dir /path/to/calibration/images \
  --calibration-images 300 \
  --width 1280 --height 736 \
  --output app/model/model-a9.tflite
```

It exports to ONNX, cuts the graph at the final two-input `Concat` (telling
coordinates from scores by which branch a `Sigmoid` produces), converts with
`onnx2tf -onimc`, quantizes to full INT8, and verifies the result. ARTPEC-8 uses
per-tensor quantization; ARTPEC-9 uses per-channel quantization. Both packages
must use an export at the same input resolution.

### Calibration images decide your accuracy

INT8 quantization derives each tensor's range from a set of representative images.
Getting this wrong costs real accuracy, and it costs it most on small, low-contrast
objects — usually exactly the ones you care about.

* Use **200–500 images**, with clear diminishing returns past 300.
* Capture them **through the camera, from the real mounting position.** The ISP's
  sharpening, noise reduction and WDR shape the pixel statistics the network sees.
  Phone photos of the same scene are not equivalent.
* Include the **full range of distances** you expect, weighted toward the far end.
  Calibrating only on close-up subjects sets the scale from strong activations, and
  distant objects then quantize into noise.
* Include **frames with no objects at all** (roughly 20%). The detector spends most
  of its life looking at empty scenes.
* **Do not mix day and IR/night frames.** They are different distributions; mixing
  them widens every tensor range and degrades both. Export two models if you need both.
* Exclude outliers — one blown-out frame can widen a range and cost accuracy on
  every other frame.

Labels are not needed; calibration is unsupervised.

### Verifying the export

The exporter fails loudly if the result is wrong. A correct export prints:

```
VERIFIED input 1x736x1280x3 uint8, outputs [1,4,19320] + [1,80,19320] uint8
  scores scale=0.00390625 zero=0
  coords scale=5.00848103 zero=0
```

If the input is float32, or there is a single output tensor, the model will not work
on camera — re-export rather than trying to fix it in the application.

---

## 6. Post-processing budget

Decoding happens on the CPU, and its cost scales with `anchors × classes`, not with
the DLPU time. At 2688×1536 with 80 classes that is 84,672 × 80 values to dequantize
and scan every frame.

Two things keep it manageable:

* **Threshold before NMS.** Discard boxes below the confidence threshold before
  sorting; NMS is O(n²) in surviving boxes and an undertrained or mismatched model
  can produce thousands.
* **Reduce the class count.** Classes you never act on still cost post-processing
  every frame. Training a 10-class model instead of using 80 cuts the score tensor
  eightfold.

---

## 7. Building the ACAP

Replace the model and labels, then build:

```bash
cp my_labels.txt app/model/labels.txt     # one label per line, in class order
./build.sh
```

`build.sh` runs the Axis ACAP SDK in Docker and produces `.eap` packages. Model
parameters — input size, tensor shapes, quantization scales — are validated and
extracted automatically at build time, so nothing needs editing by hand.

Install through the camera web interface: **Settings → Apps → Add**, then Start.

### larod backends

| Chip | larod device |
|---|---|
| ARTPEC-8 | `axis-a8-dlpu-tflite` |
| ARTPEC-9 | `a9-dlpu-tflite` |

Selected automatically at runtime.

---

## 8. Troubleshooting

| Symptom | Cause |
|---|---|
| High-confidence nonsense on ARTPEC-9, correct on ARTPEC-8 | Firmware older than Axis OS 13 |
| All confidences read as zero | Fused output tensor — re-export with the supplied script |
| Model loads but detects nothing | Label order does not match training, or wrong input scaling |
| Very slow, ~28 s load time | Falling back to CPU; an operation in the graph is unsupported by the DLPU |
| Many false detections | Confidence threshold too low, or calibration images unrepresentative |
| Squashed or stretched detections | Model aspect ratio far from the capture aspect ratio |

Application logs on camera:

```bash
journalctl -f -u detectx
```

The **About** page in the web interface shows model state, average inference time
and the active DLPU backend.
