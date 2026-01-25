/*
 * Label file parser for DetectX
 * Reads labels.txt at runtime instead of embedding in model.json
 */

#ifndef LABELPARSE_H
#define LABELPARSE_H

#include <stddef.h>
#include <stdbool.h>

/**
 * Parse a labels file into an array of string pointers.
 * Each line in the file becomes one label, indexed by line number (0-based).
 *
 * @param labels_path   Path to labels.txt file
 * @param labels        Output: array of label strings
 * @param label_buffer  Output: raw file buffer (needed for cleanup)
 * @param num_labels    Output: number of labels loaded
 * @return true on success, false on failure
 */
bool labels_parse_file(const char* labels_path,
                       char*** labels,
                       char** label_buffer,
                       size_t* num_labels);

/**
 * Get a label by class index with fallback.
 *
 * @param labels      Label array from labels_parse_file
 * @param num_labels  Number of labels in array
 * @param class_id    Class index to look up
 * @return Label string, or "class_N" if not found
 */
const char* labels_get(char** labels, size_t num_labels, int class_id);

/**
 * Free labels allocated by labels_parse_file
 *
 * @param labels       Label array to free
 * @param label_buffer Raw buffer to free
 */
void labels_free(char** labels, char* label_buffer);

#endif /* LABELPARSE_H */
