#include <stdio.h>
#include <syslog.h>
#include <stdlib.h>
#include "Video.h"
#include "imgutils.h"
#include "ACAP.h"
#include "cJSON.h"


#define LOG(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args);}
#define LOG_WARN(fmt, args...)    { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args);}
//#define LOG_TRACE(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); }
#define LOG_TRACE(fmt, args...)    {}

ImgProvider_t* yuvProvider = NULL;
VdoBuffer* yuvBuffer = NULL;
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







// A 3840x2160 NV12 frame is 12 MB, so keep the buffer count low. One frame is
// held by the app, one waits in the delivered queue, the rest go back to VDO.
#define HIRES_VDO_BUFFERS 4

// The detector stream uses the selected view (channel); the full-resolution stream
// stays on the full sensor (hiresChannel 1) so a View Area's crops keep every sensor
// pixel. Model maps the view's boxes onto it (Model_SetView).
static unsigned int videoChannel = 1;
static unsigned int hiresVideoChannel = 1;

void Video_Set_Channel(unsigned int channel, unsigned int hiresChannel) {
	videoChannel = channel ? channel : 1;
	hiresVideoChannel = hiresChannel ? hiresChannel : 1;
	imgProviderSetChannel(videoChannel);
	LOG("Video channel %u, full-resolution channel %u\n", videoChannel, hiresVideoChannel);
}

ImgProvider_t* hiresProvider = NULL;
VdoBuffer* hiresBuffer = NULL;
static unsigned int hiresWidth = 0;
static unsigned int hiresHeight = 0;

bool Video_Start_HiRes(void) {
	unsigned int width = 0, height = 0;
	if (hiresProvider)
		return true;
	// VDO lists 16:9 sizes from every capture mode, so a camera running a 1080p
	// capture mode on a larger sensor still offers e.g. 2688x1512 -- upscaled,
	// with no extra detail. VAPIX Properties.Image.Resolution follows the active
	// capture mode: cap to its largest 16:9 entry.
	unsigned int maxWidth = 0;
	cJSON* resolutions = ACAP_DEVICE_JSON("resolutions");
	cJSON* res169 = resolutions ? cJSON_GetObjectItem(resolutions, "16:9") : NULL;
	cJSON* item = NULL;
	cJSON_ArrayForEach(item, res169) {
		unsigned int w = cJSON_IsString(item) ? (unsigned int)atoi(item->valuestring) : 0;
		if (w > maxWidth)
			maxWidth = w;
	}
	imgProviderSetChannel(hiresVideoChannel);
	if (maxWidth)
		LOG("Full-resolution crop: up to %u px wide (16:9)\n", maxWidth);
	if (!chooseLargestResolution("16:9", maxWidth, &width, &height)) {
		LOG_WARN("%s: No 16:9 resolution reported by VDO\n", __func__);
		imgProviderSetChannel(videoChannel);
		return false;
	}
	hiresProvider = createImgProviderWithBuffers(width, height, 1, VDO_FORMAT_YUV, HIRES_VDO_BUFFERS);
	imgProviderSetChannel(videoChannel);
	if (!hiresProvider) {
		LOG_WARN("%s: Could not create %ux%u image provider\n", __func__, width, height);
		return false;
	}
	if (!startFrameFetch(hiresProvider)) {
		destroyImgProvider(hiresProvider);
		hiresProvider = NULL;
		LOG_WARN("%s: Unable to start frame fetch\n", __func__);
		return false;
	}
	hiresWidth = width;
	hiresHeight = height;
	LOG("Full-resolution crop video %ux%u (pitch %u) started\n", width, height, hiresProvider->pitch);
	return true;
}

void Video_Stop_HiRes(void) {
	if (hiresProvider) {
		stopFrameFetch(hiresProvider);
		destroyImgProvider(hiresProvider);
	}
	hiresProvider = NULL;
	hiresBuffer = NULL;
	hiresWidth = hiresHeight = 0;
}

VdoBuffer* Video_Capture_HiRes(void) {
	if (!hiresProvider)
		return NULL;
	if (hiresBuffer)
		returnFrame(hiresProvider, hiresBuffer);
	hiresBuffer = getLastFrameBlocking(hiresProvider);
	return hiresBuffer;
}

void Video_HiRes_Geometry(unsigned int* width, unsigned int* height, unsigned int* pitch) {
	if (width) *width = hiresWidth;
	if (height) *height = hiresHeight;
	if (pitch) *pitch = hiresProvider ? hiresProvider->pitch : 0;
}
