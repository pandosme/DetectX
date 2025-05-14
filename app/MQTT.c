/**
 * MQTT.c
 * Fred Juhlin 2025
 * Version 1.6
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <dlfcn.h>
#include <time.h>
#include <errno.h>
#include <syslog.h>
#include "ACAP.h"
#include "MQTT.h"
#include "MQTTClient.h"
#include "CERTS.h"

#define LOG(fmt, args...)    { syslog(LOG_INFO, fmt, ## args);  printf(fmt, ## args); }
#define LOG_WARN(fmt, args...)    { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args);}
#define LOG_TRACE(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args);}
//#define LOG_TRACE(fmt, args...)    { }

// Function pointers for dynamically loaded Paho MQTT library
typedef int (*MQTTClient_create_func)(MQTTClient*, const char*, const char*, int, void*);
typedef int (*MQTTClient_connect_func)(MQTTClient, MQTTClient_connectOptions*);
typedef int (*MQTTClient_disconnect_func)(MQTTClient, int);
typedef int (*MQTTClient_isConnected_func)(MQTTClient);
typedef int (*MQTTClient_publishMessage_func)(MQTTClient, const char*, MQTTClient_message*, MQTTClient_deliveryToken*);
typedef int (*MQTTClient_destroy_func)(MQTTClient*);
typedef int (*MQTTClient_subscribe_func)(MQTTClient, const char*, int);
typedef int (*MQTTClient_unsubscribe_func)(MQTTClient, const char*);
typedef int (*MQTTClient_setCallbacks_func)(MQTTClient, void*, MQTTClient_connectionLost*, MQTTClient_messageArrived*, MQTTClient_deliveryComplete*);
typedef void (*MQTTClient_freeMessage_func)(MQTTClient_message**);
typedef void (*MQTTClient_free_func)(void*);
typedef void (*MQTTClient_yield_func)(void);

static MQTTClient_create_func mqtt_client_create = NULL;
static MQTTClient_connect_func mqtt_client_connect = NULL;
static MQTTClient_disconnect_func mqtt_client_disconnect = NULL;
static MQTTClient_isConnected_func mqtt_client_isConnected = NULL;
static MQTTClient_publishMessage_func mqtt_client_publishMessage = NULL;
static MQTTClient_destroy_func mqtt_client_destroy = NULL;
static MQTTClient_subscribe_func mqtt_client_subscribe = NULL;
static MQTTClient_unsubscribe_func mqtt_client_unsubscribe = NULL;
static MQTTClient_setCallbacks_func mqtt_client_setCallbacks = NULL;
static MQTTClient_freeMessage_func mqtt_client_freeMessage = NULL;
static MQTTClient_free_func mqtt_client_free = NULL;
static MQTTClient_yield_func mqtt_client_yield = NULL;


void* 			MQTT_libHandle = 0;
static cJSON* 	MQTTSettings = NULL;
static char 	LastWillTopic[64];
static char 	LastWillMessage[512];
void*			MQTTlibHandle = 0;

static MQTTClient 			g_mqtt_client = NULL;
MQTT_Callback_Message 		userSubscriptionCallback;
MQTT_Callback_Connection 	connectionMessgage;
int MQTT_SetupClient();
void connectionLostCallback(void *context, char *cause);
int  messageArrivedCallback(void *context, char *topicName, int topicLen, MQTTClient_message *message);
void deliveryCompleteCallback(void *context, MQTTClient_deliveryToken dt);

cJSON* MQTT_Settings() {
	return MQTTSettings;
}

int
MQTT_Connect() {
    LOG_TRACE("%s: Entry\n", __func__);
    
    if (!g_mqtt_client) {
        LOG_WARN("%s: MQTT client not initialized\n", __func__);
        return 0;
    }

	if( g_mqtt_client && mqtt_client_isConnected(g_mqtt_client) ) {
        LOG("%s: MQTT client already connected\n", __func__);
        return 1;
    }

    const char* user = cJSON_GetObjectItem(MQTTSettings, "user") ? cJSON_GetObjectItem(MQTTSettings, "user")->valuestring : "";
    const char* password = cJSON_GetObjectItem(MQTTSettings, "password") ? cJSON_GetObjectItem(MQTTSettings, "password")->valuestring : "";

    MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
    conn_opts.keepAliveInterval = 60;
	conn_opts.connectTimeout = 10;	
    conn_opts.cleansession = 1;
	if( strlen( user ) && strlen( password) ) {
		conn_opts.username = user;
		conn_opts.password = password;
	}

    // Setup TLS if enabled
    MQTTClient_SSLOptions ssl_opts = MQTTClient_SSLOptions_initializer;	
    int useTLS = 0;
	int verifyTLS = 0;
	if( cJSON_GetObjectItem(MQTTSettings,"tls") && cJSON_GetObjectItem(MQTTSettings,"tls")->type == cJSON_True )
		useTLS = 1;
	if( cJSON_GetObjectItem(MQTTSettings,"verify") && cJSON_GetObjectItem(MQTTSettings,"verify")->type == cJSON_True )
		verifyTLS = 1;
    if (useTLS) {
		ssl_opts.enabledCipherSuites = NULL;  //Let client negotiate
		ssl_opts.sslVersion = ssl_opts.sslVersion;
		if( CERTS_Get_CA() )
			ssl_opts.trustStore = CERTS_Get_CA();
		if( CERTS_Get_Cert() )
			ssl_opts.keyStore = CERTS_Get_Cert();
		if( CERTS_Get_Key() )
			ssl_opts.privateKey = CERTS_Get_Key();
		if( CERTS_Get_Password() )
			ssl_opts.privateKeyPassword = CERTS_Get_Password();
		if( verifyTLS ) {
			ssl_opts.enableServerCertAuth = 1;
			ssl_opts.verify = 1;
		} else {
			ssl_opts.enableServerCertAuth = 0;
			ssl_opts.verify = 0;
		}
        conn_opts.ssl = &ssl_opts;
    }

    // Setup last-will testament
    MQTTClient_willOptions will_opts = MQTTClient_willOptions_initializer;
    cJSON* lwt = cJSON_CreateObject();
    cJSON_AddFalseToObject(lwt, "connected");
    cJSON_AddStringToObject(lwt, "address", ACAP_DEVICE_Prop("IPv4"));
    cJSON* additionalProperties = cJSON_GetObjectItem(MQTTSettings, "payload");
    if (additionalProperties) {
        const char* name = cJSON_GetObjectItem(additionalProperties, "name") ? cJSON_GetObjectItem(additionalProperties, "name")->valuestring : NULL;
        const char* location = cJSON_GetObjectItem(additionalProperties, "location") ? cJSON_GetObjectItem(additionalProperties, "location")->valuestring : NULL;
        if (name && strlen(name))
            cJSON_AddStringToObject(lwt, "name", name);
        if (location && strlen(location))
            cJSON_AddStringToObject(lwt, "location", location);
    }
    cJSON_AddStringToObject(lwt, "serial", ACAP_DEVICE_Prop("serial"));

	if( cJSON_GetObjectItem(MQTTSettings, "preTopic") && cJSON_GetObjectItem(MQTTSettings, "preTopic")->type == cJSON_String && strlen(cJSON_GetObjectItem(MQTTSettings, "preTopic")->valuestring ))
		sprintf(LastWillTopic,"%s/connect/%s",cJSON_GetObjectItem(MQTTSettings, "preTopic")->valuestring, ACAP_DEVICE_Prop("serial"));
	else
		sprintf(LastWillTopic,"connect/%s", ACAP_DEVICE_Prop("serial"));
	
    char* json = cJSON_PrintUnformatted(lwt);
    if (json) {
        snprintf(LastWillMessage, sizeof(LastWillMessage), "%s", json);
        will_opts.topicName = LastWillTopic;
        will_opts.message = LastWillMessage;
        will_opts.retained = 1;
        conn_opts.will = &will_opts;
        free(json);
    }
    cJSON_Delete(lwt);

	//Connect
    connectionMessgage(MQTT_CONNECTING);

    // Set callbacks
    int rc = mqtt_client_setCallbacks(g_mqtt_client, NULL, connectionLostCallback, messageArrivedCallback, deliveryCompleteCallback);
    if (rc != MQTTCLIENT_SUCCESS)
        LOG_WARN("%s: Failed to set callbacks, rc=%d\n", __func__, rc);

    rc = mqtt_client_connect(g_mqtt_client, &conn_opts);
    if (rc != MQTTCLIENT_SUCCESS) {
        LOG_WARN("%s: Failed to connect, return code %d\n", __func__, rc);
        connectionMessgage(MQTT_DISCONNECTED);
        return 0;
    }
    connectionMessgage(MQTT_CONNECTED);
	cJSON_GetObjectItem(MQTTSettings,"connect")->type = cJSON_True;
	ACAP_FILE_Write( "localdata/mqtt.json", MQTTSettings );

    LOG_TRACE("%s: Exit\n", __func__);
    return 1;
}

int
MQTT_Disconnect() {
    LOG_TRACE("%s: Entry\n", __func__);
    
    if (!g_mqtt_client) {
        LOG_WARN("%s: MQTT client not initialized\n", __func__);
        return 0;
    }

    if (g_mqtt_client && !mqtt_client_isConnected(g_mqtt_client)) {
        LOG("%s: MQTT client disconnected\n", __func__);
        return 1;
    }

    int rc = mqtt_client_disconnect(g_mqtt_client, 10000);
    if (rc != MQTTCLIENT_SUCCESS) {
        LOG_WARN("%s: Failed to disconnect, return code %d\n", __func__, rc);
        return 0;
    }

    LOG("%s: MQTT client disconnected\n", __func__);

    connectionMessgage(MQTT_DISCONNECTED);
    LOG_TRACE("%s: Exit\n", __func__);
    return 1;
}

static gboolean
checkConnection(gpointer user_data) {
	mqtt_client_yield();

	if( !g_mqtt_client || cJSON_GetObjectItem(MQTTSettings,"connect")->type == cJSON_False )
		return G_SOURCE_CONTINUE;
	
	if( mqtt_client_isConnected(g_mqtt_client) )
		return G_SOURCE_CONTINUE;

	connectionMessgage(MQTT_RECONNECTING);
	if( MQTT_Connect() )
		return G_SOURCE_CONTINUE;
		
	MQTT_SetupClient();
	MQTT_Connect();
    return G_SOURCE_CONTINUE; // Keep trying
}


void
connectionLostCallback(void *context, char *cause) {
    LOG_WARN("%s: Entry. Cause: %s\n", __func__, cause ? cause : "Unknown");
//    reconnectAttempt(0);
    LOG_TRACE("%s: Exit\n", __func__);
}

int
messageArrivedCallback(void *context, char *topicName, int topicLen, MQTTClient_message *message) {
    LOG_TRACE("%s: Entr - topic = %s\n", __func__, topicName);
    
    if (userSubscriptionCallback) {
        userSubscriptionCallback(topicName, message->payload);
    } else {
        LOG_WARN("%s: No message callback registered\n", __func__);
    }
    
    mqtt_client_freeMessage(&message);
    mqtt_client_free(topicName);

	LOG_TRACE("%s: Exit\n",__func__);    
    return 1;
}

void
deliveryCompleteCallback(void *context, MQTTClient_deliveryToken dt) {
    LOG_TRACE("%s: Message delivery complete for token %d\n", __func__, dt);
}

int
MQTT_SetupClient() {
    LOG_TRACE("%s: Entry\n", __func__);

    if (!MQTTSettings) {
        LOG_WARN("%s: Invalid settings\n", __func__);
        return 0;
    }

    // Clean up previous client if it exists
    if (g_mqtt_client) {
		LOG("%s: Previous MQTT instance removed\n",__func__);
        mqtt_client_destroy(&g_mqtt_client);
		g_mqtt_client = 0;
    }

    const char* address = cJSON_GetObjectItem(MQTTSettings, "address") ? cJSON_GetObjectItem(MQTTSettings, "address")->valuestring : NULL;
    const char* port = cJSON_GetObjectItem(MQTTSettings, "port") ? cJSON_GetObjectItem(MQTTSettings, "port")->valuestring : "1883";

	int useTLS = 0;

    if( cJSON_GetObjectItem(MQTTSettings, "tls") && cJSON_GetObjectItem(MQTTSettings, "tls")->type == cJSON_True )
			useTLS = 1;
    
    if (!address) {
        LOG_WARN("%s: Missing address in settings\n", __func__);
        return 0;
    }

    char* clientId = malloc(128);
    if (!clientId) {
        LOG_WARN("%s: Failed to allocate memory for clientId\n", __func__);
        return 0;
    }
    snprintf(clientId, 128, "%s-%s", ACAP_Name(), ACAP_DEVICE_Prop("serial"));

    char* serverURI = malloc(256);
    if (!serverURI) {
        LOG_WARN("%s: Failed to allocate memory for serverURI\n", __func__);
        free(clientId);
        return 0;
    }
    snprintf(serverURI, 256, "%s://%s:%s", useTLS ? "ssl" : "tcp", address, port);
	
	LOG("MQTT Connection address: %s Client Id: %s\n",serverURI,clientId);
	int rc = mqtt_client_create(&g_mqtt_client, serverURI, clientId, MQTTCLIENT_PERSISTENCE_NONE, NULL);

	free(clientId);
	free(serverURI);

	if (rc != MQTTCLIENT_SUCCESS) {
		LOG_WARN("%s: Failed to create MQTT client, rc=%d\n", __func__, rc);
		return 0;
	}

	LOG("%s: MQTT Client initialized\n",__func__);
    LOG_TRACE("%s: Exit\n", __func__);
    return 1;
}

int
MQTT_Publish(const char *topic, const char *payload, int qos, int retained) {
    if (!g_mqtt_client || !mqtt_client_isConnected(g_mqtt_client))
        return 0;

//    LOG_TRACE("%s: Entry\n", __func__);

    char *preTopic = cJSON_GetObjectItem(MQTTSettings, "preTopic") ? cJSON_GetObjectItem(MQTTSettings, "preTopic")->valuestring : NULL;
    char *fullTopic = NULL;
    
    if (preTopic && strlen(preTopic) > 0) {
        fullTopic = malloc(strlen(preTopic) + strlen(topic) + 2); // +2 for '/' and null terminator
        if (!fullTopic) {
            LOG_WARN("%s: Failed to allocate memory for full topic\n", __func__);
            return 0;
        }
        sprintf(fullTopic, "%s/%s", preTopic, topic);
    } else {
        fullTopic = (char *)topic;
    }

    MQTTClient_message pubmsg = MQTTClient_message_initializer;
    pubmsg.payload = (void*)payload;
    pubmsg.payloadlen = strlen(payload);
    pubmsg.qos = qos > 0 ? 0 : qos;  // Ensure QoS is not greater than 0
    pubmsg.retained = retained;

    MQTTClient_deliveryToken token;
    int rc = mqtt_client_publishMessage(g_mqtt_client, fullTopic, &pubmsg, &token);

    if (fullTopic != topic) {
        free(fullTopic);
    }

    if (rc != MQTTCLIENT_SUCCESS) {
        LOG_WARN("%s: Failed to publish message, return code %d\n", __func__, rc);
        return 0;
    }

//    LOG_TRACE("%s: Exit\n", __func__);
    return 1;
}

int
MQTT_Publish_JSON(const char *topic, cJSON *payload, int qos, int retained) {
    if (!payload || !g_mqtt_client || !mqtt_client_isConnected(g_mqtt_client))
        return 0;

//	LOG_TRACE("%s: Entry\n",__func__);
    
	cJSON* publish = cJSON_Duplicate(payload, 1);
	cJSON* additionalProperties = cJSON_GetObjectItem(MQTTSettings,"payload");

	if( additionalProperties ) {
		const char* name = cJSON_GetObjectItem(additionalProperties,"name")?cJSON_GetObjectItem(additionalProperties,"name")->valuestring:0;
		const char* location = cJSON_GetObjectItem(additionalProperties,"location")?cJSON_GetObjectItem(additionalProperties,"location")->valuestring:0;
		if( name && strlen(name) > 0 )
			cJSON_AddStringToObject(publish,"name",name);
		if( location && strlen(location) > 0 )
			cJSON_AddStringToObject(publish,"location",location);
	}
	cJSON_AddStringToObject(publish,"serial",ACAP_DEVICE_Prop("serial"));


	char *json = cJSON_PrintUnformatted(publish);
	if(!json) {
		LOG_TRACE("%s: Exit error\n",__func__);
		cJSON_Delete(publish);
		return 0;
	}
	cJSON_Delete(publish);
	int result = MQTT_Publish(topic, json, qos, retained );
	free(json);

//	LOG_TRACE("%s: Exit\n",__func__);
    
    return result;
}

int
MQTT_Publish_Binary(const char *topic, int payloadlen, void *payload, int qos, int retained) {
//    LOG_TRACE("%s: Entry\n", __func__);
    
    if (!g_mqtt_client || !mqtt_client_isConnected(g_mqtt_client)) {
        return 0;
    }

    char *preTopic = cJSON_GetObjectItem(MQTTSettings, "preTopic") ? cJSON_GetObjectItem(MQTTSettings, "preTopic")->valuestring : NULL;
    char *fullTopic = NULL;
    
    if (preTopic && strlen(preTopic) > 0) {
        fullTopic = malloc(strlen(preTopic) + strlen(topic) + 2); // +2 for '/' and null terminator
        if (!fullTopic) {
            LOG_WARN("%s: Failed to allocate memory for full topic\n", __func__);
            return 0;
        }
        sprintf(fullTopic, "%s/%s", preTopic, topic);
    } else {
        fullTopic = (char *)topic;
    }


    MQTTClient_message pubmsg = MQTTClient_message_initializer;
    pubmsg.payload = payload;
    pubmsg.payloadlen = payloadlen;
    pubmsg.qos = qos > 0 ? 0 : qos;  // Ensure QoS is not greater than 0
    pubmsg.retained = retained;

    MQTTClient_deliveryToken token;
    int rc = mqtt_client_publishMessage(g_mqtt_client, topic, &pubmsg, &token);

    if (fullTopic != topic) {
        free(fullTopic);
    }
    
    if (rc != MQTTCLIENT_SUCCESS) {
        LOG_WARN("%s: Failed to publish binary message, return code %d\n", __func__, rc);
        return 0;
    }

//    LOG_TRACE("%s: Exit\n", __func__);
    return 1;
}

int
MQTT_Subscribe(const char *topic) {

   if (!g_mqtt_client || !mqtt_client_isConnected(g_mqtt_client)) {
        return 0;
    }

   LOG_TRACE("%s: Entry\n", __func__);

    int rc = mqtt_client_subscribe(g_mqtt_client, topic, 0);  // QoS 0
    
    if (rc != MQTTCLIENT_SUCCESS) {
        LOG_WARN("%s: Failed to subscribe to topic %s, return code %d\n", __func__, topic, rc);
        return 0;
    }

    LOG_TRACE("%s: Exit\n", __func__);
    return 1;
}

int
MQTT_Unsubscribe(const char *topic) {
    LOG_TRACE("%s: Entry\n", __func__);
    
    if (!g_mqtt_client || !mqtt_client_isConnected(g_mqtt_client)) {
        LOG_WARN("%s: Exit error - no client\n", __func__);
        return 0;
    }

    int rc = mqtt_client_unsubscribe(g_mqtt_client, topic);
    if (rc != MQTTCLIENT_SUCCESS) {
        LOG_WARN("%s: Failed to unsubscribe from topic %s, return code %d\n", __func__, topic, rc);
        return 0;
    }

    LOG_TRACE("%s: Unsubscribed from topic %s successfully\n", __func__, topic);
    return 1;
}

void
MQTT_Cleanup() {
    if (g_mqtt_client && mqtt_client_isConnected(g_mqtt_client))
        MQTT_Disconnect();
    
    if (g_mqtt_client) {
        mqtt_client_destroy(&g_mqtt_client);
    }
    
    if (MQTTlibHandle) {
        dlclose(MQTTlibHandle);
        MQTTlibHandle = NULL;
    }
}

static int
MQTT_load_library(void) {

    LOG_TRACE("%s: Entry\n", __func__);
	
    if (MQTTlibHandle != NULL) {
        return 0;
    }
    
    DIR *dir;
    struct dirent *ent;
    char *libFile = NULL;
    char libPath[256] = {0};
    
    // Search for the library in /usr/lib
    if ((dir = opendir("/usr/lib")) == NULL) {
        char errMsg[256];
        snprintf(errMsg, sizeof(errMsg), "Could not open /usr/lib: %s", strerror(errno));
        LOG_WARN("%s: %s\n",__func__, errMsg);
        ACAP_STATUS_SetString("mqtt", "status", errMsg);
        return 0;
    }
    
    while ((ent = readdir(dir)) != NULL) {
        if (strstr(ent->d_name, "libpaho-mqtt3cs") != NULL) {
            libFile = strdup(ent->d_name);  // Make a copy of the filename
            break;
        }
    }
    closedir(dir);
    
    if (!libFile) {
        char errMsg[256];
        snprintf(errMsg, sizeof(errMsg), "MQTT library not found in /usr/lib");
        LOG_WARN("%s: %s\n", errMsg, __func__);
        return 0;
    }
    
    // Construct full path to the library
    snprintf(libPath, sizeof(libPath), "/usr/lib/%s", libFile);
    LOG_TRACE("Found MQTT library: %s\n", libPath);
    
    // Try to load the library with the full path
    MQTTlibHandle = dlopen(libPath, RTLD_LAZY);
    if (MQTTlibHandle == NULL) {
        char errMsg[256];
        snprintf(errMsg, sizeof(errMsg), "Failed to load MQTT library: %s", dlerror());
        LOG_WARN("%s: %s\n", __func__, errMsg);
        free(libFile);
        return 0;
    }
    
    dlerror();  // Clear any existing error
    
    // Load function pointers
    #define LOAD_SYMBOL(sym) \
        mqtt_client_##sym = dlsym(MQTTlibHandle, "MQTTClient_" #sym); \
        if (!mqtt_client_##sym) { \
            LOG_WARN("Error loading MQTTClient_" #sym ": %s\n", dlerror()); \
            goto cleanup_error; \
        }

    LOAD_SYMBOL(create)
    LOAD_SYMBOL(connect)
    LOAD_SYMBOL(disconnect)
    LOAD_SYMBOL(isConnected)
    LOAD_SYMBOL(publishMessage)
    LOAD_SYMBOL(destroy)
    LOAD_SYMBOL(subscribe)
    LOAD_SYMBOL(unsubscribe)
    LOAD_SYMBOL(setCallbacks)
    LOAD_SYMBOL(freeMessage)
    LOAD_SYMBOL(free)
	LOAD_SYMBOL(yield)

    #undef LOAD_SYMBOL
    
    LOG_TRACE("%s: Exit\n",__func__);
    return 1;

cleanup_error:
    dlclose(MQTTlibHandle);
    MQTTlibHandle = NULL;
    LOG_TRACE("%s: Exit error\n", __func__);
    return 0;
}

static int
MQTT_Load_Settings() {
    LOG_TRACE("%s: Entry\n", __func__);
	
    if (MQTTSettings) {
        return 0; // Already initialized
    }
    // Read default settings
    MQTTSettings = ACAP_FILE_Read("settings/mqtt.json");
    if (!MQTTSettings) {
        LOG_WARN("%s: Unable to parse default settings\n", __func__);
        return 0;
    }
    
    // Read saved settings and merge them
    cJSON* savedSettings = ACAP_FILE_Read("localdata/mqtt.json");
    LOG_TRACE("%s: File read: Settings: %s\n", __func__, savedSettings ? "OK" : "Failed");
    if (savedSettings) {
        LOG_TRACE("%s: Saved settings found\n", __func__);
        cJSON* prop = savedSettings->child;
        while (prop) {
            if (cJSON_GetObjectItem(MQTTSettings, prop->string)) {
                cJSON_ReplaceItemInObject(MQTTSettings, prop->string, cJSON_Duplicate(prop, 1));
            }
            prop = prop->next;
        }
        cJSON_Delete(savedSettings);
    }
    
    return 1;
}

static void
MQTT_HTTP_callback(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
    if (!MQTTSettings) {
        ACAP_HTTP_Respond_Error(response, 500, "Invalid settings");
        LOG_WARN("%s: Invalid settings not initialized\n", __func__);
        return;
    }
	
	LOG_TRACE("%s:\n",__func__);

	const char* json = ACAP_HTTP_Request_Param(request, "json");
	if(!json)
		json = ACAP_HTTP_Request_Param(request, "set");

    if (!json) {
		const char* action = ACAP_HTTP_Request_Param(request, "action");
		if( action ) {
			if( connectionMessgage )
				connectionMessgage(MQTT_DISCONNECTING);
			if( MQTT_Disconnect() )
				ACAP_HTTP_Respond_Text( response, "OK" );
			else
				ACAP_HTTP_Respond_Error( response, 400, "Failed disconnecting");
			cJSON_GetObjectItem(MQTTSettings,"connect")->type = cJSON_False;
			ACAP_FILE_Write( "localdata/mqtt.json", MQTTSettings );
			return;
		}
        ACAP_HTTP_Respond_JSON(response, MQTTSettings);
        return;
    }

    cJSON* settings = cJSON_Parse(json);
    if (!settings) {
        ACAP_HTTP_Respond_Error(response, 400, "Invalid JSON");
        LOG_WARN("Unable to parse json for MQTT settings\n");
        return;
    }

	cJSON* payload = cJSON_GetObjectItem(settings,"payload");

	if( payload ) {  //Just update the helper pyaload properties
		const char* name = cJSON_GetObjectItem(payload,"name")?cJSON_GetObjectItem(payload,"name")->valuestring:"";
		const char* location = cJSON_GetObjectItem(payload,"location")?cJSON_GetObjectItem(payload,"location")->valuestring:"";
		cJSON* mqttPayload = cJSON_GetObjectItem(MQTTSettings,"payload");
		if(!mqttPayload) {
			mqttPayload = cJSON_CreateObject();
			cJSON_AddStringToObject( payload,"name",name);
			cJSON_AddStringToObject( payload,"location",location);
			cJSON_AddItemToObject(MQTTSettings,"payload",mqttPayload);
		}
		cJSON_ReplaceItemInObject(mqttPayload,"name",cJSON_CreateString(name));	
		cJSON_ReplaceItemInObject(mqttPayload,"location",cJSON_CreateString(location));	
		ACAP_FILE_Write( "localdata/mqtt.json", MQTTSettings );
		ACAP_HTTP_Respond_Text(response,"Payload properties updated");
		return;
	}

	cJSON* setting = settings->child;
	while(setting) {
		if( cJSON_GetObjectItem(MQTTSettings,setting->string) )
			cJSON_ReplaceItemInObject(MQTTSettings,setting->string, cJSON_Duplicate(setting,1) );
		setting = setting->next;
	}
	ACAP_FILE_Write( "localdata/mqtt.json", MQTTSettings );
    cJSON_Delete(settings);
        
    if (!MQTT_SetupClient()) {
        ACAP_HTTP_Respond_Error(response, 400, "Unable to initialize client");
		return;
	}

	if(!MQTT_Connect()) {
        ACAP_HTTP_Respond_Error(response, 400, "Unable to connect client");
		return;
	}

    ACAP_HTTP_Respond_Text(response, "MQTT Updated");
}

int MQTT_Init( MQTT_Callback_Connection stateCallback, MQTT_Callback_Message messageCallback) {
    LOG_TRACE("%s: Entry\n", __func__);

    connectionMessgage = stateCallback;
    userSubscriptionCallback = messageCallback;
    connectionMessgage(MQTT_INITIALIZING);

    if (!MQTT_Load_Settings()) {
        return 0;
    }
    
    if (!MQTT_load_library()) {
        return 0;
    }

    CERTS_Init();

    if (!MQTT_SetupClient()) {
        return 0;
    }

    ACAP_HTTP_Node("mqtt", MQTT_HTTP_callback);
    
    if (!MQTT_Connect()) {
        return 0;
    }

    g_timeout_add_seconds(15, checkConnection, NULL);

    LOG_TRACE("%s: Exit\n", __func__);
    return 1;
}
