/*
 * For custom output upon detections, including HTTP crop API.
 * 
 */
#ifndef OUTPUT_H
#define OUTPUT_H

#include "cJSON.h"

// Output detections, SD card, MQTT, HTTP crop cache etc.
void Output(cJSON* detectionList);

// Reset output state and crop cache
void Output_reset();

// Register HTTP endpoint for crop API; call once during startup
void Output_init(void);

#endif // OUTPUT_H
