# DetectX  

Run custom trained data models.  This package includes MobileNet SSD COCO model.  The idea is to replace this model with your own.  
Please read [Train-Build.md](https://github.com/pandosme/DetectX/blob/main/Train-Build.md) to understand how to train and build the package.

# Pre-requsite
- Axis Camera based on ARTPEC-8.  A special firmware for ARTPEC-7 having a TPU can be requested.

# User interface
The user interface is designed to validate detection and apply various filters.

## Detections
The 10 latest detections is shown in video as bounding box and table.  The events are shown in a separate table.

### Confidence
Initial filter to reduce the number of false detection. 

### Set Area of Intrest
Additional filter to reduce the number of false detection. Click button and use mouse to define an area that the center of the detection must be within.

### Set Minimum Size
Additional filter to reduce the number of false detection. Click button and use mouse to define a minimum width and height that the detection must have.

## Advanced
Additional filters to apply on the detection and output.

### Detection transition
A minumum time that the detection must be stable before an event is fired.

### Min event state duration
The minumum event duration a detection may have.  The duration will be high from the lat detection of a gesture.

### Labels Processed
Enable or disable selected gestures.

## Integration
An event (DetectX: State Change) is fired when a label is detected and will reamin high as long as the label is detected.  Min event state duration setting controls how long the event will remain high after the last detection.
Each label detection has its own state.


# History

### 1.0.0	September 5, 2024
- Initial commit

### 1.0.1	Septeber 6, 2024
- Restructured SD Card image store on detect images. Fix a flaw that could result in error "Too many files open...".
- Fixed so Reset button cleared all bounding boxes and table

### 1.0.2	September 7, 2024
- Fixed flaw that prevented detections
- Fixed flaw that did not store images on SD Card when users enabled this feature

### 1.0.3	September 15, 2024
- Restructures the model.json and settings.json and code realted to those config files including prepare.py


### 1.2.0	October 7, 2024
- Added support fo filter minimum size
- Fixed a flaw that preventet detecting mutliple detections in the same scene.

### 2.1.0 October 11, 2024
- Added support for Detection transition
- Removed ability to store detectection images on SD Card

### 2.1.1	October 13, 2024
- Fixed flawed event states
- Fixed potential memoryleak
