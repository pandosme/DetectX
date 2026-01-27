/*
 * Image preprocessing module for DetectX
 * Supports multiple scaling modes: stretch, center-crop, and letterbox
 */

#include "preprocess.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <syslog.h>
#include <unistd.h>

/* Internal context structure */
struct PreprocessContext {
    larodConnection* conn;

    /* Input dimensions */
    unsigned int input_width;
    unsigned int input_height;
    VdoFormat input_format;

    /* Output dimensions (model input) */
    unsigned int output_width;
    unsigned int output_height;
    VdoFormat output_format;

    /* Scale mode */
    PreprocessScaleMode scale_mode;

    /* Larod preprocessing model and tensors */
    larodModel* pp_model;
    larodTensor** pp_input_tensors;
    larodTensor** pp_output_tensors;
    size_t pp_num_inputs;
    size_t pp_num_outputs;
    larodJobRequest* pp_request;
    larodMap* crop_map;

    /* Input buffer */
    int input_fd;
    void* input_addr;
    size_t input_size;

    /* Output buffer */
    int output_fd;
    void* output_addr;
    size_t output_size;

    /* For letterbox: intermediate scaled buffer */
    int letterbox_fd;
    void* letterbox_addr;
    size_t letterbox_size;
    unsigned int letterbox_width;
    unsigned int letterbox_height;
    larodModel* letterbox_model;
    larodTensor** letterbox_input_tensors;
    larodTensor** letterbox_output_tensors;
    size_t letterbox_num_inputs;
    size_t letterbox_num_outputs;
    larodJobRequest* letterbox_request;

    /* Coordinate transformation parameters */
    float scale_x;
    float scale_y;
    float offset_x;
    float offset_y;
};

/* Helper to get format string for larod */
static const char* get_format_string(VdoFormat format) {
    switch (format) {
        case VDO_FORMAT_YUV:
            return "nv12";
        case VDO_FORMAT_RGB:
            return "rgb-interleaved";
        case VDO_FORMAT_PLANAR_RGB:
            return "rgb-planar";
        default:
            return "nv12";
    }
}

/* Calculate bytes per pixel for a format */
static size_t get_bytes_per_pixel(VdoFormat format) {
    switch (format) {
        case VDO_FORMAT_YUV:
            return 1;  /* NV12 is 1.5 bytes per pixel on average */
        case VDO_FORMAT_RGB:
        case VDO_FORMAT_PLANAR_RGB:
            return 3;
        default:
            return 3;
    }
}

/* Calculate buffer size for given dimensions and format */
static size_t calculate_buffer_size(unsigned int width, unsigned int height, VdoFormat format) {
    switch (format) {
        case VDO_FORMAT_YUV:
            return (width * height * 3) / 2;  /* NV12 */
        case VDO_FORMAT_RGB:
        case VDO_FORMAT_PLANAR_RGB:
            return width * height * 3;
        default:
            return width * height * 3;
    }
}

/* Create a memory-mapped temporary file */
static bool create_temp_buffer(size_t size, int* fd, void** addr) {
    char template[] = "/tmp/preprocess-XXXXXX";

    *fd = mkstemp(template);
    if (*fd < 0) {
        syslog(LOG_ERR, "%s: mkstemp failed: %s", __func__, strerror(errno));
        return false;
    }

    /* Unlink immediately so file is deleted when fd is closed */
    unlink(template);

    if (ftruncate(*fd, size) < 0) {
        syslog(LOG_ERR, "%s: ftruncate failed: %s", __func__, strerror(errno));
        close(*fd);
        *fd = -1;
        return false;
    }

    *addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, *fd, 0);
    if (*addr == MAP_FAILED) {
        syslog(LOG_ERR, "%s: mmap failed: %s", __func__, strerror(errno));
        close(*fd);
        *fd = -1;
        *addr = NULL;
        return false;
    }

    return true;
}

/* Create larod preprocessing model */
static larodModel* create_pp_model(
    larodConnection* conn,
    unsigned int in_width,
    unsigned int in_height,
    VdoFormat in_format,
    unsigned int out_width,
    unsigned int out_height,
    VdoFormat out_format,
    larodMap** out_crop_map,
    unsigned int crop_x,
    unsigned int crop_y,
    unsigned int crop_w,
    unsigned int crop_h
) {
    larodError* error = NULL;
    larodModel* model = NULL;

    larodMap* map = larodCreateMap(&error);
    if (!map) {
        syslog(LOG_ERR, "%s: Failed to create larod map: %s", __func__, error->msg);
        larodClearError(&error);
        return NULL;
    }

    /* Set input parameters */
    if (!larodMapSetStr(map, "image.input.format", get_format_string(in_format), &error)) {
        syslog(LOG_ERR, "%s: Failed to set input format: %s", __func__, error->msg);
        goto cleanup;
    }
    if (!larodMapSetIntArr2(map, "image.input.size", in_width, in_height, &error)) {
        syslog(LOG_ERR, "%s: Failed to set input size: %s", __func__, error->msg);
        goto cleanup;
    }

    /* Set output parameters */
    if (!larodMapSetStr(map, "image.output.format", get_format_string(out_format), &error)) {
        syslog(LOG_ERR, "%s: Failed to set output format: %s", __func__, error->msg);
        goto cleanup;
    }
    if (!larodMapSetIntArr2(map, "image.output.size", out_width, out_height, &error)) {
        syslog(LOG_ERR, "%s: Failed to set output size: %s", __func__, error->msg);
        goto cleanup;
    }

    /* Get cpu-proc device for preprocessing */
    const larodDevice* device = larodGetDevice(conn, "cpu-proc", 0, &error);
    if (!device) {
        syslog(LOG_ERR, "%s: Failed to get cpu-proc device: %s", __func__, error->msg);
        goto cleanup;
    }

    /* Load preprocessing model */
    model = larodLoadModel(conn, -1, device, LAROD_ACCESS_PRIVATE, "preprocess", map, &error);
    if (!model) {
        syslog(LOG_ERR, "%s: Failed to load preprocessing model: %s", __func__, error->msg);
        goto cleanup;
    }

    /* Create crop map if cropping is requested */
    if (out_crop_map && crop_w > 0 && crop_h > 0) {
        *out_crop_map = larodCreateMap(&error);
        if (!*out_crop_map) {
            syslog(LOG_ERR, "%s: Failed to create crop map: %s", __func__, error->msg);
            larodDestroyModel(&model);
            model = NULL;
            goto cleanup;
        }
        if (!larodMapSetIntArr4(*out_crop_map, "image.input.crop",
                                 crop_x, crop_y, crop_w, crop_h, &error)) {
            syslog(LOG_ERR, "%s: Failed to set crop parameters: %s", __func__, error->msg);
            larodDestroyMap(out_crop_map);
            larodDestroyModel(&model);
            model = NULL;
            goto cleanup;
        }
    }

cleanup:
    larodDestroyMap(&map);
    larodClearError(&error);
    return model;
}

PreprocessContext* preprocess_create(
    larodConnection* conn,
    unsigned int input_width,
    unsigned int input_height,
    VdoFormat input_format,
    unsigned int output_width,
    unsigned int output_height,
    VdoFormat output_format,
    PreprocessScaleMode scale_mode
) {
    larodError* error = NULL;

    if (!conn) {
        syslog(LOG_ERR, "%s: NULL connection", __func__);
        return NULL;
    }

    PreprocessContext* ctx = calloc(1, sizeof(PreprocessContext));
    if (!ctx) {
        syslog(LOG_ERR, "%s: Failed to allocate context: %s", __func__, strerror(errno));
        return NULL;
    }

    ctx->conn = conn;
    ctx->input_width = input_width;
    ctx->input_height = input_height;
    ctx->input_format = input_format;
    ctx->output_width = output_width;
    ctx->output_height = output_height;
    ctx->output_format = output_format;
    ctx->scale_mode = scale_mode;
    ctx->input_fd = -1;
    ctx->output_fd = -1;
    ctx->letterbox_fd = -1;

    /* Default transform (stretch mode - no adjustment needed) */
    ctx->scale_x = (float)input_width / (float)output_width;
    ctx->scale_y = (float)input_height / (float)output_height;
    ctx->offset_x = 0.0f;
    ctx->offset_y = 0.0f;

    /* Calculate input/output buffer sizes */
    ctx->input_size = calculate_buffer_size(input_width, input_height, input_format);
    ctx->output_size = calculate_buffer_size(output_width, output_height, output_format);

    /* Create output buffer */
    if (!create_temp_buffer(ctx->output_size, &ctx->output_fd, &ctx->output_addr)) {
        syslog(LOG_ERR, "%s: Failed to create output buffer", __func__);
        goto error;
    }

    /* Mode-specific setup */
    unsigned int crop_x = 0, crop_y = 0, crop_w = 0, crop_h = 0;

    switch (scale_mode) {
        case SCALE_MODE_STRETCH:
            /* Simple scaling - input directly to output */
            ctx->pp_model = create_pp_model(
                conn,
                input_width, input_height, input_format,
                output_width, output_height, output_format,
                NULL, 0, 0, 0, 0
            );
            break;

        case SCALE_MODE_CROP: {
            /* Calculate center crop region that matches output aspect ratio */
            float output_ratio = (float)output_width / (float)output_height;
            float crop_w_f = (float)input_width;
            float crop_h_f = crop_w_f / output_ratio;

            if (crop_h_f > (float)input_height) {
                crop_h_f = (float)input_height;
                crop_w_f = crop_h_f * output_ratio;
            }

            crop_w = (unsigned int)crop_w_f;
            crop_h = (unsigned int)crop_h_f;
            crop_x = (input_width - crop_w) / 2;
            crop_y = (input_height - crop_h) / 2;

            /* Transform: model coords map to cropped region of input */
            ctx->scale_x = (float)crop_w / (float)output_width;
            ctx->scale_y = (float)crop_h / (float)output_height;
            ctx->offset_x = (float)crop_x / (float)input_width;
            ctx->offset_y = (float)crop_y / (float)input_height;

            syslog(LOG_INFO, "%s: Crop mode - region (%u,%u) %ux%u from %ux%u",
                   __func__, crop_x, crop_y, crop_w, crop_h, input_width, input_height);

            ctx->pp_model = create_pp_model(
                conn,
                input_width, input_height, input_format,
                output_width, output_height, output_format,
                &ctx->crop_map, crop_x, crop_y, crop_w, crop_h
            );
            break;
        }

        case SCALE_MODE_LETTERBOX: {
            /* Calculate scaled dimensions preserving aspect ratio */
            float input_ratio = (float)input_width / (float)input_height;
            float output_ratio = (float)output_width / (float)output_height;

            if (input_ratio > output_ratio) {
                /* Input is wider - fit to width, pad top/bottom */
                ctx->letterbox_width = output_width;
                ctx->letterbox_height = (unsigned int)((float)output_width / input_ratio);
            } else {
                /* Input is taller - fit to height, pad left/right */
                ctx->letterbox_height = output_height;
                ctx->letterbox_width = (unsigned int)((float)output_height * input_ratio);
            }

            /* Ensure dimensions are even (required for some formats) */
            ctx->letterbox_width = (ctx->letterbox_width / 2) * 2;
            ctx->letterbox_height = (ctx->letterbox_height / 2) * 2;

            unsigned int pad_x = (output_width - ctx->letterbox_width) / 2;
            unsigned int pad_y = (output_height - ctx->letterbox_height) / 2;

            /* Transform: model coords need to account for letterbox padding */
            ctx->scale_x = (float)input_width / (float)ctx->letterbox_width;
            ctx->scale_y = (float)input_height / (float)ctx->letterbox_height;
            ctx->offset_x = -(float)pad_x / (float)output_width;
            ctx->offset_y = -(float)pad_y / (float)output_height;

            syslog(LOG_INFO, "%s: Letterbox mode - scale %ux%u to %ux%u, pad (%u,%u)",
                   __func__, input_width, input_height,
                   ctx->letterbox_width, ctx->letterbox_height, pad_x, pad_y);

            /* Create intermediate buffer for scaled image */
            ctx->letterbox_size = calculate_buffer_size(
                ctx->letterbox_width, ctx->letterbox_height, output_format);

            if (!create_temp_buffer(ctx->letterbox_size, &ctx->letterbox_fd, &ctx->letterbox_addr)) {
                syslog(LOG_ERR, "%s: Failed to create letterbox buffer", __func__);
                goto error;
            }

            /* Create model to scale to letterbox dimensions */
            ctx->letterbox_model = create_pp_model(
                conn,
                input_width, input_height, input_format,
                ctx->letterbox_width, ctx->letterbox_height, output_format,
                NULL, 0, 0, 0, 0
            );

            if (!ctx->letterbox_model) {
                syslog(LOG_ERR, "%s: Failed to create letterbox model", __func__);
                goto error;
            }

            /* Set up letterbox tensors */
            ctx->letterbox_input_tensors = larodAllocModelInputs(
                conn, ctx->letterbox_model, 0, &ctx->letterbox_num_inputs, NULL, &error);
            if (!ctx->letterbox_input_tensors) {
                syslog(LOG_ERR, "%s: Failed to allocate letterbox inputs: %s", __func__, error->msg);
                goto error;
            }

            ctx->letterbox_output_tensors = larodAllocModelOutputs(
                conn, ctx->letterbox_model, 0, &ctx->letterbox_num_outputs, NULL, &error);
            if (!ctx->letterbox_output_tensors) {
                syslog(LOG_ERR, "%s: Failed to allocate letterbox outputs: %s", __func__, error->msg);
                goto error;
            }

            /* We don't create a regular pp_model for letterbox - we handle it specially */
            break;
        }
    }

    /* For non-letterbox modes, set up the standard preprocessing pipeline */
    if (scale_mode != SCALE_MODE_LETTERBOX) {
        if (!ctx->pp_model) {
            syslog(LOG_ERR, "%s: Failed to create preprocessing model", __func__);
            goto error;
        }

        /* Allocate tensors */
        ctx->pp_input_tensors = larodAllocModelInputs(
            conn, ctx->pp_model, 0, &ctx->pp_num_inputs, NULL, &error);
        if (!ctx->pp_input_tensors) {
            syslog(LOG_ERR, "%s: Failed to allocate inputs: %s", __func__, error->msg);
            goto error;
        }

        ctx->pp_output_tensors = larodAllocModelOutputs(
            conn, ctx->pp_model, 0, &ctx->pp_num_outputs, NULL, &error);
        if (!ctx->pp_output_tensors) {
            syslog(LOG_ERR, "%s: Failed to allocate outputs: %s", __func__, error->msg);
            goto error;
        }

        /* Get input tensor fd and map memory */
        ctx->input_fd = larodGetTensorFd(ctx->pp_input_tensors[0], &error);
        if (ctx->input_fd == LAROD_INVALID_FD) {
            syslog(LOG_ERR, "%s: Failed to get input fd: %s", __func__, error->msg);
            goto error;
        }

        size_t tensor_size;
        if (!larodGetTensorFdSize(ctx->pp_input_tensors[0], &tensor_size, &error)) {
            syslog(LOG_ERR, "%s: Failed to get input size: %s", __func__, error->msg);
            goto error;
        }
        ctx->input_size = tensor_size;

        ctx->input_addr = mmap(NULL, ctx->input_size, PROT_READ | PROT_WRITE,
                               MAP_SHARED, ctx->input_fd, 0);
        if (ctx->input_addr == MAP_FAILED) {
            syslog(LOG_ERR, "%s: Failed to map input: %s", __func__, strerror(errno));
            ctx->input_addr = NULL;
            goto error;
        }

        /* Bind output tensor to our output buffer */
        if (!larodSetTensorFd(ctx->pp_output_tensors[0], ctx->output_fd, &error)) {
            syslog(LOG_ERR, "%s: Failed to set output fd: %s", __func__, error->msg);
            goto error;
        }

        /* Create job request */
        ctx->pp_request = larodCreateJobRequest(
            ctx->pp_model,
            ctx->pp_input_tensors, ctx->pp_num_inputs,
            ctx->pp_output_tensors, ctx->pp_num_outputs,
            ctx->crop_map,
            &error
        );
        if (!ctx->pp_request) {
            syslog(LOG_ERR, "%s: Failed to create job request: %s", __func__, error->msg);
            goto error;
        }
    } else {
        /* Letterbox mode - set up input/output bindings for letterbox model */
        ctx->input_fd = larodGetTensorFd(ctx->letterbox_input_tensors[0], &error);
        if (ctx->input_fd == LAROD_INVALID_FD) {
            syslog(LOG_ERR, "%s: Failed to get letterbox input fd: %s", __func__, error->msg);
            goto error;
        }

        size_t tensor_size;
        if (!larodGetTensorFdSize(ctx->letterbox_input_tensors[0], &tensor_size, &error)) {
            syslog(LOG_ERR, "%s: Failed to get letterbox input size: %s", __func__, error->msg);
            goto error;
        }
        ctx->input_size = tensor_size;

        ctx->input_addr = mmap(NULL, ctx->input_size, PROT_READ | PROT_WRITE,
                               MAP_SHARED, ctx->input_fd, 0);
        if (ctx->input_addr == MAP_FAILED) {
            syslog(LOG_ERR, "%s: Failed to map letterbox input: %s", __func__, strerror(errno));
            ctx->input_addr = NULL;
            goto error;
        }

        /* Bind letterbox output to intermediate buffer */
        if (!larodSetTensorFd(ctx->letterbox_output_tensors[0], ctx->letterbox_fd, &error)) {
            syslog(LOG_ERR, "%s: Failed to set letterbox output fd: %s", __func__, error->msg);
            goto error;
        }

        /* Create letterbox job request */
        ctx->letterbox_request = larodCreateJobRequest(
            ctx->letterbox_model,
            ctx->letterbox_input_tensors, ctx->letterbox_num_inputs,
            ctx->letterbox_output_tensors, ctx->letterbox_num_outputs,
            NULL,
            &error
        );
        if (!ctx->letterbox_request) {
            syslog(LOG_ERR, "%s: Failed to create letterbox request: %s", __func__, error->msg);
            goto error;
        }
    }

    syslog(LOG_INFO, "%s: Created preprocessing context, mode=%s, %ux%u -> %ux%u",
           __func__, preprocess_mode_to_string(scale_mode),
           input_width, input_height, output_width, output_height);

    larodClearError(&error);
    return ctx;

error:
    larodClearError(&error);
    preprocess_destroy(ctx);
    return NULL;
}

bool preprocess_run(PreprocessContext* ctx, const void* input_data, size_t input_size) {
    if (!ctx || !input_data) {
        return false;
    }

    larodError* error = NULL;
    static int power_retries = 0;

    /* Copy input data to mapped buffer */
    size_t copy_size = (input_size < ctx->input_size) ? input_size : ctx->input_size;
    memcpy(ctx->input_addr, input_data, copy_size);

    if (ctx->scale_mode == SCALE_MODE_LETTERBOX) {
        /* Letterbox mode: scale to intermediate buffer, then copy centered */

        /* Run scaling job */
        if (!larodRunJob(ctx->conn, ctx->letterbox_request, &error)) {
            if (error->code == LAROD_ERROR_POWER_NOT_AVAILABLE) {
                larodClearError(&error);
                power_retries++;
                if (power_retries > 50) {
                    syslog(LOG_ERR, "%s: Power not available after %d retries",
                           __func__, power_retries);
                    return false;
                }
                usleep(250 * 1000 * power_retries);
                return false;
            }
            syslog(LOG_ERR, "%s: Letterbox job failed: %s", __func__, error->msg);
            larodClearError(&error);
            return false;
        }
        power_retries = 0;

        /* Clear output buffer (black padding) */
        memset(ctx->output_addr, 0, ctx->output_size);

        /* Calculate padding offsets */
        unsigned int pad_x = (ctx->output_width - ctx->letterbox_width) / 2;
        unsigned int pad_y = (ctx->output_height - ctx->letterbox_height) / 2;

        /* Copy scaled image to center of output buffer */
        size_t bpp = get_bytes_per_pixel(ctx->output_format);
        size_t src_stride = ctx->letterbox_width * bpp;
        size_t dst_stride = ctx->output_width * bpp;

        uint8_t* src = (uint8_t*)ctx->letterbox_addr;
        uint8_t* dst = (uint8_t*)ctx->output_addr + (pad_y * dst_stride) + (pad_x * bpp);

        for (unsigned int y = 0; y < ctx->letterbox_height; y++) {
            memcpy(dst, src, src_stride);
            src += src_stride;
            dst += dst_stride;
        }
    } else {
        /* Stretch or Crop mode: run single preprocessing job */
        if (!larodRunJob(ctx->conn, ctx->pp_request, &error)) {
            if (error->code == LAROD_ERROR_POWER_NOT_AVAILABLE) {
                larodClearError(&error);
                power_retries++;
                if (power_retries > 50) {
                    syslog(LOG_ERR, "%s: Power not available after %d retries",
                           __func__, power_retries);
                    return false;
                }
                usleep(250 * 1000 * power_retries);
                return false;
            }
            syslog(LOG_ERR, "%s: Preprocessing job failed: %s", __func__, error->msg);
            larodClearError(&error);
            return false;
        }
        power_retries = 0;
    }

    return true;
}

void* preprocess_get_output(PreprocessContext* ctx) {
    return ctx ? ctx->output_addr : NULL;
}

size_t preprocess_get_output_size(PreprocessContext* ctx) {
    return ctx ? ctx->output_size : 0;
}

int preprocess_get_output_fd(PreprocessContext* ctx) {
    return ctx ? ctx->output_fd : -1;
}

void preprocess_get_transform(
    PreprocessContext* ctx,
    float* scale_x,
    float* scale_y,
    float* offset_x,
    float* offset_y
) {
    if (!ctx) {
        if (scale_x) *scale_x = 1.0f;
        if (scale_y) *scale_y = 1.0f;
        if (offset_x) *offset_x = 0.0f;
        if (offset_y) *offset_y = 0.0f;
        return;
    }

    if (scale_x) *scale_x = ctx->scale_x;
    if (scale_y) *scale_y = ctx->scale_y;
    if (offset_x) *offset_x = ctx->offset_x;
    if (offset_y) *offset_y = ctx->offset_y;
}

bool preprocess_transform_detection(
    PreprocessContext* ctx,
    float* x,
    float* y,
    float* w,
    float* h
) {
    if (!ctx || !x || !y || !w || !h) {
        return false;
    }

    switch (ctx->scale_mode) {
        case SCALE_MODE_STRETCH:
            /*
             * STRETCH: Model input = scaled (possibly distorted) full input image
             * Model coords [0,1] map directly to input image coords [0,1]
             * No transformation needed.
             */
            break;

        case SCALE_MODE_CROP: {
            /*
             * CROP: Model input = center-cropped region scaled to model size
             *
             * Example: Input 1920x1080 (16:9), Model 640x640 (1:1)
             *   Crop region: 1080x1080 centered at offset (420, 0)
             *
             * Transform: model_coord -> input_coord
             *   input_x = (crop_x + model_x * crop_w) / input_width
             *   input_y = (crop_y + model_y * crop_h) / input_height
             */
            float crop_scale_x = (float)ctx->scale_x * ctx->output_width / ctx->input_width;
            float crop_scale_y = (float)ctx->scale_y * ctx->output_height / ctx->input_height;

            *x = ctx->offset_x + (*x) * crop_scale_x;
            *y = ctx->offset_y + (*y) * crop_scale_y;
            *w = (*w) * crop_scale_x;
            *h = (*h) * crop_scale_y;
            break;
        }

        case SCALE_MODE_LETTERBOX: {
            /*
             * LETTERBOX: Model input = scaled image with black padding
             *
             * Example: Input 1920x1080 (16:9), Model 640x640 (1:1)
             *   Letterbox: 640x360 content centered with 140px padding top/bottom
             *
             * Transform: model_coord -> input_coord
             *   1. Subtract padding offset (in normalized model coords)
             *   2. Scale by content_size / model_size
             *
             * Detections in padding region are INVALID (return false)
             */
            float pad_x_norm = (float)(ctx->output_width - ctx->letterbox_width) / 2.0f / ctx->output_width;
            float pad_y_norm = (float)(ctx->output_height - ctx->letterbox_height) / 2.0f / ctx->output_height;
            float content_scale_x = (float)ctx->letterbox_width / ctx->output_width;
            float content_scale_y = (float)ctx->letterbox_height / ctx->output_height;

            /* Check if detection center is within valid content region */
            float center_x = *x + *w / 2.0f;
            float center_y = *y + *h / 2.0f;

            if (center_x < pad_x_norm || center_x > (pad_x_norm + content_scale_x) ||
                center_y < pad_y_norm || center_y > (pad_y_norm + content_scale_y)) {
                /* Detection center is in padding - invalid */
                return false;
            }

            /* Transform to input image coordinates */
            float input_x = (*x - pad_x_norm) / content_scale_x;
            float input_y = (*y - pad_y_norm) / content_scale_y;
            float input_w = *w / content_scale_x;
            float input_h = *h / content_scale_y;

            /* Clamp to valid [0,1] range (detection may extend into padding) */
            if (input_x < 0) {
                input_w += input_x;  /* Reduce width by amount outside */
                input_x = 0;
            }
            if (input_y < 0) {
                input_h += input_y;
                input_y = 0;
            }
            if (input_x + input_w > 1.0f) {
                input_w = 1.0f - input_x;
            }
            if (input_y + input_h > 1.0f) {
                input_h = 1.0f - input_y;
            }

            /* Reject if clamping made the box invalid */
            if (input_w <= 0 || input_h <= 0) {
                return false;
            }

            *x = input_x;
            *y = input_y;
            *w = input_w;
            *h = input_h;
            break;
        }
    }

    return true;
}

void preprocess_destroy(PreprocessContext* ctx) {
    if (!ctx) {
        return;
    }

    /* Clean up larod resources */
    if (ctx->pp_request) {
        larodDestroyJobRequest(&ctx->pp_request);
    }
    if (ctx->letterbox_request) {
        larodDestroyJobRequest(&ctx->letterbox_request);
    }
    if (ctx->crop_map) {
        larodDestroyMap(&ctx->crop_map);
    }

    larodError* error = NULL;
    if (ctx->pp_input_tensors) {
        larodDestroyTensors(ctx->conn, &ctx->pp_input_tensors, ctx->pp_num_inputs, &error);
        larodClearError(&error);
    }
    if (ctx->pp_output_tensors) {
        larodDestroyTensors(ctx->conn, &ctx->pp_output_tensors, ctx->pp_num_outputs, &error);
        larodClearError(&error);
    }
    if (ctx->letterbox_input_tensors) {
        larodDestroyTensors(ctx->conn, &ctx->letterbox_input_tensors, ctx->letterbox_num_inputs, &error);
        larodClearError(&error);
    }
    if (ctx->letterbox_output_tensors) {
        larodDestroyTensors(ctx->conn, &ctx->letterbox_output_tensors, ctx->letterbox_num_outputs, &error);
        larodClearError(&error);
    }

    if (ctx->pp_model) {
        larodDestroyModel(&ctx->pp_model);
    }
    if (ctx->letterbox_model) {
        larodDestroyModel(&ctx->letterbox_model);
    }

    /* Unmap and close buffers */
    if (ctx->input_addr && ctx->input_addr != MAP_FAILED) {
        munmap(ctx->input_addr, ctx->input_size);
    }
    if (ctx->output_addr && ctx->output_addr != MAP_FAILED) {
        munmap(ctx->output_addr, ctx->output_size);
    }
    if (ctx->letterbox_addr && ctx->letterbox_addr != MAP_FAILED) {
        munmap(ctx->letterbox_addr, ctx->letterbox_size);
    }

    /* Close file descriptors - but NOT input_fd as it's owned by larod tensor */
    if (ctx->output_fd >= 0) {
        close(ctx->output_fd);
    }
    if (ctx->letterbox_fd >= 0) {
        close(ctx->letterbox_fd);
    }

    free(ctx);
}

PreprocessScaleMode preprocess_mode_from_string(const char* mode_str) {
    if (!mode_str) {
        return SCALE_MODE_STRETCH;
    }

    if (strcasecmp(mode_str, "crop") == 0 || strcasecmp(mode_str, "center-crop") == 0 ||
        strcasecmp(mode_str, "1") == 0) {
        return SCALE_MODE_CROP;
    }
    if (strcasecmp(mode_str, "letterbox") == 0 || strcasecmp(mode_str, "pad") == 0 ||
        strcasecmp(mode_str, "2") == 0) {
        return SCALE_MODE_LETTERBOX;
    }
    if (strcasecmp(mode_str, "stretch") == 0 || strcasecmp(mode_str, "balanced") == 0 ||
        strcasecmp(mode_str, "0") == 0) {
        return SCALE_MODE_STRETCH;
    }

    /* Default: stretch */
    return SCALE_MODE_STRETCH;
}

const char* preprocess_mode_to_string(PreprocessScaleMode mode) {
    switch (mode) {
        case SCALE_MODE_STRETCH:
            return "stretch";
        case SCALE_MODE_CROP:
            return "crop";
        case SCALE_MODE_LETTERBOX:
            return "letterbox";
        default:
            return "unknown";
    }
}
