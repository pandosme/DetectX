/**
 * @file output_crop_cache.h
 * @brief Ring buffer image crop cache and HTTP API for recent detections.
 *
 * Provides a thread-safe, fixed-size ring buffer to store a history of
 * cropped detection images with metadata, and the HTTP callback to dump this
 * history as a JSON array.
 */

#ifndef OUTPUT_CROP_CACHE_H
#define OUTPUT_CROP_CACHE_H

#include "cJSON.h"
#include "ACAP.h"

/**
 * Number of recent crops to keep in history.
 */
#define CROP_HISTORY_SIZE 10

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Add a crop JPEG and its metadata to the history buffer.
 *
 * The JPEG is base64-encoded for storage.
 * The buffer runs in FIFO, overwriting oldest entries.
 *
 * @param jpeg_data   Pointer to JPEG bytes.
 * @param jpeg_size   Size of jpeg_data buffer.
 * @param label       Category label (string).
 * @param confidence  Detection confidence (0..100).
 * @param x           Top left X coordinate of crop.
 * @param y           Top left Y coordinate of crop.
 * @param w           Width of crop.
 * @param h           Height of crop.
 * @param img_w       Full image width.
 * @param img_h       Full image height.
 * @return Pointer to the (owned) base64 encoded string (may be freed), or NULL on error.
 */
const char* output_crop_cache_add(
    const unsigned char *jpeg_data,
    unsigned jpeg_size,
    const char *label,
    int confidence,
    int x, int y, int w, int h,
    int img_w, int img_h);

/**
 * @brief Reset (clear) the crop cache and free any memory.
 */
void output_crop_cache_reset(void);

/**
 * @brief HTTP GET callback which responds with the latest crop history.
 *        Array is sorted newest-first.
 *
 * @param response   ACAP HTTP response handle.
 * @param request    ACAP HTTP request handle.
 */
void output_crop_cache_http_callback(
    ACAP_HTTP_Response response,
    const ACAP_HTTP_Request request);

#ifdef __cplusplus
}
#endif

#endif // OUTPUT_CROP_CACHE_H
