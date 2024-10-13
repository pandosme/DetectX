# DetectX - City

This ACAP is based on [DetectX](https://github.com/pandosme/DetectX), an open-source package.
The model is trained on [Smart city pedestrian/bike detection](https://universe.roboflow.com/simone-bernabe/smart-city-pedestrian-bike-detection/dataset/23) dataset.  
Labels: Cyclist, E-scooter, Person, Vehicle


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
Events will be fired when a hand gesture is detected and not.  The event includes the labal and it state.
Trigger event ```DetectX State changed```.  If triggering on detection, use the event as a trigger, set ```state``` high and label to what ever label should trigger.  

# History

### 2.1.0	October 11, 2024
- Added support for Detection transition

### 2.1.1	October 13, 2024
- Fixed flawed event states
- Fixed potential memoryleak
