/**
 * @file output_http.h
 * @brief HTTP export helper for crop detection JSON payloads.
 *
 * Provides an interface for exporting detections (crops) via HTTP POST,
 * supporting basic, digest, and bearer authentication.
 */

#ifndef OUTPUT_HTTP_H
#define OUTPUT_HTTP_H

#include "cJSON.h"

/**
 * @brief Export a detection crop as a HTTP POST request with authentication.
 *
 * @param url            Target endpoint.
 * @param payload        JSON payload object (ownership retained by caller).
 * @param authentication Authentication mode ("none", "basic", "digest", "bearer").
 * @param username       Username for basic/digest auth (may be NULL).
 * @param password       Password for basic/digest auth (may be NULL).
 * @param token          Bearer token (may be NULL).
 *
 * @return 1 on success (HTTP 2xx), 0 on failure (network, curl, or HTTP error).
 */
int output_http_post_json(
    const char* url,
    cJSON* payload,
    const char* authentication,
    const char* username,
    const char* password,
    const char* token
);

#endif // OUTPUT_HTTP_H
