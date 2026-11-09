#!/usr/bin/env python3
"""Export a trained YOLOv8 detector to DetectX models for ARTPEC-8 and ARTPEC-9.

    python tools/export_yolov8.py --weights best.pt --calibration-dir calib/ \
        [--size 960x544 [1280x736 ...]] [--target a8|a9|both] [--output-dir DIR]

Writes a model package that tools/install-model.sh accepts:

    <output-dir>/<name>-a8-<WxH>.tflite   per-tensor weights (ARTPEC-8)
    <output-dir>/<name>-a9-<WxH>.tflite   per-channel weights (ARTPEC-9)
    <output-dir>/labels.txt
    <output-dir>/model.json               what was exported, and how

Each file meets docs/MODEL_CONTRACT.md:
  * uint8 input [1,H,W,3] and uint8 outputs.
  * Two outputs, coordinates [1,4,N] and class scores [1,classes,N], not the one fused
    tensor of Ultralytics' own TFLite export. Fused, coordinates (0..W) and scores
    (0..1) share one int8 scale, which costs score and box precision. The ONNX graph
    is cut before its final Concat so each output gets its own scale.
  * Full int8. ARTPEC-8's DLPU needs per-tensor weights; ARTPEC-9 uses per-channel,
    which is more accurate. This is the only difference between the two files, and
    they are not interchangeable.
  * Multi-class models get their class logits clamped to +-16 before the sigmoid.
    Without it, extreme negative logits gave a lopsided int8 range and the ARTPEC-9
    DLPU returned false 100 % scores. No prediction that matters changes.

Needs: ultralytics, tensorflow, onnx, onnxslim and the onnx2tf command (see
docs/TRAINING.md). The ONNX file and intermediate saved model stay in
<output-dir>/work/.
"""
from __future__ import annotations

import argparse
import datetime
import json
import shutil
import subprocess
import sys
from pathlib import Path

STRIDES = (8, 16, 32)
CAMERA_ASPECT = 16 / 9
MAX_ASPECT_ERROR = 0.03
LOGIT_CLAMP = 16.0          # sigmoid(+-16) is 0 or 1 to 7 decimals
IMAGE_TYPES = {".jpg", ".jpeg", ".png", ".bmp"}


def parse_args():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--weights", required=True, type=Path, help="trained Ultralytics YOLOv8 .pt")
    p.add_argument("--calibration-dir", required=True, type=Path,
                   help="representative frames from the deploying camera (200-500 recommended)")
    p.add_argument("--size", nargs="+", default=["960x544"], metavar="WxH",
                   help="model input size(s); multiples of 32, 16:9 preferred (default 960x544)")
    p.add_argument("--target", choices=("a8", "a9", "both"), default="both")
    p.add_argument("--output-dir", type=Path,
                   help="package folder (default exports/<name>)")
    p.add_argument("--name", help="file name prefix (default: the weights file name)")
    p.add_argument("--calibration-images", type=int, default=300,
                   help="maximum calibration images to use (default 300)")
    p.add_argument("--no-clamp", action="store_true",
                   help="do not clamp class logits on multi-class models")
    return p.parse_args()


def parse_size(text: str) -> tuple[int, int]:
    try:
        w, h = (int(v) for v in text.lower().split("x"))
    except ValueError:
        sys.exit(f"--size {text}: expected WxH, e.g. 960x544")
    if w % 32 or h % 32:
        sys.exit(f"--size {text}: width and height must be multiples of 32")
    error = abs((w / h) / CAMERA_ASPECT - 1)
    if error > MAX_ASPECT_ERROR:
        print(f"NOTE: {text} is not 16:9 ({error:.0%} off). DetectX squeezes the camera "
              "view to the model input without letterboxing, so objects are distorted "
              "unless the view or View Area has this shape.")
    return w, h


def anchors_for(w: int, h: int) -> int:
    return sum((w // s) * (h // s) for s in STRIDES)


def onnx2tf_command() -> str:
    """onnx2tf sits next to the interpreter in a venv/conda env, which is not always
    on PATH (cron, systemd, IDEs). $ONNX2TF overrides."""
    import os
    for candidate in (os.environ.get("ONNX2TF"), str(Path(sys.executable).with_name("onnx2tf")),
                      shutil.which("onnx2tf")):
        if candidate and Path(candidate).exists():
            return candidate
    sys.exit("onnx2tf not found: install it (pip install onnx2tf) or set ONNX2TF=/path/to/onnx2tf")


def calibration_images(directory: Path, limit: int, w: int, h: int):
    from PIL import Image
    if not directory.is_dir():
        sys.exit(f"Not a directory: {directory}")
    images = sorted(p for p in directory.rglob("*") if p.suffix.lower() in IMAGE_TYPES)[:limit]
    if not images:
        sys.exit(f"No calibration images (.jpg/.png) in or below {directory}")
    off = 0
    for path in images:
        with Image.open(path) as im:
            if abs((im.width / im.height) / (w / h) - 1) > MAX_ASPECT_ERROR:
                off += 1
    if off:
        print(f"NOTE: {off} of {len(images)} calibration images do not have the {w}x{h} "
              "aspect ratio. They are squeezed like camera frames, but calibration is best "
              "on frames shaped like what the camera sends.")
    if len(images) < 100:
        print(f"NOTE: only {len(images)} calibration images; 200-500 is recommended.")
    return images


def representative_dataset(images, w: int, h: int):
    import tensorflow as tf

    def gen():
        for path in images:
            im = tf.io.decode_image(tf.io.read_file(str(path)), channels=3, expand_animations=False)
            im = tf.image.resize(im, [h, w], antialias=True)     # squeeze, as DetectX does
            yield [tf.expand_dims(tf.cast(im, tf.float32) / 255.0, 0)]
    return gen


def clamp_class_logits(weights: Path, out: Path) -> Path:
    """Copy of the weights with Hardtanh(-16, 16) after each class branch (Detect.cv3)."""
    import torch
    import torch.nn as nn
    from ultralytics import YOLO
    model = YOLO(str(weights))
    head = model.model.model[-1]
    for i in range(len(head.cv3)):
        branch = head.cv3[i]
        if not (isinstance(branch, nn.Sequential) and isinstance(branch[-1], nn.Hardtanh)):
            head.cv3[i] = nn.Sequential(branch, nn.Hardtanh(-LOGIT_CLAMP, LOGIT_CLAMP))
    checkpoint = torch.load(str(weights), map_location="cpu", weights_only=False)
    checkpoint["model"] = model.model
    checkpoint["ema"] = None
    torch.save(checkpoint, out)
    return out


def split_output_names(onnx_path: Path) -> tuple[str, str]:
    """Names of the coordinate and score tensors feeding the final two-input Concat."""
    import onnx
    graph = onnx.load(str(onnx_path)).graph
    outputs = {o.name for o in graph.output}
    concat = next((n for n in graph.node if n.op_type == "Concat" and n.output[0] in outputs), None)
    if concat is None or len(concat.input) != 2:
        sys.exit("Could not find YOLOv8's final two-input output Concat. Is this a YOLOv8 detector?")
    producers = {o: n for n in graph.node for o in n.output}
    first, second = concat.input
    first_score = first in producers and producers[first].op_type == "Sigmoid"
    second_score = second in producers and producers[second].op_type == "Sigmoid"
    if first_score == second_score:
        sys.exit("Could not tell the coordinate output from the score output.")
    return (second, first) if first_score else (first, second)


def to_saved_model(onnx_path: Path, coords: str, scores: str, work: Path) -> Path:
    import numpy as np
    saved = work / "split_saved_model"
    # onnx2tf runs an internal ONNX-vs-TF check on this file from its working directory
    # and downloads it when missing; newer numpy refuses the download. Its content does
    # not affect the model (calibration is separate), so provide a plain dummy.
    np.save(work / "calibration_image_sample_data_20x128x128x3_float32.npy",
            np.zeros((20, 128, 128, 3), dtype=np.float32))
    subprocess.run([onnx2tf_command(), "-i", str(onnx_path.resolve()), "-o", str(saved.resolve()),
                    "-onimc", coords, scores, "-osd", "-n"], check=True, cwd=str(work))
    return saved


def quantize(saved: Path, output: Path, rep, target: str):
    import tensorflow as tf
    conv = tf.lite.TFLiteConverter.from_saved_model(str(saved))
    conv.optimizations = [tf.lite.Optimize.DEFAULT]
    conv.representative_dataset = rep
    conv.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    conv.inference_input_type = tf.uint8
    conv.inference_output_type = tf.uint8
    if target == "a8":
        conv._experimental_disable_per_channel = True
    output.write_bytes(conv.convert())


def verify(output: Path, w: int, h: int, classes: int, target: str) -> dict:
    import numpy as np
    import tensorflow as tf
    it = tf.lite.Interpreter(model_path=str(output))
    it.allocate_tensors()
    inp = it.get_input_details()[0]
    outs = it.get_output_details()
    n = anchors_for(w, h)
    got = sorted((tuple(int(v) for v in d["shape"]), d["dtype"]) for d in outs)
    want = sorted([((1, 4, n), np.uint8), ((1, classes, n), np.uint8)])
    if tuple(inp["shape"]) != (1, h, w, 3) or inp["dtype"] is not np.uint8:
        sys.exit(f"{output}: unexpected input {inp['shape']} {inp['dtype']}")
    if got != want:
        sys.exit(f"{output}: unexpected outputs {got}, expected {want}")
    per_axis = sum(1 for d in it.get_tensor_details()
                   if len(d["quantization_parameters"]["scales"]) > 1)
    if target == "a8" and per_axis:
        sys.exit(f"{output}: ARTPEC-8 model has {per_axis} per-channel tensors")
    if target == "a9" and not per_axis:
        sys.exit(f"{output}: ARTPEC-9 model has no per-channel tensors")
    print(f"  verified: input 1x{h}x{w}x3 uint8, outputs [1,4,{n}] + [1,{classes},{n}] uint8, "
          f"per-channel tensors {per_axis}")
    scales = {}
    for i, d in enumerate(outs):
        if classes == 4:        # same shape: cannot tell them apart by shape
            kind = f"output{i}"
        else:
            kind = "coordinates" if d["shape"][1] == 4 else "scores"
        scale, zero = d["quantization"]
        scales[kind] = {"scale": float(scale), "zero_point": int(zero)}
        print(f"    {kind:11s} scale={scale:.9g} zero={zero}")
    return scales


def main():
    args = parse_args()
    if not args.weights.is_file():
        sys.exit(f"No such weights file: {args.weights}")
    sizes = [parse_size(s) for s in args.size]
    targets = ["a8", "a9"] if args.target == "both" else [args.target]
    name = args.name or args.weights.stem
    out_dir = (args.output_dir or Path("exports") / name).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    from ultralytics import YOLO
    names = YOLO(str(args.weights)).names
    labels = [names[i] for i in range(len(names))]
    classes = len(labels)

    work_root = out_dir / "work"
    work_root.mkdir(exist_ok=True)
    export_weights = work_root / "weights.pt"
    shutil.copy2(args.weights, export_weights)     # Ultralytics writes the ONNX next to it
    clamp = classes > 1 and not args.no_clamp
    if clamp:
        export_weights = clamp_class_logits(export_weights, work_root / "weights-clamp.pt")
        print(f"Class logits clamped to +-{LOGIT_CLAMP:g} ({classes} classes)")

    files: dict[str, dict[str, str]] = {t: {} for t in targets}
    calibration: dict[str, int] = {}
    quant: dict[str, dict] = {}
    for w, h in sizes:
        size = f"{w}x{h}"
        print(f"=== {name} {size}: {classes} classes, {anchors_for(w, h)} boxes, targets {', '.join(targets)}")
        images = calibration_images(args.calibration_dir, args.calibration_images, w, h)
        calibration[size] = len(images)
        work = work_root / size
        work.mkdir(exist_ok=True)
        onnx_path = Path(YOLO(str(export_weights)).export(
            format="onnx", imgsz=(h, w), dynamic=False, simplify=True, opset=12))
        onnx_path = Path(shutil.move(str(onnx_path), work / "model.onnx"))
        coords, scores = split_output_names(onnx_path)
        saved = to_saved_model(onnx_path, coords, scores, work)
        for target in targets:
            output = out_dir / f"{name}-{target}-{size}.tflite"
            print(f"  [{target}] quantizing {'per-tensor' if target == 'a8' else 'per-channel'} "
                  f"on {len(images)} images")
            quantize(saved, output, representative_dataset(images, w, h), target)
            quant[output.name] = verify(output, w, h, classes, target)
            files[target][size] = output.name
            print(f"    -> {output} ({output.stat().st_size / 1e6:.1f} MB)")

    (out_dir / "labels.txt").write_text("\n".join(labels) + "\n", encoding="utf-8")
    manifest = {
        "exported": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
        "exporter": "DetectX tools/export_yolov8.py",
        "weights": str(args.weights.resolve()),
        "classes": labels,
        "sizes": [f"{w}x{h}" for w, h in sizes],
        "files": files,
        "quantization": {"a8": "int8 per-tensor", "a9": "int8 per-channel"},
        "output_scales": quant,
        "logit_clamp": LOGIT_CLAMP if clamp else None,
        "calibration": {"dir": str(args.calibration_dir.resolve()), "images": calibration},
    }
    (out_dir / "model.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    print(f"\nPackage: {out_dir}")
    size_opt = f" --size {manifest['sizes'][0]}" if len(sizes) > 1 else ""
    print(f"Install: tools/install-model.sh {out_dir}{size_opt}   then ./build.sh")
    print("Or upload the .tflite for the camera's chip and labels.txt on DetectX's Model page.")


if __name__ == "__main__":
    main()
