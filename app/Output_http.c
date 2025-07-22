/**
 * @file output_http.c
 * @brief Implementation of HTTP POST export for detection JSON payloads.
 */


#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include "Output_http.h"

int output_http_post_json(
    const char* url,
    cJSON* payload,
    const char* authentication,
    const char* username,
    const char* password,
    const char* token
)
{
    if (!url || !payload)
        return 0;

    CURL *curl = curl_easy_init();
    if (!curl) {
        syslog(LOG_WARNING, "output_http_post_json: CURL initialization failed");
        return 0;
    }
    int ok = 0;
    struct curl_slist *headers = NULL;
    char *payload_str = NULL;

    // Set content-type
    headers = curl_slist_append(headers, "Content-Type: application/json");

    // Handle authentication
    if (authentication) {
        if (strcmp(authentication, "basic") == 0 && username && password) {
            curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
            char userpwd[256];
            snprintf(userpwd, sizeof(userpwd), "%s:%s", username, password);
            curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd);
        } else if (strcmp(authentication, "digest") == 0 && username && password) {
            curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);
            char userpwd[256];
            snprintf(userpwd, sizeof(userpwd), "%s:%s", username, password);
            curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd);
        } else if (strcmp(authentication, "bearer") == 0 && token) {
            char bearer_header[384];
            snprintf(bearer_header, sizeof(bearer_header), "Authorization: Bearer %s", token);
            headers = curl_slist_append(headers, bearer_header);
        }
        // else ("none" or missing) => do nothing extra
    }

    // Prepare JSON
    payload_str = cJSON_PrintUnformatted(payload);
    if (!payload_str) {
        syslog(LOG_WARNING, "output_http_post_json: Unable to encode JSON payload");
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return 0;
    }

    // Set CURL options
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload_str);

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        syslog(LOG_WARNING, "output_http_post_json: HTTP POST failed: %s", curl_easy_strerror(res));
    } else {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code >= 200 && http_code < 300) {
            ok = 1;
        } else {
            syslog(LOG_WARNING, "output_http_post_json: HTTP POST returned status %ld", http_code);
        }
    }

    if (payload_str) free(payload_str);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ok;
}
