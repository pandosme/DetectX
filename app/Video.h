#ifndef VIDEO_H
#define VIDEO_H

#include "vdo-frame.h"
#include "vdo-types.h"
#include "imgprovider.h"

bool Video_Start(unsigned int width, unsigned int height);
void Video_Stop();
VdoBuffer* Video_Image(); 

#endif
