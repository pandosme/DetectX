/*
 * Copyright (C) 2021, Axis Communications AB, Lund, Sweden
 * Licensed under the Apache License, Version 2.0 (the "License");
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *     https://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "imgutils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <jpeglib.h>

/* Logging macros */
#define LOG(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }
#define LOG_WARN(fmt, args...) { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args); }
//#define LOG_TRACE(fmt, args...) { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }
#define LOG_TRACE(fmt, args...) {}
/**
 * @brief Encode an image buffer as JPEG and store it in memory.
 *
 * @param image_buffer An image buffer with interleaved (RGB or grayscale) channel layout
 * @param jpeg_conf A struct defining how the image is to be encoded
 * @param jpeg_size The output size of the JPEG
 * @param jpeg_buffer The output buffer of the JPEG
 */
void buffer_to_jpeg(unsigned char* image_buffer,
                    struct jpeg_compress_struct* jpeg_conf,
                    unsigned long* jpeg_size,
                    unsigned char** jpeg_buffer)
{
    struct jpeg_error_mgr jerr;
    JSAMPROW row_pointer[1];
    jpeg_conf->err = jpeg_std_error(&jerr);

    jpeg_mem_dest(jpeg_conf, jpeg_buffer, jpeg_size);
    jpeg_start_compress(jpeg_conf, TRUE);

    int stride = jpeg_conf->image_width * jpeg_conf->input_components;
    while (jpeg_conf->next_scanline < jpeg_conf->image_height) {
        row_pointer[0] = &image_buffer[jpeg_conf->next_scanline * stride];
        jpeg_write_scanlines(jpeg_conf, row_pointer, 1);
    }

    jpeg_finish_compress(jpeg_conf);
    // jpeg_destroy_compress(jpeg_conf);  // Not needed here. Should be done by caller when appropriate.
}

/**
 * @brief Inserts common values into a JPEG configuration struct.
 *
 * @param width The width of the image
 * @param height The height of the image
 * @param channels The number of channels of the image
 * @param quality The desired JPEG quality (0-100)
 * @param jpeg_conf The JPEG configuration struct to modify
 */
void set_jpeg_configuration(int width,
                            int height,
                            int channels,
                            int quality,
                            struct jpeg_compress_struct* jpeg_conf)
{
    jpeg_create_compress(jpeg_conf);
    jpeg_conf->image_width = width;
    jpeg_conf->image_height = height;
    jpeg_conf->input_components = channels;
    if (channels == 1) {
        jpeg_conf->in_color_space = JCS_GRAYSCALE;
    } else if (channels == 3) {
        jpeg_conf->in_color_space = JCS_RGB;
    } else {
        printf("set_jpeg_configuration: Number of channels not supported: %d\n", channels);
        exit(1);
    }
    jpeg_set_defaults(jpeg_conf);
    jpeg_set_quality(jpeg_conf, quality, TRUE);
}

/**
 * @brief Writes a memory buffer to a file.
 *
 * @param file_name The desired path of the output file
 * @param buffer The data to be written
 * @param buffer_size The size of the data to be written
 */
void jpeg_to_file(char* file_name, unsigned char* buffer, unsigned long buffer_size)
{
    printf("jpeg_to_file: %s size: %ld\n", file_name, buffer_size);
    FILE* fp;
    if ((fp = fopen(file_name, "wb")) == NULL) {
        printf("Unable to open file!\n");
        return;
    }
    fwrite(buffer, sizeof(unsigned char), buffer_size, fp);
    fclose(fp);
}

/**
 * @brief Crops a rectangular patch from an image buffer. The image channels are expected to be interleaved.
 *
 * @param image_buffer A buffer holding an uint8 image
 * @param image_w The input image's width in pixels
 * @param image_h The input image's height in pixels
 * @param channels The input image's number of channels
 * @param crop_x The leftmost pixel coordinate of the desired crop
 * @param crop_y The top pixel coordinate of the desired crop
 * @param crop_w The width of the desired crop in pixels
 * @param crop_h The height of the desired crop in pixels
 * @return Pointer to new cropped malloc'ed buffer, or NULL on error
 */
unsigned char* crop_interleaved(unsigned char* image_buffer,
                                int image_w, int image_h, int channels,
                                int crop_x, int crop_y, int crop_w, int crop_h)
{
    LOG_TRACE("<%s: image=(%d,%d,%d) crop=(%d,%d,%d,%d)\n", __func__,
              image_w, image_h, channels, crop_x, crop_y, crop_w, crop_h);

    // Defensive checks
    if (!image_buffer || image_w <= 0 || image_h <= 0 || channels <= 0 ||
        crop_x < 0 || crop_y < 0 || crop_w <= 0 || crop_h <= 0 ||
        crop_x + crop_w > image_w ||
        crop_y + crop_h > image_h) {
        LOG_WARN("crop_interleaved: Invalid crop (%d,%d,%d,%d) for image size (%d,%d,%d)\n",
                 crop_x, crop_y, crop_w, crop_h, image_w, image_h, channels);
        return NULL;
    }

    unsigned char* crop_buffer = (unsigned char*)malloc(crop_w * crop_h * channels);
    if (!crop_buffer) {
        LOG_WARN("crop_interleaved: Memory allocation failed\n");
        return NULL;
    }

    int image_buffer_width = image_w * channels;
    int crop_buffer_width = crop_w * channels;

    for (int row = crop_y; row < crop_y + crop_h; row++) {
        int image_buffer_pos = image_buffer_width * row + crop_x * channels;
        int crop_buffer_pos = crop_buffer_width * (row - crop_y);
        memcpy(crop_buffer + crop_buffer_pos, image_buffer + image_buffer_pos, crop_buffer_width);
    }

    LOG_TRACE("%s>: Cropped buffer allocated %zu bytes\n", __func__, (size_t)crop_w*crop_h*channels);
    return crop_buffer;
}

/**
 * @brief Example/test utility: Generates an image, crops it, encodes to JPEG and saves to file.
 * This is not used in production flow but can be called for developer validation.
 */
void test_buffer_to_jpeg_file(void)
{
    int width = 1920;
    int height = 1080;
    int channels = 3;
    unsigned char* image_buffer = (unsigned char*)malloc(width * height * channels);

    // Fill with yellow top-bottom gradient
    for (int i = 0; i < width * height; i++) {
        for (int channel = 0; channel < channels; channel++) {
            int green_mask = 1;
            if (channel == 2) green_mask = 0;
            image_buffer[i * channels + channel] = (double)i / (width * height) * 255 * green_mask;
        }
    }

    int crop_x = width - 100;
    int crop_y = 0;
    int crop_w = 100;
    int crop_h = height;
    unsigned char* crop_buffer =
        crop_interleaved(image_buffer, width, height, channels, crop_x, crop_y, crop_w, crop_h);

    // Encode crop to JPEG in memory
    unsigned long jpeg_size = 0;
    unsigned char* jpeg_buffer = NULL;
    struct jpeg_compress_struct jpeg_conf;
    set_jpeg_configuration(crop_w, crop_h, channels, 80, &jpeg_conf);
    buffer_to_jpeg(crop_buffer, &jpeg_conf, &jpeg_size, &jpeg_buffer);

    jpeg_to_file("/tmp/test.jpg", jpeg_buffer, jpeg_size);

    free(image_buffer);
    free(crop_buffer);
    free(jpeg_buffer);
}
