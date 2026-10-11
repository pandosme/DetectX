#ifndef VIDEO_H
#define VIDEO_H

#include "vdo-frame.h"
#include "vdo-types.h"
#include "imgprovider.h"

bool Video_Start_YUV(unsigned int width, unsigned int height);
void Video_Stop_YUV();
VdoBuffer* Video_Capture_YUV(); 

// Second NV12 stream at the sensor's largest 16:9 resolution. Detection
// export (MQTT/HTTP crops) cuts from it so crops carry full sensor detail
// instead of the detector's downscaled frame.
bool Video_Start_HiRes(void);
void Video_Stop_HiRes(void);
VdoBuffer* Video_Capture_HiRes(void);
void Video_HiRes_Geometry(unsigned int* width, unsigned int* height, unsigned int* pitch);

#endif
