/*
 * Image preprocessing module for DetectX
 * Supports multiple scaling modes: stretch, center-crop, and letterbox
 */

#ifndef PREPROCESS_H
#define PREPROCESS_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "larod.h"
#include "vdo-types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Scaling mode enumeration */
typedef enum {
    SCALE_MODE_STRETCH = 0,    /* Scale to fit model input (may distort aspect ratio) */
    SCALE_MODE_CROP = 1,       /* Center-crop to model aspect ratio (loses edges) */
    SCALE_MODE_LETTERBOX = 2   /* Scale preserving aspect ratio with black padding */
} PreprocessScaleMode;

/* Preprocessing context */
typedef struct PreprocessContext PreprocessContext;

/**
 * @brief Create a preprocessing context
 *
 * @param conn           Active larod connection
 * @param input_width    Width of input frames from VDO
 * @param input_height   Height of input frames from VDO
 * @param input_format   Format of input frames (VDO_FORMAT_YUV, etc.)
 * @param output_width   Width required by model
 * @param output_height  Height required by model
 * @param output_format  Format required by model (VDO_FORMAT_RGB, etc.)
 * @param scale_mode     How to handle aspect ratio mismatch
 * @return Context pointer on success, NULL on failure
 */
PreprocessContext* preprocess_create(
    larodConnection* conn,
    unsigned int input_width,
    unsigned int input_height,
    VdoFormat input_format,
    unsigned int output_width,
    unsigned int output_height,
    VdoFormat output_format,
    PreprocessScaleMode scale_mode
);

/**
 * @brief Run preprocessing on an input buffer
 *
 * @param ctx           Preprocessing context
 * @param input_data    Pointer to input image data
 * @param input_size    Size of input data in bytes
 * @return true on success, false on failure (check errno/syslog)
 */
bool preprocess_run(PreprocessContext* ctx, const void* input_data, size_t input_size);

/**
 * @brief Get pointer to preprocessed output data
 *
 * @param ctx   Preprocessing context
 * @return Pointer to output buffer (valid until next preprocess_run or destroy)
 */
void* preprocess_get_output(PreprocessContext* ctx);

/**
 * @brief Get the output buffer size
 *
 * @param ctx   Preprocessing context
 * @return Size of output buffer in bytes
 */
size_t preprocess_get_output_size(PreprocessContext* ctx);

/**
 * @brief Get the file descriptor for the output buffer (for larod tensor binding)
 *
 * @param ctx   Preprocessing context
 * @return File descriptor, or -1 if not available
 */
int preprocess_get_output_fd(PreprocessContext* ctx);

/**
 * @brief Get scaling information for coordinate transformation
 *
 * For LETTERBOX mode, detections need coordinate adjustment to account for padding.
 * For CROP mode, detections need offset adjustment for the cropped region.
 *
 * @param ctx       Preprocessing context
 * @param scale_x   Output: X scale factor (model coords * scale_x = input coords)
 * @param scale_y   Output: Y scale factor (model coords * scale_y = input coords)
 * @param offset_x  Output: X offset in input image coordinates
 * @param offset_y  Output: Y offset in input image coordinates
 */
void preprocess_get_transform(
    PreprocessContext* ctx,
    float* scale_x,
    float* scale_y,
    float* offset_x,
    float* offset_y
);

/**
 * @brief Transform detection coordinates from model space to input image space
 *
 * Transforms bounding box coordinates from the model's coordinate system
 * back to the original input image coordinate system. The transformation
 * depends on the scale mode:
 *
 * - STRETCH: Direct 1:1 mapping (no change)
 * - CROP: Adds crop offset and scales to crop region
 * - LETTERBOX: Removes padding offset and scales to content region
 *
 * For LETTERBOX mode, detections with centers in the padding region are
 * considered invalid and the function returns false.
 *
 * @param ctx   Preprocessing context
 * @param x     Detection X (0-1 normalized), modified in place
 * @param y     Detection Y (0-1 normalized), modified in place
 * @param w     Detection width (0-1 normalized), modified in place
 * @param h     Detection height (0-1 normalized), modified in place
 * @return true if detection is valid, false if in padding region (letterbox)
 */
bool preprocess_transform_detection(
    PreprocessContext* ctx,
    float* x,
    float* y,
    float* w,
    float* h
);

/**
 * @brief Destroy preprocessing context and free resources
 *
 * @param ctx   Context to destroy (can be NULL)
 */
void preprocess_destroy(PreprocessContext* ctx);

/**
 * @brief Get scale mode from string
 *
 * @param mode_str  String: "stretch", "crop", or "letterbox"
 * @return Scale mode enum value, defaults to SCALE_MODE_STRETCH for unknown
 */
PreprocessScaleMode preprocess_mode_from_string(const char* mode_str);

/**
 * @brief Get string name for scale mode
 *
 * @param mode  Scale mode enum
 * @return Static string name
 */
const char* preprocess_mode_to_string(PreprocessScaleMode mode);

#ifdef __cplusplus
}
#endif

#endif /* PREPROCESS_H */
