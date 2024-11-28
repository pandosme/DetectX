/*
 * DetectX will automatically fire label events.
 * When using a custom model it is possible to have custom logic and output
 * upon detections.
 *
 * The detections is an array of pre-processed and filtered detections.
 * JSON Syntax:
 * [
 *		{
 *			"label":"some label",		The label od the detected object
 *			"c":82,						The confidence value between 0-100  
 *			"x":100,					The top left corner [0-1000]
 *			"y":100,					The top left corner [0-1000]
 *			"w":100,					The object width [0-1000] 
 *			"h":100,					The object height [0-1000]
 *			"timestamp":1731531483123	//EPOCH timestam since Jan 1 1970. millisecond resolution
 *		},
 *		....
 *	]
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <syslog.h>

#include "ACAP.h"

#define LOG(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args);}
#define LOG_WARN(fmt, args...)    { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args);}
//#define LOG_TRACE(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }
#define LOG_TRACE(fmt, args...)    {}


cJSON* lastTriggerTime = 0;
cJSON* firstTriggerTime = 0;

void
Output( cJSON* detections ) {
	ACAP_STATUS_SetObject("labels","detections",detections);

	double now = ACAP_DEVICE_Timestamp();
	cJSON* settings = ACAP_Get_Config("settings");
	if(!settings)
		return;
	if( !lastTriggerTime )
		lastTriggerTime = cJSON_CreateObject();
	if( !firstTriggerTime )
		firstTriggerTime = cJSON_CreateObject();	
	
	double minEventDuration = cJSON_GetObjectItem(settings,"minEventDuration")?cJSON_GetObjectItem(settings,"minEventDuration")->valuedouble:3000;
	double stabelizeTransition = cJSON_GetObjectItem(settings,"stabelizeTransition")?cJSON_GetObjectItem(settings,"stabelizeTransition")->valuedouble:0;

	cJSON* detection = detections->child;
	while( detection ) {
		const char* label = cJSON_GetObjectItem(detection,"label")?cJSON_GetObjectItem(detection,"label")->valuestring:"Undefined";

		if( !ACAP_STATUS_Bool("events", label) ) {
			cJSON* transitionStateTime = cJSON_GetObjectItem(firstTriggerTime,label);
			double timestamp = now;
			if( !transitionStateTime)
				cJSON_AddNumberToObject(firstTriggerTime,label, now);
			else
				timestamp = transitionStateTime->valuedouble;
			if( (now - timestamp) >= stabelizeTransition ) {
				LOG_TRACE("%s: Label %s set to high",__func__,label);
				ACAP_EVENTS_Fire_State( label, 1 );
				cJSON_DeleteItemFromObject( firstTriggerTime,label);
			}
 		}

		if( !cJSON_GetObjectItem(lastTriggerTime,label) ) {
			cJSON_AddNumberToObject(lastTriggerTime,label, now);
		} else {
			cJSON_ReplaceItemInObject(lastTriggerTime,label,cJSON_CreateNumber(now));
		}
		detection = detection->next;
	}

	cJSON* lastTrigger = lastTriggerTime->child;
	while( lastTrigger ) {
		if( ACAP_STATUS_Bool("events", lastTrigger->string) ) {
			if( (now - lastTrigger->valuedouble) > minEventDuration ) {
				ACAP_EVENTS_Fire_State( lastTrigger->string , 0 );
				LOG_TRACE("%s: Label %s set to Low",__func__,label);
			}
		}
		lastTrigger = lastTrigger->next;
	}
}

void replace_spaces(char *str) {
    while (*str != '\0') {
        if (*str == ' ') {
            *str = '_';
        }
        str++;
    }
}

void Output_reset() {
	LOG_TRACE("%s:",__func__);
	cJSON* model = ACAP_Get_Config("model");
	if(!model) {
		LOG_WARN("%s: No Model Config found",__func__);
		return;
	}
	cJSON* labels = cJSON_GetObjectItem(model,"labels");
	if(!labels) {
		LOG_WARN("%s: Model has no labels",__func__);
		return;
	}
	cJSON* label = labels->child;
	while( label ) {
		char niceName[32];
		sprintf(niceName,"DetectX: %s", label->valuestring);
		replace_spaces( label->valuestring );
		ACAP_EVENTS_Add_Event( label->valuestring, niceName, 1);
		label = label->next;
	}
	lastTriggerTime = cJSON_CreateObject();
	firstTriggerTime = cJSON_CreateObject();
	LOG_TRACE("%s: Exit",__func__);	
}
