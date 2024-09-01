/*------------------------------------------------------------------
 *  Fred Juhlin (2024)
 *------------------------------------------------------------------*/

#include <stdio.h>
#include <stdlib.h>

#include "ACAP.h"
#include <axsdk/axparameter.h>
#include <axsdk/axhttp.h>

#define LOG(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args);}
#define LOG_WARN(fmt, args...)    { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args);}
//#define LOG_TRACE(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }
#define LOG_TRACE(fmt, args...)    {}

cJSON* app = 0;
char ACAP_package_name[30];
ACAP_Config_Update	ACAP_UpdateCallback = 0;


static void
ACAP_ENDPOINT_app(const ACAP_HTTP_Response response,const ACAP_HTTP_Request request) {
	ACAP_HTTP_Respond_JSON( response, app );
}	

static void
ACAP_ENDPOINT_settings(const ACAP_HTTP_Response response,const ACAP_HTTP_Request request) {
	
	const char* json = ACAP_HTTP_Request_Param( request, "json");
	if( !json )
		json = ACAP_HTTP_Request_Param( request, "set");
	if( !json ) {
		ACAP_HTTP_Respond_JSON( response, app );
		return;
	}

	cJSON *params = cJSON_Parse(json);
	if(!params) {
		ACAP_HTTP_Respond_Error( response, 400, "Invalid JSON data");
		return;
	}
	LOG_TRACE("%s: %s\n",__func__,json);
	cJSON* settings = cJSON_GetObjectItem(app,"settings");
	cJSON* param = params->child;
	while(param) {
		if( cJSON_GetObjectItem(settings,param->string ) )
			cJSON_ReplaceItemInObject(settings,param->string,cJSON_Duplicate(param,1) );
		param = param->next;
	}
	cJSON_Delete(params);
	ACAP_FILE_Write( "localdata/settings.json", settings);	
	ACAP_HTTP_Respond_Text( response, "OK" );
	if( ACAP_UpdateCallback )
		ACAP_UpdateCallback("settings",settings);
}


cJSON*
ACAP( const char *package, ACAP_Config_Update callback ) {
	LOG_TRACE("%s:\n",__func__);
	sprintf(ACAP_package_name,"%s",package);
	ACAP_FILE_Init();
	ACAP_HTTP();
	cJSON* device = ACAP_DEVICE();

	ACAP_UpdateCallback = callback;
	
	app = cJSON_CreateObject();

	cJSON_AddItemToObject(app, "manifest", ACAP_FILE_Read( "manifest.json" ));


	cJSON* settings = ACAP_FILE_Read( "html/config/settings.json" );
	if(!settings)
		settings = cJSON_CreateObject();

	cJSON* savedSettings = ACAP_FILE_Read( "localdata/settings.json" );
	if( savedSettings ) {
		cJSON* prop = savedSettings->child;
		while(prop) {
			if( cJSON_GetObjectItem(settings,prop->string ) )
				cJSON_ReplaceItemInObject(settings,prop->string,cJSON_Duplicate(prop,1) );
			prop = prop->next;
		}
		cJSON_Delete(savedSettings);
	}

	if( !cJSON_GetObjectItem( settings,"deviceName") )
		cJSON_AddStringToObject( settings,"deviceName", ACAP_DEVICE_Prop("serial") );
	if( cJSON_GetObjectItem( settings,"deviceName")->type != cJSON_String )
		cJSON_ReplaceItemInObject(settings,"deviceName", cJSON_CreateString( ACAP_DEVICE_Prop("serial") ) );
	
	if( !cJSON_GetObjectItem( settings,"deviceLocation") )
		cJSON_AddStringToObject( settings,"deviceLocation", ACAP_DEVICE_Prop("IPv4") );
	if( cJSON_GetObjectItem( settings,"deviceLocation")->type != cJSON_String )
		cJSON_ReplaceItemInObject(settings,"deviceLocation", cJSON_CreateString( ACAP_DEVICE_Prop("IPv4") ) );

	
	cJSON_AddItemToObject(app, "settings",settings);


	ACAP_Register("status", ACAP_STATUS());
	ACAP_Register("device", device);

	
	ACAP_HTTP_Node( "app", ACAP_ENDPOINT_app );	
	ACAP_HTTP_Node( "settings", ACAP_ENDPOINT_settings );

	if( ACAP_UpdateCallback )
		ACAP_UpdateCallback("settings",settings);

	LOG_TRACE("%s:Exit\n",__func__);
	
	return settings;
}


const char* ACAP_Package() {
	cJSON* manifest = cJSON_GetObjectItem(app,"manifest");
	if(!manifest) {
		LOG_WARN("%s: invalid manifest\n",__func__);
		return "Undefined";
	}
	cJSON* acapPackageConf = cJSON_GetObjectItem(manifest,"acapPackageConf");
	if(!acapPackageConf) {
		LOG_WARN("%s: invalid acapPackageConf\n",__func__);
		return "Undefined";
	}
	cJSON* setup = cJSON_GetObjectItem(acapPackageConf,"setup");
	if(!setup) {
		LOG_WARN("%s: invalid setup\n",__func__);
		return "Undefined";
	}
	return cJSON_GetObjectItem(setup,"appName")->valuestring;
}
	
const char* ACAP_Name() {
	cJSON* manifest = cJSON_GetObjectItem(app,"manifest");
	if(!manifest) {
		LOG_WARN("%s: invalid manifest\n",__func__);
		return "Undefined";
	}
	cJSON* acapPackageConf = cJSON_GetObjectItem(manifest,"acapPackageConf");
	if(!acapPackageConf) {
		LOG_WARN("%s: invalid acapPackageConf\n",__func__);
		return "Undefined";
	}
	cJSON* setup = cJSON_GetObjectItem(acapPackageConf,"setup");
	if(!setup) {
		LOG_WARN("%s: invalid setup\n",__func__);
		return "Undefined";
	}
	return cJSON_GetObjectItem(setup,"friendlyName")->valuestring;
}

cJSON*
ACAP_Service(const char* service) {
	cJSON* reqestedService = cJSON_GetObjectItem(app, service );
	if( !reqestedService ) {
		LOG_WARN("%s: %s is undefined\n",__func__,service);
		return 0;
	}
	return reqestedService;
}

int
ACAP_Register(const char* service, cJSON* serviceSettings ) {
	LOG_TRACE("%s: %s\n",__func__,service);
	if( cJSON_GetObjectItem(app,service) ) {
		LOG_TRACE("%s: %s already registered\n",__func__);
		return 1;
	}
	cJSON_AddItemToObject( app, service, serviceSettings );
	return 1;
}


char ACAP_FILE_Path[128] = "";

const char*
ACAP_FILE_AppPath() {
	return ACAP_FILE_Path;
}


int
ACAP_FILE_Init( const char* package ) {
	LOG_TRACE("%s: %s\n",__func__,package );
	sprintf( ACAP_FILE_Path,"/usr/local/packages/%s/", package );
	return 1;
}

FILE*
ACAP_FILE_Open( const char *filepath, const char *mode ) {
	char   fullpath[128];
	sprintf( fullpath, "%s", filepath );
	FILE *file = fopen( fullpath , mode );;
	if(file) {
		LOG_TRACE("Opening File %s (%s)\n",fullpath, mode);
	} else {
		LOG_TRACE("Error opening file %s\n",fullpath);
	}
	return file;
}

int
ACAP_FILE_Delete( const char *filepath ) {

  char fullpath[128];

  sprintf( fullpath, "%s%s", ACAP_FILE_Path, filepath );

  if( remove ( fullpath ) != 0 ) {
    LOG_WARN("ACAP_FILE_delete: Could not delete %s\n", fullpath);
    return 0;
  }
  return 1;	
}

cJSON*
ACAP_FILE_Read( const char* filepath ) {
	FILE*   file;
	char   *jsonString;
	cJSON  *object;
	size_t bytesRead, size;

	LOG_TRACE("%s: %s\n",__func__,filepath);

	file = ACAP_FILE_Open( filepath , "r");
	if( !file ) {
		//    LOG("%s does not exist\n", filepath);
		return 0;
	}

	fseek( file, 0, SEEK_END );
	size = ftell( file );
	fseek( file , 0, SEEK_SET);

	if( size < 2 ) {
		fclose( file );
		LOG_WARN("ACAP_FILE_read: File size error in %s\n", filepath);
		return 0;
	}

	jsonString = malloc( size + 1 );
	if( !jsonString ) {
		fclose( file );
		LOG_WARN("ACAP_FILE_read: Memory allocation error\n");
		return 0;
	}

	bytesRead = fread ( (void *)jsonString, sizeof(char), size, file );
	fclose( file );
	if( bytesRead != size ) {
		free( jsonString );
		LOG_WARN("ACAP_FILE_read: Read error in %s\n", filepath);
	return 0;
	}

	jsonString[bytesRead] = 0;
	object = cJSON_Parse( jsonString );
	free( jsonString );
	if( !object ) {
		LOG_WARN("ACAP_FILE_read: JSON Parse error for %s\n", filepath);
		return 0;
	}
	return object;
}

int
ACAP_FILE_WriteData( const char *filepath, const char *data ) {
  FILE *file;
  if( !filepath || !data ) {
    LOG_WARN("ACAP_FILE_write: Invalid input\n");
    return 0;
  }
  
  file = ACAP_FILE_Open( filepath, "w" );
  if( !file ) {
    LOG_WARN("ACAP_FILE_write: Error opening %s\n", filepath);
    return 0;
  }


  if( !fputs( data,file ) ) {
    LOG_WARN("ACAP_FILE_write: Could not save data to %s\n", filepath);
    fclose( file );
    return 0;
  }
  fclose( file );
  return 1;
}

int
ACAP_FILE_Write( const char *filepath,  cJSON* object )  {
  FILE *file;
  char *jsonString;
  if( !filepath || !object ) {
    LOG_WARN("ACAP_FILE_write: Invalid input\n");
    return 0;
  }
  
  file = ACAP_FILE_Open( filepath, "w" );
  if( !file ) {
    LOG_WARN("ACAP_FILE_write: Error opening %s\n", filepath);
    return 0;
  }

  jsonString = cJSON_Print( object );

  if( !jsonString ) {
    LOG_WARN("ACAP_FILE_write: JSON parse error for %s\n", filepath);
    fclose( file );
    return 0;
  }

  if( !fputs( jsonString,file ) ){
    LOG_WARN("ACAP_FILE_write: Could not save data to %s\n", filepath);
    free( jsonString );
    fclose( file );
    return 0;
  }
  free( jsonString );
  fclose( file );
  return 1;
}

int
ACAP_FILE_Exists( const char *filepath ) {
  FILE *file;
  file = ACAP_FILE_Open( filepath, "r");
  if( file )
    fclose( file );
  return (file != 0 );
}

AXHttpHandler  *ACAP_HTTP_handler = 0;
GHashTable     *ACAP_HTTP_node_table = 0; 
gchar          *ACAP_HTTP_Package_ID = 0;
static void     ACAP_HTTP_main_callback(const gchar *path,const gchar *method,const gchar *query,GHashTable *params,GOutputStream *output_stream,gpointer user_data);

int
ACAP_HTTP_Node( const char *name, ACAP_HTTP_Callback callback ) {
	if( !ACAP_HTTP_handler ) {
		LOG_WARN("ACAP_HTTP_cgi: HTTP handler not initialized\n");
		return 0;
	}

	gchar path[128];
	g_snprintf (path, 128, "/local/%s/%s", ACAP_package_name, name );

	LOG_TRACE("%s:%s", __func__, path );

	if( !ACAP_HTTP_node_table )
		ACAP_HTTP_node_table = g_hash_table_new_full(g_str_hash, g_str_equal,g_free, NULL);
	g_hash_table_insert( ACAP_HTTP_node_table, g_strdup( path ), (gpointer*)callback);

	LOG_TRACE("%s: Exit", __func__);

	return 1;
}

int
ACAP_HTTP_Respond_String( ACAP_HTTP_Response response,const gchar *fmt, ...) {
  va_list ap;
  gchar *tmp_str;
  GDataOutputStream *stream = (GDataOutputStream *)response;
  if( !stream ) {
    LOG_WARN("ACAP_HTTP_Respond_String: Cannot send data to http.  Handler = NULL\n");
    return 0;
  }
  
  va_start(ap, fmt);
  tmp_str = g_strdup_vprintf(fmt, ap);
  g_data_output_stream_put_string((GDataOutputStream *)response, tmp_str, NULL, NULL);
  
  g_free(tmp_str);

  va_end(ap);
  return 1;
}

int
ACAP_HTTP_Respond_Data( ACAP_HTTP_Response response, size_t count, void *data ) {
  gsize data_sent;
  
  if( count == 0 || data == 0 ) {
    LOG_WARN("ACAP_HTTP_Data: Invalid data\n");
    return 0;
  }
  
  if( !g_output_stream_write_all((GOutputStream *)response, data, count, &data_sent, NULL, NULL) ) {
    LOG_WARN("ACAP_HTTP_Data: Error sending data.");
    return 0;
  }  
  return 1;
}

const char*
ACAP_HTTP_Request_Param( const ACAP_HTTP_Request request, const char* name) {
  gchar *value;
  if( !request )
    return 0;
  if(!g_hash_table_lookup_extended((GHashTable *)request, name, NULL, (gpointer*)&value)) {
//    printf("ACAP_HTTP_Request_Param: Invalid option %s\n", name);
    return 0;
  }
  return value;   
}

cJSON*
ACAP_HTTP_Request_JSON( const ACAP_HTTP_Request request, const char *param ) {
  const char *jsonstring;
  cJSON *object;
  jsonstring = ACAP_HTTP_Request_Param( request, param);
  if( !jsonstring ) {
    return 0;
  }
  object = cJSON_Parse(jsonstring);
  if(!object) {
    LOG_WARN("ACAP_HTTP_Request_JSON: Invalid JSON: %s",jsonstring);
    return 0;
  }
  return object;
}

int
ACAP_HTTP_Header_XML( ACAP_HTTP_Response response ) {
  ACAP_HTTP_Respond_String( response,"Content-Type: text/xml; charset=utf-8; Cache-Control: no-cache\r\n\r\n");
  ACAP_HTTP_Respond_String( response,"<?xml version=\"1.0\"?>\r\n");
  return 1;
}

int
ACAP_HTTP_Header_JSON( ACAP_HTTP_Response response ) {
  ACAP_HTTP_Respond_String( response,"Content-Type: application/json; charset=utf-8; Cache-Control: no-cache\r\n\r\n");
  return 1;
}

int
ACAP_HTTP_Header_TEXT( ACAP_HTTP_Response response ) {
  ACAP_HTTP_Respond_String( response,"Content-Type: text/plain; charset=utf-8; Cache-Control: no-cache\r\n\r\n");
  return 1;
}

int
ACAP_HTTP_Header_FILE( const ACAP_HTTP_Response response, const char *filename, const char *contenttype, unsigned filelength ) {
  ACAP_HTTP_Respond_String( response, "HTTP/1.1 200 OK\r\n");
  ACAP_HTTP_Respond_String( response, "Content-Description: File Transfer\r\n");
  ACAP_HTTP_Respond_String( response, "Content-Type: %s\r\n", contenttype);
  ACAP_HTTP_Respond_String( response, "Content-Disposition: attachment; filename=%s\r\n", filename);
  ACAP_HTTP_Respond_String( response, "Content-Transfer-Encoding: binary\r\n");
  ACAP_HTTP_Respond_String( response, "Content-Length: %u\r\n", filelength );
  ACAP_HTTP_Respond_String( response, "\r\n");
  return 1;
}

int
ACAP_HTTP_Respond_Error( ACAP_HTTP_Response response, int code, const char *message ) {
  ACAP_HTTP_Respond_String( response,"status: %d %s Error\r\nContent-Type: text/plain\r\n\r\n", code, (code < 500) ? "Client" : (code < 600) ? "Server":"");
  if( code < 500 )
    LOG_WARN("HTTP response %d: %s\n", code, message);
  if( code >= 500 )
    LOG_WARN("HTTP response %d: %s\n", code, message);
  ACAP_HTTP_Respond_String( response,"%s", message);
  return 1;
}

int
ACAP_HTTP_Respond_Text( ACAP_HTTP_Response response, const char *message ) {
  ACAP_HTTP_Header_TEXT( response );
  ACAP_HTTP_Respond_String( response,"%s", message);
  return 1;
}

int
ACAP_HTTP_Respond_JSON( ACAP_HTTP_Response response, cJSON *object) {
  char *jsonstring;
  if( !object ) {
    LOG_WARN("ACAP_HTTP_Respond_JSON: Invalid object");
    return 0;
  }
  jsonstring = cJSON_Print( object );  
  ACAP_HTTP_Header_JSON( response );
  ACAP_HTTP_Respond_String( response, jsonstring );
  free( jsonstring );
  return 0;
}

static void
ACAP_HTTP_main_callback(const gchar *path,const gchar *method, const gchar *query, GHashTable *request, GOutputStream *output_stream, gpointer user_data){
  GDataOutputStream *response;
  gchar *key;
  ACAP_HTTP_Callback callback = 0;

  if( request ) {
    LOG_TRACE("HTTP request: %s?%s\n", path, query);
  } else  {
    LOG_TRACE("HTTP request: %s\n", path);
  }
  
  response = g_data_output_stream_new(output_stream);

  if( !ACAP_HTTP_node_table ) {
    ACAP_HTTP_Respond_Error( response, 500, "ACAP_HTTP_main_callback: Invalid table" );
    return;
  }
  if( !g_hash_table_lookup_extended( ACAP_HTTP_node_table, path, (gpointer*)&key,(gpointer*)&callback) ) {
    LOG_WARN("ACAP_HTTP_main_callback: CGI table lookup failed for %s (key = %s)\n", path, key );
  }

  if( callback ) {
    callback( (ACAP_HTTP_Response)response, (ACAP_HTTP_Request) request);
  } else {
    ACAP_HTTP_Respond_Error( response,400,"ACAP_HTTP_main_callback: No valid HTTP consumer");
  }
  g_object_unref(response);
  (void) user_data;
}

void
ACAP_ENDPOINT_license(const ACAP_HTTP_Response response,const ACAP_HTTP_Request request) {

	FILE* f = ACAP_FILE_Open( "LICENSE", "r" );
	if( !f ) {
		ACAP_HTTP_Respond_Error( response, 500, "No license found");
		return;
	}
	char* buffer = malloc(10000);
	if(!buffer) {
		ACAP_HTTP_Respond_Error( response, 500, "No license found");
		return;
	}
	int bytes = fread(buffer, sizeof(char), 10000, f);
	fclose(f);
	if( !bytes ) {
		free(buffer);
		ACAP_HTTP_Respond_Error( response, 500, "No license found");
		return;
	}
	ACAP_HTTP_Header_TEXT(response );	
	ACAP_HTTP_Respond_Data( response, bytes, buffer );
	free(buffer);
}

void
ACAP_HTTP() {
	if( !ACAP_HTTP_handler )
		ACAP_HTTP_handler = ax_http_handler_new( ACAP_HTTP_main_callback, NULL);
	if( !ACAP_HTTP_handler )
		LOG_WARN("HTTP_init: Failed to initialize HTTP\n");
	
	gchar path[128];
	g_snprintf (path, 128, "/local/%s/LICENSE", ACAP_package_name );

	if( !ACAP_HTTP_node_table )
		ACAP_HTTP_node_table = g_hash_table_new_full(g_str_hash, g_str_equal,g_free, NULL);
	g_hash_table_insert( ACAP_HTTP_node_table, g_strdup( path ), ACAP_ENDPOINT_license);
}


void
ACAP_HTTP_Close() {
  if( ACAP_HTTP_handler ) {
    ax_http_handler_free( ACAP_HTTP_handler );
    ACAP_HTTP_handler = 0;
  }
  if( ACAP_HTTP_node_table ) {
    g_hash_table_destroy( ACAP_HTTP_node_table );
    ACAP_HTTP_node_table = 0;
  } 
}


cJSON *ACAP_DEVICE_Container = 0;
AXParameter *ACAP_DEVICE_parameter_handler;

char**
string_split( char* a_str,  char a_delim) {
    char** result    = 0;
    size_t count     = 0;
    const char* tmp  = a_str;
    const char* last_comma = 0;
    char delim[2];
    delim[0] = a_delim;
    delim[1] = 0;
    /* Count how many elements will be extracted. */
    while (*tmp)
    {
        if (a_delim == *tmp)
        {
            count++;
            last_comma = tmp;
        }
        tmp++;
    }

    /* Add space for trailing token. */
    count += last_comma < (a_str + strlen(a_str) - 1);

    /* Add space for terminating null string so caller
       knows where the list of returned strings ends. */
    count++;
    result = malloc(sizeof(char*) * count);
    if (result) {
        size_t idx  = 0;
        char* token = strtok(a_str, delim);
        while (token) {
//            assert(idx < count);
            *(result + idx++) = strdup(token);
            token = strtok(0, delim);
        }
//        assert(idx == count - 1);
        *(result + idx) = 0;
    }
    return result;
}

const char* 
ACAP_DEVICE_Prop( const char *attribute ) {
	if( !ACAP_DEVICE_Container )
		return 0;
	if( strcmp(attribute,"IPv4") == 0 ) {
		gchar *value = 0;
		if(ax_parameter_get(ACAP_DEVICE_parameter_handler, "root.Network.eth0.IPAddress",&value,0 )) {
			cJSON_ReplaceItemInObject(ACAP_DEVICE_Container,"IPv4",cJSON_CreateString(value));
			g_free(value);
		}
	}  
	return cJSON_GetObjectItem(ACAP_DEVICE_Container,attribute) ? cJSON_GetObjectItem(ACAP_DEVICE_Container,attribute)->valuestring : 0;
}

int
ACAP_DEVICE_Prop_Int( const char *attribute ) {
  if( !ACAP_DEVICE_Container )
    return 0;
  return cJSON_GetObjectItem(ACAP_DEVICE_Container,attribute) ? cJSON_GetObjectItem(ACAP_DEVICE_Container,attribute)->valueint : 0;
}

cJSON* 
ACAP_DEVICE_JSON( const char *attribute ) {
  if( !ACAP_DEVICE_Container )
    return 0;
  return cJSON_GetObjectItem(ACAP_DEVICE_Container,attribute);
}


int
ACAP_DEVICE_Seconds_Since_Midnight() {
	time_t t = time(NULL);
	struct tm tm = *localtime(&t);
	int seconds = tm.tm_hour * 3600;
	seconds += tm.tm_min * 60;
	seconds += tm.tm_sec;
	return seconds;
}

char ACAP_DEVICE_date[128] = "2023-01-01";
char ACAP_DEVICE_time[128] = "00:00:00";

const char*
ACAP_DEVICE_Date() {
	time_t t = time(NULL);
	struct tm *tm = localtime(&t);
	sprintf(ACAP_DEVICE_date,"%d-%02d-%02d",tm->tm_year + 1900,tm->tm_mon + 1, tm->tm_mday);
	return ACAP_DEVICE_date;
}

const char*
ACAP_DEVICE_Time() {
	time_t t = time(NULL);
	struct tm *tm = localtime(&t);
	sprintf(ACAP_DEVICE_time,"%02d:%02d:%02d",tm->tm_hour,tm->tm_min,tm->tm_sec);
	return ACAP_DEVICE_time;
}

char ACAP_DEVICE_timestring[128] = "2020-01-01 00:00:00";

const char*
ACAP_DEVICE_Local_Time() {
	time_t t = time(NULL);
	struct tm *tm = localtime(&t);
	sprintf(ACAP_DEVICE_timestring,"%d-%02d-%02d %02d:%02d:%02d",tm->tm_year + 1900,tm->tm_mon + 1, tm->tm_mday,tm->tm_hour,tm->tm_min,tm->tm_sec);
	LOG_TRACE("Local Time: %s\n",ACAP_DEVICE_timestring);
	return ACAP_DEVICE_timestring;
}

char ACAP_DEVICE_isostring[128] = "2020-01-01T00:00:00+0000";

const char*
ACAP_DEVICE_ISOTime() {
	time_t t = time(NULL);
	struct tm *tm = localtime(&t);
	strftime(ACAP_DEVICE_isostring, 50, "%Y-%m-%dT%T%z",tm);
	return ACAP_DEVICE_isostring;
}

double
ACAP_DEVICE_Timestamp(void) {
	long ms;
	time_t s;
	struct timespec spec;
	double timestamp;
	clock_gettime(CLOCK_REALTIME, &spec);
	s  = spec.tv_sec;
	ms = round(spec.tv_nsec / 1.0e6); // Convert nanoseconds to milliseconds
	if (ms > 999) {
		s++;
		ms -= 999;
	}
	timestamp = (double)s;
	timestamp *= 1000;
	timestamp += (double)ms;
	return timestamp;  
}

static void
ACAP_ENDPOINT_device(ACAP_HTTP_Response response, ACAP_HTTP_Request request) {
	if( !ACAP_DEVICE_Container) {
		ACAP_HTTP_Respond_Error( response, 500, "Device properties not initialized");
		return;
	}
	cJSON_ReplaceItemInObject(ACAP_DEVICE_Container,"date", cJSON_CreateString(ACAP_DEVICE_Date()) );
	cJSON_ReplaceItemInObject(ACAP_DEVICE_Container,"time", cJSON_CreateString(ACAP_DEVICE_Time()) );
	ACAP_HTTP_Respond_JSON( response, ACAP_DEVICE_Container );
}

double previousTransmitted = 0;
double previousNetworkTimestamp = 0;
double previousNetworkAverage = 0;

double
ACAP_DEVICE_Network_Average() {
	FILE *fd;
	char   data[500] = "";
	char   readstr[500];
	char **stringArray;
	char   *subString;
	char *ptr;
	double transmitted = 0, rx = 0;
	 

	fd = fopen("/proc/net/dev","r");
	if( !fd ) {
		LOG_WARN("Error opening /proc/net/dev");
		return 0;
	}
  
	while( fgets(readstr,500,fd) ) {
		if( strstr( readstr,"eth0") != 0 )
			strcpy( data, readstr );
	}
	fclose(fd);
  
	if( strlen( data ) < 20 ) {
		LOG_WARN("Read_Ethernet_Traffic: read error");
		return 0;
	}
  
	stringArray = string_split(data, ' ');
	if( stringArray ) {
		int i;
		for (i = 0; *(stringArray + i); i++) {
			if( i == 1 ) {
				subString = *(stringArray + i);
				rx = (double)strtol( subString, &ptr, 10);
			}
			if( i == 9 ) {
				subString = *(stringArray + i);
				if(strlen(subString) > 9 )
					subString++;
				if(strlen(subString) > 9 )
					subString++;
				if(strlen(subString) > 9 )
					subString++;
				transmitted = (double)strtol( subString, &ptr, 10);
			}
			free(*(stringArray + i));
		}
		free(stringArray);
	}
	(void)rx;
	double diff = transmitted - previousTransmitted;
	previousTransmitted = transmitted;
	if( diff < 0 )
		return previousNetworkAverage;
	double timestamp = ACAP_DEVICE_Timestamp();
	double timeDiff = timestamp - previousNetworkTimestamp;
	previousNetworkTimestamp = timestamp;
	timeDiff /= 1000;
	diff *= 8;  //Make to bits;
	diff /= 1024;  //Make Kbits;
	
	previousNetworkAverage = diff / timeDiff;
	if( previousNetworkAverage < 0.001 )
		previousNetworkAverage = 0;
	return previousNetworkAverage;
}

double
ACAP_DEVICE_CPU_Average() {
	double loadavg = 0;
	struct sysinfo info;
	
	sysinfo(&info);
	loadavg = (double)info.loads[2];
	loadavg /= 65536.0;
	LOG_TRACE("%f\n",loadavg);
	return loadavg; 
}

double
ACAP_DEVICE_Uptime() {
	struct sysinfo info;
	sysinfo(&info);
	return (double)info.uptime; 
};	


cJSON*
ACAP_DEVICE() {
	gchar *value = 0, *pHead,*pTail;

	ACAP_DEVICE_Container = cJSON_CreateObject();
	ACAP_DEVICE_parameter_handler = ax_parameter_new(ACAP_package_name, 0);
	if( !ACAP_DEVICE_parameter_handler ) {
		LOG_WARN("Cannot create parameter ACAP_DEVICE_parameter_handler");
		return cJSON_CreateNull();
	}
	

	if(ax_parameter_get(ACAP_DEVICE_parameter_handler, "root.Properties.System.SerialNumber", &value, 0)) {
		cJSON_AddItemToObject(ACAP_DEVICE_Container,"serial",cJSON_CreateString(value));
		g_free(value);
	}
  
	if(ax_parameter_get(ACAP_DEVICE_parameter_handler, "root.brand.ProdShortName", &value, 0)) {
		cJSON_AddItemToObject(ACAP_DEVICE_Container,"model",cJSON_CreateString(value));
		g_free(value);
	}

	if(ax_parameter_get(ACAP_DEVICE_parameter_handler, "root.Properties.System.Architecture", &value, 0)) {
		cJSON_AddItemToObject(ACAP_DEVICE_Container,"platform",cJSON_CreateString(value));
		g_free(value);
	}

	if(ax_parameter_get(ACAP_DEVICE_parameter_handler, "root.Properties.System.Soc", &value, 0)) {
		cJSON_AddItemToObject(ACAP_DEVICE_Container,"chip",cJSON_CreateString(value));
		g_free(value);
	}

	if(ax_parameter_get(ACAP_DEVICE_parameter_handler, "root.brand.ProdType", &value, 0)) {
		cJSON_AddItemToObject(ACAP_DEVICE_Container,"type",cJSON_CreateString(value));
		g_free(value);
	}

//	if(ax_parameter_get(ACAP_DEVICE_parameter_handler, "root.Network.VolatileHostName.HostName", &value, 0)) {
//		cJSON_AddItemToObject(ACAP_DEVICE_Container,"hostname",cJSON_CreateString(value));
//		g_free(value);
//	}
  
	int aspect = 169;

	if(ax_parameter_get(ACAP_DEVICE_parameter_handler, "root.ImageSource.I0.Sensor.AspectRatio",&value,0 )) {
		cJSON_AddItemToObject(ACAP_DEVICE_Container,"aspect",cJSON_CreateString(value));
		if(strcmp(value,"4:3") == 0)
			aspect = 43;
		if(strcmp(value,"16:10") == 0)
			aspect = 1610;
		if(strcmp(value,"1:1") == 0)
			aspect = 11;
		g_free(value);
	} else {
		cJSON_AddStringToObject(ACAP_DEVICE_Container,"aspect","16:9");
	}
  
	cJSON* resolutionList = cJSON_CreateArray();
	cJSON* resolutions = cJSON_CreateObject();
	cJSON_AddItemToObject(ACAP_DEVICE_Container,"resolutions",resolutions);
	cJSON* resolutions169 = cJSON_CreateArray();
	cJSON_AddItemToObject(resolutions,"16:9",resolutions169);
	cJSON* resolutions43 = cJSON_CreateArray();
	cJSON_AddItemToObject(resolutions,"4:3",resolutions43);
	cJSON* resolutions11 = cJSON_CreateArray();
	cJSON_AddItemToObject(resolutions,"1:1",resolutions11);
	cJSON* resolutions1610 = cJSON_CreateArray();
	cJSON_AddItemToObject(resolutions,"16:10",resolutions1610);
	if(ax_parameter_get(ACAP_DEVICE_parameter_handler, "root.Properties.Image.Resolution", &value, 0)) {
		pHead = value;
		pTail = value;
		while( *pHead ) {
			if( *pHead == ',' ) {
				*pHead = 0;
				cJSON_AddItemToArray( resolutionList, cJSON_CreateString(pTail) );
				pTail = pHead + 1;
			}
			pHead++;
		}
		cJSON_AddItemToArray( resolutionList, cJSON_CreateString(pTail) );
		g_free(value);

		int length = cJSON_GetArraySize(resolutionList);
		int index;
		char data[30];
		LOG_TRACE("Resolutions");
		int width = 0;
		int height = 0;
		for( index = 0; index < length; index++ ) {
			char* resolution = strcpy(data,cJSON_GetArrayItem(resolutionList,index)->valuestring);
			if( resolution ) {
				char* sX = resolution;
				char* sY = 0;
				while( *sX != 0 ) {
					if( *sX == 'x' ) {
						*sX = 0;
						sY = sX + 1;
					}
					sX++;
				}
				if( sY ) {
					int x = atoi(resolution);
					int y = atoi(sY);
					if( x && y ) {
						int a = (x*100)/y;
						if( a == 177 ) {
							cJSON_AddItemToArray(resolutions169, cJSON_CreateString(cJSON_GetArrayItem(resolutionList,index)->valuestring));
							if(aspect == 169 && x > width )
								width = x;
							if(aspect == 169 && y > height )
								height = y;
						}
						if( a == 133 ) {
							cJSON_AddItemToArray(resolutions43, cJSON_CreateString(cJSON_GetArrayItem(resolutionList,index)->valuestring));
							if(aspect == 43 && x > width )
								width = x;
							if(aspect == 43 && y > height )
								height = y;
						}
						if( a == 160 ) {
							cJSON_AddItemToArray(resolutions1610, cJSON_CreateString(cJSON_GetArrayItem(resolutionList,index)->valuestring));
							if(aspect == 1610 && x > width )
								width = x;
							if(aspect == 1610 && y > height )
								height = y;
						}
						if( a == 100 ) {
							cJSON_AddItemToArray(resolutions11, cJSON_CreateString(cJSON_GetArrayItem(resolutionList,index)->valuestring));
							if(aspect == 11 && x > width )
								width = x;
							if(aspect == 11 && y > height )
								height = y;
						}
					}
				}
			}
		}
		cJSON_AddItemToObject(ACAP_DEVICE_Container,"width",cJSON_CreateNumber(width));
		cJSON_AddItemToObject(ACAP_DEVICE_Container,"height",cJSON_CreateNumber(height));
		
		int a = (width*100)/height;
		if( a == 133 )
			cJSON_ReplaceItemInObject(ACAP_DEVICE_Container,"aspect",cJSON_CreateString("4:3") );
		if( a == 160 )
			cJSON_ReplaceItemInObject(ACAP_DEVICE_Container,"aspect",cJSON_CreateString("16:10") );
		if( a == 100 )
			cJSON_ReplaceItemInObject(ACAP_DEVICE_Container,"aspect",cJSON_CreateString("1:1") );
		cJSON_Delete(resolutionList);
		LOG_TRACE("Resolutions: Done");
	}
  
	if(ax_parameter_get(ACAP_DEVICE_parameter_handler, "root.Network.eth0.MACAddress",&value,0 )) {
		cJSON_AddItemToObject(ACAP_DEVICE_Container,"mac",cJSON_CreateString(value));
		g_free(value);
	}  

	if(ax_parameter_get(ACAP_DEVICE_parameter_handler, "root.Image.I0.Appearance.Rotation",&value,0 )) {
		cJSON_AddItemToObject(ACAP_DEVICE_Container,"rotation",cJSON_CreateNumber( atoi(value) ));
		g_free(value);
	}  

	if(ax_parameter_get(ACAP_DEVICE_parameter_handler, "root.Network.eth0.IPAddress",&value,0 )) {
		cJSON_AddItemToObject(ACAP_DEVICE_Container,"IPv4",cJSON_CreateString(value));
		g_free(value);
	}  
  
	if(ax_parameter_get(ACAP_DEVICE_parameter_handler, "root.Properties.Firmware.Version",&value,0 )) {
		cJSON_AddItemToObject(ACAP_DEVICE_Container,"firmware",cJSON_CreateString(value));
		g_free(value);
	}  

 
	ax_parameter_free(ACAP_DEVICE_parameter_handler);
	
	ACAP_HTTP_Node("device",ACAP_ENDPOINT_device);
	return ACAP_DEVICE_Container;
}

cJSON *ACAP_STATUS_Container = 0;

static void
ACAP_ENDPOINT_status(const ACAP_HTTP_Response response,const ACAP_HTTP_Request request) {
	if( ACAP_STATUS_Container == 0 ) {
		ACAP_HTTP_Respond_Error( response, 500, "Status container not initialized");
		return;
	}
	ACAP_HTTP_Respond_JSON( response, ACAP_STATUS_Container);	
}

cJSON*
ACAP_STATUS_Group( const char *group ) {
	if(!ACAP_STATUS_Container)
		ACAP_STATUS_Container = cJSON_CreateObject();
	cJSON* g = cJSON_GetObjectItem(ACAP_STATUS_Container, group );
	return g;
}

int
ACAP_STATUS_Bool( const char *group, const char *name ) {
	cJSON* g = ACAP_STATUS_Group( group );
	if(!g) {
		LOG_WARN("%s: %s is undefined\n",__func__,group);
		return 0;
	}
	cJSON *object = cJSON_GetObjectItem(g, name);
	if( !object ) {
		LOG_WARN("%s: %s/%s is undefined\n",__func__,group,name);
		return 0;
	}
	if( !( object->type == cJSON_True || object->type == cJSON_False || object->type == cJSON_NULL)) {
		LOG_WARN("%s: %s/%s is not bool (Type = %d)\n",__func__,group,name,object->type); 
		return 0;
	}
	if( object->type == cJSON_True )
		return 1;
	return 0;
}

double
ACAP_STATUS_Double( const char *group, const char *name ) {
	cJSON* g = ACAP_STATUS_Group( group );
	if(!g) {
		LOG_WARN("%s: %s is undefined\n",__func__,group);
		return 0;
	}
	cJSON *object = cJSON_GetObjectItem(g, name);
	if( !object ) {
		LOG_WARN("%s: %s/%s is undefined\n",__func__,group,name);
		return 0;
	}
	if( object->type != cJSON_Number ) {
		LOG_WARN("%s: %s/%s is not a number\n",__func__,group,name); 
		return 0;
	}
	return object->valuedouble;
}

int
ACAP_STATUS_Int( const char *group, const char *name ) {
	cJSON* g = ACAP_STATUS_Group( group );
	if(!g) {
		LOG_WARN("%s: %s is undefined\n",__func__,group);
		return 0;
	}
	cJSON *object = cJSON_GetObjectItem(g, name);
	if( !object ) {
		LOG_WARN("%s: %s/%s is undefined\n",__func__,group,name);
		return 0;
	}
	if( object->type != cJSON_Number ) {
		LOG_WARN("%s: %s/%s is not a number\n",__func__,group,name); 
		return 0;
	}
	return object->valueint;
}

char*
ACAP_STATUS_String( const char *group, const char *name ) {
	cJSON* g = ACAP_STATUS_Group(group);
	if(!g) {
		LOG_WARN("%s: %s is undefined\n",__func__,group);
		return 0;
	}
	cJSON *object = cJSON_GetObjectItem(g, name);
	if( !object ) {
		LOG_WARN("%s: %s/%s is undefined\n",__func__,group,name);
		return 0;
	}
	if( object->type != cJSON_String ) {
		LOG_WARN("%s: %s/%s is not a string\n",__func__,group,name); 
		return 0;
	}
	return object->valuestring;
}

cJSON*
ACAP_STATUS_Object( const char *group, const char *name ) {
	cJSON* g = ACAP_STATUS_Group(group);
	if(!g) {
		LOG_WARN("%s: %s is undefined\n",__func__,group);
		return 0;
	}
	cJSON *object = cJSON_GetObjectItem(g, name);
	if( !object ) {
		LOG_WARN("%s: %s/%s is undefined\n",__func__,group,name);
		return 0;
	}
	return object;
}

void
ACAP_STATUS_SetBool( const char *group, const char *name, int state ) {
	LOG_TRACE("%s: %s.%s=%s \n",__func__, group, name, state?"true":"false" );
	cJSON* g = ACAP_STATUS_Group(group);
	if(!g) {
		g = cJSON_CreateObject();
		cJSON_AddItemToObject(ACAP_STATUS_Container, group, g);
	}
	cJSON *object = cJSON_GetObjectItem( g, name);
	if( !object ) {
		if( state )
			cJSON_AddTrueToObject( g, name );
		else
			cJSON_AddFalseToObject( g, name );
		return;
	}
	if( state )
		cJSON_ReplaceItemInObject(g, name, cJSON_CreateTrue() );
	else
		cJSON_ReplaceItemInObject(g, name, cJSON_CreateFalse() );
}

void
ACAP_STATUS_SetNumber( const char *group, const char *name, double value ) {
	LOG_TRACE("%s: %s.%s=%f \n",__func__, group, name, value );
	cJSON* g = ACAP_STATUS_Group(group);
	if(!g) {
		g = cJSON_CreateObject();
		cJSON_AddItemToObject(ACAP_STATUS_Container, group, g);
	}
	cJSON *object = cJSON_GetObjectItem( g, name);
	if( !object ) {
		cJSON_AddNumberToObject( g, name, value );
		return;
	}
	cJSON_ReplaceItemInObject(g, name, cJSON_CreateNumber( value ));
}

void
ACAP_STATUS_SetString( const char *group, const char *name, const char *string ) {
	LOG_TRACE("%s: %s.%s=%s \n",__func__, group, name, string );
	cJSON* g = ACAP_STATUS_Group(group);
	if(!g) {
		g = cJSON_CreateObject();
		cJSON_AddItemToObject(ACAP_STATUS_Container, group, g);
	}
	cJSON *object = cJSON_GetObjectItem( g, name);
	if( !object ) {
		cJSON_AddStringToObject( g, name, string );
		return;
	}
	cJSON_ReplaceItemInObject(g, name, cJSON_CreateString( string ));
}

void
ACAP_STATUS_SetObject( const char *group, const char *name, cJSON* data ) {
	LOG_TRACE("%s: %s.%s\n",__func__, group, name );
	
	cJSON* g = ACAP_STATUS_Group(group);
	if(!g) {
		g = cJSON_CreateObject();
		cJSON_AddItemToObject(ACAP_STATUS_Container, group, g);
	}

	cJSON *object = cJSON_GetObjectItem( g, name);
	if( !object ) {
		cJSON_AddItemToObject( g, name, cJSON_Duplicate(data,1) );
		return;
	}
	cJSON_ReplaceItemInObject(g, name, cJSON_Duplicate(data,1));
}

void
ACAP_STATUS_SetNull( const char *group, const char *name ) {
	LOG_TRACE("%s: %s.%s=NULL\n",__func__, group, name );
	
	cJSON* g = ACAP_STATUS_Group(group);
	if(!g) {
		g = cJSON_CreateObject();
		cJSON_AddItemToObject(ACAP_STATUS_Container, group, g);
	}

	cJSON *object = cJSON_GetObjectItem( g, name);
	if( !object ) {
		cJSON_AddItemToObject( g, name, cJSON_CreateNull() );
		return;
	}
	cJSON_ReplaceItemInObject(g, name, cJSON_CreateNull());
}

cJSON*
ACAP_STATUS() {
	if( ACAP_STATUS_Container != 0 )
		return ACAP_STATUS_Container;
	LOG_TRACE("%s:\n",__func__);
	ACAP_STATUS_Container = cJSON_CreateObject();
	ACAP_HTTP_Node( "status", ACAP_ENDPOINT_status );
	return ACAP_STATUS_Container;
}


