#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <pthread.h>
#include <sys/stat.h>
#include <errno.h>

#include "ACAP.h"
#include "MQTT.h"
#include "Model.h"
#include "cJSON.h"

#define LOG(fmt, args...) { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args);}
#define LOG_WARN(fmt, args...) { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args);}
//#define LOG_TRACE(fmt, args...) { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args);}
#define LOG_TRACE(fmt, args...) {}

#define SD_FOLDER "/var/spool/storage/SD_DISK/detectx"
#define CROP_HISTORY_SIZE 10

// --- Base64 Encoding Helper ---

static const char base64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *base64_encode(const unsigned char *src, size_t len) {
    size_t olen = 4 * ((len + 2) / 3);
    char *out = malloc(olen + 1);
    if (!out) return NULL;
    char *pos = out;
    int val = 0, valb = -6;
    for (size_t i = 0; i < len; i++) {
        val = (val << 8) + src[i];
        valb += 8;
        while (valb >= 0) {
            *pos++ = base64_table[(val >> valb) & 0x3F];
            valb -= 6;
        }
    }
    if (valb > -6) *pos++ = base64_table[((val << 8) >> (valb + 8)) & 0x3F];
    while ((pos - out) % 4) *pos++ = '=';
    *pos = '\0';
    return out;
}

// --- Ring Buffer for Recent Detections ---

typedef struct {
    char *base64_image; // malloc'd
    char label[64];
    int confidence;
    int x, y, w, h;
} CropEntry;

static CropEntry crop_history[CROP_HISTORY_SIZE];
static int crop_history_head = 0; // next insert
static int crop_history_count = 0;
static pthread_mutex_t crop_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

static void add_crop_to_history(const unsigned char *jpeg_data, unsigned jpeg_size,
                                const char *label, int confidence, int x, int y, int w, int h) {
    char *b64img = base64_encode(jpeg_data, jpeg_size);
    if (!b64img) return;
	
    pthread_mutex_lock(&crop_cache_mutex);
	
    CropEntry *entry = &crop_history[crop_history_head];
    
	if (entry->base64_image)
		free(entry->base64_image);
    entry->base64_image = b64img;
    strncpy(entry->label, label, sizeof(entry->label) - 1);
    entry->label[sizeof(entry->label) - 1] = 0;
    entry->confidence = confidence;
    entry->x = x;
	entry->y = y;
	entry->w = w;
	entry->h = h;
    crop_history_head = (crop_history_head + 1) % CROP_HISTORY_SIZE;
    if (crop_history_count < CROP_HISTORY_SIZE) crop_history_count++;
	
    pthread_mutex_unlock(&crop_cache_mutex);
}

static void reset_crop_history() {
    pthread_mutex_lock(&crop_cache_mutex);
    for (int i = 0; i < CROP_HISTORY_SIZE; ++i) {
        if (crop_history[i].base64_image) free(crop_history[i].base64_image);
        crop_history[i].base64_image = NULL;
        crop_history[i].label[0] = '\0';
    }
    crop_history_head = 0;
    crop_history_count = 0;
    pthread_mutex_unlock(&crop_cache_mutex);
}

// --- HTTP Callback for Latest Crops ---

void Crops_HTTP_Callback(ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
    if (strcmp(ACAP_HTTP_Get_Method(request), "GET") != 0) {
        ACAP_HTTP_Respond_Error(response, 405, "Method Not Allowed");
        return;
    }
    cJSON *arr = cJSON_CreateArray();

    pthread_mutex_lock(&crop_cache_mutex);
    int idx = (crop_history_head - 1 + CROP_HISTORY_SIZE) % CROP_HISTORY_SIZE;
    for (int i = 0; i < crop_history_count; ++i) {
        CropEntry *entry = &crop_history[idx];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "image", entry->base64_image ? entry->base64_image : "");
        cJSON_AddStringToObject(item, "label", entry->label);
        cJSON_AddNumberToObject(item, "confidence", entry->confidence);
        cJSON_AddNumberToObject(item, "x", entry->x);
        cJSON_AddNumberToObject(item, "y", entry->y);
        cJSON_AddNumberToObject(item, "w", entry->w);
        cJSON_AddNumberToObject(item, "h", entry->h);
        cJSON_AddItemToArray(arr, item);
        idx = (idx - 1 + CROP_HISTORY_SIZE) % CROP_HISTORY_SIZE;
    }
    pthread_mutex_unlock(&crop_cache_mutex);
    ACAP_HTTP_Respond_JSON(response, arr);
    cJSON_Delete(arr);
}

// --- Helpers, Output Logic ---

void replace_spaces(char *str) {
    while (*str) {
        if (*str == ' ') *str = '_';
        str++;
    }
}

int ensure_sd_directory() {
    struct stat st = {0};
    if (stat(SD_FOLDER, &st) == -1) {
        if (mkdir(SD_FOLDER, 0755) == -1) {
            LOG_WARN("%s: Failed to create SD directory %s: %s\n", __func__, SD_FOLDER, strerror(errno));
            return 0;
        }
    }
    return 1;
}

int save_jpeg_to_file(const char* path, const unsigned char* jpeg, unsigned size) {
    FILE* f = fopen(path, "wb");
    if (!f) {
        LOG_WARN("%s: Failed to open %s for writing: %s\n",__func__, path, strerror(errno));
        return 0;
    }
    size_t w = fwrite((void*)jpeg, 1, size, f);
    fclose(f);
    return w == size ? 1 : 0;
}

int save_label_to_file(const char* path, const char* label, int x, int y, int w, int h) {
    FILE* f = fopen(path, "w");
    if (!f) {
        LOG_WARN("%s: Failed to open %s for writing: %s\n",__func__, path, strerror(errno));
        return 0;
    }
    fprintf(f, "%s %d %d %d %d\n", label, x, y, w, h);
    fclose(f);
    return 1;
}

// State tracking objects
cJSON* lastTriggerTime = 0;
cJSON* firstTriggerTime = 0;
int lastDetectionsWhereEmpty = 0;

// --- Core Output Function ---

void Output(cJSON* detections) {
    if (!detections || cJSON_GetArraySize(detections) == 0 ) return;
	LOG_TRACE("<%s %d\n",__func__,cJSON_GetArraySize(detections));

	ACAP_STATUS_SetObject("labels","detections",detections);
    double now = ACAP_DEVICE_Timestamp();
    cJSON* settings = ACAP_Get_Config("settings");
    if (!settings) {
		LOG_TRACE("ERROR %s>\n",__func__);
		return;
	}

    // Cropping output config
    cJSON* cropping = cJSON_GetObjectItem(settings, "cropping");
    int cropping_active = cropping && cJSON_IsTrue(cJSON_GetObjectItem(cropping, "active"));
    int sdcard_enable = cropping && cJSON_IsTrue(cJSON_GetObjectItem(cropping, "sdcard"));
    int mqtt_enable = cropping && cJSON_IsTrue(cJSON_GetObjectItem(cropping, "mqtt"));

    if (sdcard_enable && !ensure_sd_directory()) {
        sdcard_enable = 0;
    }

    char topic[256];
    sprintf(topic, "detection/%s", ACAP_DEVICE_Prop("serial"));
    cJSON* mqttPayload = cJSON_CreateObject();
    cJSON_AddItemReferenceToObject(mqttPayload, "detections", detections);

    if (cJSON_GetArraySize(detections)) {
        MQTT_Publish_JSON(topic, mqttPayload, 0, 0);
        lastDetectionsWhereEmpty = 0;
    } else {
        if (!lastDetectionsWhereEmpty)
            MQTT_Publish_JSON(topic, mqttPayload, 0, 0);
        lastDetectionsWhereEmpty = 1;
    }
    cJSON_Delete(mqttPayload);

    if (!lastTriggerTime) lastTriggerTime = cJSON_CreateObject();
    if (!firstTriggerTime) firstTriggerTime = cJSON_CreateObject();

    cJSON* minEventDurationObj = cJSON_GetObjectItem(settings, "minEventDuration");
    cJSON* stabelizeTransitionObj = cJSON_GetObjectItem(settings, "stabelizeTransition");
    double minEventDuration = minEventDurationObj ? minEventDurationObj->valuedouble : 3000;
    double stabelizeTransition = stabelizeTransitionObj ? stabelizeTransitionObj->valuedouble : 0;

    int idx = 0;
    cJSON* detection = detections->child;

    int leftborder_offset = cropping && cJSON_GetObjectItem(cropping, "leftborder") ? cJSON_GetObjectItem(cropping, "leftborder")->valueint : 0;
    int rightborder_offset = cropping && cJSON_GetObjectItem(cropping, "rightborder") ? cJSON_GetObjectItem(cropping, "rightborder")->valueint : 0;
    int topborder_offset = cropping && cJSON_GetObjectItem(cropping, "topborder") ? cJSON_GetObjectItem(cropping, "topborder")->valueint : 0;
    int bottomborder_offset = cropping && cJSON_GetObjectItem(cropping, "bottomborder") ? cJSON_GetObjectItem(cropping, "bottomborder")->valueint : 0;

    while (detection) {
        cJSON* labelObj = cJSON_GetObjectItem(detection, "label");
        cJSON* confObj = cJSON_GetObjectItem(detection, "confidence");
        cJSON* timestampObj = cJSON_GetObjectItem(detection, "timestamp");
        const char* label = labelObj && cJSON_IsString(labelObj) ? labelObj->valuestring : "Undefined";
        int conf = confObj && cJSON_IsNumber(confObj) ? confObj->valueint : 0;
        double timestamp = timestampObj && cJSON_IsNumber(timestampObj) ? timestampObj->valuedouble : now;

        // --- Crop/image logic ---
        if (cropping_active) {
            unsigned jpeg_size = 0;
            int crop_x = 0, crop_y = 0, crop_w = 0, crop_h = 0, img_w = 0,img_h = 0;
            const unsigned char* jpeg_data = Model_GetImageData(detection, &jpeg_size, &crop_x, &crop_y, &crop_w, &crop_h, &img_w, &img_h);

            if (jpeg_data && jpeg_size > 0) {
                // In-memory crop cache for API
				crop_x = leftborder_offset;
				crop_y = topborder_offset;
				crop_w = img_w - leftborder_offset - rightborder_offset;
				crop_h = img_h - topborder_offset - bottomborder_offset;
                add_crop_to_history(jpeg_data, jpeg_size, label, conf, crop_x, crop_y, crop_w, crop_h);

                // SD card output
                if (sdcard_enable) {
                    char safe_label[64];
                    strncpy(safe_label, label, sizeof(safe_label) - 1);
                    safe_label[sizeof(safe_label) - 1] = 0;
                    replace_spaces(safe_label);

                    char fname_img[256], fname_label[256];
                    snprintf(fname_img, sizeof(fname_img), "%s/crop_%s_%.0f_%d.jpg",
                            SD_FOLDER, safe_label, timestamp, idx);
                    snprintf(fname_label, sizeof(fname_label), "%s/crop_%s_%.0f_%d.txt",
                            SD_FOLDER, safe_label, timestamp, idx);

                    if (save_jpeg_to_file(fname_img, jpeg_data, jpeg_size)) {
                        if (save_label_to_file(fname_label, label, crop_x, crop_y, crop_w, crop_h)) {
                            LOG_TRACE("Cropped detection and label written to SD: %s, %s\n", fname_img, fname_label);
                        } else {
                            LOG_WARN("%s: Failed to save crop label to SD: %s\n", __func__, fname_label);
                        }
                    } else {
                        LOG_WARN("%s: Failed to save crop to SD: %s\n",__func__, fname_img);
                    }
                } // SD

                // MQTT output (optionally extend here for binary/JPEG)
                if (mqtt_enable) {
					//LOG_TRACE("%s Process item active mqtt_enambled\n",__func__);
                    cJSON* mqttMsg = cJSON_CreateObject();
                    cJSON_AddStringToObject(mqttMsg, "label", label);
                    cJSON_AddNumberToObject(mqttMsg, "timestamp", timestamp);
                    cJSON_AddNumberToObject(mqttMsg, "x", crop_x);
                    cJSON_AddNumberToObject(mqttMsg, "y", crop_y);
                    cJSON_AddNumberToObject(mqttMsg, "w", crop_w);
                    cJSON_AddNumberToObject(mqttMsg, "h", crop_h);
                    cJSON_AddNumberToObject(mqttMsg, "jpeg_size", jpeg_size);
                    // Optionally: add cropped JPEG as Base64 string to MQTT

                    char crop_topic[256];
                    sprintf(crop_topic, "detection/%s/crop", ACAP_DEVICE_Prop("serial"));
                    MQTT_Publish_JSON(crop_topic, mqttMsg, 0, 0);
                    LOG("Detection with crop metadata sent to MQTT (label: %s, ts: %.0f, sz: %u)\n",
                        label, timestamp, jpeg_size);
                    cJSON_Delete(mqttMsg);
                }
				LOG_TRACE("%s Process item active done\n",__func__);	
            }
			LOG_TRACE("%s Process item done\n",__func__);
        }

        // --- Event states, transitions (unchanged) ---
        if (!ACAP_STATUS_Bool("events", label)) {
            cJSON* transitionStateTime = cJSON_GetObjectItem(firstTriggerTime, label);
            double tr_ts = transitionStateTime ? transitionStateTime->valuedouble : now;
            if (!transitionStateTime)
                cJSON_AddNumberToObject(firstTriggerTime, label, now);

            if ((now - tr_ts) >= stabelizeTransition) {
                LOG_TRACE("%s: Label %s set to high", __func__, label);
                ACAP_EVENTS_Fire_State(label, 1);
                sprintf(topic, "event/%s/%s/true", ACAP_DEVICE_Prop("serial"), label);
                cJSON_AddTrueToObject(detection, "state");
                MQTT_Publish_JSON(topic, detection, 0, 0);
                cJSON_DeleteItemFromObject(firstTriggerTime, label);

                cJSON* lastTriggerObj = cJSON_GetObjectItem(lastTriggerTime, label);
                if (!lastTriggerObj) {
                    cJSON_AddNumberToObject(lastTriggerTime, label, now);
                } else {
                    cJSON_ReplaceItemInObject(lastTriggerTime, label, cJSON_CreateNumber(now));
                }
            }
        }
        idx++;
        detection = detection->next;
    } // processing loop

    // Deactivation logic (unchanged)
    cJSON* lastTrigger = lastTriggerTime ? lastTriggerTime->child : NULL;
    while (lastTrigger) {
        if (ACAP_STATUS_Bool("events", lastTrigger->string)) {
            if ((now - lastTrigger->valuedouble) > minEventDuration) {
                ACAP_EVENTS_Fire_State(lastTrigger->string, 0);
                sprintf(topic, "event/%s/%s/false", ACAP_DEVICE_Prop("serial"), lastTrigger->string);
                cJSON* statePayload = cJSON_CreateObject();
                cJSON_AddStringToObject(statePayload, "label", lastTrigger->string);
                cJSON_AddFalseToObject(statePayload, "state");
                cJSON_AddNumberToObject(statePayload, "timestamp", ACAP_DEVICE_Timestamp());
                MQTT_Publish_JSON(topic, statePayload, 0, 0);
                cJSON_Delete(statePayload);
                LOG_TRACE("%s: Label %s set to Low", __func__, lastTrigger->string);
            }
        }
        lastTrigger = lastTrigger->next;
    }
	LOG_TRACE("%s>\n",__func__);
}

// --- Reset Output ---

void Output_reset() {
	LOG_TRACE("<%s\n",__func__);
    cJSON* model = ACAP_Get_Config("model");
    if (!model) {
        LOG_WARN("%s: No Model Config found", __func__);
        return;
    }
    cJSON* labels = cJSON_GetObjectItem(model, "labels");
    if (!labels) {
        LOG_WARN("%s: Model has no labels", __func__);
        return;
    }
    cJSON* label = labels->child;
    while (label) {
        if (cJSON_IsString(label)) {
            char niceName[32];
            snprintf(niceName, sizeof(niceName), "DetectX: %s", label->valuestring);
            char* labelCopy = strdup(label->valuestring);
            if (labelCopy) {
                replace_spaces(labelCopy);
                ACAP_EVENTS_Add_Event(labelCopy, niceName, 1);
                free(labelCopy);
            }
        }
        label = label->next;
    }
    if (lastTriggerTime) cJSON_Delete(lastTriggerTime);
    if (firstTriggerTime) cJSON_Delete(firstTriggerTime);
    lastTriggerTime = cJSON_CreateObject();
    firstTriggerTime = cJSON_CreateObject();
    reset_crop_history();
	LOG_TRACE("%s>\n",__func__);
}

// --- Initialization Function ---

void Output_init() {
	LOG_TRACE("<%s\n",__func__);
    // Register HTTP node for 'crops'
    ACAP_HTTP_Node("crops", Crops_HTTP_Callback);
    reset_crop_history();
	LOG_TRACE("%s>\n",__func__);	
}

// --- Call Output_init() early in your application startup ---

