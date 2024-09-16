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

#define LOG(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args);}
#define LOG_WARN(fmt, args...)    { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args);}
//#define LOG_TRACE(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }
#define LOG_TRACE(fmt, args...)    {}

#define APP_PACKAGE	"detectx"

cJSON* settings = 0;
cJSON* model = 0;
int SDCARD = 0;
int captureSDCARD = 0;

void
ConfigUpdate( const char *service, cJSON* data) {
	LOG_TRACE("%s: %s\n",__func__,service);
	cJSON* setting = data->child;
	while(setting) {
		LOG_TRACE("%s: Processing %s\n",__func__,setting->string);
		if( strcmp( "capture", setting->string ) == 0 ) {
			captureSDCARD = cJSON_GetObjectItem( settings, "capture" ) ? cJSON_GetObjectItem( settings, "capture" )->type == cJSON_True:0;
			LOG("Updated capture to %d\n", setting->valueint);
		}
		if( strcmp( "eventTimer", setting->string ) == 0 ) {
			LOG("Changed event state to %d\n", captureSDCARD);
		}
		if( strcmp( "aoi", setting->string ) == 0 ) {
			LOG("Updated area of intrest\n");
		}
		if( strcmp( "ignore", setting->string ) == 0 ) {
			LOG("Update labels to be processed\n");
		}
		if( strcmp( "confidence", setting->string ) == 0 ) {
			LOG("Updated confidence threshold to %d\n", setting->valueint);
		}
		setting = setting->next;
	}
	LOG_TRACE("%s: Exit\n",__func__);
}

// Event state management
GHashTable *label_timers;
gboolean label_state_expired(gpointer data) {
    const char *label = (const char *)data;
	LOG_TRACE("%s: %s\n",__func__,label);
	cJSON* eventData = cJSON_CreateObject();
	cJSON_AddFalseToObject(eventData,"state");
	cJSON_AddStringToObject(eventData,"label",label);
	ACAP_EVENTS_Fire_JSON( "label", eventData );
    // Remove the timer from the hash table
    g_hash_table_remove(label_timers, label);

    return FALSE; // Do not repeat the timer
}

void label_event(const char *label) {
    GTimer *timer = g_hash_table_lookup(label_timers, label);

    if (timer == NULL) {
        // No timer exists, create a new one and fite state high
        timer = g_timer_new();
        g_hash_table_insert(label_timers, g_strdup(label), timer);
		cJSON* eventData = cJSON_CreateObject();
		cJSON_AddTrueToObject(eventData,"state");
		cJSON_AddStringToObject(eventData,"label",label);
		ACAP_EVENTS_Fire_JSON( "label", eventData );
        // Create a GLib timeout
		LOG_TRACE("%s: %s\n",__func__,label);
		guint interval = cJSON_GetObjectItem(settings,"eventTimer")?cJSON_GetObjectItem(settings,"eventTimer")->valueint:5;
        g_timeout_add_seconds(interval, label_state_expired, g_strdup(label));
    } else {
        // Timer exists, reset it
        g_timer_start(timer);
    }
}

//Process image capture, run inference and process output
cJSON* lastDetections = 0;
VdoMap *capture_VDO_map = NULL;

int inferenceCounter = 0;
unsigned int inferenceAverage = 0;

void
Save_To_SDCARD(double timestamp, VdoBuffer* buffer, cJSON* detections ) {
	//Store image capture and update detections.txt on SD Card

	if( !SDCARD || !captureSDCARD || !buffer || !detections || cJSON_GetArraySize(detections) == 0)
		return;	
		
	FILE* fp_detection = fopen("/var/spool/storage/SD_DISK/DetectX/detections.txt", "a");
	if( !fp_detection ) {
		LOG_WARN("Unable to create detection file on SD Card\n");
		return;	
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
		return;	
	}

	VdoFrame*  frame = vdo_buffer_get_frame(buffer);
	if (!frame) {
		LOG_WARN("Unable to get frame for jpeg file\n");
		fclose(fp_image);
		return;	
	}

	void *jpegdata = (void *)vdo_buffer_get_data(buffer);
	unsigned int size = vdo_frame_get_size(frame);
	if( jpegdata && size )
		fwrite(jpegdata, sizeof(char), size, fp_image);
	fclose(fp_image);
	LOG_TRACE("JPEG: %.0f.jpg\n",timestamp);
}


gboolean
ImageProcess(gpointer data) {
    struct timeval startTs, endTs;	

	if( !settings || !model )
		return G_SOURCE_REMOVE;

	VdoBuffer* buffer = Video_Capture_YUV();	
	VdoBuffer* jpegBuffer = NULL;
	if( SDCARD && captureSDCARD > 0 ) {
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
		if( jpegBuffer )
			g_object_unref(jpegBuffer);
		return G_SOURCE_REMOVE;
	}

    gettimeofday(&startTs, NULL);
	cJSON* detections = Model_Inference(buffer);
    gettimeofday(&endTs, NULL);

	unsigned int inferenceTime = (unsigned int)(((endTs.tv_sec - startTs.tv_sec) * 1000) + ((endTs.tv_usec - startTs.tv_usec) / 1000));
	inferenceCounter++;
	inferenceAverage += inferenceTime;
	if( inferenceCounter >= 10 ) {
		ACAP_STATUS_SetNumber(  "model", "averageTime", (int)(inferenceAverage / 10) );
		inferenceCounter = 0;
		inferenceAverage = 0;
	}

	double timestamp = ACAP_DEVICE_Timestamp();

	//Apply Transform detection data and apply user filters
	cJSON* processedDetections = cJSON_CreateArray();
	cJSON* aoi = cJSON_GetObjectItem(settings,"aoi");
	if(!aoi) {
		ACAP_STATUS_SetString("model","status","Error. Check log");
		ACAP_STATUS_SetBool("model","state", 0);
		LOG_WARN("No aoi settings\n");
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

		//FILTER DETECTIONS
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
			cJSON_AddItemToArray(processedDetections, cJSON_Duplicate(detection,1));
			cJSON_AddItemToArray(lastDetections, cJSON_Duplicate(detection,1));
			label_event(label);
		}
		detection = detection->next;
	}

	Save_To_SDCARD( timestamp, jpegBuffer, processedDetections );
	if( jpegBuffer )
		g_object_unref(jpegBuffer);

	//Add code here if you want to create some specific output for the preocesses detection list
	cJSON_Delete(processedDetections);

	while( cJSON_GetArraySize( lastDetections ) > 10 )
		cJSON_DeleteItemFromArray(lastDetections, 0);
	return G_SOURCE_CONTINUE;
}


void HTTP_ENDPOINT_detections(const ACAP_HTTP_Response response,const ACAP_HTTP_Request request) {
	const char *reset = ACAP_HTTP_Request_Param( request, "reset");
	if( reset ) {
		cJSON_Delete(lastDetections);
		lastDetections = cJSON_CreateArray();
	}
	ACAP_HTTP_Respond_JSON(  response, lastDetections);
}

int main(void) {
	static GMainLoop *main_loop = NULL;
	setbuf(stdout, NULL);
	unsigned int videoWidth = 800;
	unsigned int videoHeight = 600;

	openlog(APP_PACKAGE, LOG_PID|LOG_CONS, LOG_USER);

	lastDetections = cJSON_CreateArray();
	ACAP( APP_PACKAGE, ConfigUpdate );
	ACAP_HTTP_Node( "detections", HTTP_ENDPOINT_detections );
	ACAP_EVENTS();

	settings = ACAP_Service("settings");
	if(!settings) {
		ACAP_STATUS_SetString("model","status","Error. Check log");
		ACAP_STATUS_SetBool("model","state", 0);
		LOG_WARN("No settings found\n");
		return 1;
	}

	model = Model_Setup();


	if( cJSON_GetObjectItem(settings,"capture") && cJSON_GetObjectItem(settings,"capture")->type == cJSON_True )
		captureSDCARD = 1;

	videoWidth = cJSON_GetObjectItem(model,"videoWidth")?cJSON_GetObjectItem(model,"videoWidth")->valueint:800;
	videoHeight = cJSON_GetObjectItem(model,"videoHeight")?cJSON_GetObjectItem(model,"videoHeight")->valueint:600;
	
	struct stat sb;
	if (stat("/var/spool/storage/SD_DISK/DetectX", &sb) != 0) {  //Check if directory exists
		if( mkdir("/var/spool/storage/SD_DISK/DetectX", 0777) < 0) {
			SDCARD = 0;
			captureSDCARD = 0;
			LOG_WARN("Unable to access SD Card\n");
			
		} else {
			SDCARD = 1;
		}
	} else {
		SDCARD = 1;
	}

	if( SDCARD ) {
		capture_VDO_map = vdo_map_new();
		vdo_map_set_uint32(capture_VDO_map, "format", VDO_FORMAT_JPEG);
		vdo_map_set_uint32(capture_VDO_map, "width", videoWidth);
		vdo_map_set_uint32(capture_VDO_map, "height", videoHeight);
		LOG("JPEG capture %dx%d running to /var/spool/storage/SD_DISK/DetectX\n", videoWidth,videoHeight);
	}

	label_timers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_timer_destroy);

	if( model ) {
		ACAP_Register("model", model );
		if( Video_Start_YUV( videoWidth, videoHeight ) ) {
			LOG("Video %ux%u started\n",videoWidth,videoHeight);
		} else {
			LOG_WARN("Video stream for image capture failed\n");
			captureSDCARD = 0;
		}
		g_idle_add(ImageProcess, NULL);
	} else {
		LOG_WARN("Model setup failed\n");
	}

	main_loop = g_main_loop_new(NULL, FALSE);
	g_main_loop_run(main_loop);
	
    return 0;
}
