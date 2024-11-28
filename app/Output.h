/*
 * For custom output upon detections.
 * 
 */
#ifndef OUTPUT_H
#define OUTPUT_H

#include "cJSON.h"

void Output(cJSON* detectionList);
void Output_reset();

#endif
