/**
 * @file output.h
 * @brief Output control for detections/results, crop API, and status/event states.
 *
 * Provides Output(), Output_init(), and Output_reset() entry points for the
 * core application. Handles exporting output to MQTT, SD card, HTTP API,
 * and maintains transient state for event activation/deactivation.
 */

#ifndef OUTPUT_H
#define OUTPUT_H

#include "cJSON.h"

/**
 * @brief Processes detections and exports as configured (MQTT, SD, HTTP, crop cache).
 *
 * @param detectionList cJSON array of detected objects (from model).
 */
void Output(cJSON* detectionList);

/**
 * @brief Resets all output and event/transient state (crop cache, timers).
 */
void Output_reset(void);

/**
 * @brief Registers HTTP endpoint for crop API and sets up event state labels.
 *
 * Must be called early during application initialization.
 */
void Output_init(void);

#endif // OUTPUT_H
