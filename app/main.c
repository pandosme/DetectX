#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <glib-unix.h>
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
#include "Output.h"
#include "Output_helpers.h"
#include "MQTT.h"


#define LOG(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args);}
#define LOG_WARN(fmt, args...)    { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args);}
//#define LOG_TRACE(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }
#define LOG_TRACE(fmt, args...)    {}

#define APP_PACKAGE	"detectx"

cJSON* settings = 0;
cJSON* model = 0;
cJSON* eventsTransition = 0;
cJSON* eventLabelCounter = 0;
GTimer *cleanupTransitionTimer = 0;
static int restartPending = 0;

static int point_in_polygon(cJSON* points, double px, double py) {
	if (!points || !cJSON_IsArray(points) || cJSON_GetArraySize(points) < 3)
		return 0;

	int inside = 0;
	int count = cJSON_GetArraySize(points);
	for (int i = 0, j = count - 1; i < count; j = i++) {
		cJSON* pi = cJSON_GetArrayItem(points, i);
		cJSON* pj = cJSON_GetArrayItem(points, j);
		if (!cJSON_IsObject(pi) || !cJSON_IsObject(pj))
			continue;

		double xi = cJSON_GetObjectItem(pi, "x") ? cJSON_GetObjectItem(pi, "x")->valuedouble : 0.0;
		double yi = cJSON_GetObjectItem(pi, "y") ? cJSON_GetObjectItem(pi, "y")->valuedouble : 0.0;
		double xj = cJSON_GetObjectItem(pj, "x") ? cJSON_GetObjectItem(pj, "x")->valuedouble : 0.0;
		double yj = cJSON_GetObjectItem(pj, "y") ? cJSON_GetObjectItem(pj, "y")->valuedouble : 0.0;

		if (((yi > py) != (yj > py)) &&
			(px < (xj - xi) * (py - yi) / (((yj - yi) == 0.0) ? 1e-9 : (yj - yi)) + xi)) {
			inside = !inside;
		}
	}

	return inside;
}

static cJSON* rect_to_polygon(int x1, int y1, int x2, int y2) {
	cJSON* polygon = cJSON_CreateArray();
	if (!polygon)
		return NULL;

	cJSON* p1 = cJSON_CreateObject();
	cJSON* p2 = cJSON_CreateObject();
	cJSON* p3 = cJSON_CreateObject();
	cJSON* p4 = cJSON_CreateObject();
	if (!p1 || !p2 || !p3 || !p4) {
		cJSON_Delete(p1);
		cJSON_Delete(p2);
		cJSON_Delete(p3);
		cJSON_Delete(p4);
		cJSON_Delete(polygon);
		return NULL;
	}

	cJSON_AddNumberToObject(p1, "x", x1);
	cJSON_AddNumberToObject(p1, "y", y1);
	cJSON_AddNumberToObject(p2, "x", x2);
	cJSON_AddNumberToObject(p2, "y", y1);
	cJSON_AddNumberToObject(p3, "x", x2);
	cJSON_AddNumberToObject(p3, "y", y2);
	cJSON_AddNumberToObject(p4, "x", x1);
	cJSON_AddNumberToObject(p4, "y", y2);

	cJSON_AddItemToArray(polygon, p1);
	cJSON_AddItemToArray(polygon, p2);
	cJSON_AddItemToArray(polygon, p3);
	cJSON_AddItemToArray(polygon, p4);
	return polygon;
}

static int polygon_list_contains_point(cJSON* list, double px, double py) {
	if (!list || !cJSON_IsArray(list))
		return 0;

	cJSON* item = NULL;
	cJSON_ArrayForEach(item, list) {
		if (!cJSON_IsObject(item))
			continue;

		cJSON* active = cJSON_GetObjectItem(item, "active");
		if (active && cJSON_IsBool(active) && !cJSON_IsTrue(active))
			continue;

		cJSON* polygon = cJSON_GetObjectItem(item, "polygon");
		if (point_in_polygon(polygon, px, py))
			return 1;
	}

	return 0;
}

static int label_is_active(cJSON* settingsObj, const char* label) {
	if (!settingsObj || !label)
		return 1;

	cJSON* ignore = cJSON_GetObjectItem(settingsObj, "ignore");
	if (!ignore || !cJSON_IsArray(ignore))
		return 1;

	cJSON* item = NULL;
	cJSON_ArrayForEach(item, ignore) {
		if (cJSON_IsString(item) && item->valuestring && strcmp(item->valuestring, label) == 0)
			return 0;
	}

	return 1;
}

static int write_binary_file(const char* path, const unsigned char* data, size_t length) {
	if (!path || !data || length == 0)
		return 0;

	FILE* fp = ACAP_FILE_Open(path, "wb");
	if (!fp)
		return 0;

	size_t written = fwrite((void*)data, 1, length, fp);
	fclose(fp);
	return written == length;
}

static gboolean restart_acap_cb(gpointer user_data) {
	(void)user_data;
	LOG("Restart requested from model endpoint\n");
	raise(SIGTERM);
	return G_SOURCE_REMOVE;
}

static void migrate_legacy_regions_to_polygons(cJSON* settingsObj) {
	int changed = 0;
	if (!settingsObj)
		return;

	if (!cJSON_GetObjectItem(settingsObj, "modelDescription")) {
		cJSON_AddStringToObject(settingsObj, "modelDescription", "");
		changed = 1;
	}

	cJSON* aoiPolygon = cJSON_GetObjectItem(settingsObj, "aoi_polygon");
	if (!aoiPolygon || !cJSON_IsArray(aoiPolygon) || cJSON_GetArraySize(aoiPolygon) < 3) {
		cJSON* aoi = cJSON_GetObjectItem(settingsObj, "aoi");
		if (aoi) {
			int x1 = cJSON_GetObjectItem(aoi, "x1") ? cJSON_GetObjectItem(aoi, "x1")->valueint : 0;
			int y1 = cJSON_GetObjectItem(aoi, "y1") ? cJSON_GetObjectItem(aoi, "y1")->valueint : 0;
			int x2 = cJSON_GetObjectItem(aoi, "x2") ? cJSON_GetObjectItem(aoi, "x2")->valueint : 0;
			int y2 = cJSON_GetObjectItem(aoi, "y2") ? cJSON_GetObjectItem(aoi, "y2")->valueint : 0;
			if (x2 > x1 && y2 > y1) {
				cJSON* polygon = rect_to_polygon(x1, y1, x2, y2);
				if (polygon) {
					if (aoiPolygon)
						cJSON_ReplaceItemInObject(settingsObj, "aoi_polygon", polygon);
					else
						cJSON_AddItemToObject(settingsObj, "aoi_polygon", polygon);
					changed = 1;
				}
			}
		}
	}

	cJSON* excludePolygons = cJSON_GetObjectItem(settingsObj, "exclude_polygons");
	if (!excludePolygons || !cJSON_IsArray(excludePolygons)) {
		excludePolygons = cJSON_CreateArray();
		if (excludePolygons) {
			if (cJSON_GetObjectItem(settingsObj, "exclude_polygons"))
				cJSON_ReplaceItemInObject(settingsObj, "exclude_polygons", excludePolygons);
			else
				cJSON_AddItemToObject(settingsObj, "exclude_polygons", excludePolygons);
			changed = 1;
		}
	}

	if (excludePolygons && cJSON_GetArraySize(excludePolygons) == 0) {
		cJSON* exclude = cJSON_GetObjectItem(settingsObj, "exclude");
		if (exclude) {
			int x1 = cJSON_GetObjectItem(exclude, "x1") ? cJSON_GetObjectItem(exclude, "x1")->valueint : 0;
			int y1 = cJSON_GetObjectItem(exclude, "y1") ? cJSON_GetObjectItem(exclude, "y1")->valueint : 0;
			int x2 = cJSON_GetObjectItem(exclude, "x2") ? cJSON_GetObjectItem(exclude, "x2")->valueint : 0;
			int y2 = cJSON_GetObjectItem(exclude, "y2") ? cJSON_GetObjectItem(exclude, "y2")->valueint : 0;
			if (x2 > x1 && y2 > y1) {
				cJSON* polygon = rect_to_polygon(x1, y1, x2, y2);
				cJSON* zone = cJSON_CreateObject();
				if (polygon && zone) {
					cJSON_AddNumberToObject(zone, "id", 1);
					cJSON_AddStringToObject(zone, "name", "Exclude 1");
					cJSON_AddBoolToObject(zone, "active", 1);
					cJSON_AddItemToObject(zone, "polygon", polygon);
					cJSON_AddItemToArray(excludePolygons, zone);
					changed = 1;
				} else {
					cJSON_Delete(zone);
					cJSON_Delete(polygon);
				}
			}
		}
	}

	if (changed) {
		ACAP_Set_Config("settings", settingsObj);
		ACAP_FILE_Write("localdata/settings.json", settingsObj);
	}
}

static void HTTP_ENDPOINT_modelinput(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	(void)request;
	unsigned jpeg_size = 0;
	unsigned width = 0;
	unsigned height = 0;
	unsigned char* jpeg = Model_GetModelInputJPEG(&jpeg_size, &width, &height);

	if (!jpeg || jpeg_size == 0) {
		ACAP_HTTP_Respond_Error(response, 503, "Model input frame unavailable");
		return;
	}

	ACAP_HTTP_Header_FILE(response, "model_input.jpg", "image/jpeg", jpeg_size);
	ACAP_HTTP_Respond_Data(response, jpeg_size, jpeg);
	free(jpeg);
}

static void HTTP_ENDPOINT_model(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	const char* method = ACAP_HTTP_Get_Method(request);
	if (!method) {
		ACAP_HTTP_Respond_Error(response, 400, "Invalid request method");
		return;
	}

	if (strcmp(method, "GET") == 0) {
		cJSON* payload = model ? cJSON_Duplicate(model, 1) : cJSON_CreateObject();
		if (!payload)
			payload = cJSON_CreateObject();

		cJSON* description = settings ? cJSON_GetObjectItem(settings, "modelDescription") : NULL;
		cJSON_AddStringToObject(payload, "description", (description && cJSON_IsString(description) && description->valuestring) ? description->valuestring : "");
		ACAP_HTTP_Respond_JSON(response, payload);
		cJSON_Delete(payload);
		return;
	}

	if (strcmp(method, "POST") == 0) {
		const char* contentType = ACAP_HTTP_Get_Content_Type(request);
		if (!contentType || strstr(contentType, "application/json") == NULL) {
			ACAP_HTTP_Respond_Error(response, 415, "Unsupported media type");
			return;
		}

		if (!request->postData || request->postDataLength == 0) {
			ACAP_HTTP_Respond_Error(response, 400, "Missing POST data");
			return;
		}

		cJSON* payload = cJSON_Parse(request->postData);
		if (!payload) {
			ACAP_HTTP_Respond_Error(response, 400, "Invalid JSON payload");
			return;
		}

		int restartNeeded = 0;
		cJSON* description = cJSON_GetObjectItem(payload, "description");
		if (description && cJSON_IsString(description)) {
			cJSON* value = cJSON_CreateString(description->valuestring ? description->valuestring : "");
			if (cJSON_GetObjectItem(settings, "modelDescription"))
				cJSON_ReplaceItemInObject(settings, "modelDescription", value);
			else
				cJSON_AddItemToObject(settings, "modelDescription", value);
		}

		cJSON* tfliteB64 = cJSON_GetObjectItem(payload, "tflite_b64");
		if (tfliteB64 && cJSON_IsString(tfliteB64) && tfliteB64->valuestring && strlen(tfliteB64->valuestring) > 0) {
			gsize modelLength = 0;
			guchar* modelData = g_base64_decode(tfliteB64->valuestring, &modelLength);
			if (!modelData || modelLength == 0 || !write_binary_file("localdata/model.tflite", modelData, modelLength)) {
				if (modelData)
					g_free(modelData);
				cJSON_Delete(payload);
				ACAP_HTTP_Respond_Error(response, 500, "Failed to write uploaded TFLite model");
				return;
			}
			g_free(modelData);
			restartNeeded = 1;
		}

		cJSON* labelsContent = cJSON_GetObjectItem(payload, "labels_content");
		if (labelsContent && cJSON_IsString(labelsContent) && labelsContent->valuestring) {
			if (!ACAP_FILE_WriteData("localdata/labels.txt", labelsContent->valuestring)) {
				cJSON_Delete(payload);
				ACAP_HTTP_Respond_Error(response, 500, "Failed to write uploaded labels file");
				return;
			}
			restartNeeded = 1;
		}

		ACAP_FILE_Write("localdata/settings.json", settings);
		cJSON_Delete(payload);

		if (restartNeeded && !restartPending) {
			restartPending = 1;
			g_timeout_add(800, restart_acap_cb, NULL);
		}

		ACAP_HTTP_Respond_Text(response, restartNeeded ? "Model update accepted. Restarting ACAP." : "Model settings saved.");
		return;
	}

	ACAP_HTTP_Respond_Error(response, 405, "Method Not Allowed - Use GET or POST");
}

void
ConfigUpdate( const char *setting, cJSON* data) {
	LOG_TRACE("<%s\n",__func__);
	if(!setting || !data)
		return;
	char *json = cJSON_PrintUnformatted(data);
	if( json ) {
		free(json);
	}
	LOG_TRACE("%s>\n",__func__);
}

static void
migrate_box_to_normalized(cJSON* box, int modelWidth, int modelHeight) {
	if (!box)
		return;
	cJSON* x1 = cJSON_GetObjectItem(box, "x1");
	cJSON* y1 = cJSON_GetObjectItem(box, "y1");
	cJSON* x2 = cJSON_GetObjectItem(box, "x2");
	cJSON* y2 = cJSON_GetObjectItem(box, "y2");

	if (x1) x1->valueint = (int)(x1->valueint * 1000 / modelWidth);
	if (y1) y1->valueint = (int)(y1->valueint * 1000 / modelHeight);
	if (x2) x2->valueint = (int)(x2->valueint * 1000 / modelWidth);
	if (y2) y2->valueint = (int)(y2->valueint * 1000 / modelHeight);
}

static void
migrate_polygon_to_normalized(cJSON* polygon, int modelWidth, int modelHeight) {
	if (!polygon || !cJSON_IsArray(polygon))
		return;
	cJSON* point = polygon->child;
	while (point) {
		cJSON* x = cJSON_GetObjectItem(point, "x");
		cJSON* y = cJSON_GetObjectItem(point, "y");
		if (x) x->valueint = (int)(x->valueint * 1000 / modelWidth);
		if (y) y->valueint = (int)(y->valueint * 1000 / modelHeight);
		point = point->next;
	}
}

void
migrate_settings_to_pixel_coordinates(cJSON* settings, int modelWidth, int modelHeight) {
	cJSON* version = cJSON_GetObjectItem(settings, "coordinateVersion");
	int current = version ? version->valueint : 0;

	if (current >= 3)
		return;  // Already in the 0..1000 normalized space

	if (current == 2) {
		// Version 2 stored AOI, size, exclude and polygons in model pixels.
		// Everything is now expressed in a resolution-independent 0..1000
		// space, so scale those settings back.
		LOG("Migrating settings from pixel to 0..1000 coordinates...\n");
		migrate_box_to_normalized(cJSON_GetObjectItem(settings, "aoi"), modelWidth, modelHeight);
		migrate_box_to_normalized(cJSON_GetObjectItem(settings, "size"), modelWidth, modelHeight);
		migrate_box_to_normalized(cJSON_GetObjectItem(settings, "exclude"), modelWidth, modelHeight);
		migrate_polygon_to_normalized(cJSON_GetObjectItem(settings, "aoi_polygon"), modelWidth, modelHeight);
		cJSON* excludePolygons = cJSON_GetObjectItem(settings, "exclude_polygons");
		if (excludePolygons && cJSON_IsArray(excludePolygons)) {
			cJSON* poly = excludePolygons->child;
			while (poly) {
				migrate_polygon_to_normalized(poly, modelWidth, modelHeight);
				poly = poly->next;
			}
		}
	}
	// Versions below 2 were already stored in 0..1000; nothing to convert.

	if (version)
		version->valueint = 3;
	else
		cJSON_AddNumberToObject(settings, "coordinateVersion", 3);
	ACAP_Set_Config("settings", settings);
	LOG("Settings migration complete.\n");
}


VdoMap *capture_VDO_map = NULL;

int inferenceCounter = 0;
unsigned int inferenceAverage = 0;


gboolean ImageProcess(gpointer data);
static gboolean deferred_model_start(gpointer user_data);

// Bring the model up and start feeding it frames. Returns false if the model
// could not be loaded, which is recoverable -- see model_retry_timer.
#define MODEL_RETRY_SECONDS 15

static gboolean
model_start(void) {
	model = Model_Setup();
	if( !model )
		return FALSE;

	const char* json = cJSON_PrintUnformatted(model);
	if( json ) {
		LOG("Model settings: %s\n",json);
		free( (void*)json );
	}

	unsigned int videoWidth = cJSON_GetObjectItem(model,"videoWidth")?cJSON_GetObjectItem(model,"videoWidth")->valueint:800;
	unsigned int videoHeight = cJSON_GetObjectItem(model,"videoHeight")?cJSON_GetObjectItem(model,"videoHeight")->valueint:600;
	int modelWidth = cJSON_GetObjectItem(model,"modelWidth")?cJSON_GetObjectItem(model,"modelWidth")->valueint:640;
	int modelHeight = cJSON_GetObjectItem(model,"modelHeight")?cJSON_GetObjectItem(model,"modelHeight")->valueint:640;

	// Migrate settings if needed. Needs the real model dimensions, so it only
	// runs once the model is actually up.
	migrate_settings_to_pixel_coordinates(settings, modelWidth, modelHeight);
	migrate_legacy_regions_to_polygons(settings);

	ACAP_Set_Config("model", model );
	if( Video_Start_YUV( videoWidth, videoHeight ) ) {
		LOG("Video %ux%u started\n",videoWidth,videoHeight);
	} else {
		LOG_WARN("Video stream for image capture failed\n");
	}
	g_idle_add(ImageProcess, NULL);
	return TRUE;
}

// The DLPU may be busy or still settling when we first try to claim it. Keep
// retrying in the background so the app recovers on its own instead of needing
// a manual restart.
static gboolean
model_retry_timer(gpointer user_data) {
	(void)user_data;
	LOG("Retrying model setup\n");
	if( model_start() ) {
		LOG("Model setup succeeded on retry\n");
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

// Runs once, shortly after the main loop starts. Everything here used to happen
// before g_main_loop_run(), which meant the application could not answer a single
// HTTP request until the model was up.
static gboolean
deferred_model_start(gpointer user_data) {
	(void)user_data;
	LOG("Loading model (this takes 30-60 seconds on a cold start)\n");

	if( !model_start() ) {
		LOG_WARN("Model setup failed; retrying every %d seconds\n", MODEL_RETRY_SECONDS);
		ACAP_STATUS_SetString("model","status","Model load failed - retrying");
		ACAP_STATUS_SetBool("model","state", 0);
		g_timeout_add_seconds( MODEL_RETRY_SECONDS, model_retry_timer, NULL );
	}

	// Output_init() reads the model config to declare one event per label, so it
	// can only run once the model is up.
	Output_init();
	return G_SOURCE_REMOVE;
}

gboolean
ImageProcess(gpointer data) {
	LOG_TRACE("<%s\n",__func__);	
	const char* label = "Undefined";
    struct timeval startTs, endTs;	

	LOG_TRACE("%s: Start\n",__func__);

	if( !settings || !model )
		return G_SOURCE_REMOVE;

	LOG_TRACE("%s: Capture\n",__func__);
	VdoBuffer* buffer = Video_Capture_YUV();	
	
	if( !buffer ) {
		ACAP_STATUS_SetString("model","status","Image capture failed");
		ACAP_STATUS_SetString("model","error","Unable to capture video frames from camera");
		ACAP_STATUS_SetBool("model","state", 0);
		LOG_WARN("Image capture failed\n");
		return G_SOURCE_REMOVE;
	}

	LOG_TRACE("%s: Image\n",__func__);
    gettimeofday(&startTs, NULL);
	cJSON* detections = Model_Inference(buffer);
    gettimeofday(&endTs, NULL);
	LOG_TRACE("%s: Done\n",__func__);

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

	// Extract model dimensions for pixel coordinate system
	int modelWidth = cJSON_GetObjectItem(model,"modelWidth")?cJSON_GetObjectItem(model,"modelWidth")->valueint:640;
	int modelHeight = cJSON_GetObjectItem(model,"modelHeight")?cJSON_GetObjectItem(model,"modelHeight")->valueint:640;

	// AOI and Size are in pixel space relative to model dimensions
	cJSON* aoi = cJSON_GetObjectItem(settings,"aoi");
	if(!aoi) {
		ACAP_STATUS_SetString("model","status","Configuration error");
		ACAP_STATUS_SetString("model","error","Missing AOI settings");
		ACAP_STATUS_SetBool("model","state", 0);
		LOG_WARN("No aoi settings\n");
		return G_SOURCE_REMOVE;
	}
	unsigned int x1 = cJSON_GetObjectItem(aoi,"x1")?cJSON_GetObjectItem(aoi,"x1")->valueint:100;
	unsigned int y1 = cJSON_GetObjectItem(aoi,"y1")?cJSON_GetObjectItem(aoi,"y1")->valueint:100;
	unsigned int x2 = cJSON_GetObjectItem(aoi,"x2")?cJSON_GetObjectItem(aoi,"x2")->valueint:900;
	unsigned int y2 = cJSON_GetObjectItem(aoi,"y2")?cJSON_GetObjectItem(aoi,"y2")->valueint:900;

	cJSON* size = cJSON_GetObjectItem(settings,"size");
	if(!size) {
		ACAP_STATUS_SetString("model","status","Configuration error");
		ACAP_STATUS_SetString("model","error","Missing size filter settings");
		ACAP_STATUS_SetBool("model","state", 0);
		LOG_WARN("No size settings\n");
		return G_SOURCE_REMOVE;
	}
	unsigned int minWidth = cJSON_GetObjectItem(size,"x2")->valueint - cJSON_GetObjectItem(size,"x1")->valueint;
	unsigned int minHeight = cJSON_GetObjectItem(size,"y2")->valueint - cJSON_GetObjectItem(size,"y1")->valueint;

	if ((int)minWidth < 0)
		minWidth = (unsigned int)(-(int)minWidth);
	if ((int)minHeight < 0)
		minHeight = (unsigned int)(-(int)minHeight);

	int confidenceThreshold = cJSON_GetObjectItem(settings,"confidence")?cJSON_GetObjectItem(settings,"confidence")->valueint:50;
		
	cJSON* detection = detections->child;
	while(detection) {
		unsigned cx = 0;
		unsigned cy = 0;
		unsigned width = 0;
		unsigned height = 0;
		unsigned c = 0;
		label = "Undefined";
		cJSON* property = detection->child;
		while(property) {
			if( strcmp("c",property->string) == 0 ) {
				property->valueint = property->valuedouble * 100;
				property->valuedouble = property->valueint;
				c = property->valueint;
			}
			// Model_Inference emits 0..1; every interface (filters, MQTT,
			// web UI, settings) uses a 0..1000 normalized space, independent
			// of the model's input resolution.
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
				width = property->valueint;
				property->valuedouble = property->valueint;
				cx += property->valueint / 2;
			}
			if( strcmp("h",property->string) == 0 ) {
				property->valueint = property->valuedouble * 1000;
				height = property->valueint;
				property->valuedouble = property->valueint;
				cy += property->valueint / 2;
			}
			if( strcmp("label",property->string) == 0 ) {
				label = property->valuestring;
			}
			property = property->next;
		}
		
		//FILTER DETECTIONS
		// Coordinates are in the 0..1000 normalized space, as are the AOI,
		// size and exclude settings, so they compare directly.
		unsigned int pixel_cx = cx;
		unsigned int pixel_cy = cy;
		unsigned int pixel_width = width;
		unsigned int pixel_height = height;

		int insert = 0;
		if( c >= confidenceThreshold )
			insert = 1;
		if( pixel_width < minWidth || pixel_height < minHeight )
			insert = 0;

		if (insert) {
			cJSON* aoiPolygon = cJSON_GetObjectItem(settings, "aoi_polygon");
			if (aoiPolygon && cJSON_IsArray(aoiPolygon) && cJSON_GetArraySize(aoiPolygon) >= 3) {
				insert = point_in_polygon(aoiPolygon, pixel_cx, pixel_cy);
			} else if (!(pixel_cx >= x1 && pixel_cx <= x2 && pixel_cy >= y1 && pixel_cy <= y2)) {
				insert = 0;
			}
		}

		// Check exclude area - if detection center is inside exclude box, ignore it
		if( insert ) {
			cJSON* excludePolygons = cJSON_GetObjectItem(settings, "exclude_polygons");
			if (excludePolygons && cJSON_IsArray(excludePolygons) && polygon_list_contains_point(excludePolygons, pixel_cx, pixel_cy)) {
				insert = 0;
			} else {
				cJSON* exclude = cJSON_GetObjectItem(settings,"exclude");
				if( exclude ) {
				unsigned int exclude_x1 = cJSON_GetObjectItem(exclude,"x1")?cJSON_GetObjectItem(exclude,"x1")->valueint:0;
				unsigned int exclude_y1 = cJSON_GetObjectItem(exclude,"y1")?cJSON_GetObjectItem(exclude,"y1")->valueint:0;
				unsigned int exclude_x2 = cJSON_GetObjectItem(exclude,"x2")?cJSON_GetObjectItem(exclude,"x2")->valueint:1000;
				unsigned int exclude_y2 = cJSON_GetObjectItem(exclude,"y2")?cJSON_GetObjectItem(exclude,"y2")->valueint:1000;
				
				if( pixel_cx >= exclude_x1 && pixel_cx <= exclude_x2 && pixel_cy >= exclude_y1 && pixel_cy <= exclude_y2 )
					insert = 0;
				}
			}
		}

		if (insert && !label_is_active(settings, label))
			insert = 0;
		//Add custom filter here.  Set "insert = 0" if you want to exclude the detection

		if( insert ) {
			cJSON_AddNumberToObject( detection, "timestamp", timestamp );
			cJSON_AddItemToArray(processedDetections, cJSON_Duplicate(detection,1));
		}
		detection = detection->next;
	}

	cJSON_Delete( detections );

	// Debug channel: every few seconds publish the frame the model actually
	// received plus raw tensor statistics. Detections alone cannot tell a
	// broken input path from a misbehaving DLPU; these can.
	{
		static gint64 lastDebugPublish = 0;
		gint64 nowUs = g_get_monotonic_time();
		if (nowUs - lastDebugPublish > 3 * G_USEC_PER_SEC) {
			lastDebugPublish = nowUs;
			char debugTopic[128];
			const char* serial = ACAP_DEVICE_Prop("serial");

			cJSON* stats = Model_GetDebugStats();
			if (stats) {
				snprintf(debugTopic, sizeof(debugTopic), "debug/%s/tensors", serial);
				MQTT_Publish_JSON(debugTopic, stats, 0, 0);
				cJSON_Delete(stats);
			}

			unsigned jpegSize = 0, jpegW = 0, jpegH = 0;
			unsigned char* jpeg = Model_GetModelInputJPEG(&jpegSize, &jpegW, &jpegH);
			if (jpeg) {
				snprintf(debugTopic, sizeof(debugTopic), "debug/%s/input", serial);
				MQTT_Publish_Binary(debugTopic, (int)jpegSize, jpeg, 0, 0);
				free(jpeg);
			}

			// Raw output tensors for the same frame. Comparing these against a
			// CPU run of the identical model on the identical input tells us
			// whether the DLPU computed different values or merely wrote the
			// right values in a different memory order.
			size_t rawSize = 0;
			const void* raw = Model_GetRawOutput(0, &rawSize);
			if (raw && rawSize) {
				snprintf(debugTopic, sizeof(debugTopic), "debug/%s/coords", serial);
				MQTT_Publish_Binary(debugTopic, (int)rawSize, (void*)raw, 0, 0);
			}
			raw = Model_GetRawOutput(1, &rawSize);
			if (raw && rawSize) {
				snprintf(debugTopic, sizeof(debugTopic), "debug/%s/scores", serial);
				MQTT_Publish_Binary(debugTopic, (int)rawSize, (void*)raw, 0, 0);
			}
		}
	}

	Output( processedDetections, modelWidth, modelHeight );
	Model_Reset();

	cJSON_Delete(processedDetections);
	LOG_TRACE("%s>\n",__func__);
	return G_SOURCE_CONTINUE;
}

void HTTP_ENDPOINT_eventsTransition(const ACAP_HTTP_Response response,const ACAP_HTTP_Request request) {
	if( !eventsTransition )
		eventsTransition = cJSON_CreateObject();
	ACAP_HTTP_Respond_JSON(  response, eventsTransition);
}

/* ------------------------------------------------------------------
 * SD Capture: download zip and clear endpoints
 * ------------------------------------------------------------------ */

static void HTTP_ENDPOINT_sd_download(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
    (void)request;
    const char* tmp_path = "/tmp/detectx_export.zip";
    const char* fallback  = "/tmp/detectx_export.tar.gz";

    if (sd_is_busy()) {
        ACAP_HTTP_Respond_Error(response, 503, "SD capture busy, try again later");
        return;
    }
    sd_set_busy(1);

    int ok = sd_create_zip(tmp_path);
    const char* out_path = tmp_path;
    const char* content_type = "application/zip";
    const char* dl_filename  = "detectx_export.zip";

    if (!ok) {
        /* zip failed, try the tar.gz fallback path */
        ok = sd_create_zip(fallback);  /* sd_create_zip tries tar.gz internally */
        if (ok) {
            out_path      = fallback;
            content_type  = "application/gzip";
            dl_filename   = "detectx_export.tar.gz";
        }
    }

    if (!ok) {
        sd_set_busy(0);
        ACAP_HTTP_Respond_Error(response, 500, "Failed to create archive");
        return;
    }

    /* Serve the file */
    FILE* f = fopen(out_path, "rb");
    if (!f) {
        sd_set_busy(0);
        ACAP_HTTP_Respond_Error(response, 500, "Archive not found");
        return;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    unsigned char* buf = malloc(fsize);
    if (!buf) {
        fclose(f);
        sd_set_busy(0);
        ACAP_HTTP_Respond_Error(response, 500, "Out of memory");
        return;
    }
    long nread = (long)fread(buf, 1, fsize, f);
    fclose(f);
    remove(out_path);

    ACAP_HTTP_Header_FILE(response, dl_filename, content_type, (int)nread);
    ACAP_HTTP_Respond_Data(response, (int)nread, buf);
    free(buf);
    sd_set_busy(0);
}

static void HTTP_ENDPOINT_sd_clear(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
    (void)request;
    if (sd_is_busy()) {
        ACAP_HTTP_Respond_Error(response, 503, "SD capture busy, try again later");
        return;
    }
    sd_set_busy(1);
    sd_clear_directories();
    ACAP_STATUS_SetNumber("sd_capture", "count", 0);
    sd_set_busy(0);
    cJSON* ok = cJSON_CreateObject();
    cJSON_AddBoolToObject(ok, "ok", 1);
    ACAP_HTTP_Respond_JSON(response, ok);
    cJSON_Delete(ok);
}

static GMainLoop *main_loop = NULL;

static gboolean
signal_handler(gpointer user_data) {
    LOG("Received SIGTERM, initiating shutdown\n");
    if (main_loop && g_main_loop_is_running(main_loop)) {
        g_main_loop_quit(main_loop);
    }
    return G_SOURCE_REMOVE;
}

void
Main_MQTT_Subscription_Message(const char *topic, const char *payload) {
	LOG("Message arrived: %s %s\n",topic,payload);
}

void Main_MQTT_Status(int state) {
    char topic[64];
    cJSON* message = 0;
	LOG_TRACE("<%s\n",__func__);

    switch (state) {
        case MQTT_INITIALIZING:
            LOG("%s: Initializing\n", __func__);
            break;
        case MQTT_CONNECTING:
            LOG("%s: Connecting\n", __func__);
            break;
        case MQTT_CONNECTED:
            LOG("%s: Connected\n", __func__);
            sprintf(topic, "connect/%s", ACAP_DEVICE_Prop("serial"));
            message = cJSON_CreateObject();
            cJSON_AddTrueToObject(message, "connected");
            cJSON_AddStringToObject(message, "address", ACAP_DEVICE_Prop("IPv4"));
            MQTT_Publish_JSON(topic, message, 0, 1);
            cJSON_Delete(message);
            break;
        case MQTT_DISCONNECTING:
            sprintf(topic, "connect/%s", ACAP_DEVICE_Prop("serial"));
            message = cJSON_CreateObject();
            cJSON_AddFalseToObject(message, "connected");
            cJSON_AddStringToObject(message, "address", ACAP_DEVICE_Prop("IPv4"));
            MQTT_Publish_JSON(topic, message, 0, 1);
            cJSON_Delete(message);
            break;
        case MQTT_RECONNECTED:
            LOG("%s: Reconnected\n", __func__);
            break;
        case MQTT_DISCONNECTED:
            LOG("%s: Disconnect\n", __func__);
            break;
    }
	LOG_TRACE("%s>\n",__func__);
	
}

static gboolean
MAIN_STATUS_Timer() {
	ACAP_STATUS_SetNumber("device", "cpu", ACAP_DEVICE_CPU_Average());	
	ACAP_STATUS_SetNumber("device", "network", ACAP_DEVICE_Network_Average());
	return TRUE;
}


int
Setup_SD_Card() {
    const char* sd_mount = "/var/spool/storage/SD_DISK";
    const char* detectx_dir = "/var/spool/storage/SD_DISK/detectx";

    struct stat sb;

    // Check if SD mount point exists and is a directory
    if (stat(sd_mount, &sb) != 0 || !S_ISDIR(sb.st_mode)) {
        ACAP_STATUS_SetBool("SDCARD", "available", 0);
        LOG("SD Card not detected");
        return 0;
    }

    // Check if DetectX directory exists
    if (stat(detectx_dir, &sb) != 0) {
        // Not found: try to create the directory with appropriate access rights
        if (mkdir(detectx_dir, 0770) != 0) {
            ACAP_STATUS_SetBool("SDCARD", "available", 0);
	        LOG_WARN("SD Card detected but could not create directory %s: %s\n", detectx_dir, strerror(errno));
            return 0;
        }
    } else if (!S_ISDIR(sb.st_mode)) {
        // Exists but is not a directory
        ACAP_STATUS_SetBool("SDCARD", "available", 0);
        LOG_WARN("Error: SD Card structure propblem\n");
        return 0;
    }

    ACAP_STATUS_SetBool("SDCARD", "available", 1);
	LOG("SD Card is ready to be used\n");
    return 1;
}

int main(void) {
	setbuf(stdout, NULL);

	openlog(APP_PACKAGE, LOG_PID|LOG_CONS, LOG_USER);

	ACAP( APP_PACKAGE, ConfigUpdate );
	LOG("------------ %s ----------\n",APP_PACKAGE);

	settings = ACAP_Get_Config("settings");
	if(!settings) {
		ACAP_STATUS_SetString("model","status","Configuration error");
		ACAP_STATUS_SetString("model","error","Unable to load application settings");
		ACAP_STATUS_SetBool("model","state", 0);
		LOG_WARN("No settings found\n");
		return 1;
	}

//	Setup_SD_Card();

	eventLabelCounter = cJSON_CreateObject();

	// Loading the model onto the DLPU takes 30-60 seconds on a cold start, and
	// GLib's main loop is single-threaded, so nothing can be served while larod
	// is working. Publish the loading state first, then let the main loop start
	// and do the load from a short timeout -- otherwise the web UI's requests
	// hang and the application looks crashed rather than busy.
	ACAP_STATUS_SetString("model","status","Loading model");
	ACAP_STATUS_SetString("model","error", NULL);
	ACAP_STATUS_SetBool("model","state", 0);
	g_timeout_add( 1500, deferred_model_start, NULL );
	ACAP_HTTP_Node("model", HTTP_ENDPOINT_model);
	ACAP_HTTP_Node("modelinput", HTTP_ENDPOINT_modelinput);
	ACAP_HTTP_Node("sd_download", HTTP_ENDPOINT_sd_download);
	ACAP_HTTP_Node("sd_clear", HTTP_ENDPOINT_sd_clear);
	MQTT_Init( Main_MQTT_Status, Main_MQTT_Subscription_Message  );	
	ACAP_Set_Config("mqtt", MQTT_Settings() );
	
	ACAP_DEVICE_CPU_Average();
	ACAP_DEVICE_Network_Average();
	g_timeout_add_seconds( 60 , MAIN_STATUS_Timer, NULL );

    LOG("Entering main loop\n");
	main_loop = g_main_loop_new(NULL, FALSE);
    GSource *signal_source = g_unix_signal_source_new(SIGTERM);
    if (signal_source) {
		g_source_set_callback(signal_source, signal_handler, NULL, NULL);
		g_source_attach(signal_source, NULL);
	} else {
		LOG_WARN("Signal detection failed");
	}
	LOG_TRACE("%s>\n",__func__);	
    g_main_loop_run(main_loop);
	LOG("Terminating and cleaning up %s\n",APP_PACKAGE);

	Main_MQTT_Status(MQTT_DISCONNECTING); //Send graceful disconnect message
	MQTT_Cleanup();
    ACAP_Cleanup();
	Model_Cleanup();
    closelog();

	
    return 0;
}
