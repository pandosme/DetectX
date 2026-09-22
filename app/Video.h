#ifndef VIDEO_H
#define VIDEO_H

#include "vdo-frame.h"
#include "vdo-types.h"
#include "imgprovider.h"

bool Video_Start_YUV(unsigned int width, unsigned int height);
void Video_Stop_YUV();
VdoBuffer* Video_Capture_YUV(); 

#endif
