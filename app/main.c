#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <errno.h>

#include "ACAP.h"
#include "Model.h"
#include "Video.h"
#include "cJSON.h"

#define LOG(fmt, args...)    { syslog(LOG_INFO, fmt, ## args);}
#define LOG_WARN(fmt, args...)    { syslog(LOG_WARNING, fmt, ## args);}
//#define LOG_TRACE(fmt, args...)    { syslog(LOG_INFO, fmt, ## args);}
#define LOG_TRACE(fmt, args...)    {}

#define APP_PACKAGE	"detectx"

cJSON* settings = 0;
cJSON* model = 0;
int captureSDCARD = 0;
unsigned int captureWidth = 1280;
unsigned int captureHeight = 720;

void
ConfigUpdate( const char *service, cJSON* data) {
	char *json = cJSON_PrintUnformatted( data );
	if( json ) {
		LOG("%s: %s:%s\n", __func__, service,json );
		free(json);
	}
	cJSON* setting = data->child;
	while(setting) {
		if( strcmp( "sdcard", setting->string ) == 0 ) {
			LOG("Updating sdcard \n");
		}
		setting = setting->next;
	}
}

cJSON* lastDetections = 0;
VdoMap *capture_VDO_map = NULL;

int processBusy = 0;


gboolean
ImageProcess(gpointer data) {
    struct timeval startTs, endTs;	

	if( !settings || !model )
		return G_SOURCE_REMOVE;

	if( processBusy ) {
		LOG("Busy\n");
		return G_SOURCE_REMOVE;
	}

	processBusy = 1;
	
	VdoBuffer* buffer = Video_Capture_YUV();	
	VdoBuffer* jpegBuffer = NULL;
	if( captureSDCARD > 0 ) {
		GError *error = NULL;
		jpegBuffer = vdo_stream_snapshot(capture_VDO_map, &error);
		if( !jpegBuffer ) {
			LOG_WARN("%s: Capture failed. %s\n",__func__,error->message);
			g_error_free( error );
		}
	}

	if( !buffer ) {
		ACAP_STATUS_SetString("model","status","Error. Check log");
		ACAP_STATUS_SetBool("model","state", 0);
		LOG_WARN("Image capture failed\n");
		processBusy = 0;
		return G_SOURCE_REMOVE;
	}

	cJSON_Delete(lastDetections);
	lastDetections = cJSON_CreateArray();

    gettimeofday(&startTs, NULL);
	cJSON* detections = Model_Inference(buffer);
    gettimeofday(&endTs, NULL);
	if( !detections || cJSON_GetArraySize(detections) == 0 ) {
		processBusy = 0;
		return G_SOURCE_CONTINUE;
	}

	double timestamp = ACAP_DEVICE_Timestamp();

	//Apply User filters
	unsigned int inferenceTime = (unsigned int)(((endTs.tv_sec - startTs.tv_sec) * 1000) + ((endTs.tv_usec - startTs.tv_usec) / 1000));
	LOG("Inference %u ms\n", inferenceTime);

	cJSON* aoi = cJSON_GetObjectItem(settings,"aoi");
	if(!aoi) {
		ACAP_STATUS_SetString("model","status","Error. Check log");
		ACAP_STATUS_SetBool("model","state", 0);
		LOG_WARN("No aoi settings\n");
		processBusy = 0;
		return G_SOURCE_REMOVE;
	}

	int confidenceThreshold = cJSON_GetObjectItem(settings,"confidence")?cJSON_GetObjectItem(settings,"confidence")->valueint:0.5;
		
	cJSON* detection = detections->child;
	while(detection) {
		cJSON* property = detection->child;
		unsigned cx = 0;
		unsigned cy = 0;
		unsigned c = 0;
		const char* label = "Undefined";
		while(property) {
			if( strcmp("c",property->string) == 0 ) {
				property->valueint = property->valuedouble * 100;
				property->valuedouble = property->valueint;
				c = property->valueint;
			}
			if( strcmp("x",property->string) == 0 ) {
				property->valueint = property->valuedouble * 1000;
				property->valuedouble = property->valueint;
				cx += property->valueint;
			}
			if( strcmp("y",property->string) == 0 ) {
				property->valueint = property->valuedouble * 1000;
				property->valuedouble = property->valueint;
				cy += property->valueint;
			}
			if( strcmp("w",property->string) == 0 ) {
				property->valueint = property->valuedouble * 1000;
				property->valuedouble = property->valueint;
				cx += property->valueint / 2;
			}
			if( strcmp("h",property->string) == 0 ) {
				property->valueint = property->valuedouble * 1000;
				property->valuedouble = property->valueint;
				cy += property->valueint / 2;
			}
			if( strcmp("label",property->string) == 0 ) {
				label = property->valuestring;
			}
			property = property->next;
		}
		unsigned int x1 = cJSON_GetObjectItem(aoi,"x1")?cJSON_GetObjectItem(aoi,"x1")->valueint:100;
		unsigned int y1 = cJSON_GetObjectItem(aoi,"y1")?cJSON_GetObjectItem(aoi,"y1")->valueint:100;
		unsigned int x2 = cJSON_GetObjectItem(aoi,"x2")?cJSON_GetObjectItem(aoi,"x2")->valueint:900;
		unsigned int y2 = cJSON_GetObjectItem(aoi,"y2")?cJSON_GetObjectItem(aoi,"y2")->valueint:900;
		int insert = 0;
		if( c >= confidenceThreshold && cx >= x1 && cx <= x2 && cy >= y1 && cy <= y2 )
			insert = 1;
		cJSON* ignore = cJSON_GetObjectItem(settings,"ignore");
		if( insert && ignore && ignore->type == cJSON_Array && cJSON_GetArraySize(ignore) > 0 ) {
			cJSON* ignoreLabel = ignore->child;
			while( ignoreLabel && insert ) {
				if( strcmp( label, ignoreLabel->valuestring) == 0 )
					insert = 0;
				ignoreLabel = ignoreLabel->next;
			}
		}

		if( insert ) {
			cJSON_AddNumberToObject( detection, "timestamp", timestamp );
			cJSON_AddItemToArray(lastDetections, cJSON_Duplicate(detection,1));
		}
		detection = detection->next;
	}

	if( cJSON_GetArraySize(lastDetections) == 0 || !jpegBuffer ) {
		processBusy = 0;
		return G_SOURCE_CONTINUE;	
	}

	//Store image capture and update detections.txt on SD Card
		
	FILE* fp_detection = fopen("/var/spool/storage/SD_DISK/DetectX/detections.txt", "a");
	if( !fp_detection ) {
		LOG_WARN("Unable to create detection file on SD Card\n");
		processBusy = 0;
		return G_SOURCE_CONTINUE;	
	}

	cJSON* item = detections->child;
	while( item ) {
		char *json = cJSON_PrintUnformatted(item);
		if( json ) {
			fprintf(fp_detection, "%s\n", json);
			free(json);
		}
		item = item->next;
	}
	fclose( fp_detection );

	char filepath[128];
	sprintf(filepath,"/var/spool/storage/SD_DISK/DetectX/%.0f.jpg",timestamp);
	FILE* fp_image = fopen(filepath, "wb");
	if( !fp_image ) {
		LOG_WARN("Unable to create a jpeg file on SD Card\n");
		processBusy = 0;
		return G_SOURCE_CONTINUE;	
	}

	VdoFrame*  frame = vdo_buffer_get_frame(jpegBuffer);
	if (!frame) {
		g_object_unref(jpegBuffer);
		LOG_WARN("Unable to get frame for jpeg file\n");
		fclose(fp_image);
		processBusy = 0;
		return G_SOURCE_CONTINUE;	
	}

	void *jpegdata = (void *)vdo_buffer_get_data(jpegBuffer);
	unsigned int size = vdo_frame_get_size(frame);
	if( jpegdata && size )
		fwrite(jpegdata, sizeof(char), size, fp_image);
	fclose(fp_image);
	g_object_unref(jpegBuffer);
	LOG_TRACE("JPEG: %.0f.jpg\n",timestamp);
	
	cJSON_Delete( detections );
	processBusy = 0;
	return G_SOURCE_CONTINUE;	
}


void HTTP_ENDPOINT_detections(const ACAP_HTTP_Response response,const ACAP_HTTP_Request request) {
	ACAP_HTTP_Respond_JSON(  response, lastDetections);
}

int main(void) {
	static GMainLoop *main_loop = NULL;
	openlog(APP_PACKAGE, LOG_PID|LOG_CONS, LOG_USER);

	lastDetections = cJSON_CreateArray();

	ACAP( APP_PACKAGE, ConfigUpdate );
	ACAP_HTTP_Node( "detections", HTTP_ENDPOINT_detections );
	
	model = Model_Setup();
	if( model ) {
		ACAP_Register("model", model );
		unsigned int width = cJSON_GetObjectItem( model, "videoWidth")?cJSON_GetObjectItem( model, "videoWidth")->valueint:0;
		unsigned int height = cJSON_GetObjectItem( model, "videoHeight")?cJSON_GetObjectItem( model, "videoHeight")->valueint:0;
		if( width && height && Video_Start_YUV( width, height ) ) {
			LOG("Video %ux%u started\n",width,height);
			g_idle_add(ImageProcess, NULL);
		} else {
			LOG_WARN("Video stream failed\n");
		}
	} else {
		LOG_WARN("Model setup failed\n");
	}

	settings = ACAP_Service("settings");
	if(!settings) {
		ACAP_STATUS_SetString("model","status","Error. Check log");
		ACAP_STATUS_SetBool("model","state", 0);
		LOG_WARN("No settings found\n");
		return G_SOURCE_REMOVE;
	}

	if( cJSON_GetObjectItem(settings,"sdcard") ) {
		cJSON* sdcard = cJSON_GetObjectItem(settings,"sdcard");
		captureSDCARD = -1;

		if( cJSON_GetObjectItem(sdcard,"capture") )
			captureSDCARD = cJSON_GetObjectItem(sdcard,"capture")->type == cJSON_True;
		struct stat sb;
		captureWidth = cJSON_GetObjectItem(sdcard,"width")?cJSON_GetObjectItem(sdcard,"width")->valueint:1280;
		captureHeight = cJSON_GetObjectItem(sdcard,"height")?cJSON_GetObjectItem(sdcard,"height")->valueint:720;
		if (stat("/var/spool/storage/SD_DISK/DetectX", &sb) != 0) {  //Check if directory exists
			if( mkdir("/var/spool/storage/SD_DISK/DetectX", 0777) < 0)
				captureSDCARD = -1;
			LOG("JPEG capture to /var/spool/storage/SD_DISK/DetectX\n");
		}
		capture_VDO_map = vdo_map_new();
		vdo_map_set_uint32(capture_VDO_map, "format", VDO_FORMAT_JPEG);
		vdo_map_set_uint32(capture_VDO_map, "width", captureWidth);
		vdo_map_set_uint32(capture_VDO_map, "height", captureHeight);
	}

	main_loop = g_main_loop_new(NULL, FALSE);
	g_main_loop_run(main_loop);
	
    return 0;
}
