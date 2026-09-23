#!/usr/bin/env python3
"""Export a YOLOv8 detector for the HandGesture ARTPEC runtime.

The application requires separate uint8 coordinate and class-score tensors.
Keeping these values in separate tensors gives each a suitable quantization
scale and avoids losing class confidence during INT8 conversion.
"""

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
import onnx
import tensorflow as tf
from ultralytics import YOLO


def _onnx2tf_bin():
    """onnx2tf lives in the same bin/ as this interpreter; PATH is not
    always set (systemd, cron), so resolve it explicitly."""
    cand = Path(sys.executable).with_name("onnx2tf")
    if cand.exists():
        return str(cand)
    found = shutil.which("onnx2tf")
    if not found:
        raise RuntimeError("onnx2tf not found next to %s nor on PATH" % sys.executable)
    return found
STRIDES = (8, 16, 32)


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", required=True, choices=("a8", "a9"))
    parser.add_argument("--weights", required=True, help="Ultralytics checkpoint")
    parser.add_argument("--calibration-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--labels", type=Path, default=Path("app/model/labels.txt"))
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=736)
    parser.add_argument("--calibration-images", type=int, default=128)
    return parser.parse_args()


def anchors_for(width: int, height: int) -> int:
    return sum((width // stride) * (height // stride) for stride in STRIDES)


def export_onnx(weights: str, width: int, height: int) -> Path:
    model = YOLO(weights)
    exported = model.export(
        format="onnx",
        imgsz=(height, width),
        dynamic=False,
        simplify=True,
        opset=12,
    )
    return Path(exported)


def split_output_names(onnx_path: Path):
    model = onnx.load(onnx_path)
    graph_outputs = {output.name for output in model.graph.output}
    final_concat = next(
        (node for node in model.graph.node
         if node.op_type == "Concat" and node.output[0] in graph_outputs),
        None,
    )
    if final_concat is None or len(final_concat.input) != 2:
        raise RuntimeError("Could not find the final two-input YOLOv8 output concat.")

    producers = {output: node for node in model.graph.node for output in node.output}
    first, second = final_concat.input
    first_is_score = producers.get(first) and producers[first].op_type == "Sigmoid"
    second_is_score = producers.get(second) and producers[second].op_type == "Sigmoid"
    if first_is_score == second_is_score:
        raise RuntimeError("Could not distinguish YOLOv8 coordinate and score outputs.")
    return (second, first) if first_is_score else (first, second)


def convert_to_saved_model(onnx_path: Path, coordinates: str, scores: str) -> Path:
    output_dir = onnx_path.with_suffix("").with_name(
        onnx_path.stem + "_split_saved_model"
    ).resolve()
    with tempfile.TemporaryDirectory() as temporary_dir:
        sample_path = Path(temporary_dir) / "calibration_image_sample_data_20x128x128x3_float32.npy"
        np.save(sample_path, np.zeros((20, 128, 128, 3), dtype=np.float32))
        subprocess.run(
            [
                _onnx2tf_bin(),
                "-i", str(onnx_path.resolve()),
                "-o", str(output_dir),
                "-onimc", coordinates, scores,
                "-osd",
                "-n",
            ],
            check=True,
            cwd=temporary_dir,
        )
    return output_dir


def calibration_images(directory: Path, limit: int):
    images = sorted(
        path for extension in ("*.jpg", "*.jpeg", "*.png", "*.JPG", "*.JPEG", "*.PNG")
        for path in directory.glob(extension)
    )[:limit]
    if not images:
        raise RuntimeError(f"No calibration images found in {directory}")
    return images


def verify_labels(path: Path, names):
    expected = [names[index] for index in range(len(names))]
    actual = [line.strip() for line in path.read_text(encoding="utf-8").splitlines()
              if line.strip()]
    if actual != expected:
        raise RuntimeError(f"{path} does not match the {len(expected)} checkpoint classes")


def quantize(saved_model: Path, output: Path, images, width: int, height: int, target: str):
    def representative_dataset():
        for image_path in images:
            image = tf.io.decode_image(tf.io.read_file(str(image_path)), channels=3,
                                       expand_animations=False)
            image = tf.image.resize(image, [height, width], antialias=True)
            image = tf.cast(image, tf.float32) / 255.0
            yield [tf.expand_dims(image, 0)]

    converter = tf.lite.TFLiteConverter.from_saved_model(str(saved_model))
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.representative_dataset = representative_dataset
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type = tf.uint8
    converter.inference_output_type = tf.uint8
    if target == "a8":
        converter._experimental_disable_per_channel = True
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(converter.convert())


def verify(output: Path, width: int, height: int, classes: int, target: str):
    interpreter = tf.lite.Interpreter(model_path=str(output))
    interpreter.allocate_tensors()
    input_detail = interpreter.get_input_details()[0]
    output_details = interpreter.get_output_details()
    shapes = sorted((tuple(detail["shape"]), detail["dtype"]) for detail in output_details)
    boxes = anchors_for(width, height)
    expected_shapes = sorted([((1, 4, boxes), np.uint8), ((1, classes, boxes), np.uint8)])

    if tuple(input_detail["shape"]) != (1, height, width, 3) or input_detail["dtype"] is not np.uint8:
        raise RuntimeError(f"Unexpected input tensor: {input_detail['shape']} {input_detail['dtype']}")
    if shapes != expected_shapes:
        raise RuntimeError(f"Unexpected output tensors: {shapes}")

    per_axis = [detail["name"] for detail in interpreter.get_tensor_details()
                if len(detail["quantization_parameters"]["scales"]) > 1]
    if target == "a8" and per_axis:
        raise RuntimeError(f"ARTPEC-8 export contains {len(per_axis)} per-axis tensors")
    if target == "a9" and not per_axis:
        raise RuntimeError("ARTPEC-9 export contains no per-axis tensors")

    print(f"Verified {output}: input 1x{height}x{width}x3, "
          f"outputs [1,4,{boxes}] + [1,{classes},{boxes}], per-axis tensors={len(per_axis)}")
    for detail in output_details:
        print(f"  {detail['name']}: scale={detail['quantization'][0]:.9g}, "
              f"zero={detail['quantization'][1]}")


def main():
    args = parse_args()
    if args.width % 32 or args.height % 32:
        raise ValueError("Model width and height must both be multiples of 32.")
    output = args.output or Path(f"app/model/model-{args.target}.tflite")

    images = calibration_images(args.calibration_dir, args.calibration_images)
    print(f"Exporting {args.weights} at {args.width}x{args.height} using {len(images)} calibration images")
    model = YOLO(args.weights)
    verify_labels(args.labels, model.names)
    classes = len(model.names)
    onnx_path = export_onnx(args.weights, args.width, args.height)
    coordinates, scores = split_output_names(onnx_path)
    print(f"Split outputs: coordinates={coordinates}, scores={scores}")
    saved_model = convert_to_saved_model(onnx_path, coordinates, scores)
    quantize(saved_model, output, images, args.width, args.height, args.target)
    verify(output, args.width, args.height, classes, args.target)


if __name__ == "__main__":
    main()