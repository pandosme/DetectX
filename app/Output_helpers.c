/**
 * @file output_helpers.c
 * @brief Implementation of general-purpose helper functions used in Output subsystem.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include "Output_helpers.h"
#include "Model.h"
#include "cJSON.h"

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
 * Returns 0 silently if the SD card is not mounted (base path absent).
 * Returns 0 with a warning if the card is mounted but mkdir fails.
 */
int ensure_sd_directory(void)
{
    // Check SD card mount point exists — if not, card is simply not inserted
    struct stat sd_root = {0};
    if (stat("/var/spool/storage/SD_DISK", &sd_root) == -1) {
        return 0;  // SD card not mounted; no warning, caller silently disables
    }

    struct stat st = {0};
    if (stat(SD_FOLDER, &st) == -1) {
        if (mkdir(SD_FOLDER, 0755) == -1) {
            syslog(LOG_WARNING, "Failed to create SD directory %s: %s\n", SD_FOLDER, strerror(errno));
            return 0;
        }
    }
    return 1;
}

int ensure_sd_images_directory(void)
{
    if (!ensure_sd_directory()) return 0;
    char path[256];
    snprintf(path, sizeof(path), "%s/images", SD_FOLDER);
    struct stat st = {0};
    if (stat(path, &st) == -1) {
        if (mkdir(path, 0755) == -1) {
            syslog(LOG_WARNING, "Failed to create SD images directory %s: %s\n", path, strerror(errno));
            return 0;
        }
    }
    return 1;
}

int ensure_sd_labels_directory(void)
{
    if (!ensure_sd_directory()) return 0;
    char path[256];
    snprintf(path, sizeof(path), "%s/labels", SD_FOLDER);
    struct stat st = {0};
    if (stat(path, &st) == -1) {
        if (mkdir(path, 0755) == -1) {
            syslog(LOG_WARNING, "Failed to create SD labels directory %s: %s\n", path, strerror(errno));
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

int save_yolo_labels_to_file(const char* path, const cJSON* detections, int modelWidth, int modelHeight)
{
    if (!path || !detections || modelWidth <= 0 || modelHeight <= 0) return 0;
    FILE* f = fopen(path, "w");
    if (!f) {
        syslog(LOG_WARNING, "Failed to open %s for writing YOLO labels: %s\n", path, strerror(errno));
        return 0;
    }
    const cJSON* det = detections->child;
    while (det) {
        const cJSON* labelObj = cJSON_GetObjectItem(det, "label");
        const cJSON* xObj    = cJSON_GetObjectItem(det, "x");
        const cJSON* yObj    = cJSON_GetObjectItem(det, "y");
        const cJSON* wObj    = cJSON_GetObjectItem(det, "w");
        const cJSON* hObj    = cJSON_GetObjectItem(det, "h");
        if (labelObj && cJSON_IsString(labelObj) &&
            xObj && cJSON_IsNumber(xObj) &&
            yObj && cJSON_IsNumber(yObj) &&
            wObj && cJSON_IsNumber(wObj) &&
            hObj && cJSON_IsNumber(hObj)) {
            int class_id = Model_GetLabelIndex(labelObj->valuestring);
            double x = xObj->valuedouble;
            double y = yObj->valuedouble;
            double w = wObj->valuedouble;
            double h = hObj->valuedouble;
            double cx = (x + w * 0.5) / modelWidth;
            double cy = (y + h * 0.5) / modelHeight;
            double nw = w / modelWidth;
            double nh = h / modelHeight;
            fprintf(f, "%d %.6f %.6f %.6f %.6f\n", class_id, cx, cy, nw, nh);
        }
        det = det->next;
    }
    fclose(f);
    return 1;
}

/* ---------------------------------------------------------------
 * SD Capture helpers: count, clear, zip, busy flag
 * --------------------------------------------------------------- */
static volatile int sd_busy_flag = 0;

void sd_set_busy(int busy) { sd_busy_flag = busy ? 1 : 0; }
int  sd_is_busy(void)      { return sd_busy_flag; }

int sd_count_images(void)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/images", SD_FOLDER);
    DIR* d = opendir(path);
    if (!d) return 0;
    int count = 0;
    struct dirent* entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] != '.')
            count++;
    }
    closedir(d);
    return count;
}

static int delete_dir_contents(const char* dirpath)
{
    DIR* d = opendir(dirpath);
    if (!d) return 1;  /* not an error if dir doesn't exist */
    struct dirent* entry;
    char fpath[512];
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        snprintf(fpath, sizeof(fpath), "%s/%s", dirpath, entry->d_name);
        if (remove(fpath) != 0)
            syslog(LOG_WARNING, "sd_clear: failed to remove %s: %s\n", fpath, strerror(errno));
    }
    closedir(d);
    return 1;
}

int sd_clear_directories(void)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/images", SD_FOLDER);
    delete_dir_contents(path);
    snprintf(path, sizeof(path), "%s/labels", SD_FOLDER);
    delete_dir_contents(path);
    return 1;
}

int sd_create_zip(const char* output_path)
{
    if (!output_path) return 0;
    remove(output_path);  /* Remove any stale file first */

    /* Build command: cd into SD_FOLDER, then zip images/ and labels/ */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "cd \"%s\" && zip -q -r \"%s\" images labels 2>/dev/null",
        SD_FOLDER, output_path);

    int ret = system(cmd);
    if (ret != 0) {
        /* zip not available — fall back to tar+gzip */
        snprintf(cmd, sizeof(cmd),
            "cd \"%s\" && tar czf \"%s\" images labels 2>/dev/null",
            SD_FOLDER, output_path);
        ret = system(cmd);
    }
    return (ret == 0) ? 1 : 0;
}
