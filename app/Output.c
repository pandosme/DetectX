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
#include "Storage.h"

#define LOG(fmt, args...)      { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args);}
#define LOG_WARN(fmt, args...) { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args);}
#define LOG_TRACE(fmt, args...) {}

#define MAX_LABELS 32
#define MAX_HISTORY 64   // frames with hands kept for the event vote
#define ANY_DETECTION_EVENT_ID "event"

typedef struct {
    char name[64];
    int state; // 0=LOW, 1=HIGH
    double last_detect_time;
} LabelEventState;

/* Event vote ("prioritize"). Every frame with at least one detection is recorded with
 * the set of labels seen in it. In "balanced" and "accuracy" a label's event goes HIGH
 * only when, over the last window_ms:
 *   - it is in the current frame,
 *   - it was seen in at least min_hits frames,
 *   - it was seen in at least min_share of the frames with detections, and
 *   - no other label was seen more often (a tie is allowed, so two objects with
 *     different labels can both fire).
 * An object entering the scene or changing pose is often misclassified for a frame or
 * two before its label settles; those transitional labels fail the share and lead tests.
 * "speed" fires on the first frame. Defaults per mode below; settings "eventLogic"
 * {window, frames, share} overrides them. The window is stretched when frames arrive
 * too slowly to fit min_hits in it. */
typedef struct {
    double t;
    unsigned int seen;   // bit i = eventsCache[i] seen in this frame
} FrameRecord;

static FrameRecord frameHistory[MAX_HISTORY];
static int frameHistoryHead = 0;    // next write position
static int frameHistoryCount = 0;
static double lastFrameTime = 0;
static double frameInterval = 0;    // smoothed time between processed frames, ms

static LabelEventState eventsCache[MAX_LABELS];
static int eventsCache_len = 0;
static int lastDetectionsWereEmpty = 0;
static double last_output_time_ms = 0;
static double last_sd_output_time_ms = 0;
static int    sd_any_event_was_active = 0; /* tracks transition: no events → first event */

static void update_any_detection_event(void) {
    int any_event_active = 0;
    for (int i = 0; i < eventsCache_len; ++i) {
        if (eventsCache[i].state == 1) {
            any_event_active = 1;
            break;
        }
    }
    ACAP_EVENTS_Fire_State(ANY_DETECTION_EVENT_ID, any_event_active);
}

// Helper: manage per-label state
static LabelEventState* find_or_create_label_state(const char* label) {
    for (int i = 0; i < eventsCache_len; ++i)
        if (strcmp(eventsCache[i].name, label) == 0)
            return &eventsCache[i];
    if (eventsCache_len < MAX_LABELS) {
        strncpy(eventsCache[eventsCache_len].name, label, 63);
        eventsCache[eventsCache_len].name[63] = 0;
        eventsCache[eventsCache_len].state = 0;
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
    update_any_detection_event();
    return TRUE;
}

static void fire_event_high(LabelEventState* evt, cJSON* detection, double now) {
    char topic[256];
    evt->state = 1;
    evt->last_detect_time = now;
    ACAP_EVENTS_Fire_State(evt->name, 1);
    snprintf(topic, sizeof(topic), "event/%s/%s/true", ACAP_DEVICE_Prop("serial"), evt->name);
    cJSON* eventPayload = cJSON_Duplicate(detection, 1);
    cJSON_AddTrueToObject(eventPayload, "state");
    MQTT_Publish_JSON(topic, eventPayload, 0, 0);
    cJSON_Delete(eventPayload);
}

void Output(cJSON* detections, int modelWidth, int modelHeight) {
    {   // frame rate, for stretching the vote window on slow cameras
        double t = ACAP_DEVICE_Timestamp();
        if (lastFrameTime > 0) {
            double dt = t - lastFrameTime;
            if (dt > 0 && dt < 5000)
                frameInterval = frameInterval > 0 ? 0.8 * frameInterval + 0.2 * dt : dt;
        }
        lastFrameTime = t;
    }
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
    int mqtt_export     = cropping && cJSON_IsTrue(cJSON_GetObjectItem(cropping, "mqtt"));
    int http_export     = cropping && cJSON_IsTrue(cJSON_GetObjectItem(cropping, "http"));
    int throttle        = cJSON_GetObjectItem(cropping, "throttle") ?
                         cJSON_GetObjectItem(cropping, "throttle")->valueint : 500;

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

    int speed_mode = strcmp(prioritize, "speed") == 0;
    int balanced_mode = strcmp(prioritize, "balanced") == 0;
    double window_ms = balanced_mode ? 300 : 500;
    int min_hits = balanced_mode ? 2 : 3;
    double min_share = balanced_mode ? 0.75 : 0.6;   // balanced: ~3 frames in a row at 9 fps
    cJSON* logic = cJSON_GetObjectItem(settings, "eventLogic");
    if (logic) {
        cJSON* item;
        if ((item = cJSON_GetObjectItem(logic, "frames")) && cJSON_IsNumber(item)) min_hits = item->valueint;
        if ((item = cJSON_GetObjectItem(logic, "window")) && cJSON_IsNumber(item)) window_ms = item->valuedouble;
        if ((item = cJSON_GetObjectItem(logic, "share")) && cJSON_IsNumber(item)) min_share = item->valuedouble;
    }
    if (min_hits < 1) min_hits = 1;
    // Slow camera: make room for min_hits frames (plus some jitter) in the window.
    if (frameInterval > 0 && window_ms < frameInterval * (min_hits - 0.5))
        window_ms = frameInterval * (min_hits - 0.5);
    unsigned int frame_seen = 0;
    cJSON* frame_detection[MAX_LABELS] = {0};   // highest-confidence detection per label this frame

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

        LabelEventState* evt = find_or_create_label_state(label);
        if (!evt) { idx++; detection = detection->next; continue; }

        if (speed_mode) {
            if (evt->state == 0)
                fire_event_high(evt, detection, now);
        } else {
            int li = (int)(evt - eventsCache);
            frame_seen |= 1u << li;
            if (!frame_detection[li] || conf > cJSON_GetObjectItem(frame_detection[li], "c")->valueint)
                frame_detection[li] = detection;
        }
        // An active event stays HIGH while its label keeps being seen.
        evt->last_detect_time = now;

        // Cropping output path (MQTT / HTTP only — SD card uses full-frame export below)
        // Cut a crop only when the throttle lets one out: at full resolution each
        // crop is an NV12->RGB conversion plus a JPEG encode.
        if (cropping_active && ACAP_DEVICE_Timestamp() - last_output_time_ms > throttle) {
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
                    // Frame the crop was cut from (full sensor resolution, or the
                    // inference frame), so x/y/w/h can be read as sensor pixels.
                    unsigned int frame_w = 0, frame_h = 0;
                    Model_GetCropFrameSize(&frame_w, &frame_h);
                    cJSON_AddNumberToObject(payload, "frame_w", frame_w);
                    cJSON_AddNumberToObject(payload, "frame_h", frame_h);
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

    // Event vote (balanced / accuracy), see FrameRecord above.
    if (!speed_mode && frame_seen) {
        frameHistory[frameHistoryHead].t = now;
        frameHistory[frameHistoryHead].seen = frame_seen;
        frameHistoryHead = (frameHistoryHead + 1) % MAX_HISTORY;
        if (frameHistoryCount < MAX_HISTORY) frameHistoryCount++;

        int frames = 0;
        int hits[MAX_LABELS] = {0};
        for (int k = 0; k < frameHistoryCount; ++k) {
            const FrameRecord* r = &frameHistory[(frameHistoryHead - 1 - k + MAX_HISTORY) % MAX_HISTORY];
            if (now - r->t > window_ms)
                break;
            frames++;
            for (int i = 0; i < eventsCache_len; ++i)
                if (r->seen & (1u << i)) hits[i]++;
        }
        for (int i = 0; i < eventsCache_len; ++i) {
            if (!(frame_seen & (1u << i)) || eventsCache[i].state == 1 || !frame_detection[i])
                continue;
            int leads = 1;
            for (int j = 0; j < eventsCache_len; ++j)
                if (j != i && hits[j] > hits[i]) leads = 0;
            if (hits[i] >= min_hits && hits[i] >= min_share * frames && leads)
                fire_event_high(&eventsCache[i], frame_detection[i], now);
        }
    }

    update_any_detection_event();

    // --- SD Capture: event-driven full-frame capture ---
    // Reads sd_capture settings; triggers on first active event, then at configured interval.
    cJSON* sd_cfg    = cJSON_GetObjectItem(settings, "sd_capture");
    int    sd_enable = sd_cfg && cJSON_IsTrue(cJSON_GetObjectItem(sd_cfg, "enabled"));
    int    sd_interval_sec = (sd_cfg && cJSON_GetObjectItem(sd_cfg, "interval")) ?
                             cJSON_GetObjectItem(sd_cfg, "interval")->valueint : 2;
    int    sd_interval_ms  = sd_interval_sec * 1000;
    int    max_images      = 2000;

    // Check if any label event is currently HIGH
    int any_event_active = 0;
    for (int i = 0; i < eventsCache_len; ++i)
        if (eventsCache[i].state == 1) { any_event_active = 1; break; }

    if (!any_event_active) {
        // All events went low — reset timer so next activation captures immediately
        last_sd_output_time_ms = 0;
        sd_any_event_was_active = 0;
    } else if (sd_enable && !sd_is_busy()) {
        int img_count = sd_count_images();
        if (img_count < max_images && ensure_sd_images_directory() && ensure_sd_labels_directory()) {
            double now_ts = ACAP_DEVICE_Timestamp();
            int first_activation = !sd_any_event_was_active;
            if (first_activation || (now_ts - last_sd_output_time_ms >= sd_interval_ms)) {
                last_sd_output_time_ms = now_ts;
                sd_any_event_was_active = 1;
                unsigned full_jpeg_size = 0;
                unsigned char* full_jpeg = Model_GetFullFrameJPEG(&full_jpeg_size);
                const char* sd_base = Storage_GetPath();
                if (full_jpeg && full_jpeg_size > 0 && sd_base) {
                    char fname_img[512], fname_lbl[512];
                    snprintf(fname_img, sizeof(fname_img), "%s/images/%.0f.jpg", sd_base, now_ts);
                    snprintf(fname_lbl, sizeof(fname_lbl), "%s/labels/%.0f.txt", sd_base, now_ts);
                    if (save_jpeg_to_file(fname_img, full_jpeg, full_jpeg_size)) {
                        if (!save_yolo_labels_to_file(fname_lbl, detections, modelWidth, modelHeight))
                            LOG_WARN("%s: Failed to save YOLO labels: %s\n", __func__, fname_lbl);
                    } else {
                        LOG_WARN("%s: Failed to save full frame: %s\n", __func__, fname_img);
                    }
                    free(full_jpeg);
                    // Update stored image count in status
                    ACAP_STATUS_SetNumber("sd_capture", "count", img_count + 1);
                    if (img_count + 1 >= max_images)
                        LOG_WARN("%s: SD capture reached max %d images\n", __func__, max_images);
                } else {
                    LOG_WARN("%s: Full frame JPEG unavailable\n", __func__);
                }
            }
        }
    } else if (!sd_enable) {
        sd_any_event_was_active = 0;
        last_sd_output_time_ms  = 0;
    }

    LOG_TRACE("%s>\n", __func__);
}

// Reset all state/crop API/eventsCache
void Output_reset(void) {
    LOG_TRACE("<%s\n", __func__);
    eventsCache_len = 0;
    lastDetectionsWereEmpty = 0;
    last_output_time_ms = 0;
    last_sd_output_time_ms = 0;
    sd_any_event_was_active = 0;
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
    ACAP_EVENTS_Add_Event(ANY_DETECTION_EVENT_ID, "DetectX: Any detection", 1);
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

    // Initialize SD capture image count in status from disk
    int initial_count = sd_count_images();
    ACAP_STATUS_SetNumber("sd_capture", "count", initial_count);

    // Optionally: Cleanup crop cache every 5 minutes
//    g_timeout_add_seconds(300, output_crop_cache_cleanup, NULL);
    LOG_TRACE("%s>\n", __func__);
}
