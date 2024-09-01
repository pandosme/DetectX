#ifndef MODEL_H
#define MODEL_H


#include "imgprovider.h"
#include "larod.h"
#include "vdo-frame.h"
#include "vdo-types.h"
#include "cJSON.h"

cJSON* Model_Setup();
cJSON* Model_Inference(VdoBuffer* image);

#endif
