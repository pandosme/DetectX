#include <stdio.h>
#include <syslog.h>
#include "Video.h"

#define LOG(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args);}
#define LOG_WARN(fmt, args...)    { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args);}
#define LOG_TRACE(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }
//#define LOG_TRACE(fmt, args...)    {}

ImgProvider_t* imageProvider = NULL;
VdoBuffer* videoBuffer = NULL;

bool Video_Start(unsigned int width, unsigned int height) {
    imageProvider = createImgProvider(width, height, 2, VDO_FORMAT_YUV);
    if (!imageProvider) {
        LOG_WARN("%s: Could not create image provider\n", __func__);
		return false;
	}
    if (!startFrameFetch(imageProvider)) {
        destroyImgProvider(imageProvider);
        LOG_WARN("%s: Unable to start frame fetch\n", __func__);
		return false;
    }
	return true;
}

void
Video_Stop() {
	if( imageProvider ) {
		stopFrameFetch(imageProvider);
        destroyImgProvider(imageProvider);
    }
	imageProvider = NULL;
}

VdoBuffer*
Video_Image() {
	if(!imageProvider) {
		LOG_TRACE("-");
		return 0;
	}
	if( videoBuffer )
		returnFrame(imageProvider, videoBuffer);	
    videoBuffer = getLastFrameBlocking(imageProvider);
    return videoBuffer;
}
