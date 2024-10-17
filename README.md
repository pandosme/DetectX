# DetectX - Hand-Gestures

This ACAP is based on [DetectX](https://github.com/pandosme/DetectX), an open-source package.
The model is trained on selected labels in the [Hagird V2](https://github.com/hukenovs/hagrid) dataset.  

![gestures](https://raw.githubusercontent.com/hukenovs/hagrid/Hagrid_v1/images/gestures.jpg)

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

### Events labels
call, dislike, fist, four, like, middle_finger, mute, no_gesture, ok, one, palm, peace, peace_inverted, rock, stop, stop_inverted, three, thumb_index, two_up, two_up_inverted,

# History

### 2.0.0	October 10, 2024
- Updated model with training with selected labes from Hagrid V2

### 2.1.0	October 11, 2024
- Added support for Detection transition

### 2.1.1	October 13, 2024
- Fixed flawed event states
- Fixed potential memoryleak

### 2.1.3	October 17, 2024
- Fixed model tflite export that resulted in very high (2s) inference time.
