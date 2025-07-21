#ifndef MODEL_H
#define MODEL_H

#include "imgprovider.h"
#include "larod.h"
#include "vdo-frame.h"
#include "vdo-types.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes and configures the detection model for inference.
 *
 * This function sets up the neural network, allocates all required buffers, 
 * and reads model parameters and configuration. 
 * It must be called before any inference or image processing.
 *
 * @return Pointer to a cJSON object containing model and video configuration data,
 *         or NULL on failure. This object can be used for downstream configuration needs.
 *         Do not free this object; management is handled internally.
 */
cJSON* Model_Setup(void);

/**
 * @brief Perform inference on a captured video frame and return detected objects.
 *
 * This function runs the preprocessing and inference pipeline. It returns a cJSON array
 * of detection objects. Each detection object will include fields such as:
 *   - "label": Detected object class as a string
 *   - "c": Confidence value, 0–1 (or 0–100 as an integer)
 *   - "x", "y", "w", "h": Detection region (relative to input image, float 0–1)
 *   - "timestamp": Epoch milliseconds of detection
 *   - "refId": A unique integer reference for this detection (valid until next inference/reset)
 *
 * @param image  The input image buffer (YUV or RGB). Ownership is not transferred.
 * @return A cJSON array of detection objects. The caller is responsible for freeing this array (cJSON_Delete).
 */
cJSON* Model_Inference(VdoBuffer* image);

/**
 * @brief Clean up and free all model resources and buffers.
 *
 * Call this once on shutdown to properly release all memory and handles used by the model.
 */
void Model_Cleanup(void);

/**
 * @brief Retrieve cropped JPEG data and true pixel bounding box coordinates for a detection.
 *
 * This function allows output modules (SD card, MQTT, or others) to fetch a cropped,
 * optionally border-expanded, JPEG image for a specific detection, **after filtering**.
 * If the operation is not possible (e.g., no image, cropping not active), returns NULL and sets *jpeg_size = 0.
 *
 * All pixel coordinates are **relative to the original source image size**.
 *
 * @param detection  A pointer to a single cJSON detection object (from Model_Inference), MUST contain "refId".
 * @param jpeg_size  Output: Set to the JPEG buffer's length in bytes on success, or 0 on failure.
 * @param out_x      Output: Left pixel coordinate of the actual detected object in the cropped image.
 * @param out_y      Output: Top pixel coordinate of the actual detected object in the cropped image.
 * @param out_w      Output: Width of the detection region in the cropped image, in pixels.
 * @param out_h      Output: Height of the detection region in the cropped image, in pixels.
 *                     (All out_* arguments are output params; can be NULL if not needed.)
 *
 * @return Pointer to an internally managed JPEG buffer if available (do NOT free), or NULL if unavailable.
 *         Buffer is valid until Model_Reset() is called or until the next Model_Inference().
 *
 * Typical usage in output loop:
 *   int x,y,w,h;
 *   unsigned size;
 *   const unsigned char* jpeg = Model_GetImageData(detection, &size, &x, &y, &w, &h);
 *   if (jpeg && size) {
 *       // save or send jpeg; save label, (x, y, w, h) for annotation or re-training
 *   }
 */
const unsigned char* Model_GetImageData(
    const cJSON* detection,
    unsigned* jpeg_size,
    int* out_x,
    int* out_y,
    int* out_w,
    int* out_h,
    int* img_w,
    int* img_h
);

/**
 * @brief Reset/cleanup per-inference buffers used for image crops and JPEG encoding.
 *
 * This MUST be called (typically after Output has finished handling all detections)
 * to guarantee that memory is released and JPEG/crop state is not leaked.
 */
void Model_Reset(void);

#ifdef __cplusplus
}
#endif

#endif // MODEL_H
