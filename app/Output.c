#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <curl/curl.h>
#include <errno.h>
#include <pthread.h>

#include "ACAP.h"
#include "MQTT.h"
#include "Model.h"
#include "cJSON.h"

#include "Output.h"
#include "Output_crop_cache.h"
#include "Output_helpers.h"
#include "Output_http.h"

#define LOG(fmt, args...)      { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args);}
#define LOG_WARN(fmt, args...) { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args);}
#define LOG_TRACE(fmt, args...) {}

#define MAX_LABELS 32
#define MAX_ROLLING 16

#define SD_FOLDER "/var/spool/storage/SD_DISK/detectx"

typedef struct {
    char name[64];
    int state; // 0=LOW, 1=HIGH
    int rolling[MAX_ROLLING];
    int rolling_head;
    int rolling_count;
    double last_detect_time;
} LabelEventState;

static LabelEventState eventsCache[MAX_LABELS];
static int eventsCache_len = 0;
static int lastDetectionsWereEmpty = 0;
static double last_output_time_ms = 0;

// Helper: manage per-label state
static LabelEventState* find_or_create_label_state(const char* label) {
    for (int i = 0; i < eventsCache_len; ++i)
        if (strcmp(eventsCache[i].name, label) == 0)
            return &eventsCache[i];
    if (eventsCache_len < MAX_LABELS) {
        strncpy(eventsCache[eventsCache_len].name, label, 63);
        eventsCache[eventsCache_len].name[63] = 0;
        eventsCache[eventsCache_len].state = 0;
        eventsCache[eventsCache_len].rolling_head = 0;
        eventsCache[eventsCache_len].rolling_count = 0;
        memset(eventsCache[eventsCache_len].rolling, 0, sizeof(eventsCache[eventsCache_len].rolling));
        eventsCache[eventsCache_len].last_detect_time = 0;
        return &eventsCache[eventsCache_len++];
    }
    return NULL;
}

static gboolean Output_DeactivateExpired(gpointer user_data) {
    double now = ACAP_DEVICE_Timestamp();
    cJSON* settings = ACAP_Get_Config("settings");
    if(!settings)
        return TRUE;
    double minEventDuration = cJSON_GetObjectItem(settings,"minEventDuration")?cJSON_GetObjectItem(settings,"minEventDuration")->valuedouble:3000;

    char topic[256];
    for (int i = 0; i < eventsCache_len; ++i) {
        if (eventsCache[i].state == 1) {
            // Use the last_detect_time set by Output() on detection
            if ((now - eventsCache[i].last_detect_time) > minEventDuration) {
                eventsCache[i].state = 0;
                ACAP_EVENTS_Fire_State(eventsCache[i].name, 0);
                snprintf(topic, sizeof(topic), "event/%s/%s/false", ACAP_DEVICE_Prop("serial"), eventsCache[i].name);
                cJSON* statePayload = cJSON_CreateObject();
                cJSON_AddStringToObject(statePayload, "label", eventsCache[i].name);
                cJSON_AddFalseToObject(statePayload, "state");
                cJSON_AddNumberToObject(statePayload, "timestamp", ACAP_DEVICE_Timestamp());
                MQTT_Publish_JSON(topic, statePayload, 0, 0);
                cJSON_Delete(statePayload);
                LOG_TRACE("%s: Label %s set to LOW\n", __func__, eventsCache[i].name);
            }
        }
    }
    return TRUE;
}

void Output(cJSON* detections, int modelWidth, int modelHeight) {
    if (!detections || cJSON_GetArraySize(detections) == 0) {
        cJSON* emptyArr = cJSON_CreateArray();
        ACAP_STATUS_SetObject("labels", "detections", emptyArr);
        cJSON_Delete(emptyArr);
        return;
    }

    LOG_TRACE("<%s %d\n", __func__, cJSON_GetArraySize(detections));

    // Export current detections to status (DUPLICATE for safety)
    cJSON* statusDup = cJSON_Duplicate(detections, 1);
    ACAP_STATUS_SetObject("labels", "detections", statusDup);
    cJSON_Delete(statusDup);

    double now = ACAP_DEVICE_Timestamp();

    cJSON* settings = ACAP_Get_Config("settings");
    if (!settings) {
        LOG_TRACE("ERROR %s - no settings>\n", __func__);
        return;
    }

    // Cropping/crop export config
    cJSON* cropping = cJSON_GetObjectItem(settings, "cropping");
    int cropping_active = cropping && cJSON_IsTrue(cJSON_GetObjectItem(cropping, "active"));
    int sdcard_enable   = cropping && cJSON_IsTrue(cJSON_GetObjectItem(cropping, "sdcard"));
    int mqtt_export     = cropping && cJSON_IsTrue(cJSON_GetObjectItem(cropping, "mqtt"));
    int http_export     = cropping && cJSON_IsTrue(cJSON_GetObjectItem(cropping, "http"));
    int throttle        = cJSON_GetObjectItem(cropping, "throttle") ?
                         cJSON_GetObjectItem(cropping, "throttle")->valueint : 500;

    if (sdcard_enable && !ensure_sd_directory()) {
        sdcard_enable = 0;
    }

    // --- Export all detections as MQTT (non-crop summary) with metadata ---
    char topic[256];
    snprintf(topic, sizeof(topic), "detection/%s", ACAP_DEVICE_Prop("serial"));
    cJSON* mqttPayload = cJSON_CreateObject();
    cJSON_AddItemToObject(mqttPayload, "detections", cJSON_Duplicate(detections, 1));

    // Add metadata for coordinate interpretation
    cJSON* metadata = cJSON_CreateObject();
    cJSON_AddNumberToObject(metadata, "modelWidth", modelWidth);
    cJSON_AddNumberToObject(metadata, "modelHeight", modelHeight);
    cJSON_AddStringToObject(metadata, "coordinateSystem", "pixels");
    cJSON_AddItemToObject(mqttPayload, "metadata", metadata);

    MQTT_Publish_JSON(topic, mqttPayload, 0, 0);
    cJSON_Delete(mqttPayload);
    lastDetectionsWereEmpty = (cJSON_GetArraySize(detections) == 0);

    // --- Adaptive event gating
    const char *prioritize = cJSON_GetObjectItem(settings, "prioritize")?cJSON_GetObjectItem(settings, "prioritize")->valuestring:"accuracy";

    double averageInferenceTime = ACAP_STATUS_Double("mode", "averageTime");
    int   desired_window_ms = 1000;
    int   min_frames_in_window = 3;
    int   window_size = (int)((desired_window_ms + averageInferenceTime - 1) / averageInferenceTime);
    if (window_size < 2) window_size = 2;
    if (window_size > MAX_ROLLING) window_size = MAX_ROLLING;

    cJSON* logic = cJSON_GetObjectItem(settings, "eventLogic");
    if (logic) {
        if (cJSON_GetObjectItem(logic, "frames")) min_frames_in_window = cJSON_GetObjectItem(logic, "frames")->valueint;
        if (cJSON_GetObjectItem(logic, "window")) desired_window_ms = cJSON_GetObjectItem(logic, "window")->valueint;
    }

    // Cropping settings
    int leftborder_offset   = cropping && cJSON_GetObjectItem(cropping, "leftborder") ?
                                cJSON_GetObjectItem(cropping, "leftborder")->valueint : 0;
    int rightborder_offset  = cropping && cJSON_GetObjectItem(cropping, "rightborder") ?
                                cJSON_GetObjectItem(cropping, "rightborder")->valueint : 0;
    int topborder_offset    = cropping && cJSON_GetObjectItem(cropping, "topborder") ?
                                cJSON_GetObjectItem(cropping, "topborder")->valueint : 0;
    int bottomborder_offset = cropping && cJSON_GetObjectItem(cropping, "bottomborder") ?
                                cJSON_GetObjectItem(cropping, "bottomborder")->valueint : 0;

    int idx = 0;
    cJSON* detection = detections->child;
    char frame_labels[MAX_LABELS][64];
    int n_frame_labels = 0;

    while (detection) {
        const char* label = "Undefined";
        int conf = 0;
        double timestamp = now;
        cJSON* labelObj;
        if ((labelObj = cJSON_GetObjectItem(detection, "label")))
            if (cJSON_IsString(labelObj)) label = labelObj->valuestring;
        cJSON* confObj;
        if ((confObj = cJSON_GetObjectItem(detection, "c")))
            if (cJSON_IsNumber(confObj)) conf = confObj->valueint;
        cJSON* timestampObj;
        if ((timestampObj = cJSON_GetObjectItem(detection, "timestamp")))
            if (cJSON_IsNumber(timestampObj)) timestamp = timestampObj->valuedouble;

        if (n_frame_labels < MAX_LABELS) {
            strncpy(frame_labels[n_frame_labels++], label, 63);
            frame_labels[n_frame_labels - 1][63] = 0;
        }

        LabelEventState* evt = find_or_create_label_state(label);
        if (!evt) { idx++; detection = detection->next; continue; }

        if (strcmp(prioritize, "speed") == 0) {
            if (evt->state == 0) {
                evt->state = 1;
                evt->last_detect_time = now;
                ACAP_EVENTS_Fire_State(label, 1);
                snprintf(topic, sizeof(topic), "event/%s/%s/true", ACAP_DEVICE_Prop("serial"), label);

                // Create temp object for event
                cJSON* eventPayload = cJSON_Duplicate(detection, 1);
                cJSON_AddTrueToObject(eventPayload, "state");
                MQTT_Publish_JSON(topic, eventPayload, 0, 0);
                cJSON_Delete(eventPayload);
            } else {
                evt->last_detect_time = now;
            }
        } else {
            evt->rolling_head = (evt->rolling_head + 1) % window_size;
            evt->rolling[evt->rolling_head] = 1;
            if (evt->rolling_count < window_size) evt->rolling_count++;
            evt->last_detect_time = now;

            int sum = 0;
            for (int i = 0; i < evt->rolling_count; ++i)
                sum += evt->rolling[i];
            if ((evt->state == 0) && (sum >= min_frames_in_window)) {
                evt->state = 1;
                evt->last_detect_time = now;
                ACAP_EVENTS_Fire_State(label, 1);
                snprintf(topic, sizeof(topic), "event/%s/%s/true", ACAP_DEVICE_Prop("serial"), label);

                // Create temp object for event
                cJSON* eventPayload = cJSON_Duplicate(detection, 1);
                cJSON_AddTrueToObject(eventPayload, "state");
                MQTT_Publish_JSON(topic, eventPayload, 0, 0);
                cJSON_Delete(eventPayload);
            }
            if (evt->state == 1) evt->last_detect_time = now;
        }

        // Cropping output path
        if (cropping_active) {
            int crop_x = 0, crop_y = 0, crop_w = 0, crop_h = 0, img_w = 0, img_h = 0;
            unsigned jpeg_size = 0;
            const unsigned char* jpeg_data =
                Model_GetImageData(detection, &jpeg_size,
                                   &crop_x, &crop_y, &crop_w, &crop_h,
                                   &img_w, &img_h);

            crop_x = leftborder_offset;
            crop_y = topborder_offset;
            crop_w = img_w - leftborder_offset - rightborder_offset;
            crop_h = img_h - topborder_offset - bottomborder_offset;

            // NOTE: Implement cache eviction in output_crop_cache_add for safety
            const char* imageDataBase64 = output_crop_cache_add(
                jpeg_data, jpeg_size, label, conf, crop_x, crop_y, crop_w, crop_h, img_w, img_h);

            double now_ts = ACAP_DEVICE_Timestamp();
            if (imageDataBase64 && now_ts - last_output_time_ms > throttle) {
                last_output_time_ms = now_ts;

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
                            LOG_TRACE("Saved crop to SD: %s, %s\n", fname_img, fname_label);
                        } else {
                            LOG_WARN("%s: Failed to save crop label to SD: %s\n", __func__, fname_label);
                        }
                    } else {
                        LOG_WARN("%s: Failed to save crop to SD: %s\n", __func__, fname_img);
                    }
                }

                // MQTT and HTTP Export
                if (mqtt_export || http_export) {
                    cJSON* payload = cJSON_CreateObject();
                    cJSON_AddStringToObject(payload, "label", label);
                    cJSON_AddNumberToObject(payload, "timestamp", timestamp);
                    cJSON_AddNumberToObject(payload, "confidence", conf);
                    cJSON_AddNumberToObject(payload, "x", crop_x);
                    cJSON_AddNumberToObject(payload, "y", crop_y);
                    cJSON_AddNumberToObject(payload, "w", crop_w);
                    cJSON_AddNumberToObject(payload, "h", crop_h);
                    cJSON_AddStringToObject(payload, "image", imageDataBase64);
                    if (mqtt_export) {
                        char crop_topic[64];
                        snprintf(crop_topic, sizeof(crop_topic), "crop/%s", ACAP_DEVICE_Prop("serial"));
                        int mqtt_ok = MQTT_Publish_JSON(crop_topic, payload, 0, 0);
                        if (!mqtt_ok) {
                            LOG_WARN("MQTT crop publish failed - message may be too large (JPEG size: %u bytes)\n", jpeg_size);
                            ACAP_STATUS_SetString("output", "mqttCropError", "MQTT crop publish failed - message too large");
                            ACAP_STATUS_SetNumber("output", "mqttCropErrorTime", ACAP_DEVICE_Timestamp());
                        } else {
                            ACAP_STATUS_SetNull("output", "mqttCropError");
                        }
                    }
                    if (http_export) {
                        cJSON_AddStringToObject(payload, "serial", ACAP_DEVICE_Prop("serial"));
                        const char* url = cJSON_GetObjectItem(cropping, "http_url") ?
                                        cJSON_GetObjectItem(cropping, "http_url")->valuestring : NULL;
                        const char* authentication = cJSON_GetObjectItem(cropping, "http_auth") ?
                                                    cJSON_GetObjectItem(cropping, "http_auth")->valuestring : "none";
                        const char* username = cJSON_GetObjectItem(cropping, "http_username") ?
                                            cJSON_GetObjectItem(cropping, "http_username")->valuestring : NULL;
                        const char* password = cJSON_GetObjectItem(cropping, "http_password") ?
                                            cJSON_GetObjectItem(cropping, "http_password")->valuestring : NULL;
                        const char* token = cJSON_GetObjectItem(cropping, "http_token") ?
                                            cJSON_GetObjectItem(cropping, "http_token")->valuestring : NULL;

                        if (url && url[0] != 0) {
                            int http_ok = output_http_post_json(url, payload, authentication, username, password, token);
                            if (!http_ok) {
                                LOG_WARN("HTTP POST failed: %s\n", url);
                                ACAP_STATUS_SetString("output", "httpCropError", "HTTP POST failed");
                                ACAP_STATUS_SetString("output", "httpCropErrorUrl", url);
                                ACAP_STATUS_SetNumber("output", "httpCropErrorTime", ACAP_DEVICE_Timestamp());
                            } else {
                                ACAP_STATUS_SetNull("output", "httpCropError");
                            }
                        } else {
                            LOG_WARN("HTTP export enabled, but URL is not set.\n");
                            ACAP_STATUS_SetString("output", "httpCropError", "HTTP export enabled but URL not configured");
                            ACAP_STATUS_SetNumber("output", "httpCropErrorTime", ACAP_DEVICE_Timestamp());
                        }
                    }
                    cJSON_Delete(payload);
                }
            }
        }
        idx++;
        detection = detection->next;
    }

    // Mark 0 for non-detected labels for rolling buffer
    if (strcmp(prioritize, "accuracy") == 0) {
        for (int i = 0; i < eventsCache_len; ++i) {
            int seen = 0;
            for (int j = 0; j < n_frame_labels; ++j)
                if (strcmp(eventsCache[i].name, frame_labels[j]) == 0) seen = 1;
            if (!seen) {
                eventsCache[i].rolling_head = (eventsCache[i].rolling_head + 1) % window_size;
                eventsCache[i].rolling[eventsCache[i].rolling_head] = 0;
                if (eventsCache[i].rolling_count < window_size) eventsCache[i].rolling_count++;
            }
        }
    }

    LOG_TRACE("%s>\n", __func__);
}

// Reset all state/crop API/eventsCache
void Output_reset(void) {
    LOG_TRACE("<%s\n", __func__);
    eventsCache_len = 0;
    lastDetectionsWereEmpty = 0;
    last_output_time_ms = 0;
    output_crop_cache_reset();
    LOG_TRACE("%s>\n", __func__);
}

// Initialization
void Output_init(void) {
    LOG_TRACE("<%s\n", __func__);
    ACAP_HTTP_Node("crops", output_crop_cache_http_callback);

    cJSON* model = ACAP_Get_Config("model");
    if (!model) {
        LOG_WARN("%s: No Model Config found\n", __func__);
        return;
    }
    cJSON* labels = cJSON_GetObjectItem(model, "labels");
    if (!labels) {
        LOG_WARN("%s: Model has no labels\n", __func__);
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
    output_crop_cache_reset();
    g_timeout_add(200, Output_DeactivateExpired, NULL);

    // Optionally: Cleanup crop cache every 5 minutes
//    g_timeout_add_seconds(300, output_crop_cache_cleanup, NULL);
    LOG_TRACE("%s>\n", __func__);
}
