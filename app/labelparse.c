/*
 * Label file parser for DetectX
 * Reads labels.txt at runtime instead of embedding in model.json
 */

#include "labelparse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <syslog.h>

#define MAX_LABEL_FILE_SIZE (1024 * 1024)  /* 1MB max */
#define MAX_LABEL_LENGTH 60

/* Static fallback buffer for unknown labels */
static char fallback_label[32];

bool labels_parse_file(const char* labels_path,
                       char*** labels,
                       char** label_buffer,
                       size_t* num_labels) {
    struct stat file_stats;
    char* buffer = NULL;
    char** label_array = NULL;

    if (!labels_path || !labels || !label_buffer || !num_labels) {
        syslog(LOG_ERR, "%s: Invalid parameters", __func__);
        return false;
    }

    *labels = NULL;
    *label_buffer = NULL;
    *num_labels = 0;

    /* Get file size */
    if (stat(labels_path, &file_stats) < 0) {
        syslog(LOG_WARNING, "%s: Cannot stat labels file %s: %s",
               __func__, labels_path, strerror(errno));
        return false;
    }

    size_t file_size = (size_t)file_stats.st_size;
    if (file_size == 0) {
        syslog(LOG_WARNING, "%s: Labels file is empty", __func__);
        return false;
    }
    if (file_size > MAX_LABEL_FILE_SIZE) {
        syslog(LOG_WARNING, "%s: Labels file too large: %zu bytes", __func__, file_size);
        return false;
    }

    /* Open and read file */
    int fd = open(labels_path, O_RDONLY);
    if (fd < 0) {
        syslog(LOG_WARNING, "%s: Cannot open labels file %s: %s",
               __func__, labels_path, strerror(errno));
        return false;
    }

    buffer = malloc(file_size + 1);
    if (!buffer) {
        syslog(LOG_ERR, "%s: Failed to allocate buffer: %s", __func__, strerror(errno));
        close(fd);
        return false;
    }

    ssize_t total_read = 0;
    while (total_read < (ssize_t)file_size) {
        ssize_t bytes_read = read(fd, buffer + total_read, file_size - total_read);
        if (bytes_read < 0) {
            syslog(LOG_ERR, "%s: Failed to read labels file: %s", __func__, strerror(errno));
            free(buffer);
            close(fd);
            return false;
        }
        if (bytes_read == 0) {
            break;  /* EOF */
        }
        total_read += bytes_read;
    }
    close(fd);
    buffer[total_read] = '\0';

    /* Count lines */
    size_t line_count = 0;
    for (size_t i = 0; i < (size_t)total_read; i++) {
        if (buffer[i] == '\n') {
            line_count++;
        }
    }
    /* Account for last line without trailing newline */
    if (total_read > 0 && buffer[total_read - 1] != '\n') {
        line_count++;
    }

    if (line_count == 0) {
        syslog(LOG_WARNING, "%s: No labels found in file", __func__);
        free(buffer);
        return false;
    }

    /* Allocate label pointer array */
    label_array = malloc(line_count * sizeof(char*));
    if (!label_array) {
        syslog(LOG_ERR, "%s: Failed to allocate label array: %s", __func__, strerror(errno));
        free(buffer);
        return false;
    }

    /* Parse lines - replace newlines with null terminators */
    size_t label_idx = 0;
    label_array[0] = buffer;

    for (size_t i = 0; i < (size_t)total_read; i++) {
        if (buffer[i] == '\n') {
            buffer[i] = '\0';
            /* Trim carriage return if present (Windows line endings) */
            if (i > 0 && buffer[i - 1] == '\r') {
                buffer[i - 1] = '\0';
            }
            /* Point to next label if not at end */
            if (i + 1 < (size_t)total_read) {
                label_idx++;
                if (label_idx < line_count) {
                    label_array[label_idx] = &buffer[i + 1];
                }
            }
        }
    }

    /* Truncate overly long labels */
    for (size_t i = 0; i < line_count; i++) {
        size_t len = strlen(label_array[i]);
        if (len > MAX_LABEL_LENGTH) {
            label_array[i][MAX_LABEL_LENGTH] = '\0';
        }
    }

    *labels = label_array;
    *label_buffer = buffer;
    *num_labels = line_count;

    syslog(LOG_INFO, "%s: Loaded %zu labels from %s", __func__, line_count, labels_path);
    return true;
}

const char* labels_get(char** labels, size_t num_labels, int class_id) {
    if (labels && class_id >= 0 && (size_t)class_id < num_labels) {
        return labels[class_id];
    }
    /* Fallback: return class index as string */
    snprintf(fallback_label, sizeof(fallback_label), "class_%d", class_id);
    return fallback_label;
}

void labels_free(char** labels, char* label_buffer) {
    if (labels) {
        free(labels);
    }
    if (label_buffer) {
        free(label_buffer);
    }
}
