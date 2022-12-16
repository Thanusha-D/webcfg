/*
 * If not stated otherwise in this file or this component's Licenses.txt file the
 * following copyright and licenses apply:
 *
 * Copyright 2022 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/nameser.h>
#include <resolv.h>
#include <ctype.h>
#include <pthread.h>
#include <errno.h>
#include "webcfg_generic.h"
#include "webcfg_multipart.h"
#include "webcfg_mqtt.h"
#include "webcfg_rbus.h"

void on_connect(struct mosquitto *mosq, void *obj, int reason_code);
void on_subscribe(struct mosquitto *mosq, void *obj, int mid, int qos_count, const int *granted_qos);
void on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg);
void on_publish(struct mosquitto *mosq, void *obj, int mid);

static int g_mqttConnected = 0;
static int systemStatus = 0;
struct mosquitto *mosq = NULL;
static char g_deviceId[64]={'\0'};
//global flag to do bootupsync only once after connect and subscribe callback.
static int bootupsync = 0;
static int subscribeFlag = 0;

static char g_systemReadyTime[64]={'\0'};
static char g_FirmwareVersion[64]={'\0'};
static char g_bootTime[64]={'\0'};
static char g_productClass[64]={'\0'};
static char g_ModelName[64]={'\0'};
static char g_PartnerID[64]={'\0'};
static char g_AccountID[64]={'\0'};
static char g_NodeID[64] = { 0 };
static char *supportedVersion_header=NULL;
static char *supportedDocs_header=NULL;
static char *supplementaryDocs_header=NULL;

pthread_mutex_t mqtt_retry_mut=PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t mqtt_retry_con=PTHREAD_COND_INITIALIZER;

int get_global_mqtt_connected()
{
    return g_mqttConnected;
}

void reset_global_mqttConnected()
{
	g_mqttConnected = 0;
}

void set_global_mqttConnected()
{
	g_mqttConnected = 1;
}

void convertToUppercase(char* deviceId)
{
	int j =0;
	while (deviceId[j])
	{
		deviceId[j] = toupper(deviceId[j]);
		j++;
	}
}

void checkMqttParamSet()
{
	WebcfgInfo("checkMqttParamSet\n");
	pthread_mutex_lock(get_global_mqtt_mut());
	pthread_cond_wait(get_global_mqtt_cond(), get_global_mqtt_mut());
	pthread_mutex_unlock(get_global_mqtt_mut());
	WebcfgInfo("Received mqtt signal proceed to mqtt init\n");
}

void init_mqtt_timer (mqtt_timer_t *timer, int max_count)
{
  timer->count = 1;
  timer->max_count = max_count;
  timer->delay = 1;
  clock_gettime (CLOCK_REALTIME, &timer->ts);
}

unsigned update_mqtt_delay (mqtt_timer_t *timer)
{
  if (timer->count < timer->max_count) {
    timer->count += 1;
    timer->delay = timer->delay + timer->delay + 1;
    // 3,7,15,31 ..
  }
  return (unsigned) timer->delay;
}

unsigned mqtt_rand_secs (int random_num, unsigned max_secs)
{
  unsigned delay_secs = (unsigned) random_num & max_secs;
  if (delay_secs < 3)
    return delay_secs + 3;
  else
    return delay_secs;
}

unsigned mqtt_rand_nsecs (int random_num)
{
	/* random _num is in range 0..2147483648 */
	unsigned n = (unsigned) random_num >> 1;
	/* n is in range 0..1073741824 */
	if (n < 1000000000)
	  return n;
	return n - 1000000000;
}

void mqtt_add_timespec (struct timespec *t1, struct timespec *t2)
{
	t2->tv_sec += t1->tv_sec;
	t2->tv_nsec += t1->tv_nsec;
	if (t2->tv_nsec >= 1000000000) {
	  t2->tv_sec += 1;
	  t2->tv_nsec -= 1000000000;
	}
}

void mqtt_rand_expiration (int random_num1, int random_num2, mqtt_timer_t *timer, struct timespec *ts)
{
	unsigned max_secs = update_mqtt_delay (timer); // 3,7,15,31
	struct timespec ts_delay = {3, 0};

	if (max_secs > 3) {
	  ts_delay.tv_sec = mqtt_rand_secs (random_num1, max_secs);
	  ts_delay.tv_nsec = mqtt_rand_nsecs (random_num2);
	}
    WebcfgInfo("Waiting max delay %u mqttRetryTime %ld secs %ld usecs\n",
      max_secs, ts_delay.tv_sec, ts_delay.tv_nsec/1000);

	/* Add delay to expire time */
    mqtt_add_timespec (&ts_delay, ts);
}

/* mqtt_retry
 *
 * delays for the number of seconds specified in parameter timer
 * g_shutdown can break out of the delay.
 *
 * returns -1 pthread_cond_timedwait error
 *  1   shutdown
 *  0    delay taken
*/
static int mqtt_retry(mqtt_timer_t *timer)
{
  struct timespec ts;
  int rtn;

  clock_gettime(CLOCK_REALTIME, &ts);

  mqtt_rand_expiration(random(), random(), timer, &ts);

  pthread_mutex_lock(&mqtt_retry_mut);
  // The condition variable will only be set if we shut down.
  rtn = pthread_cond_timedwait(&mqtt_retry_con, &mqtt_retry_mut, &ts);
  pthread_mutex_unlock(&mqtt_retry_mut);

  if (get_global_shutdown())
    return MQTT_RETRY_SHUTDOWN;
  if ((rtn != 0) && (rtn != ETIMEDOUT)) {
    WebcfgError ("pthread_cond_timedwait error (%d) in mqtt_retry.\n", rtn);
    return MQTT_RETRY_ERR;
  }

  return MQTT_DELAY_TAKEN;
}

//Initialize mqtt library and connect to mqtt broker
bool webcfg_mqtt_init(int status, char *systemreadytime)
{
	char *client_id , *username = NULL;
	char hostname[256] = { 0 };
	int rc = 1;
	char PORT[32] = { 0 };
	int port = 0;
	mqtt_timer_t mqtt_timer;

	checkMqttParamSet();
	res_init();
	WebcfgInfo("Initializing MQTT library\n");

	mosquitto_lib_init();

	systemStatus = status;
	WebcfgInfo("systemStatus is %d\n", systemStatus);

	if(systemreadytime !=NULL)
	{
		char *systemReadyTime = strdup(systemreadytime);
		strncpy(g_systemReadyTime, systemReadyTime, sizeof(g_systemReadyTime)-1);
		WebcfgInfo("g_systemReadyTime is %s\n", g_systemReadyTime);
	}

	int clean_session = true;

	Get_Mqtt_NodeId(g_NodeID);
	WebcfgInfo("g_NodeID fetched from Get_Mqtt_NodeId is %s\n", g_NodeID);
	client_id = strdup(g_NodeID);
	WebcfgInfo("client_id is %s\n", client_id);
	
	if(client_id !=NULL)
	{
		username = client_id;
		WebcfgInfo("client_id is %s username is %s\n", client_id, username);

		execute_mqtt_script(OPENSYNC_CERT);

		Get_Mqtt_Broker(hostname);
		if(hostname != NULL && strlen(hostname)>0)
		{
			WebcfgInfo("The hostname is %s\n", hostname);
		}
		else
		{
			WebcfgError("Invalid config, hostname is NULL\n");
			return MOSQ_ERR_INVAL;
		}

		if(client_id !=NULL)
		{
			mosq = mosquitto_new(client_id, clean_session, NULL);
		}
		else
		{
			WebcfgInfo("client_id is NULL, init with clean_session true\n");
			mosq = mosquitto_new(NULL, true, NULL);
		}
		if(!mosq)
		{
			WebcfgError("Error initializing mosq instance\n");
			return MOSQ_ERR_NOMEM;
		}

		Get_Mqtt_Port(PORT);
		WebcfgInfo("PORT fetched from TR181 is %s\n", PORT);
		if(strlen(PORT) > 0)
		{
			port = atoi(PORT);
		}
		else
		{
			port = MQTT_PORT;
		}
		WebcfgInfo("port int %d\n", port);

		struct libmosquitto_tls *tls;
		tls = malloc (sizeof (struct libmosquitto_tls));
		if(tls)
		{
			memset(tls, 0, sizeof(struct libmosquitto_tls));

			char * CAFILE, *CERTFILE , *KEYFILE = NULL;

			get_from_file("CA_FILE_PATH=", &CAFILE, MQTT_CONFIG_FILE);
			get_from_file("CERT_FILE_PATH=", &CERTFILE, MQTT_CONFIG_FILE);
			get_from_file("KEY_FILE_PATH=", &KEYFILE, MQTT_CONFIG_FILE);

			if(CAFILE !=NULL && CERTFILE!=NULL && KEYFILE !=NULL)
			{
				WebcfgInfo("CAFILE %s, CERTFILE %s, KEYFILE %s MOSQ_TLS_VERSION %s\n", CAFILE, CERTFILE, KEYFILE, MOSQ_TLS_VERSION);

				tls->cafile = CAFILE;
				tls->certfile = CERTFILE;
				tls->keyfile = KEYFILE;
				tls->tls_version = MOSQ_TLS_VERSION;

				rc = mosquitto_tls_set(mosq, tls->cafile, tls->capath, tls->certfile, tls->keyfile, tls->pw_callback);
				WebcfgInfo("mosquitto_tls_set rc %d\n", rc);
				if(rc)
				{
					WebcfgError("Failed in mosquitto_tls_set %d %s\n", rc, mosquitto_strerror(rc));
					mosquitto_destroy(mosq);
					return rc;
				}
				rc = mosquitto_tls_opts_set(mosq, tls->cert_reqs, tls->tls_version, tls->ciphers);
				WebcfgInfo("mosquitto_tls_opts_set rc %d\n", rc);
				if(rc)
				{
					WebcfgError("Failed in mosquitto_tls_opts_set %d %s\n", rc, mosquitto_strerror(rc));
					mosquitto_destroy(mosq);
					return rc;
				}

				//connect to mqtt broker
				mosquitto_connect_callback_set(mosq, on_connect);
				mosquitto_subscribe_callback_set(mosq, on_subscribe);
				mosquitto_message_callback_set(mosq, on_message);
				mosquitto_publish_callback_set(mosq, on_publish);

				WebcfgInfo("port %d\n", port);

				init_mqtt_timer(&mqtt_timer, MAX_MQTT_RETRY);
				while(rc != MOSQ_ERR_SUCCESS)
				{
					rc = mosquitto_connect(mosq, hostname, port, KEEPALIVE);
					WebcfgInfo("mosquitto_connect rc %d\n", rc);
					if(rc != MOSQ_ERR_SUCCESS)
					{

						WebcfgError("mqtt connect Error: %s\n", mosquitto_strerror(rc));
						if(mqtt_retry(&mqtt_timer) != MQTT_DELAY_TAKEN)
						{
							mosquitto_destroy(mosq);
							return rc;
						}
					}
					else
					{
						WebcfgInfo("mqtt broker connect success %d\n", rc);
						set_global_mqttConnected();
					}
				}
				/*WebcfgInfo("mosquitto_loop_forever\n");
				rc = mosquitto_loop_forever(mosq, -1, 1);*/
				rc = mosquitto_loop_start(mosq);
				if(rc != MOSQ_ERR_SUCCESS)
				{
					mosquitto_destroy(mosq);
					WebcfgError("mosquitto_loop_start Error: %s\n", mosquitto_strerror(rc));
					return rc;
				}
				WebcfgInfo("after loop rc is %d\n", rc);
			}
			else
			{
				WebcfgError("Failed to get tls cert files\n");
				return 1;
			}
		}
		else
		{
			WebcfgError("Allocation failed\n");
			return MOSQ_ERR_NOMEM;
		}
	}
	else
	{
		WebcfgError("Failed to get client_id\n");
		return 1;

	}
	return rc;
}

// callback called when the client receives a CONNACK message from the broker
void on_connect(struct mosquitto *mosq, void *obj, int reason_code)
{
        int rc;
	char topic[256] = { 0 };
        WebcfgInfo("on_connect: reason_code %d %s\n", reason_code, mosquitto_connack_string(reason_code));
        if(reason_code != 0)
	{
		WebcfgError("on_connect received error\n");
                //reconnect
                mosquitto_disconnect(mosq);
		return;
        }

	if(!subscribeFlag)
	{
		snprintf(topic,MAX_MQTT_LEN,"%s%s", MQTT_SUBSCRIBE_TOPIC_PREFIX,g_NodeID);
		if(topic != NULL && strlen(topic)>0)
		{
			WebcfgInfo("subscribe to topic %s\n", topic);
		}

		rc = mosquitto_subscribe(mosq, NULL, topic, 1);

		if(rc != MOSQ_ERR_SUCCESS)
		{
			WebcfgError("Error subscribing: %s\n", mosquitto_strerror(rc));
			mosquitto_disconnect(mosq);
		}
		else
		{
			WebcfgInfo("subscribe to topic %s success\n", topic);
			subscribeFlag = 1;
		}
	}
}


// callback called when the broker sends a SUBACK in response to a SUBSCRIBE.
void on_subscribe(struct mosquitto *mosq, void *obj, int mid, int qos_count, const int *granted_qos)
{
        int i;
        bool have_subscription = false;

	WebcfgInfo("on_subscribe callback: qos_count %d\n", qos_count);
        //SUBSCRIBE can contain many topics at once
        for(i=0; i<qos_count; i++)
	{
                WebcfgInfo("on_subscribe: %d:granted qos = %d\n", i, granted_qos[i]);
                if(granted_qos[i] <= 2)
		{
                        have_subscription = true;
                }
		WebcfgInfo("on_subscribe: bootupsync %d\n", bootupsync);
		if(!bootupsync)
		{
			WebcfgInfo("mqtt is connected and subscribed to topic, trigger bootup sync to cloud.\n");
			int ret = triggerBootupSync();
			if(ret)
			{
				WebcfgInfo("Triggered bootup sync via mqtt\n");
			}
			else
			{
				WebcfgError("Failed to trigger bootup sync via mqtt\n");
			}
			bootupsync = 1;
		}
        }
        if(have_subscription == false)
	{
                WebcfgError("Error: All subscriptions rejected.\n");
                mosquitto_disconnect(mosq);
        }
}

/* callback called when the client receives a message. */
void on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg)
{
	if(msg !=NULL)
	{
		if(msg->payload !=NULL)
		{
			WebcfgInfo("Received message from %s qos %d payloadlen %d payload %s\n", msg->topic, msg->qos, msg->payloadlen, (char *)msg->payload);

			int dataSize = msg->payloadlen;
			char * data = malloc(sizeof(char) * dataSize+1);
			memset(data, 0, sizeof(char) * dataSize+1);
			data = memcpy(data, (char *) msg->payload, dataSize+1);
			data[dataSize] = '\0';

			int status = 0;
			WebcfgInfo("Received dataSize is %d\n", dataSize);
			WebcfgInfo("write to file /tmp/subscribe_message.bin\n");
			writeToDBFile("/tmp/subscribe_message.bin",(char *)data,dataSize);
			WebcfgInfo("write to file done\n");
			status = processPayload((char *)data, dataSize);
			WebcfgInfo("processPayload status %d\n", status);
		}
		else
		{
			WebcfgError("Received payload from mqtt is NULL\n");
		}
	}
	else
	{
		WebcfgError("Received message from mqtt is NULL\n");
	}
}

void on_publish(struct mosquitto *mosq, void *obj, int mid)
{
        WebcfgInfo("Message with mid %d has been published.\n", mid);
}

/* This function pretends to read some data from a sensor and publish it.*/
void publish_notify_mqtt(char *pub_topic, void *payload, ssize_t len, char * dest)
{
        int rc;
	if(dest != NULL)
	{
		ssize_t payload_len = 0;
		char * pub_payload = createMqttPubHeader(payload, dest, &payload_len);
		if(pub_payload != NULL)
		{
			len = payload_len;
			WEBCFG_FREE(payload);
			payload = (char *) malloc(sizeof(char) * 1024);
			payload = strdup(pub_payload);

			WEBCFG_FREE(pub_payload);
		}
	}

	if(pub_topic == NULL)
	{
		char publish_topic[256] = { 0 };
		char locationID[256] = { 0 };

		Get_Mqtt_LocationId(locationID);
		WebcfgInfo("locationID fetched from tr181 is %s\n", locationID);
		snprintf(publish_topic, MAX_MQTT_LEN, "%s%s/%s", MQTT_PUBLISH_NOTIFY_TOPIC_PREFIX, g_NodeID,locationID);
		if(strlen(publish_topic)>0)
		{
			WebcfgInfo("publish_topic fetched from tr181 is %s\n", publish_topic);
			pub_topic = strdup(publish_topic);
			WebcfgInfo("pub_topic from file is %s\n", pub_topic);
		}
		else
		{
			WebcfgError("Failed to fetch publish topic\n");
		}
	}
	else
	{
		WebcfgInfo("pub_topic is %s\n", pub_topic);
	}
	WebcfgInfo("Payload published is \n%s\n", (char*)payload);
	//writeToDBFile("/tmp/payload.bin", (char *)payload, len);
        rc = mosquitto_publish(mosq, NULL, pub_topic, len, payload, 2, false);
	WebcfgInfo("Publish rc %d\n", rc);
        if(rc != MOSQ_ERR_SUCCESS)
	{
                WebcfgError("Error publishing: %s\n", mosquitto_strerror(rc));
        }
	else
	{
		WebcfgInfo("Publish payload success %d\n", rc);
	}
	mosquitto_loop(mosq, 0, 1);
	WebcfgInfo("Publish mosquitto_loop done\n");
}

void get_from_file(char *key, char **val, char *filepath)
{
        FILE *fp = fopen(filepath, "r");

        if (NULL != fp)
        {
                char str[255] = {'\0'};
                while (fgets(str, sizeof(str), fp) != NULL)
                {
                    char *value = NULL;

                    if(NULL != (value = strstr(str, key)))
                    {
                        value = value + strlen(key);
                        value[strlen(value)-1] = '\0';
                        *val = strdup(value);
                        break;
                    }

                }
                fclose(fp);
        }

        if (NULL == *val)
        {
                WebcfgError("WebConfig val is not present in file\n");

        }
        else
        {
                WebcfgDebug("val fetched is %s\n", *val);
        }
}

int triggerBootupSync()
{
	char *mqttheaderList = NULL;
	mqttheaderList = (char *) malloc(sizeof(char) * 1024);
	char *pub_get_topic = NULL;

	if(mqttheaderList != NULL)
	{
		WebcfgInfo("B4 createMqttHeader\n");
		createMqttHeader(&mqttheaderList);
		if(mqttheaderList !=NULL)
		{
			WebcfgInfo("mqttheaderList generated is \n%s len %zu\n", mqttheaderList, strlen(mqttheaderList));
			char publish_get_topic[256] = { 0 };
			char locationID[256] = { 0 };
			Get_Mqtt_LocationId(locationID);
			WebcfgInfo("locationID is %s\n", locationID);
			snprintf(publish_get_topic, MAX_MQTT_LEN, "%s%s/%s", MQTT_PUBLISH_GET_TOPIC_PREFIX, g_NodeID,locationID);
			if(strlen(publish_get_topic) >0)
			{
				pub_get_topic = strdup(publish_get_topic);
				WebcfgInfo("pub_get_topic from tr181 is %s\n", pub_get_topic);
				publish_notify_mqtt(pub_get_topic, (void*)mqttheaderList, strlen(mqttheaderList), NULL);
				WebcfgInfo("triggerBootupSync published to topic %s\n", pub_get_topic);
			}
			else
			{
				WebcfgError("Failed to fetch publish_get_topic\n");
			}
		}
		else
		{
			WebcfgError("Failed to generate mqttheaderList\n");
			return 0;
		}
	}
	else
	{
		WebcfgError("Failed to allocate mqttheaderList\n");
		return 0;
	}
	WebcfgInfo("triggerBootupSync end\n");
	return 1;
}

int createMqttHeader(char **header_list)
{
	char *version_header = NULL;
	char *accept_header = NULL;
	char *status_header=NULL;
	char *schema_header=NULL;
	char *bootTime = NULL, *bootTime_header = NULL;
	char *FwVersion = NULL, *FwVersion_header=NULL;
	char *supportedDocs = NULL;
	char *supportedVersion = NULL;
	char *supplementaryDocs = NULL;
        char *productClass = NULL, *productClass_header = NULL;
	char *ModelName = NULL, *ModelName_header = NULL;
	char *systemReadyTime = NULL, *systemReadyTime_header=NULL;
	char *telemetryVersion_header = NULL;
	char *PartnerID = NULL, *PartnerID_header = NULL;
	char *AccountID = NULL, *AccountID_header = NULL;
	char *deviceId_header = NULL, *doc_header = NULL;
	char *contenttype_header = NULL, *contentlen_header = NULL;
	struct timespec cTime;
	char currentTime[32];
	char *currentTime_header=NULL;
	char *uuid_header = NULL;
	char *transaction_uuid = NULL;
	char version[512]={'\0'};
	size_t supported_doc_size = 0;
	size_t supported_version_size = 0;
	size_t supplementary_docs_size = 0;
	char docList[512] = {'\0'};

	WebcfgInfo("Start of createMqttHeader\n");

	if( get_deviceMAC() != NULL && strlen(get_deviceMAC()) !=0 )
	{
	       strncpy(g_deviceId, get_deviceMAC(), sizeof(g_deviceId)-1);
	       WebcfgInfo("g_deviceId fetched is %s\n", g_deviceId);
	}

	if(strlen(g_deviceId))
	{
		deviceId_header = (char *) malloc(sizeof(char)*MAX_BUF_SIZE);
		if(deviceId_header !=NULL)
		{
			convertToUppercase(g_deviceId);
			snprintf(deviceId_header, MAX_BUF_SIZE, "Device-Id: %s", g_deviceId);
			//snprintf(deviceId_header, MAX_BUF_SIZE, "Mac: %s", tmp);
			WebcfgInfo("deviceId_header formed %s\n", deviceId_header);
			//WEBCFG_FREE(deviceId_header);
		}
	}
	else
	{
		WebcfgError("Failed to get deviceId\n");
	}

	if(!get_global_supplementarySync())
	{
		//Update query param in the URL based on the existing doc names from db
		getConfigDocList(docList);
		WebcfgInfo("docList is %s\n", docList);

		doc_header = (char *) malloc(sizeof(char)*MAX_BUF_SIZE);
		if(doc_header !=NULL)
		{
			snprintf(doc_header, MAX_BUF_SIZE, "\r\nDoc-Name:%s", ((strlen(docList)!=0) ? docList : "NULL"));
			WebcfgInfo("doc_header formed %s\n", doc_header);
			//WEBCFG_FREE(doc_header);
		}
	}
	if(!get_global_supplementarySync())
	{
		version_header = (char *) malloc(sizeof(char)*MAX_BUF_SIZE);
		if(version_header !=NULL)
		{
			refreshConfigVersionList(version, 0);
			snprintf(version_header, MAX_BUF_SIZE, "\r\nIF-NONE-MATCH:%s", ((strlen(version)!=0) ? version : "0"));
			WebcfgInfo("version_header formed %s\n", version_header);
			//WEBCFG_FREE(version_header);
		}
	}
	accept_header = (char *) malloc(sizeof(char)*MAX_BUF_SIZE);
	if(accept_header !=NULL)
	{
		snprintf(accept_header, MAX_BUF_SIZE, "\r\nAccept: application/msgpack");
	}

	schema_header = (char *) malloc(sizeof(char)*MAX_BUF_SIZE);
	if(schema_header !=NULL)
	{
		snprintf(schema_header, MAX_BUF_SIZE, "\r\nSchema-Version: %s", "v2.0");
		WebcfgInfo("schema_header formed %s\n", schema_header);
		//WEBCFG_FREE(schema_header);
	}

	if(!get_global_supplementarySync())
	{
		if(supportedVersion_header == NULL)
		{
			supportedVersion = getsupportedVersion();

			if(supportedVersion !=NULL)
			{
				supported_version_size = strlen(supportedVersion)+strlen("X-System-Schema-Version: ");
				supportedVersion_header = (char *) malloc(supported_version_size+1);
				memset(supportedVersion_header,0,supported_version_size+1);
				WebcfgDebug("supportedVersion fetched is %s\n", supportedVersion);
				snprintf(supportedVersion_header, supported_version_size+1, "\r\nX-System-Schema-Version: %s", supportedVersion);
				WebcfgInfo("supportedVersion_header formed %s\n", supportedVersion_header);
			}
			else
			{
				WebcfgInfo("supportedVersion fetched is NULL\n");
			}
		}
		else
		{
			WebcfgInfo("supportedVersion_header formed %s\n", supportedVersion_header);
		}

		if(supportedDocs_header == NULL)
		{
			supportedDocs = getsupportedDocs();

			if(supportedDocs !=NULL)
			{
				supported_doc_size = strlen(supportedDocs)+strlen("X-System-Supported-Docs: ");
				supportedDocs_header = (char *) malloc(supported_doc_size+1);
				memset(supportedDocs_header,0,supported_doc_size+1);
				WebcfgDebug("supportedDocs fetched is %s\n", supportedDocs);
				snprintf(supportedDocs_header, supported_doc_size+1, "\r\nX-System-Supported-Docs: %s", supportedDocs);
				WebcfgInfo("supportedDocs_header formed %s\n", supportedDocs_header);
			}
			else
			{
				WebcfgInfo("SupportedDocs fetched is NULL\n");
			}
		}
		else
		{
			WebcfgInfo("supportedDocs_header formed %s\n", supportedDocs_header);
		}
	}
	else
	{
		if(supplementaryDocs_header == NULL)
		{
			supplementaryDocs = getsupplementaryDocs();

			if(supplementaryDocs !=NULL)
			{
				supplementary_docs_size = strlen(supplementaryDocs)+strlen("X-System-SupplementaryService-Sync: ");
				supplementaryDocs_header = (char *) malloc(supplementary_docs_size+1);
				memset(supplementaryDocs_header,0,supplementary_docs_size+1);
				WebcfgDebug("supplementaryDocs fetched is %s\n", supplementaryDocs);
				snprintf(supplementaryDocs_header, supplementary_docs_size+1, "\r\nX-System-SupplementaryService-Sync: %s", supplementaryDocs);
				WebcfgInfo("supplementaryDocs_header formed %s\n", supplementaryDocs_header);
			}
			else
			{
				WebcfgInfo("supplementaryDocs fetched is NULL\n");
			}
		}
		else
		{
			WebcfgInfo("supplementaryDocs_header formed %s\n", supplementaryDocs_header);
		}
	}

	if(strlen(g_bootTime) ==0)
	{
		bootTime = getDeviceBootTime();
		if(bootTime !=NULL)
		{
		       strncpy(g_bootTime, bootTime, sizeof(g_bootTime)-1);
		       WebcfgDebug("g_bootTime fetched is %s\n", g_bootTime);
		       WEBCFG_FREE(bootTime);
		}
	}

	if(strlen(g_bootTime))
	{
		bootTime_header = (char *) malloc(sizeof(char)*MAX_BUF_SIZE);
		if(bootTime_header !=NULL)
		{
			snprintf(bootTime_header, MAX_BUF_SIZE, "\r\nX-System-Boot-Time: %s", g_bootTime);
			WebcfgInfo("bootTime_header formed %s\n", bootTime_header);
			//WEBCFG_FREE(bootTime_header);
		}
	}
	else
	{
		WebcfgError("Failed to get bootTime\n");
	}

	if(strlen(g_FirmwareVersion) ==0)
	{
		FwVersion = getFirmwareVersion();
		if(FwVersion !=NULL)
		{
		       strncpy(g_FirmwareVersion, FwVersion, sizeof(g_FirmwareVersion)-1);
		       WebcfgDebug("g_FirmwareVersion fetched is %s\n", g_FirmwareVersion);
		       WEBCFG_FREE(FwVersion);
		}
	}

	if(strlen(g_FirmwareVersion))
	{
		FwVersion_header = (char *) malloc(sizeof(char)*MAX_BUF_SIZE);
		if(FwVersion_header !=NULL)
		{
			snprintf(FwVersion_header, MAX_BUF_SIZE, "\r\nX-System-Firmware-Version: %s", g_FirmwareVersion);
			WebcfgInfo("FwVersion_header formed %s\n", FwVersion_header);
			//WEBCFG_FREE(FwVersion_header);
		}
	}
	else
	{
		WebcfgError("Failed to get FwVersion\n");
	}

	status_header = (char *) malloc(sizeof(char)*MAX_BUF_SIZE);
	if(status_header !=NULL)
	{
		if(systemStatus !=0)
		{
			snprintf(status_header, MAX_BUF_SIZE, "\r\nX-System-Status: %s", "Non-Operational");
		}
		else
		{
			snprintf(status_header, MAX_BUF_SIZE, "\r\nX-System-Status: %s", "Operational");
		}
		WebcfgInfo("status_header formed %s\n", status_header);
		//WEBCFG_FREE(status_header);
	}

	memset(currentTime, 0, sizeof(currentTime));
	getCurrent_Time(&cTime);
	snprintf(currentTime,sizeof(currentTime),"%d",(int)cTime.tv_sec);
	currentTime_header = (char *) malloc(sizeof(char)*MAX_BUF_SIZE);
	if(currentTime_header !=NULL)
	{
		snprintf(currentTime_header, MAX_BUF_SIZE, "\r\nX-System-Current-Time: %s", currentTime);
		WebcfgInfo("currentTime_header formed %s\n", currentTime_header);
		//WEBCFG_FREE(currentTime_header);
	}

        if(strlen(g_systemReadyTime) ==0)
        {
		WebcfgInfo("get_global_systemReadyTime\n");
                systemReadyTime = get_global_systemReadyTime();
                if(systemReadyTime !=NULL)
                {
                       strncpy(g_systemReadyTime, systemReadyTime, sizeof(g_systemReadyTime)-1);
                       WebcfgInfo("g_systemReadyTime fetched is %s\n", g_systemReadyTime);
                       //WEBCFG_FREE(systemReadyTime);
                }
        }

        if(strlen(g_systemReadyTime))
        {
                systemReadyTime_header = (char *) malloc(sizeof(char)*MAX_BUF_SIZE);
                if(systemReadyTime_header !=NULL)
                {
			snprintf(systemReadyTime_header, MAX_BUF_SIZE, "\r\nX-System-Ready-Time: %s", g_systemReadyTime);
	                WebcfgInfo("systemReadyTime_header formed %s\n", systemReadyTime_header);
	                //WEBCFG_FREE(systemReadyTime_header);
                }
        }
        else
        {
                WebcfgDebug("Failed to get systemReadyTime\n");
        }

	if(transaction_uuid == NULL)
	{
		transaction_uuid = generate_trans_uuid();
	}

	if(transaction_uuid !=NULL)
	{
		uuid_header = (char *) malloc(sizeof(char)*MAX_BUF_SIZE);
		if(uuid_header !=NULL)
		{
			snprintf(uuid_header, MAX_BUF_SIZE, "\r\nTransaction-ID: %s", transaction_uuid);
			WebcfgInfo("uuid_header formed %s\n", uuid_header);
			//char *trans_uuid = strdup(transaction_uuid);
			//WEBCFG_FREE(transaction_uuid);
			//WEBCFG_FREE(uuid_header);
		}
	}
	else
	{
		WebcfgError("Failed to generate transaction_uuid\n");
	}

	if(strlen(g_productClass) ==0)
	{
		productClass = getProductClass();
		if(productClass !=NULL)
		{
		       strncpy(g_productClass, productClass, sizeof(g_productClass)-1);
		       WebcfgDebug("g_productClass fetched is %s\n", g_productClass);
		       WEBCFG_FREE(productClass);
		}
	}

	if(strlen(g_productClass))
	{
		productClass_header = (char *) malloc(sizeof(char)*MAX_BUF_SIZE);
		if(productClass_header !=NULL)
		{
			snprintf(productClass_header, MAX_BUF_SIZE, "\r\nX-System-Product-Class: %s", g_productClass);
			WebcfgInfo("productClass_header formed %s\n", productClass_header);
			//WEBCFG_FREE(productClass_header);
		}
	}
	else
	{
		WebcfgError("Failed to get productClass\n");
	}

	if(strlen(g_ModelName) ==0)
	{
		ModelName = getModelName();
		if(ModelName !=NULL)
		{
		       strncpy(g_ModelName, ModelName, sizeof(g_ModelName)-1);
		       WebcfgDebug("g_ModelName fetched is %s\n", g_ModelName);
		       WEBCFG_FREE(ModelName);
		}
	}

	if(strlen(g_ModelName))
	{
		ModelName_header = (char *) malloc(sizeof(char)*MAX_BUF_SIZE);
		if(ModelName_header !=NULL)
		{
			snprintf(ModelName_header, MAX_BUF_SIZE, "\r\nX-System-Model-Name: %s", g_ModelName);
			WebcfgInfo("ModelName_header formed %s\n", ModelName_header);
			//WEBCFG_FREE(ModelName_header);
		}
	}
	else
	{
		WebcfgError("Failed to get ModelName\n");
	}

	if(strlen(g_PartnerID) ==0)
	{
		PartnerID = getPartnerID();
		if(PartnerID !=NULL)
		{
		       strncpy(g_PartnerID, PartnerID, sizeof(g_PartnerID)-1);
		       WebcfgDebug("g_PartnerID fetched is %s\n", g_PartnerID);
		       WEBCFG_FREE(PartnerID);
		}
	}

	if(strlen(g_PartnerID))
	{
		PartnerID_header = (char *) malloc(sizeof(char)*MAX_BUF_SIZE);
		if(PartnerID_header !=NULL)
		{
			snprintf(PartnerID_header, MAX_BUF_SIZE, "\r\nX-System-PartnerID: %s", g_PartnerID);
			WebcfgInfo("PartnerID_header formed %s\n", PartnerID_header);
			//WEBCFG_FREE(PartnerID_header);
		}
	}
	else
	{
		WebcfgError("Failed to get PartnerID\n");
	}

	contenttype_header = (char *) malloc(sizeof(char)*MAX_BUF_SIZE);
	if(contenttype_header !=NULL)
	{
		snprintf(contenttype_header, MAX_BUF_SIZE, "\r\nContent-type: application/json");
		WebcfgInfo("contenttype_header formed %s\n", contenttype_header);
	}
	contentlen_header = (char *) malloc(sizeof(char)*MAX_BUF_SIZE);
	if(contentlen_header !=NULL)
	{
		snprintf(contentlen_header, MAX_BUF_SIZE, "\r\nContent-length: 0");
		WebcfgInfo("contentlen_header formed %s\n", contentlen_header);
	}
	//Addtional headers for telemetry sync
	if(get_global_supplementarySync())
	{
		telemetryVersion_header = (char *) malloc(sizeof(char)*MAX_BUF_SIZE);
		if(telemetryVersion_header !=NULL)
		{
			snprintf(telemetryVersion_header, MAX_BUF_SIZE, "\r\nX-System-Telemetry-Profile-Version: %s", "2.0");
			WebcfgInfo("telemetryVersion_header formed %s\n", telemetryVersion_header);
			//WEBCFG_FREE(telemetryVersion_header);
		}

		if(strlen(g_AccountID) ==0)
		{
			AccountID = getAccountID();
			if(AccountID !=NULL)
			{
			       strncpy(g_AccountID, AccountID, sizeof(g_AccountID)-1);
			       WebcfgDebug("g_AccountID fetched is %s\n", g_AccountID);
			       WEBCFG_FREE(AccountID);
			}
		}

		if(strlen(g_AccountID))
		{
			AccountID_header = (char *) malloc(sizeof(char)*MAX_BUF_SIZE);
			if(AccountID_header !=NULL)
			{
				snprintf(AccountID_header, MAX_BUF_SIZE, "\r\nX-System-AccountID: %s", g_AccountID);
				WebcfgInfo("AccountID_header formed %s\n", AccountID_header);
				//WEBCFG_FREE(AccountID_header);
			}
		}
		else
		{
			WebcfgError("Failed to get AccountID\n");
		}
	}
	if(!get_global_supplementarySync())
	{
		WebcfgInfo("Framing primary sync header\n");
		snprintf(*header_list, 1024, "%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s\r\n\r\n", (deviceId_header!=NULL)?deviceId_header:"",(doc_header!=NULL)?doc_header:"", (version_header!=NULL)?version_header:"", (accept_header!=NULL)?accept_header:"", (schema_header!=NULL)?schema_header:"", (supportedVersion_header!=NULL)?supportedVersion_header:"", (supportedDocs_header!=NULL)?supportedDocs_header:"", (bootTime_header!=NULL)?bootTime_header:"", (FwVersion_header!=NULL)?FwVersion_header:"", (status_header!=NULL)?status_header:"", (currentTime_header!=NULL)?currentTime_header:"", (systemReadyTime_header!=NULL)?systemReadyTime_header:"", (uuid_header!=NULL)?uuid_header:"", (productClass_header!=NULL)?productClass_header:"", (ModelName_header!=NULL)?ModelName_header:"", (contenttype_header!=NULL)?contenttype_header:"", (contentlen_header!=NULL)?contentlen_header:"",(PartnerID_header!=NULL)?PartnerID_header:"");
	}
	else
	{
		WebcfgInfo("Framing supplementary sync header\n");
		snprintf(*header_list, 1024, "%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s\r\n\r\n", (deviceId_header!=NULL)?deviceId_header:"",(doc_header!=NULL)?doc_header:"", (version_header!=NULL)?version_header:"", (accept_header!=NULL)?accept_header:"", (schema_header!=NULL)?schema_header:"", (supportedVersion_header!=NULL)?supportedVersion_header:"", (supportedDocs_header!=NULL)?supportedDocs_header:"", (bootTime_header)?bootTime_header:"", (FwVersion_header)?FwVersion_header:"", (status_header)?status_header:"", (currentTime_header)?currentTime_header:"", (systemReadyTime_header!=NULL)?systemReadyTime_header:"", (uuid_header!=NULL)?uuid_header:"", (productClass_header!=NULL)?productClass_header:"", (ModelName_header!=NULL)?ModelName_header:"",(contenttype_header!=NULL)?contenttype_header:"", (contentlen_header!=NULL)?contentlen_header:"", (PartnerID_header!=NULL)?PartnerID_header:"",(supplementaryDocs_header!=NULL)?supplementaryDocs_header:"", (telemetryVersion_header!=NULL)?telemetryVersion_header:"", (AccountID_header!=NULL)?AccountID_header:"");
	}
	writeToDBFile("/tmp/header_list.txt", *header_list, strlen(*header_list));
	WebcfgInfo("mqtt header_list is \n%s\n", *header_list);
	return 0;
}

char * createMqttPubHeader(char * payload, char * dest, ssize_t * payload_len)
{
	char * destination = NULL;
	char * content_type = NULL;
	char * content_length = NULL;
	char *pub_headerlist = NULL;

	pub_headerlist = (char *) malloc(sizeof(char) * 1024);

	if(pub_headerlist != NULL)
	{
		if(payload != NULL)
		{
			if(dest != NULL)
			{
				destination = (char *) malloc(sizeof(char)*MAX_BUF_SIZE);
				if(destination !=NULL)
				{
					snprintf(destination, MAX_BUF_SIZE, "Destination: %s", dest);
					WebcfgInfo("destination formed %s\n", destination);
				}
			}

			content_type = (char *) malloc(sizeof(char)*MAX_BUF_SIZE);
			if(content_type !=NULL)
			{
				snprintf(content_type, MAX_BUF_SIZE, "\r\nContent-type: application/json");
				WebcfgInfo("content_type formed %s\n", content_type);
			}

			content_length = (char *) malloc(sizeof(char)*MAX_BUF_SIZE);
			if(content_length !=NULL)
			{
				snprintf(content_length, MAX_BUF_SIZE, "\r\nContent-length: %zu", strlen(payload));
				WebcfgInfo("content_length formed %s\n", content_length);
			}

			WebcfgInfo("Framing publish notification header\n");
			snprintf(pub_headerlist, 1024, "%s%s%s\r\n\r\n%s\r\n", (destination!=NULL)?destination:"", (content_type!=NULL)?content_type:"", (content_length!=NULL)?content_length:"",(payload!=NULL)?payload:"");
	    }
	}
	WebcfgInfo("mqtt pub_headerlist is \n%s", pub_headerlist);
	*payload_len = strlen(pub_headerlist);
	return pub_headerlist;
}
