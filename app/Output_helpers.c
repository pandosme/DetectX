/**
 * @file output_helpers.c
 * @brief Implementation of general-purpose helper functions used in Output subsystem.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/stat.h>
#include <errno.h>
#include "Output_helpers.h"

#define SD_FOLDER "/var/spool/storage/SD_DISK/detectx"   ///< Directory for SD card crops

// --- base64 encoder table ---
static const char base64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/**
 * @brief Encode a memory buffer to base64. Allocates a new string.
 */
char* base64_encode(const unsigned char *src, size_t len)
{
    if (!src || len == 0) return NULL;

    size_t olen = 4 * ((len + 2) / 3);     // Output is always a multiple of 4
    char *out = (char*)malloc(olen + 1);
    if (!out) return NULL;
    char *pos = out;

    int val = 0, valb = -6;
    for (size_t i = 0; i < len; ++i) {
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

/**
 * @brief Replace all spaces in a null-terminated string with underscores (modifies in-place).
 */
void replace_spaces(char *str)
{
    if (!str) return;
    while (*str) {
        if (*str == ' ')
            *str = '_';
        ++str;
    }
}

/**
 * @brief Ensure the SD_FOLDER path exists; create if not present.
 */
int ensure_sd_directory(void)
{
    struct stat st = {0};
    if (stat(SD_FOLDER, &st) == -1) {
        if (mkdir(SD_FOLDER, 0755) == -1) {
            syslog(LOG_WARNING, "Failed to create SD directory %s: %s\n", SD_FOLDER, strerror(errno));
            return 0;
        }
    }
    return 1;
}

/**
 * @brief Write binary JPEG data to a given path.
 */
int save_jpeg_to_file(const char* path, const unsigned char* jpeg, unsigned size)
{
    FILE* f = fopen(path, "wb");
    if (!f) {
        syslog(LOG_WARNING, "Failed to open %s for writing JPEG: %s\n", path, strerror(errno));
        return 0;
    }
    size_t written = fwrite(jpeg, 1, size, f);
    fclose(f);
    return (written == size) ? 1 : 0;
}

/**
 * @brief Write label and bounding box data to a plain text file.
 */
int save_label_to_file(const char* path, const char* label, int x, int y, int w, int h)
{
    FILE* f = fopen(path, "w");
    if (!f) {
        syslog(LOG_WARNING, "Failed to open %s for writing label: %s\n", path, strerror(errno));
        return 0;
    }
    fprintf(f, "%s %d %d %d %d\n", label, x, y, w, h);
    fclose(f);
    return 1;
}
