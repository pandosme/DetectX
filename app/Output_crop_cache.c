/**
 * @file output_crop_cache.c
 * @brief Implementation of detection crop ring buffer and HTTP callback API.
 */

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "Output_crop_cache.h"
#include "Output_helpers.h"

/**
 * @brief An entry in the FIFO crop cache.
 */
typedef struct {
    char *base64_image;        ///< malloc'ed, may be NULL if uninitialized.
    char label[64];            ///< Category name
    int confidence;            ///< Confidence 0..100
    int x, y, w, h;            ///< Crop rectangle
} CropEntry;

// Crop cache ring buffer and synchronization
static CropEntry crop_history[CROP_HISTORY_SIZE];
static int crop_history_head = 0;        // index to write next
static int crop_history_count = 0;       // number of valid entries (<= CROP_HISTORY_SIZE)
static pthread_mutex_t crop_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

const char*
output_crop_cache_add(
    const unsigned char *jpeg_data,
    unsigned jpeg_size,
    const char *label,
    int confidence,
    int x, int y, int w, int h)
{
    // Defensive: null input
    if (!jpeg_data || jpeg_size == 0 || !label) return NULL;
    char *b64img = base64_encode(jpeg_data, jpeg_size);
    if (!b64img) return NULL;

    pthread_mutex_lock(&crop_cache_mutex);

    CropEntry *entry = &crop_history[crop_history_head];

    if (entry->base64_image) {
        free(entry->base64_image);
        entry->base64_image = NULL;
    }
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

    return b64img;
}

void output_crop_cache_reset(void)
{
    pthread_mutex_lock(&crop_cache_mutex);

    for (int i = 0; i < CROP_HISTORY_SIZE; ++i) {
        if (crop_history[i].base64_image) {
            free(crop_history[i].base64_image);
            crop_history[i].base64_image = NULL;
        }
        crop_history[i].label[0] = '\0';
        crop_history[i].confidence = 0;
        crop_history[i].x = crop_history[i].y = crop_history[i].w = crop_history[i].h = 0;
    }
    crop_history_head = 0;
    crop_history_count = 0;

    pthread_mutex_unlock(&crop_cache_mutex);
}

void output_crop_cache_http_callback(
    ACAP_HTTP_Response response,
    const ACAP_HTTP_Request request)
{
    if (strcmp(ACAP_HTTP_Get_Method(request), "GET") != 0) {
        ACAP_HTTP_Respond_Error(response, 405, "Method Not Allowed");
        return;
    }

    cJSON *arr = cJSON_CreateArray();

    pthread_mutex_lock(&crop_cache_mutex);

    // Show most recent to least recent
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
