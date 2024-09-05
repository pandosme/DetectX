# DetectX
This package is based on packages published on [Axis Communication GitHub](https://github.com/AxisCommunications/acap-native-sdk-examples)

The Axis camera has great built-in object detection analytics targeting common use cases.  
However, some use cases require a very specific detection.  The current solution for these cases
is to train a model to be run on a server, fetching images from the camera.
This open source package targets developers and integrators that wants to train or already have trained a 
YOLO5 Object detection model and wants to run that model in the camera based on ARTPEC-8.  The Camera
does not nativly support YOLO5 so the model needs to be trained and exported to 
quantized TFLITE model in order for the camera to be able to run it in iits DLPU.  Why not train a TensorFlow?
That process suffers from dependency-from-hell and hard to get to work on a camera.  As described in
 [YOLOv5 on ARTEPC-8](https://github.com/AxisCommunications/axis-model-zoo/blob/main/docs/yolov5-on-artpec8.md),
model trained in exported by folloing the description can it it to work in an Axis ARTPEC-8 camera with firmware Axis OS 11.7 or later.

You are responsioble for creating a dataset of images and label them.  Once the model is trained and exported, many challange 
building an Axis ACAP.  This package builds the ACAP with the following features:
* Capturing images in the camera and run inference
* Process output including non-maximum supression
* Fire a camera event we a label is detected and keep that state high as long as more labels are detected.
* Provide a user:
    - Detection verification with video augmentation and table
	- Filter detection based Area-Of-Intrest and confidence level
	- Status about of ACAP state, model state and average inference time
The package is designed to minimize customumization to various type of outputs, hiding all the complex stuff in wrappers.  The only file you may want to edit is main.c.



## Pre-requistes
1. Linux with Git, Python and Docker installed
2. A labaled dataset of images

## Train your model
To get the model accepted by the camera this part is very important
```
git clone https://github.com/ultralytics/yolov5
cd yolov5
git checkout 95ebf68f92196975e53ebc7e971d0130432ad107
curl -L https://acap-ml-model-storage.s3.amazonaws.com/yolov5/yolov5-axis-A8.patch | git apply
pip install -r requirements.txt
```
Create a directory of images and label files.  [Read more here](https://docs.ultralytics.com/yolov5/tutorials/train_custom_data/)

Now you need to decide two thing.  Image input size and base model (weights).  Different values will impact performace
(how many images can be processed per second) and detection result.
There are 5 base models to use for training
1. yolov5n (nano)
2. yolov5s (small)
3. yolov5m (medium)
4. yolov5l (large)
5. yolov5x (extra large)

These base models differes in size and complexity.  It is recommeded to start with yolov5n and move up to yolov5s if the results are not sufficient.
The second decision is the model size.  You can select any size as long it is a multiple of 32.  Default for YOLO us 640.
Reducing the input image size or model size decreses the inference time.  More images are porcessed every second.
If you need better quality detection you can increase the image inpute size or the model size.
The example ACAP provided uses yolov5 with image size 640x640.  The inference time varies between 110-150ms.

Note that image capture uses 1280x720.  This image will be scaled to 640x640 before inference.  This scaling distorts the
image and may impact detection results.  It is possible to train the model with center-cropped images (loosing the edges)
letterbox scaling.  This will require adjustment to the ACAP by having the same preprocessing and result coordinate adjustments.
Support for this may be introduced in the future. 

When setting upp the yolo.yaml training configuration, use 80% of images for training and 20% for validation.

## Training
This example assumes you want train on yolov5n.
Go to directory yolov5.
Edit the file .models/yolov5s.yaml to change line 4 "nc: 80" to set valu to the number of values you may have.
```
python train.py --img 640 --batch 50 --epochs 300 --data [DIRECTORY TO YOUR DATASET]/data.yaml --weights yolov5n.pt --cfg yolov5n.yaml
```
--batch 50 says how many images at once.  Higher value increase training speed but may exhaust your memory
--epochs 300 says how many times trining will see the complete.  Higher values increase detection accuracy and training time 

## Exporting
Once training is complete we need to export it to tflite.
```
python export.py --weights runs/train/exp[X]/weights/best.pt --include tflite --int8 --per-tensor --img-size 640
```
Not that for each new training a new directory exp[X] is created.

If you run into challanges, use [Perplexity](https://www.perplexity.ai), tell it what you are doing and what challanges you may have.
You can use any LLM of your choosing for assistance.

# Building the ACAP
## Installation
Open a Linux shell and go to your home directory
```
git clone https://github.com/pandosme/DetectX.git
```

## Package overview
.
├── app
│	├── ACAP.c (Wrapper around ACAP SDK Resources to simplify)
│	├── ACAP.h
│	├── cJSON.c
│	├── cJSON.h
│	├── html
│	│	├── about.html
│	│	├── advanced.html
│	│	├── config
│	│	│	├── events.json
│	│	│	├── labels.json
│	│	│	├── model.json
│	│	│	└── settings.json
│	│	├── css
│	│	│	├── app.css
│	│	│	├── bootstrap.min.css
│	│	│	├── border-anim-h.gif
│	│	│	├── border-anim-v.gif
│	│	│	├── border-h.gif
│	│	│	├── border-v.gif
│	│	│	├── imgareaselect-animated.css
│	│	│	├── imgareaselect-default.css
│	│	│	├── mqtt.html
│	│	│	└── regular.min.css
│	│	├── index.html
│	│	└── js
│	│	    ├── bootstrap.min.js
│	│	    ├── jquery.imgareaselect.js
│	│	    ├── jquery.min.js
│	│	    ├── jquery.svg.min.js
│	│	    └── media-stream-player.min.js
│	├── imgprovider.c
│	├── imgprovider.h
│	├── LICENSE
│	├── main.c 
│	├── Makefile
│	├── manifest.json
│	├── model
│	│	├── labels.txt
│	│	└── model.tflite
│	├── Model.c
│	├── Model.h
│	├── Video.c
│	└── Video.h
├── build.sh
├── DetectX_1_0_0_aarch64.eap
├── Dockerfile
├── LICENSE
├── prepare.py
└── README.md

You do not need to alter any files.  However, making a Cusom ACAP you may want to alter
1. main.c
2. manifest.json
3. Makefile

## Building the package
1. Replace the file app/model/labels.txt with your own labels
2. Replace the file app/model/model.tflite with your own modle.tflite
3. run ```python prepare.py```.  This will analyze the model.tflite and update app/html/config/model.json
4. run ```. build.sh"

You should now have a new EAP-file that you can install in your camera.

Don't forget Perplexity or other LLM are your friends when facing challanges.