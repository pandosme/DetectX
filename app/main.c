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
#include <errno.h>

#include "ACAP.h"
#include "Model.h"
#include "Video.h"
#include "cJSON.h"

#define LOG(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args);}
#define LOG_WARN(fmt, args...)    { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args);}
//#define LOG_TRACE(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }
#define LOG_TRACE(fmt, args...)    {}

#define APP_PACKAGE	"detectx"

cJSON* model = 0;


void
ConfigUpdate( const char *service, cJSON* data) {
	char *json = cJSON_PrintUnformatted( data );
	if( json ) {
		LOG("%s: %s:%s\n", __func__, service,json );
		free(json);
	}
}

cJSON* lastDetections = 0;

gboolean
ImageProcess(gpointer data) {
    struct timeval startTs, endTs;	

	cJSON* settings = ACAP_Service("settings");
	if(!settings) {
		ACAP_STATUS_SetString("model","status","Error. Check log");
		ACAP_STATUS_SetBool("model","state", 0);
		LOG_WARN("No settings found\n");
		return G_SOURCE_REMOVE;
	}
		

	cJSON* aoi = cJSON_GetObjectItem(settings,"aoi");
	if(!aoi) {
		ACAP_STATUS_SetString("model","status","Error. Check log");
		ACAP_STATUS_SetBool("model","state", 0);
		LOG_WARN("No aoi settings\n");
		return G_SOURCE_REMOVE;
	}

	cJSON* labels = cJSON_GetObjectItem(settings,"labels");
	if(!labels) {
		ACAP_STATUS_SetString("model","status","Error. Check log");
		ACAP_STATUS_SetBool("model","state", 0);
		LOG_WARN("No labels found\n");
		return G_SOURCE_REMOVE;
	}
	int confidenceThreshold = cJSON_GetObjectItem(settings,"confidence")?cJSON_GetObjectItem(settings,"confidence")->valueint:0.5;
	
	VdoBuffer* image = Video_Image();	

	if( !image ) {
		ACAP_STATUS_SetString("model","status","Error. Check log");
		ACAP_STATUS_SetBool("model","state", 0);
		LOG_WARN("Image capture failed\n");
		return G_SOURCE_REMOVE;
	}

	cJSON_Delete(lastDetections);
	lastDetections = cJSON_CreateArray();

    gettimeofday(&startTs, NULL);
	cJSON* detections = Model_Inference(image);
    gettimeofday(&endTs, NULL);

    unsigned int inferenceTime = (unsigned int)(((endTs.tv_sec - startTs.tv_sec) * 1000) + ((endTs.tv_usec - startTs.tv_usec) / 1000));
	if( !detections || cJSON_GetArraySize(detections) == 0 ) {
		return G_SOURCE_CONTINUE;
	}

	if( cJSON_GetArraySize(detections) ) {
		char *json = cJSON_PrintUnformatted(detections);
		if( json ) {
			LOG_TRACE("%s\n",json);
			free(json);
		}
	}

	if( cJSON_GetArraySize( detections ) ) {
		LOG_TRACE("Detections %d:%u\n", cJSON_GetArraySize( detections ), inferenceTime);

		cJSON* detection = detections->child;
		while(detection) {
			cJSON* property = detection->child;
			unsigned cx = 0;
			unsigned cy = 0;
			unsigned c = 0;
			const char* label = 0;
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
			if( c >= confidenceThreshold && 
			    cx >= x1 && 
				cx <= x2 && 
				cy >= y1 && 
				cy <= y2 && 
				cJSON_GetObjectItem( labels, label )->type == cJSON_True )
					cJSON_AddItemToArray(lastDetections, cJSON_Duplicate(detection,1));
				
			detection = detection->next;
		}
	}
	cJSON_Delete( detections );
	return G_SOURCE_CONTINUE;	
}

void HTTP_ENPOINT_detections(const ACAP_HTTP_Response response,const ACAP_HTTP_Request request) {
	ACAP_HTTP_Respond_JSON(  response, lastDetections);
}

int main(void) {
	static GMainLoop *main_loop = NULL;
	openlog(APP_PACKAGE, LOG_PID|LOG_CONS, LOG_USER);

	lastDetections = cJSON_CreateArray();

	ACAP( APP_PACKAGE, ConfigUpdate );

	ACAP_HTTP_Node( "detections", HTTP_ENPOINT_detections );
	
	model = Model_Setup();
	if( model ) {
		unsigned int width = cJSON_GetObjectItem( model, "videoWidth")?cJSON_GetObjectItem( model, "videoWidth")->valueint:0;
		unsigned int height = cJSON_GetObjectItem( model, "videoHeight")?cJSON_GetObjectItem( model, "videoHeight")->valueint:0;
		if( width && height && Video_Start( width, height ) ) {
			LOG("Video %ux%u started\n",width,height);
			g_idle_add(ImageProcess, NULL);
		} else {
			LOG_WARN("Video stream failed\n");
		}
	} else {
		LOG_WARN("Model setup failed\n");
	}

	main_loop = g_main_loop_new(NULL, FALSE);
	g_main_loop_run(main_loop);
	
    return 0;
}
