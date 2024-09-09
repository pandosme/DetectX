## DetectX

DetectX is an open-source package designed for developers and integrators who wish to train or deploy a YOLOv5 object detection model directly on Axis cameras with ARTPEC-8. While Axis cameras offer robust built-in object detection analytics for common use cases, some scenarios require more specialized detection. This package allows you to leverage a trained YOLOv5 model on the camera itself, bypassing the need for server-based processing.

This package is based on material published on [Axis Communication GitHub](https://github.com/AxisCommunications/acap-native-sdk-examples)

### Key Features

- Capture images directly from the camera and perform inference.
- Process output with non-maximum suppression.
- Trigger camera events when labels are detected, maintaining the event state as long as detections continue.
- User features:
  - Detection verification with video augmentation and tables.
  - Filter detections based on Area-Of-Interest and confidence levels.
  - Monitor ACAP state, model status, and average inference time.

The package is designed to minimize customization needs, encapsulating complexity within wrappers. The primary file you might need to edit is `main.c`.

## Prerequisites

1. Linux with Git, Python, and Docker installed.
2. A labeled dataset of images.

## Training Your Model

To ensure the model is compatible with the camera, follow steps:

```bash
git clone https://github.com/ultralytics/yolov5
cd yolov5
git checkout 95ebf68f92196975e53ebc7e971d0130432ad107
curl -L https://acap-ml-model-storage.s3.amazonaws.com/yolov5/yolov5-axis-A8.patch | git apply
pip install -r requirements.txt
```

More info found in [YOLOv5 on ARTPEC-8](https://github.com/AxisCommunications/axis-model-zoo/blob/main/docs/yolov5-on-artpec8.md)

Create a directory for images and label files.
[Read more here on labels and training](https://docs.ultralytics.com/yolov5/tutorials/train_custom_data/).


### Model Selection

Decide on the image input size and base model (weights). These choices impact performance and detection results. Available base models:

1. yolov5n (nano)
2. yolov5s (small)
3. yolov5m (medium)
4. yolov5l (large)
5. yolov5x (extra large)

Start with yolov5n and move to yolov5s if needed. Choose a model size that is a multiple of 32 (default is 640). Smaller sizes reduce inference time, while larger sizes improve detection quality.

Inference time: 

| Model size | Model image size | Inference time |
|----------|----------|----------|
| yolov5n | 640   | 110-150 ms   |
| yolov5s | 800   | 250-300 ms   |

### Building a dataset
If you do not already have a dataset of images you must build one or download an opensource dataset of images (Google Search).  How many images you need for each label depends on what accuracy you want and how complext the detection is.  Start with 20-30 images for each label and grow as you build your dataset to a good prediction point.  

Use one or more cameras to capture images to your computer and store them in a directory e.g. /dataset/mymodel/images.  
Example:
Use an LLM to help you create a script to capture images.
Prompt:
```
Write a pyhton script that captures images from an Axis camera with IP address 1.2.3.4 and user/password root/pass.  Note that the camera uses digest authentication.  Capture images with a resolution of 1280x720 resoution every 10 seconds and store the images in /dataset/mymodel/images.  THe image filen name is epoch timestamp millisecond resolution.
```
For labeling you can download a labeling tool.

### Training Configuration

Use 80% of images for training and 20% for validation. For example, to train on yolov5n:

```bash
python train.py --img 640 --batch 50 --epochs 300 --data [DIRECTORY TO YOUR DATASET]/data.yaml --weights ./models/yolov5n.pt
```

- `--batch 50`: Number of images processed at once. Higher values increase speed but may exhaust memory.
- `--epochs 300`: Number of complete training cycles. Higher values improve accuracy but increase training time.

## Exporting the Model

After training, export the model to TFLite:

```bash
python export.py --weights runs/train/exp[X]/weights/best.pt --include tflite --int8 --per-tensor --img-size 640
```

Each training session creates a new directory `exp[X]`.

## Building the ACAP
Before building the ACAP with your model,  test the model on your PC/server.  Troubleshooting a model when it is running in the cameras is hard.
It is recommended to use LLM assistance to generate scripts.  E.g. a script that capture images from the Axis camera, runs inference using you molde and labels and prints out the result.  

### Installation
Open a Linux shell and navigate to your home directory:

```bash
git clone https://github.com/pandosme/DetectX.git
```
### Building the Package
You do not need to alter any C or H files. However, to create a custom ACAP, you may want to alter:

- `main.c`
- `manifest.json`
- `Makefile`

1. Replace `app/model/labels.txt` with your labels.
2. Replace `app/model/model.tflite` with your model.tflite.
3. Run `python prepare.py` to update `app/html/config/model.json`.
4. Run `./build.sh`.

You should now have a new EAP file ready for installation on your camera.

Remember, tools like Perplexity or other LLMs can assist you with any challenges you encounter.

## History

### 1.0.0	June 5, 2024
- Initial commit

### 1.0.1	June 6, 2024
- Restructured SD Card image store on detect images. Fix a flaw that could result in error "Too many files open...".
- Fixed so Reset button cleared all bounding boxes and table

### 1.0.2	June 7, 2024
- Fixed flaw that prevented detections
- Fixed flaw that images was not stored on SD Card when users enabled that feature. 
