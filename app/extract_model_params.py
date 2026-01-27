#!/usr/bin/env python3
"""
Extract model parameters from TFLite model
Generates model_params.h with quantization and dimension information
Based on Axis ACAP SDK examples
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

    # Input dimensions (NHWC format: batch, height, width, channels)
    model_input_height = input_details[0]["shape"][1]
    model_input_width = input_details[0]["shape"][2]
    model_input_channels = input_details[0]["shape"][3]

    # Output quantization
    quantization_scale, quantization_zero_point = output_details[0]['quantization']

    # YOLOv5 output format: [batch, num_detections, (x,y,w,h,obj_conf + classes)]
    num_detections = output_details[0]['shape'][1]
    num_classes = output_details[0]['shape'][2] - 5  # Remove x,y,w,h,obj_conf

    # Write header file
    with open(output_file, "w") as f:
        f.write("/*\n")
        f.write(" * Auto-generated model parameters\n")
        f.write(f" * Extracted from: {model_path}\n")
        f.write(" * DO NOT EDIT - Generated at build time\n")
        f.write(" */\n\n")
        f.write("#ifndef MODEL_PARAMS_H\n")
        f.write("#define MODEL_PARAMS_H\n\n")
        f.write(f"#define MODEL_INPUT_HEIGHT {model_input_height}\n")
        f.write(f"#define MODEL_INPUT_WIDTH {model_input_width}\n")
        f.write(f"#define MODEL_INPUT_CHANNELS {model_input_channels}\n\n")
        f.write(f"#define QUANTIZATION_SCALE {quantization_scale}f\n")
        f.write(f"#define QUANTIZATION_ZERO_POINT {quantization_zero_point}\n\n")
        f.write(f"#define NUM_CLASSES {num_classes}\n")
        f.write(f"#define NUM_DETECTIONS {num_detections}\n\n")
        f.write("#endif // MODEL_PARAMS_H\n")

    print(f"✓ Model parameters extracted to {output_file}")
    print(f"  - Model: {model_input_width}x{model_input_height}x{model_input_channels}")
    print(f"  - Output: {num_detections} detections, {num_classes} classes")
    print(f"  - Quantization: scale={quantization_scale:.15f}, zero_point={quantization_zero_point}")

except Exception as e:
    print(f"Error extracting model parameters: {e}", file=sys.stderr)
    sys.exit(1)
