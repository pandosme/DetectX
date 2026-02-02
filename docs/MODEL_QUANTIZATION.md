# Model Quantization Parameters

## Overview

DetectX extracts most model parameters automatically at runtime (dimensions, classes, etc.), but quantization parameters cannot be extracted using larod API v3. Instead, DetectX uses the **official Axis method** of build-time parameter extraction.

## How It Works

During the Docker build process:

1. Python and TensorFlow are installed in the build image
2. The `extract_model_params.py` script runs automatically
3. The script reads `model/model.tflite` using TensorFlow Lite API
4. It generates `model_params.h` with quantization parameters
5. The C code includes this header and uses the extracted values

**This is fully automatic** - you don't need to do anything when replacing models!

## Verifying Quantization Parameters

### Using Netron (Visual Inspection)

1. Go to https://netron.app
2. Upload your `.tflite` model
3. Click on the **first output tensor**
4. Look for the quantization section:
   - **Scale**: e.g., `0.004190513398498297`
   - **Zero point**: usually `0`

These values should match what the build script extracts.

### Manual Extraction Script

The `app/extract_model_params.py` script can be run manually:

```bash
cd app
python3 extract_model_params.py model/model.tflite
cat model_params.h
```

This shows exactly what values will be used during compilation.

### Build Output

During `./build.sh`, you'll see output like:

```
✓ Model parameters extracted to model_params.h
  - Model: 640x640x3
  - Output: 25200 detections, 80 classes
  - Quantization: scale=0.004190513398498, zero_point=0
```

## Float Models

For float32 models (not quantized), the extraction script automatically detects this and sets appropriate values. The runtime code checks the tensor data type and handles float models correctly.

## Replacing Models

When you replace the TFLite model:

1. Copy your new model to `app/model/model.tflite`
2. Update `app/model/labels.txt` if needed
3. Rebuild:
   ```bash
   ./build.sh
   ```

The build process automatically extracts all parameters from your new model!

## Troubleshooting

**Build fails during parameter extraction:**
- Ensure `app/model/model.tflite` exists and is a valid TFLite model
- Check Docker build logs for Python errors
- Verify the model is compatible with TensorFlow Lite

**Incorrect detections or confidence values:**
- Verify model quantization in Netron matches build output
- Check camera logs for quantization values: `journalctl -f -u detectx`
- Compare build output with Netron inspection

## Comparison with Legacy prepare.py

| Aspect | Legacy (v3.x) | Current (v4.0+) |
|--------|---------------|-----------------|
| **Model dimensions** | Manual prepare.py script | Auto-detected at runtime |
| **Classes/boxes** | Manual prepare.py script | Auto-detected at runtime |
| **Labels** | Stored in model.json file | Loaded from labels.txt |
| **Quantization** | Manual prepare.py script | Auto-extracted during build |
| **Video dimensions** | Calculated by prepare.py | Auto-calculated at runtime |
| **Platform detection** | Manual in model.json | Auto-detected at runtime |

The new approach is **fully automatic**:
- No manual scripts to run before building
- Just drop in a new model and run `./build.sh`
- Follows official Axis ACAP SDK examples
- Works with any YOLOv5 TFLite model
