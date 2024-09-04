#include <stdio.h>
#include <syslog.h>
#include "Video.h"

#define LOG(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args);}
#define LOG_WARN(fmt, args...)    { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args);}
#define LOG_TRACE(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); }
//#define LOG_TRACE(fmt, args...)    {}

ImgProvider_t* yuvProvider = NULL;
VdoBuffer* yuvBuffer = NULL;
ImgProvider_t* rgbProvider = NULL;
VdoBuffer* rgbBuffer = NULL;

bool Video_Start_YUV(unsigned int width, unsigned int height) {
    yuvProvider = createImgProvider(width, height, 2, VDO_FORMAT_YUV);
    if (!yuvProvider) {
        LOG_WARN("%s: Could not create image provider\n", __func__);
		return false;
	}
    if (!startFrameFetch(yuvProvider)) {
        destroyImgProvider(yuvProvider);
        LOG_WARN("%s: Unable to start frame fetch\n", __func__);
		return false;
    }
	LOG_TRACE("%s: YUV Video %ux%u\n",__func__,width,height);
	return true;
}

void
Video_Stop_YUV() {
	if( yuvProvider ) {
		stopFrameFetch(yuvProvider);
        destroyImgProvider(yuvProvider);
    }
	yuvProvider = NULL;
}

VdoBuffer*
Video_Capture_YUV() {
	if(!yuvProvider) {
		LOG_TRACE("-");
		return 0;
	}
	if( yuvBuffer )
		returnFrame(yuvProvider, yuvBuffer);	
    yuvBuffer = getLastFrameBlocking(yuvProvider);
    return yuvBuffer;
}

bool Video_Start_RGB(unsigned int width, unsigned int height) {
    rgbProvider = createImgProvider(width, height, 1, VDO_FORMAT_JPEG);
    if (!rgbProvider) {
        LOG_WARN("%s: Could not create image provider\n", __func__);
		return false;
	}
    if (!startFrameFetch(rgbProvider)) {
        destroyImgProvider(rgbProvider);
        LOG_WARN("%s: Unable to start frame fetch\n", __func__);
		return false;
    }
	LOG_TRACE("%s: RGB Video %ux%u\n",__func__,width,height);
	return true;
}

void
Video_Stop_RGB() {
	if( rgbProvider ) {
		stopFrameFetch(rgbProvider);
        destroyImgProvider(rgbProvider);
    }
	rgbProvider = NULL;
}

VdoBuffer*
Video_Capture_RGB() {
	if(!rgbProvider) {
		LOG_TRACE("-");
		return 0;
	}
	if( rgbBuffer )
		returnFrame(rgbProvider, rgbBuffer);	
    rgbBuffer = getLastFrameBlocking(rgbProvider);
    return rgbBuffer;
}
