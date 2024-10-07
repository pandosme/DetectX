/*------------------------------------------------------------------
 *  Fred Juhlin (2024)
 *------------------------------------------------------------------*/
 
#ifndef _ACAP_H_
#define _ACAP_H_

#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <glib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <unistd.h>
#include <ctype.h>
#include <gio/gio.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <axsdk/axevent.h>

#include "cJSON.h"

#ifdef  __cplusplus
extern "C" {
#endif

/*-----------------------------------------------------
	ACAP
  -----------------------------------------------------*/

typedef void (*ACAP_Config_Update) ( const char *service, cJSON* data);

/**
 * @brief Initialize the ACAP framework
 * @param package The package name
 * @param updateCallback Callback function for configuration updates
 * @return JSON object containing the settings
 */
cJSON* ACAP(const char *package, ACAP_Config_Update updateCallback);

/**
 * @brief Get the ACAP package name
 * @return The package name as a string
 */
const char* ACAP_Package();

/**
 * @brief Get the ACAP application name
 * @return The application name as a string
 */
const char* ACAP_Name();

/**
 * @brief Get a specific ACAP service
 * @param service The name of the service
 * @return JSON object representing the service
 */
cJSON* ACAP_Service(const char* service);

/**
 * @brief Register a new ACAP service
 * @param service The name of the service
 * @param serviceSettings JSON object containing the service settings
 * @return 1 if successful, 0 otherwise
 */
int ACAP_Register(const char* service, cJSON* serviceSettings);

/**
 * @brief Initialize the ACAP file system
 * @return 1 if successful, 0 otherwise
 */
int ACAP_FILE_Init();

/**
 * @brief Get the ACAP application path
 * @return The application path as a string
 */
const char* ACAP_FILE_AppPath();

/**
 * @brief Open a file in the ACAP file system
 * @param filepath The path to the file
 * @param mode The file open mode
 * @return FILE pointer if successful, NULL otherwise
 */
FILE* ACAP_FILE_Open(const char *filepath, const char *mode);

/**
 * @brief Delete a file from the ACAP file system
 * @param filepath The path to the file
 * @return 1 if successful, 0 otherwise
 */
int ACAP_FILE_Delete(const char *filepath);

/**
 * @brief Read a JSON file from the ACAP file system
 * @param filepath The path to the file
 * @return JSON object if successful, NULL otherwise
 */
cJSON* ACAP_FILE_Read(const char *filepath);

/**
 * @brief Write a JSON object to a file in the ACAP file system
 * @param filepath The path to the file
 * @param object The JSON object to write
 * @return 1 if successful, 0 otherwise
 */
int ACAP_FILE_Write(const char *filepath, cJSON* object);

/**
 * @brief Write raw data to a file in the ACAP file system
 * @param filepath The path to the file
 * @param data The data to write
 * @return 1 if successful, 0 otherwise
 */
int ACAP_FILE_WriteData(const char *filepath, const char *data);

/**
 * @brief Check if a file exists in the ACAP file system
 * @param filepath The path to the file
 * @return 1 if the file exists, 0 otherwise
 */
int ACAP_FILE_Exists(const char *filepath);

/*-----------------------------------------------------
	DEVICE
  -----------------------------------------------------*/

/**
 * @brief Initialize the ACAP device
 * @return JSON object containing device information
 */
cJSON* ACAP_DEVICE();

/**
 * @brief Get a device property
 * @param name The name of the property
 * @return The property value as a string
 */
const char* ACAP_DEVICE_Prop(const char *name);

/**
 * @brief Get an integer device property
 * @param name The name of the property
 * @return The property value as an integer
 */
int ACAP_DEVICE_Prop_Int(const char *name);

/**
 * @brief Get a device property as a JSON object
 * @param name The name of the property
 * @return JSON object representing the property
 */
cJSON* ACAP_DEVICE_JSON(const char *name);

/**
 * @brief Get the number of seconds since midnight
 * @return The number of seconds
 */
int ACAP_DEVICE_Seconds_Since_Midnight();

/**
 * @brief Get the current timestamp in milliseconds
 * @return The timestamp as a double
 */
double ACAP_DEVICE_Timestamp();

/**
 * @brief Get the local time as a string
 * @return The local time in format "YYYY-MM-DD HH:MM:SS"
 */
const char* ACAP_DEVICE_Local_Time();

/**
 * @brief Get the current time in ISO format
 * @return The time in format "YYYY-MM-DDTHH:MM:SS+0000"
 */
const char* ACAP_DEVICE_ISOTime();

/**
 * @brief Get the current date
 * @return The date in format "YYYY-MM-DD"
 */
const char* ACAP_DEVICE_Date();

/**
 * @brief Get the current time
 * @return The time in format "HH:MM:SS"
 */
const char* ACAP_DEVICE_Time();

/**
 * @brief Get the device uptime
 * @return The uptime in seconds
 */
double ACAP_DEVICE_Uptime();

/**
 * @brief Get the average CPU usage
 * @return The CPU usage as a percentage
 */
double ACAP_DEVICE_CPU_Average();

/**
 * @brief Get the average network usage
 * @return The network usage in Kbps
 */
double ACAP_DEVICE_Network_Average();

/*-----------------------------------------------------
	HTTP
-------------------------------------------------------*/

typedef GDataOutputStream *ACAP_HTTP_Response;
typedef GHashTable *ACAP_HTTP_Request;

typedef void (*ACAP_HTTP_Callback) (const ACAP_HTTP_Response response, const ACAP_HTTP_Request request);

/**
 * @brief Initialize the HTTP server
 */
void ACAP_HTTP();

/**
 * @brief Close the HTTP server
 */
void ACAP_HTTP_Close();

/**
 * @brief Register an HTTP endpoint
 * @param nodename The name of the endpoint
 * @param callback The callback function to handle requests
 * @return 1 if successful, 0 otherwise
 */
int ACAP_HTTP_Node(const char *nodename, ACAP_HTTP_Callback callback);

/**
 * @brief Get a parameter from an HTTP request
 * @param request The HTTP request
 * @param param The name of the parameter
 * @return The parameter value as a string
 */
const char* ACAP_HTTP_Request_Param(const ACAP_HTTP_Request request, const char *param);

/**
 * @brief Get a JSON parameter from an HTTP request
 * @param request The HTTP request
 * @param param The name of the parameter
 * @return JSON object representing the parameter
 */
cJSON* ACAP_HTTP_Request_JSON(const ACAP_HTTP_Request request, const char *param);

/**
 * @brief Set the HTTP response header for XML content
 * @param response The HTTP response
 * @return 1 if successful, 0 otherwise
 */
int ACAP_HTTP_Header_XML(ACAP_HTTP_Response response);

/**
 * @brief Set the HTTP response header for JSON content
 * @param response The HTTP response
 * @return 1 if successful, 0 otherwise
 */
int ACAP_HTTP_Header_JSON(ACAP_HTTP_Response response);

/**
 * @brief Set the HTTP response header for plain text content
 * @param response The HTTP response
 * @return 1 if successful, 0 otherwise
 */
int ACAP_HTTP_Header_TEXT(ACAP_HTTP_Response response);

/**
 * @brief Set the HTTP response header for file download
 * @param response The HTTP response
 * @param filename The name of the file
 * @param contenttype The content type of the file
 * @param filelength The length of the file in bytes
 * @return 1 if successful, 0 otherwise
 */
int ACAP_HTTP_Header_FILE(ACAP_HTTP_Response response, const char *filename, const char *contenttype, unsigned filelength);

/**
 * @brief Send a formatted string as an HTTP response
 * @param response The HTTP response
 * @param fmt The format string
 * @param ... Additional arguments for the format string
 * @return 1 if successful, 0 otherwise
 */
int ACAP_HTTP_Respond_String(ACAP_HTTP_Response response, const char *fmt, ...);

/**
 * @brief Send a JSON object as an HTTP response
 * @param response The HTTP response
 * @param object The JSON object to send
 * @return 1 if successful, 0 otherwise
 */
int ACAP_HTTP_Respond_JSON(ACAP_HTTP_Response response, cJSON *object);

/**
 * @brief Send raw data as an HTTP response
 * @param response The HTTP response
 * @param count The number of bytes to send
 * @param data The data to send
 * @return 1 if successful, 0 otherwise
 */
int ACAP_HTTP_Respond_Data(ACAP_HTTP_Response response, size_t count, void *data);

/**
 * @brief Send an HTTP error response
 * @param response The HTTP response
 * @param code The HTTP status code
 * @param message The error message
 * @return 1 if successful, 0 otherwise
 */
int ACAP_HTTP_Respond_Error(ACAP_HTTP_Response response, int code, const char *message);

/**
 * @brief Send a plain text HTTP response
 * @param response The HTTP response
 * @param message The text message to send
 * @return 1 if successful, 0 otherwise
 */
int ACAP_HTTP_Respond_Text(ACAP_HTTP_Response response, const char *message);

/*-----------------------------------------------------
	STATUS
-------------------------------------------------------*/

/**
 * @brief Initialize the status container
 * @return JSON object representing the status container
 */
cJSON* ACAP_STATUS();

/**
 * @brief Get a status group
 * @param name The name of the status group
 * @return JSON object representing the status group
 */
cJSON* ACAP_STATUS_Group(const char *name);

/**
 * @brief Get a boolean status value
 * @param group The name of the status group
 * @param name The name of the status item
 * @return The boolean value (1 for true, 0 for false)
 */
int ACAP_STATUS_Bool(const char *group, const char *name);

/**
 * @brief Get an integer status value
 * @param group The name of the status group
 * @param name The name of the status item
 * @return The integer value
 */
int ACAP_STATUS_Int(const char *group, const char *name);

/**
 * @brief Get a double status value
 * @param group The name of the status group
 * @param name The name of the status item
 * @return The double value
 */
double ACAP_STATUS_Double(const char *group, const char *name);

/**
 * @brief Get a string status value
 * @param group The name of the status group
 * @param name The name of the status item
 * @return The string value
 */
char* ACAP_STATUS_String(const char *group, const char *name);

/**
 * @brief Get a JSON object status value
 * @param group The name of the status group
 * @param name The name of the status item
 * @return JSON object representing the status value
 */
cJSON* ACAP_STATUS_Object(const char *group, const char *name);

/**
 * @brief Set a boolean status value
 * @param group The name of the status group
 * @param name The name of the status item
 * @param state The boolean value to set
 */
void ACAP_STATUS_SetBool(const char *group, const char *name, int state);

/**
 * @brief Set a numeric status value
 * @param group The name of the status group
 * @param name The name of the status item
 * @param value The numeric value to set
 */
void ACAP_STATUS_SetNumber(const char *group, const char *name, double value);

/**
 * @brief Set a string status value
 * @param group The name of the status group
 * @param name The name of the status item
 * @param string The string value to set
 */
void ACAP_STATUS_SetString(const char *group, const char *name, const char *string);

/**
 * @brief Set a JSON object status value
 * @param group The name of the status group
 * @param name The name of the status item
 * @param data The JSON object to set
 */
void ACAP_STATUS_SetObject(const char *group, const char *name, cJSON* data);

/**
 * @brief Set a null status value
 * @param group The name of the status group
 * @param name The name of the status item
 */
void ACAP_STATUS_SetNull(const char *group, const char *name);

typedef void (*ACAP_EVENTS_Callback) (cJSON *event);

/**
 * @brief Initialize the events system
 * @return JSON object representing the events configuration
 */
cJSON* ACAP_EVENTS();

/**
 * @brief Add a new event
 * @param Id The event ID
 * @param NiceName The human-readable name for the event
 * @param state The initial state of the event
 * @return 1 if successful, 0 otherwise
 */
int ACAP_EVENTS_Add_Event(const char* Id, const char* NiceName, int state);

/**
 * @brief Add a new event using a JSON configuration
 * @param event JSON object containing the event configuration
 * @return The event declaration ID if successful, 0 otherwise
 */
int ACAP_EVENTS_Add_Event_JSON(cJSON* event);

/**
 * @brief Remove an event
 * @param Id The event ID to remove
 * @return 1 if successful, 0 otherwise
 */
int ACAP_EVENTS_Remove_Event(const char* Id);

/**
 * @brief Fire a stateful event
 * @param Id The event ID
 * @param value The new state value
 * @return 1 if successful, 0 otherwise
 */
int ACAP_EVENTS_Fire_State(const char* Id, int value);

/**
 * @brief Fire a stateless event
 * @param Id The event ID
 * @return 1 if successful, 0 otherwise
 */
int ACAP_EVENTS_Fire(const char* Id);

/**
 * @brief Fire an event with custom JSON data
 * @param Id The event ID
 *
