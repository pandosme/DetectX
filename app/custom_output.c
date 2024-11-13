/*
 * DetectX will automatically fire events "label" and "counter".
 * When using a custom model it is possible to have custom logic and output
 * upon detections.
 *
 * The detectionList is an array of pre-processed and filtered detections.
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
#include <string.h>
#include <syslog.h>

#include "ACAP.h"

#define LOG(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args);}
#define LOG_WARN(fmt, args...)    { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args);}
//#define LOG_TRACE(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }
#define LOG_TRACE(fmt, args...)    {}


void
custom_output( cJSON* detectionList ) {
	if( cJSON_GetArraySize( detectionList ) == 0 )
		return;
	
	//Get custom settings
	//cJSON* settings = ACAP_Service("settings");
	
/* Remove comments to print the whole JSON structure on 
	char* json = cJSON_PrintUnformatted( detectionList );
	LOG("%s\n",json);
	free(json);
*/

/* Remove comment to Iterate over the array of detections to add custom logic and output
	cJSON* detection = detectionList->child;
	while( detection ) {
		const char* label = cJSON_GetArrayItem( detection,"label")->valuestring;
		int confidence = cJSON_GetArrayItem( detection,"x")->valueint;
		int x = cJSON_GetArrayItem( detection,"x")->valueint;
		int y = cJSON_GetArrayItem( detection,"y")->valueint;
		int w = cJSON_GetArrayItem( detection,"w")->valueint;
		int h = cJSON_GetArrayItem( detection,"h")->valueint;
		if( confidence > 50 ) {
			//Do stuff here
		}
		detection = detection->next;
	}
*/	
}

