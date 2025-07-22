/**
 * @file output_helpers.h
 * @brief Helper functions for Output subsystem (filesystem, strings, base64 encoding).
 */

#ifndef OUTPUT_HELPERS_H
#define OUTPUT_HELPERS_H

#include <stddef.h>  // for size_t

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Replace all spaces in a string with underscores (in-place).
 *
 * @param str String to modify.
 */
void replace_spaces(char *str);

/**
 * @brief Ensures the SD output directory exists. Creates it if necessary.
 *        You may define SD_FOLDER elsewhere if needed.
 *
 * @return 1 if directory exists or was created, 0 on error.
 */
int ensure_sd_directory(void);

/**
 * @brief Write a JPEG buffer to disk.
 *
 * @param path Filename (including full path) to write to.
 * @param jpeg Pointer to JPEG buffer.
 * @param size Length of buffer in bytes.
 * @return 1 on success, 0 on failure.
 */
int save_jpeg_to_file(const char* path, const unsigned char* jpeg, unsigned size);

/**
 * @brief Write label/class and bounding box coordinates to a text file in YOLOv5 format.
 *
 * @param path  Target path.
 * @param label Label string.
 * @param x     X coordinate (top left).
 * @param y     Y coordinate (top left).
 * @param w     Width.
 * @param h     Height.
 * @return 1 on success, 0 on failure.
 */
int save_label_to_file(const char* path, const char* label, int x, int y, int w, int h);

/**
 * @brief Encode a binary buffer in base64.
 *
 * @param src Pointer to input buffer.
 * @param len Input length in bytes.
 * @return New malloc'ed NUL-terminated string, or NULL on allocation error.
 *         Caller is responsible for freeing the returned pointer.
 */
char* base64_encode(const unsigned char *src, size_t len);

#ifdef __cplusplus
}
#endif

#endif // OUTPUT_HELPERS_H
