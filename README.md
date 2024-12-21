# DetectX  - Default COCO

Run custom trained data models.  This package includes MobileNet SSD COCO model.  The idea is to replace this model with your own.  
Please read [Train-Build.md](https://github.com/pandosme/DetectX/blob/main/Train-Build.md) to understand how to train and build the package.


**Breaking change:  You need to remove previous 2.x.x version before installing 3.x.x.**

# Pre-requsite
- Axis Camera based on ARTPEC-8. 

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

## MQTT
Configure the MQTT Broker to connect to.  
Properties names name & location are properties targeting systems with many devices in order to identify who sent the message.
### Topics & Payload
**Detections**  
[pretopic]/detection
{
	"detections":[
		{"label":"car","c":50,"x":530,"y":146,"w":46,"h":50,"timestamp":1734820170032},
		{"label":"car","c":69,"x":222,"y":203,"w":150,"h":96,"timestamp":1734820170032}
	],
	"name":"Parking 3",
	"location":"Site 4"
}

**Events**  
[pretopic]/event/B8A44F000000/car/true
{
	"state": true,
	"label": "car",
	"c": 50,
	"x": 530,
	"y": 146,
	"w": 46,
	"h": 50,
	"timestamp": 1734820051124,
	"name":"Parking 3",
	"location":"Site 4"
}

[pretopic]/event/B8A44F000000/car/false
{
	"label": "car",
	"state": false,
	"timestamp": 1734820051124,
	"name":"Parking 3",
	"location":"Site 4"
}

## Advanced
Additional filters to apply on the detection and output.

### Detection transition
A minumum time that the detection must be stable before an event is fired.  It define how trigger-happy the evant shall be.

### Min event state duration
The minumum event duration a detection may have.  

### Labels Processed
Enable or disable selected gestures.

## Integration
The service fires two different events targeting different use cases.  Service may monitor these event using camera event syste, ONVIF event stream and MQTT.
## Label state
A stateful event (high/low) for each detected label.

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

### 2.2.0	October 19, 2024
- Added event "Label Counter" for use cases needing to know how many objects are detected
- Fixed flaw for Detection transition

### 3.1.0	November 27, 2024
- Switched to latest ACAP SDK.  Please remove previous version if they are below 3.0.0.
  * Refactoring on various files
- Modified events to give all labels its own event
- Updated visualization in user interface

### 3.1.5	December 11, 2024
- Fixed a flaw that impact events.

### 3.2.0	December 20, 2024
- Bumbed ACAP wrapper up to 3.2.0

### 3.3.0	December 21, 2024
- Added support for MQTT

