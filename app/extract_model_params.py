#!/usr/bin/env python3
"""
Extract quantization parameters from a YOLOv8 TFLite model.

The app expects an export cut before the final concat (onnx2tf -onimc), giving
two uint8 output tensors -- coords [1,4,boxes] and class scores [1,nc,boxes] --
so each carries its own quantization scale. larod exposes tensor shapes and
dtypes at runtime but not scales, so only the scales need baking in here.
"""

import argparse
from pathlib import Path

import numpy as np
import tensorflow as tf

def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("model", type=Path)
    parser.add_argument("--target", required=True, choices=("a8", "a9"))
    parser.add_argument("--output", type=Path, default=Path("model_params.h"))
    return parser.parse_args()

def label_count(path):
    labels = [line.strip() for line in path.read_text(encoding="utf-8").splitlines()
              if line.strip()]
    if not labels:
        raise ValueError(f"No labels found in {path}")
    return len(labels)

def load_interpreter(path):
    interpreter = tf.lite.Interpreter(model_path=str(path))
    interpreter.allocate_tensors()
    return interpreter

def require_quantization(detail, description):
    scale, zero_point = detail["quantization"]
    if not np.isfinite(scale) or scale <= 0:
        raise ValueError(f"{description} has invalid quantization scale {scale}")
    return scale, zero_point

def validate_quantization(interpreter, path, target):
    per_axis = [detail["name"] for detail in interpreter.get_tensor_details()
                if len(detail["quantization_parameters"]["scales"]) > 1]
    if target == "a8" and per_axis:
        raise ValueError(
            f"{path} has {len(per_axis)} per-axis quantized tensors; ARTPEC-8 requires per-tensor quantization"
        )
    if target == "a9" and not per_axis:
        raise ValueError(
            f"{path} has no per-axis quantized tensors; expected an ARTPEC-9 per-channel export"
        )
    print(f"  - Quantization: {len(per_axis)} per-axis tensors ({target})")

def validate_detector(model_path, labels_path, target):
    interpreter = load_interpreter(model_path)
    input_details = interpreter.get_input_details()
    output_details = interpreter.get_output_details()

    if len(input_details) != 1:
        raise ValueError(f"Detector has {len(input_details)} inputs, expected 1")
    input_detail = input_details[0]
    input_shape = tuple(int(value) for value in input_detail["shape"])
    if len(input_shape) != 4 or input_shape[0] != 1 or input_shape[3] != 3:
        raise ValueError(f"Unexpected detector input shape: {input_shape}")
    if input_detail["dtype"] != np.uint8:
        raise ValueError(f"Detector input must be uint8, got {input_detail['dtype']}")
    require_quantization(input_detail, "Detector input")
    if len(output_details) != 2:
        raise ValueError(f"Detector has {len(output_details)} outputs, expected 2 split YOLOv8 outputs")

    for detail in output_details:
        if (detail["dtype"] != np.uint8 or len(detail["shape"]) != 3
                or int(detail["shape"][0]) != 1):
            raise ValueError(
                f"Detector outputs must be batch-1 rank-3 uint8 tensors, got {detail['shape']} {detail['dtype']}"
            )
        require_quantization(detail, f"Detector output {detail['name']}")

    coordinate_outputs = [detail for detail in output_details if int(detail["shape"][1]) == 4]
    if len(coordinate_outputs) != 1:
        raise ValueError("Detector must have exactly one coordinate output with 4 channels")
    coord = coordinate_outputs[0]
    score = next(detail for detail in output_details if detail is not coord)
    boxes = int(coord["shape"][2])
    if int(score["shape"][2]) != boxes:
        raise ValueError("Coordinate and score outputs have different box counts")

    height, width = input_shape[1:3]
    expected_boxes = sum((height // stride) * (width // stride) for stride in (8, 16, 32))
    if boxes != expected_boxes:
        raise ValueError(f"Detector has {boxes} boxes, expected {expected_boxes} for {width}x{height}")
    classes = int(score["shape"][1])
    labels = label_count(labels_path)
    if classes != labels:
        raise ValueError(f"Detector has {classes} classes but {labels_path} has {labels} labels")

    validate_quantization(interpreter, model_path, target)
    return input_detail, coord, score

def main():
    args = parse_args()
    model_path = args.model
    model_dir = model_path.parent
    input_detail, coord, score = validate_detector(
        model_path, model_dir / "labels.txt", args.target
    )

    coord_scale, coord_zero = require_quantization(coord, "Detector coordinates")
    score_scale, score_zero = require_quantization(score, "Detector scores")

    with args.output.open("w", encoding="utf-8") as output:
        output.write("/*\n")
        output.write(" * Auto-generated model parameters\n")
        output.write(f" * Extracted from: {model_path} ({args.target})\n")
        output.write(" * DO NOT EDIT - Generated at build time\n")
        output.write(" */\n\n#ifndef MODEL_PARAMS_H\n#define MODEL_PARAMS_H\n\n")
        output.write(f"#define COORD_QUANTIZATION_SCALE {coord_scale}f\n")
        output.write(f"#define COORD_QUANTIZATION_ZERO_POINT {coord_zero}\n")
        output.write(f"#define SCORE_QUANTIZATION_SCALE {score_scale}f\n")
        output.write(f"#define SCORE_QUANTIZATION_ZERO_POINT {score_zero}\n\n")
        output.write("#endif // MODEL_PARAMS_H\n")

    print(f"Model parameters extracted to {args.output}")
    print(f"  - Input:  {input_detail['shape'][2]}x{input_detail['shape'][1]}"
          f"x{input_detail['shape'][3]} {input_detail['dtype'].__name__}")
    print(f"  - Coords: {list(coord['shape'])} scale={coord_scale:.9g} zero={coord_zero}")
    print(f"  - Scores: {list(score['shape'])} scale={score_scale:.9g} zero={score_zero}"
          f"  ({score['shape'][1]} classes, {score['shape'][2]} boxes)")

if __name__ == "__main__":
    main()
