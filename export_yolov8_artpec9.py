#!/usr/bin/env python3
"""Export a COCO YOLOv8 model for the DetectX ARTPEC-9 runtime.

The application requires separate uint8 coordinate and class-score tensors.
Keeping these values in separate tensors gives each a suitable quantization
scale and avoids losing class confidence during INT8 conversion.
"""

import argparse
import subprocess
from pathlib import Path

import numpy as np
import onnx
import tensorflow as tf
from ultralytics import YOLO


DEFAULT_CALIBRATION_DIR = Path("/home/fred/development/datasets/coco128/images/train2017")
DEFAULT_OUTPUT = Path("app/model/model.tflite")


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--weights", default="yolov8m.pt", help="Ultralytics checkpoint")
    parser.add_argument("--calibration-dir", type=Path, default=DEFAULT_CALIBRATION_DIR)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--width", type=int, default=1280)
    # YOLOv8's stride-32 feature pyramid requires a 736-pixel model height.
    # DetectX continues to capture the requested 1280x720 camera stream.
    parser.add_argument("--height", type=int, default=736)
    parser.add_argument("--calibration-images", type=int, default=128)
    return parser.parse_args()


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
    output_dir = onnx_path.with_suffix("").with_name(onnx_path.stem + "_split_saved_model")
    subprocess.run(
        [
            "onnx2tf",
            "-i", str(onnx_path),
            "-o", str(output_dir),
            "-onimc", coordinates, scores,
            "-osd",
            "-n",
        ],
        check=True,
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


def quantize(saved_model: Path, output: Path, images, width: int, height: int):
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
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(converter.convert())


def verify(output: Path, width: int, height: int):
    interpreter = tf.lite.Interpreter(model_path=str(output))
    interpreter.allocate_tensors()
    input_detail = interpreter.get_input_details()[0]
    output_details = interpreter.get_output_details()
    shapes = sorted((tuple(detail["shape"]), detail["dtype"]) for detail in output_details)
    expected_shapes = sorted([((1, 4, 19320), np.uint8), ((1, 80, 19320), np.uint8)])

    if tuple(input_detail["shape"]) != (1, height, width, 3) or input_detail["dtype"] is not np.uint8:
        raise RuntimeError(f"Unexpected input tensor: {input_detail['shape']} {input_detail['dtype']}")
    if shapes != expected_shapes:
        raise RuntimeError(f"Unexpected output tensors: {shapes}")

    print(f"Verified {output}: input 1x{height}x{width}x3, outputs [1,4,19320] + [1,80,19320]")
    for detail in output_details:
        print(f"  {detail['name']}: scale={detail['quantization'][0]:.9g}, "
              f"zero={detail['quantization'][1]}")


def main():
    args = parse_args()
    if args.width != 1280 or args.height != 736:
        raise ValueError("This exporter is intentionally fixed to a 1280x736 model input.")

    images = calibration_images(args.calibration_dir, args.calibration_images)
    print(f"Exporting {args.weights} at {args.width}x{args.height} using {len(images)} calibration images")
    onnx_path = export_onnx(args.weights, args.width, args.height)
    coordinates, scores = split_output_names(onnx_path)
    print(f"Split outputs: coordinates={coordinates}, scores={scores}")
    saved_model = convert_to_saved_model(onnx_path, coordinates, scores)
    quantize(saved_model, args.output, images, args.width, args.height)
    verify(args.output, args.width, args.height)


if __name__ == "__main__":
    main()