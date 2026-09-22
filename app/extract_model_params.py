#!/usr/bin/env python3
"""
Extract quantization parameters from a YOLOv8 TFLite model.

The app expects an export cut before the final concat (onnx2tf -onimc), giving
two uint8 output tensors -- coords [1,4,boxes] and class scores [1,nc,boxes] --
so each carries its own quantization scale. larod exposes tensor shapes and
dtypes at runtime but not scales, so only the scales need baking in here.
"""

import sys
import tensorflow as tf

if len(sys.argv) > 1:
    model_path = sys.argv[1]
else:
    print("Error: No model path provided. Usage: python extract_model_params.py <model.tflite>")
    sys.exit(1)

output_file = "model_params.h"

try:
    interpreter = tf.lite.Interpreter(model_path)
    interpreter.allocate_tensors()

    input_details = interpreter.get_input_details()
    output_details = interpreter.get_output_details()

    if len(output_details) != 2:
        print(f"Error: model has {len(output_details)} output tensor(s), expected 2.\n"
              "       Export YOLOv8 with the graph cut before the final concat, e.g.\n"
              "       onnx2tf ... -onimc /model.22/Mul_2_output_0 /model.22/Sigmoid_output_0\n"
              "       A single concatenated output cannot be quantized usefully: one scale\n"
              "       must then cover both pixel coordinates and 0..1 scores, which rounds\n"
              "       the scores to zero.", file=sys.stderr)
        sys.exit(1)

    # Tensor order is not guaranteed, so identify by channel count.
    d0, d1 = output_details
    coord, score = (d0, d1) if d0["shape"][1] == 4 else (d1, d0)
    coord_scale, coord_zero = coord["quantization"]
    score_scale, score_zero = score["quantization"]

    with open(output_file, "w") as f:
        f.write("/*\n")
        f.write(" * Auto-generated model parameters\n")
        f.write(f" * Extracted from: {model_path}\n")
        f.write(" * DO NOT EDIT - Generated at build time\n")
        f.write(" */\n\n")
        f.write("#ifndef MODEL_PARAMS_H\n")
        f.write("#define MODEL_PARAMS_H\n\n")
        f.write(f"#define COORD_QUANTIZATION_SCALE {coord_scale}f\n")
        f.write(f"#define COORD_QUANTIZATION_ZERO_POINT {coord_zero}\n")
        f.write(f"#define SCORE_QUANTIZATION_SCALE {score_scale}f\n")
        f.write(f"#define SCORE_QUANTIZATION_ZERO_POINT {score_zero}\n\n")
        f.write("#endif // MODEL_PARAMS_H\n")

    print(f"✓ Model parameters extracted to {output_file}")
    print(f"  - Input:  {input_details[0]['shape'][2]}x{input_details[0]['shape'][1]}"
          f"x{input_details[0]['shape'][3]} {input_details[0]['dtype'].__name__}")
    print(f"  - Coords: {list(coord['shape'])} scale={coord_scale:.9g} zero={coord_zero}")
    print(f"  - Scores: {list(score['shape'])} scale={score_scale:.9g} zero={score_zero}"
          f"  ({score['shape'][1]} classes, {score['shape'][2]} boxes)")

except SystemExit:
    raise
except Exception as e:
    print(f"Error extracting model parameters: {e}", file=sys.stderr)
    sys.exit(1)
