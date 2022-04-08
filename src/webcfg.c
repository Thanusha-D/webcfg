/*
 * Copyright 2020 Comcast Cable Communications Management, LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <math.h>
#include <string.h>
#include "webcfg.h"
#include "webcfg_multipart.h"
#include "webcfg_notify.h"
#include "webcfg_generic.h"
#include "webcfg_param.h"
#include "webcfg_auth.h"
#include <msgpack.h>
#include <wdmp-c.h>
#include <base64.h>
#include "webcfg_db.h"
#include "webcfg_metadata.h"
#include "webcfg_event.h"
#include "webcfg_blob.h"
#include "webcfg_timer.h"

#ifdef FEATURE_SUPPORT_AKER
#include "webcfg_aker.h"
#endif
/*----------------------------------------------------------------------------*/
/*                                   Macros                                   */
/*----------------------------------------------------------------------------*/


#ifdef MULTIPART_UTILITY
#ifdef DEVICE_EXTENDER
#define TEST_FILE_LOCATION              "/usr/opensync/data/multipart.bin"
#elif BUILD_YOCTO
#define TEST_FILE_LOCATION		"/nvram/multipart.bin"
#else
#define TEST_FILE_LOCATION		"/tmp/multipart.bin"
#endif
#endif
/*----------------------------------------------------------------------------*/
/*                               Data Structures                              */
/*----------------------------------------------------------------------------*/
/* none */

/*----------------------------------------------------------------------------*/
/*                            File Scoped Variables                           */
/*----------------------------------------------------------------------------*/
pthread_mutex_t sync_mutex=PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t sync_condition=PTHREAD_COND_INITIALIZER;
bool g_shutdown  = false;
bool bootSyncInProgress = false;
bool maintenanceSyncInProgress = false;
pthread_t* g_mpthreadId;
#ifdef MULTIPART_UTILITY
static int g_testfile = 0;
#endif
static int g_supplementarySync = 0;
/*----------------------------------------------------------------------------*/
/*                             Function Prototypes                            */
/*----------------------------------------------------------------------------*/
void *WebConfigMultipartTask(void *status);
int handlehttpResponse(long response_code, char *webConfigData, int retry_count, char* transaction_uuid, char* ct, size_t dataSize);
/*----------------------------------------------------------------------------*/
/*                             External Functions                             */
/*----------------------------------------------------------------------------*/

void initWebConfigMultipartTask(unsigned long status)
{
	int err = 0;
	pthread_t threadId;

	err = pthread_create(&threadId, NULL, WebConfigMultipartTask, (void *) status);
	g_mpthreadId = &threadId; 
	if (err != 0) 
	{
		WebcfgError("Error creating WebConfigMultipartTask thread :[%s]\n", strerror(err));
	}
	else
	{
		WebcfgInfo("WebConfigMultipartTask Thread created Successfully.\n");
	}
}

void *WebConfigMultipartTask(void *status)
{
	pthread_detach(pthread_self());
	int rv=0;
	int forced_sync=0;
        int Status = 0;
	int retry_flag = 0;
	int maintenance_doc_sync = 0;
	char *syncDoc = NULL;
	int value = 0;
	int wait_flag = 1;
	int maintenance_count = 0;
	time_t t;
	struct timespec ts;
	Status = (unsigned long)status;

	initWebcfgProperties(WEBCFG_PROPERTIES_FILE);

	//start webconfig notification thread.
	initWebConfigNotifyTask();

#ifdef FEATURE_SUPPORT_AKER
	WebcfgInfo("FEATURE_SUPPORT_AKER initWebConfigClient\n");
	initWebConfigClient();
#endif
	WebcfgDebug("initDB %s\n", WEBCFG_DB_FILE);

	initDB(WEBCFG_DB_FILE);

	//To disable supplementary sync for RDKV platforms
#if !defined(RDK_PERSISTENT_PATH_VIDEO)
	initMaintenanceTimer();

	//The event handler intialisation is disabled in RDKV platforms as blob type is not applicable
	if(get_global_eventFlag() == 0)
	{
		WebcfgInfo("Starting initEventHandlingTask\n");
		initEventHandlingTask();
		processWebcfgEvents();
		set_global_eventFlag();
	}
#endif
	//For Primary sync set flag to 0
	set_global_supplementarySync(0);
	set_bootSync(true);
	processWebconfgSync((int)Status, NULL);

	//For supplementary sync set flag to 1
	set_global_supplementarySync(1);

	SupplementaryDocs_t *spDocs = NULL;
	spDocs = get_global_spInfoHead();

	while(spDocs != NULL)
	{
		if(spDocs->name != NULL)
		{
			WebcfgInfo("Supplementary sync for %s\n",spDocs->name);
			processWebconfgSync((int)Status, spDocs->name);
		}
		spDocs = spDocs->next;
	}

	//Resetting the supplementary sync
	set_global_supplementarySync(0);
	set_bootSync(false);

	while(1)
	{
		if(forced_sync)
		{
			WebcfgDebug("Triggered Forced sync\n");
			processWebconfgSync((int)Status, syncDoc);
			WebcfgDebug("reset forced_sync after sync\n");
			forced_sync = 0;
			if(get_global_supplementarySync() && syncDoc !=NULL)
			{
				WEBCFG_FREE(syncDoc);
			}
			setForceSync("", "", 0);
			set_global_supplementarySync(0);
		}

		if(!wait_flag)
		{
			if(maintenance_doc_sync == 1 && checkMaintenanceTimer() == 1 )
			{
				WebcfgInfo("Maintenance window started. set maintenanceSync to true\n");
				set_maintenanceSync(true);
				char *ForceSyncDoc = NULL;
				char* ForceSyncTransID = NULL;
				getForceSync(&ForceSyncDoc, &ForceSyncTransID);
				if((ForceSyncDoc == NULL) && (ForceSyncTransID == NULL) && (!forced_sync) && (!get_bootSync()))
				{
					WebcfgInfo("release success docs at every maintenance window\n");	
					release_success_docs_tmplist();
				}
				if(ForceSyncDoc != NULL)
				{
					WEBCFG_FREE(ForceSyncDoc);
				}
				if(ForceSyncTransID !=NULL)
				{
					WEBCFG_FREE(ForceSyncTransID);
				}
				WebcfgDebug("Triggered Supplementary doc boot sync\n");
				SupplementaryDocs_t *sp = NULL;
				sp = get_global_spInfoHead();

				while(sp != NULL)
				{
					if(sp->name != NULL)
					{
						WebcfgInfo("Supplementary sync for %s\n",sp->name);
						processWebconfgSync((int)Status, sp->name);
					}
					sp = sp->next;
				}

				initMaintenanceTimer();
				maintenance_doc_sync = 0;//Maintenance trigger flag
				maintenance_count = 1;//Maintenance count to restrict one sync per day
				set_global_supplementarySync(0);
			}
		}
		pthread_mutex_lock (&sync_mutex);

		if (g_shutdown)
		{
			WebcfgInfo("g_shutdown is %d, proceeding to kill webconfig thread\n", g_shutdown);
			pthread_mutex_unlock (&sync_mutex);
			break;
		}

		clock_gettime(CLOCK_REALTIME, &ts);

		retry_flag = get_doc_fail();
		WebcfgDebug("The retry flag value is %d\n", retry_flag);

		if ( retry_flag == 0)
		{
		//To disable supplementary sync for RDKV platforms
		#if !defined(RDK_PERSISTENT_PATH_VIDEO)
			ts.tv_sec += getMaintenanceSyncSeconds(maintenance_count);
			maintenance_doc_sync = 1;
			WebcfgInfo("The Maintenance Sync triggers at %s\n", printTime((long long)ts.tv_sec));
		#else
			maintenance_doc_sync = 0;
			maintenance_count = 0;
			WebcfgDebug("maintenance_count is %d\n", maintenance_count);
		#endif
		}
		else
		{
			if(get_global_retry_timestamp() != 0)
			{
				set_retry_timer(retrySyncSeconds());
			}
			ts.tv_sec += get_retry_timer();
			WebcfgDebug("The retry triggers at %s\n", printTime((long long)ts.tv_sec));
		}

		if(retry_flag == 1 || maintenance_doc_sync == 1)
		{
			WebcfgDebug("B4 sync_condition pthread_cond_timedwait\n");
			set_maintenanceSync(false);
			WebcfgInfo("reset maintenanceSync to false\n");
			rv = pthread_cond_timedwait(&sync_condition, &sync_mutex, &ts);
			WebcfgDebug("The retry flag value is %d\n", get_doc_fail());
			WebcfgDebug("The value of rv %d\n", rv);
		}
		else 
		{
			rv = pthread_cond_wait(&sync_condition, &sync_mutex);
		}

		if(rv == ETIMEDOUT && !g_shutdown)
		{
			if(get_doc_fail() == 1)
			{
				set_doc_fail(0);
				set_retry_timer(900);
				set_global_retry_timestamp(0);
				failedDocsRetry();
				WebcfgDebug("After the failedDocsRetry\n");
			}
			else
			{
				time(&t);
				wait_flag = 0;
				maintenance_count = 0;
				WebcfgDebug("Supplementary Sync Interval %d sec and syncing at %s\n",value,ctime(&t));
			}
		}
		else if(!rv && !g_shutdown)
		{
			char *ForceSyncDoc = NULL;
			char* ForceSyncTransID = NULL;

			// Identify ForceSync based on docname
			getForceSync(&ForceSyncDoc, &ForceSyncTransID);
			WebcfgInfo("ForceSyncDoc %s ForceSyncTransID. %s\n", ForceSyncDoc, ForceSyncTransID);
			if(ForceSyncTransID !=NULL)
			{
				if((ForceSyncDoc != NULL) && strlen(ForceSyncDoc)>0)
				{
					forced_sync = 1;
					wait_flag = 1;
					WebcfgDebug("Received signal interrupt to Force Sync\n");

					//To check poke string received is supplementary doc or not.
					if(isSupplementaryDoc(ForceSyncDoc) == WEBCFG_SUCCESS)
					{
						WebcfgInfo("Received supplementary poke request for %s\n", ForceSyncDoc);
						set_global_supplementarySync(1);
						syncDoc = strdup(ForceSyncDoc);
						WebcfgDebug("syncDoc is %s\n", syncDoc);
					}
					WEBCFG_FREE(ForceSyncDoc);
					WEBCFG_FREE(ForceSyncTransID);
				}
				else
				{
					WebcfgError("ForceSyncDoc is NULL\n");
					WEBCFG_FREE(ForceSyncTransID);
				}
			}

			WebcfgDebug("forced_sync is %d\n", forced_sync);
		}
		else if(g_shutdown)
		{
			WebcfgInfo("Received signal interrupt to RFC disable. g_shutdown is %d, proceeding to kill webconfig thread\n", g_shutdown);
			pthread_mutex_unlock (&sync_mutex);
			break;
		}
		
		pthread_mutex_unlock(&sync_mutex);

	}

	/* release all active threads before shutdown */
#ifdef FEATURE_SUPPORT_AKER
	pthread_mutex_lock (get_global_client_mut());
	pthread_cond_signal (get_global_client_con());
	pthread_mutex_unlock (get_global_client_mut());
#endif

	pthread_mutex_lock (get_global_notify_mut());
	pthread_cond_signal (get_global_notify_con());
	pthread_mutex_unlock (get_global_notify_mut());


	if(get_global_eventFlag())
	{
		pthread_mutex_lock (get_global_event_mut());
		pthread_cond_signal (get_global_event_con());
		pthread_mutex_unlock (get_global_event_mut());

		WebcfgDebug("event process thread: pthread_join\n");
		JoinThread (get_global_process_threadid());

		WebcfgDebug("event thread: pthread_join\n");
		JoinThread (get_global_event_threadid());
	}

	WebcfgDebug("notify thread: pthread_join\n");
	JoinThread (get_global_notify_threadid());

	WebcfgDebug("client thread: pthread_join\n");
#ifdef FEATURE_SUPPORT_AKER
	JoinThread (get_global_client_threadid());
#endif
	reset_global_eventFlag();
	set_doc_fail(0);
	reset_numOfMpDocs();
	reset_successDocCount();
	set_maintenanceSync(false);
	set_global_maintenance_time(0);
	set_global_retry_timestamp(0);
	set_retry_timer(0);
	set_global_supplementarySync(0);
#ifdef FEATURE_SUPPORT_AKER
	set_send_aker_flag(false);
#endif
	//delete tmp, db, and mp cache lists.
	delete_tmp_list();

	WebcfgDebug("webcfgdb_destroy\n");
	webcfgdb_destroy (get_global_db_node() );
	reset_db_node();
	

	WebcfgDebug("multipart_destroy\n");
	delete_multipart();

	WebcfgDebug("supplementary_destroy\n");
	delete_supplementary_list();

	WebcfgInfo("B4 pthread_exit\n");
	g_mpthreadId = NULL;
	pthread_exit(0);
	WebcfgDebug("After pthread_exit\n");
	return NULL;
}

pthread_cond_t *get_global_sync_condition(void)
{
    return &sync_condition;
}

pthread_mutex_t *get_global_sync_mutex(void)
{
    return &sync_mutex;
}

pthread_t *get_global_mpThreadId(void)
{
    return g_mpthreadId;
}

bool get_global_shutdown()
{
    return g_shutdown;
}

void set_global_shutdown(bool shutdown)
{
    g_shutdown = shutdown;
}

bool get_bootSync()
{
    return bootSyncInProgress;
}

void set_bootSync(bool bootsync)
{
   bootSyncInProgress  = bootsync;
}

bool get_maintenanceSync()
{
    return maintenanceSyncInProgress;
}

void set_maintenanceSync(bool maintenancesync)
{
   maintenanceSyncInProgress  = maintenancesync;
}

#ifdef MULTIPART_UTILITY
int get_g_testfile()
{
    return g_testfile;
}

void set_g_testfile(int value)
{
    g_testfile = value;
}
#endif

void set_global_supplementarySync(int value)
{
    g_supplementarySync = value;
}

int get_global_supplementarySync()
{
    return g_supplementarySync;
}

/*----------------------------------------------------------------------------*/
/*                             Internal functions                             */
/*----------------------------------------------------------------------------*/
void processWebconfgSync(int status, char* docname)
{
	int retry_count=0;
	int r_count=0;
	int configRet = -1;
	char *webConfigData = NULL;
	long res_code;
	int rv=0;
	char *transaction_uuid =NULL;
	char ct[256] = {0};
	size_t dataSize=0;

	WebcfgDebug("========= Start of processWebconfgSync =============\n");
	while(1)
	{
		transaction_uuid =NULL;
		#ifdef MULTIPART_UTILITY
		if(testUtility()==1)
		{
			break;
		}
		#endif

		if(retry_count >3)
		{
			WebcfgInfo("Webcfg curl retry to server has reached max limit. Exiting.\n");
			retry_count=0;
			break;
		}
		configRet = webcfg_http_request(&webConfigData, r_count, status, &res_code, &transaction_uuid, ct, &dataSize, docname);
		if(configRet == 0)
		{
			rv = handlehttpResponse(res_code, webConfigData, retry_count, transaction_uuid, ct, dataSize);
			if(rv ==1)
			{
				WebcfgDebug("No curl retries are required. Exiting..\n");
				break;
			}
		}
		else
		{
			WebcfgError("Failed to get webConfigData from cloud\n");
			WEBCFG_FREE(transaction_uuid);
		}
		WebcfgInfo("webcfg_http_request BACKOFF_SLEEP_DELAY_SEC is %d seconds\n", BACKOFF_SLEEP_DELAY_SEC);
		sleep(BACKOFF_SLEEP_DELAY_SEC);
		retry_count++;
		r_count++;
		if(retry_count <= 3)
		{
			WebcfgInfo("Webconfig curl retry_count to server is %d\n", retry_count);
		}
	}
	WebcfgDebug("========= End of processWebconfgSync =============\n");
	return;
}

int handlehttpResponse(long response_code, char *webConfigData, int retry_count, char* transaction_uuid, char *ct, size_t dataSize)
{
	int first_digit=0;
	int msgpack_status=0;
	int err = 0;
	char version[512]={'\0'};
	uint32_t db_root_version = 0;
	char *db_root_string = NULL;
	int subdocList = 0;
	char *contentLength = NULL;
	uint16_t errorcode = 0;
	char* result =  NULL;

	if(response_code == 304)
	{
		WebcfgInfo("webConfig is in sync with cloud. response_code:%ld\n", response_code);
		getRootDocVersionFromDBCache(&db_root_version, &db_root_string, &subdocList);
		addWebConfgNotifyMsg(NULL, db_root_version, NULL, NULL, transaction_uuid, 0, "status", 0, db_root_string, response_code);
		if(db_root_string !=NULL)
		{
			WEBCFG_FREE(db_root_string);
		}
		WEBCFG_FREE(transaction_uuid);
		return 1;
	}
	else if(response_code == 200)
	{
		WebcfgDebug("webConfig is not in sync with cloud. response_code:%ld\n", response_code);

		if(webConfigData !=NULL && (strlen(webConfigData)>0))
		{
			WebcfgDebug("webConfigData fetched successfully\n");
			WebcfgDebug("parseMultipartDocument\n");
			msgpack_status = parseMultipartDocument(webConfigData, ct, dataSize, transaction_uuid);

			if(msgpack_status == WEBCFG_SUCCESS)
			{
				WebcfgInfo("webConfigData applied successfully\n");
				return 1;
			}
			else
			{
				WebcfgDebug("root webConfigData processed, check apply status events\n");
				return 1;
			}
		}
		else
		{
			WebcfgDebug("webConfigData is empty\n");
			//After factory reset when server sends 200 with empty config, set POST-NONE root version
			contentLength = get_global_contentLen();
			if((contentLength !=NULL) && (strcmp(contentLength, "0") == 0))
			{
				WebcfgInfo("webConfigData content length is 0\n");
				refreshConfigVersionList(version, response_code);
				WEBCFG_FREE(contentLength);
				set_global_contentLen(NULL);
				WEBCFG_FREE(transaction_uuid);
				WEBCFG_FREE(webConfigData);
				return 1;
			}
			if(retry_count == 3)
			{
				getRootDocVersionFromDBCache(&db_root_version, &db_root_string, &subdocList);
				errorcode = getStatusErrorCodeAndMessage(WEBCONFIG_DATA_EMPTY, &result);
				WebcfgDebug("The error_details is %s and err_code is %d\n", result, errorcode);
				addWebConfgNotifyMsg("root", db_root_version, "failed", result, transaction_uuid ,0, "status", errorcode, db_root_string, response_code);
				if(db_root_string !=NULL)
				{
					WEBCFG_FREE(db_root_string);
				}
				WEBCFG_FREE(result);
			}
		}
	}
	else if(response_code == 204)
	{
		WebcfgInfo("No configuration available for this device. response_code:%ld\n", response_code);
		getRootDocVersionFromDBCache(&db_root_version, &db_root_string, &subdocList);
		addWebConfgNotifyMsg(NULL, db_root_version, NULL, NULL, transaction_uuid, 0, "status", 0, db_root_string, response_code);
		if(db_root_string !=NULL)
		{
			WEBCFG_FREE(db_root_string);
		}
		WEBCFG_FREE(transaction_uuid);
		return 1;
	}
	else if(response_code == 403)
	{
		WebcfgError("Token is expired, fetch new token. response_code:%ld\n", response_code);
		createNewAuthToken(get_global_auth_token(), TOKEN_SIZE, get_deviceMAC(), get_global_serialNum() );
		WebcfgDebug("createNewAuthToken done in 403 case\n");
		err = 1;
	}
	else if(response_code == 429)
	{
		WebcfgInfo("No action required from client. response_code:%ld\n", response_code);
		getRootDocVersionFromDBCache(&db_root_version, &db_root_string, &subdocList);
		addWebConfgNotifyMsg(NULL, db_root_version, NULL, NULL, transaction_uuid, 0, "status", 0, db_root_string, response_code);
		if(db_root_string !=NULL)
		{
			WEBCFG_FREE(db_root_string);
		}
		WEBCFG_FREE(transaction_uuid);
		return 1;
	}
	first_digit = (int)(response_code / pow(10, (int)log10(response_code)));
	if((response_code !=403) && (first_digit == 4)) //4xx
	{
		WebcfgInfo("Action not supported. response_code:%ld\n", response_code);
		if (response_code == 404)
		{
			//To set POST-NONE root version when 404
			refreshConfigVersionList(version, response_code);
		}
		getRootDocVersionFromDBCache(&db_root_version, &db_root_string, &subdocList);
		addWebConfgNotifyMsg(NULL, db_root_version, NULL, NULL, transaction_uuid, 0, "status", 0, db_root_string, response_code);
		if(db_root_string !=NULL)
		{
			WEBCFG_FREE(db_root_string);
		}
		WEBCFG_FREE(transaction_uuid);
		return 1;
	}
	else //5xx & all other errors
	{
		WebcfgError("Error code returned, need to do curl retry to server. response_code:%ld\n", response_code);
		if(retry_count == 3 && !err)
		{
			WebcfgDebug("3 curl retry attempts\n");
			getRootDocVersionFromDBCache(&db_root_version, &db_root_string, &subdocList);
			addWebConfgNotifyMsg(NULL, db_root_version, NULL, NULL, transaction_uuid, 0, "status", 0, db_root_string, response_code);
			if(db_root_string !=NULL)
			{
				WEBCFG_FREE(db_root_string);
			}
			WEBCFG_FREE(transaction_uuid);
			return 0;
		}
		WEBCFG_FREE(transaction_uuid);
	}
	return 0;
}

void webcfgStrncpy(char *destStr, const char *srcStr, size_t destSize)
{
    strncpy(destStr, srcStr, destSize-1);
    destStr[destSize-1] = '\0';
}

void getCurrent_Time(struct timespec *timer)
{
	clock_gettime(CLOCK_REALTIME, timer);
}

long timeVal_Diff(struct timespec *starttime, struct timespec *finishtime)
{
	long msec;
	msec=(finishtime->tv_sec-starttime->tv_sec)*1000;
	msec+=(finishtime->tv_nsec-starttime->tv_nsec)/1000000;
	return msec;
}

#ifdef MULTIPART_UTILITY
int testUtility()
{
	char *data = NULL;
	char command[30] = {0};
	int test_dataSize = 0;
	int test_file_status = 0;

	char *transaction_uuid =NULL;
	char ct[256] = {0};

	if(readFromFile(TEST_FILE_LOCATION, &data, &test_dataSize) == 1)
	{
		set_g_testfile(1);
		WebcfgInfo("Using Test file \n");

		char * data_body = malloc(sizeof(char) * test_dataSize+1);
		memset(data_body, 0, sizeof(char) * test_dataSize+1);
		data_body = memcpy(data_body, data, test_dataSize+1);
		data_body[test_dataSize] = '\0';
		char *ptr_count = data_body;
		char *ptr1_count = data_body;
		char *temp = NULL;
		char *etag_header = NULL;
		char* version = NULL;

		while((ptr_count - data_body) < test_dataSize )
		{
			ptr_count = memchr(ptr_count, 'C', test_dataSize - (ptr_count - data_body));
			if(0 == memcmp(ptr_count, "Content-type:", strlen("Content-type:")))
			{
				ptr1_count = memchr(ptr_count+1, '\r', test_dataSize - (ptr_count - data_body));
				temp = strndup(ptr_count, (ptr1_count-ptr_count));
				strncpy(ct,temp,(sizeof(ct)-1));
				break;
			}
			ptr_count++;
		}

		ptr_count = data_body;
		ptr1_count = data_body;
		while((ptr_count - data_body) < test_dataSize )
		{
			ptr_count = memchr(ptr_count, 'E', test_dataSize - (ptr_count - data_body));
			if(0 == memcmp(ptr_count, "Etag:", strlen("Etag:")))
			{
				ptr1_count = memchr(ptr_count+1, '\r', test_dataSize - (ptr_count - data_body));
				etag_header = strndup(ptr_count, (ptr1_count-ptr_count));
				if(etag_header !=NULL)
				{
					WebcfgDebug("etag header extracted is %s\n", etag_header);
					//Extract root version from Etag: <value> header.
					version = strtok(etag_header, ":");
					if(version !=NULL)
					{
						version = strtok(NULL, ":");
						version++;
						//g_ETAG should be updated only for primary sync.
						if(!get_global_supplementarySync())
						{
							set_global_ETAG(version);
							WebcfgInfo("g_ETAG updated for test utility %s\n", get_global_ETAG());
						}
						break;
					}
				}
			}
			ptr_count++;
		}
		free(data_body);
		free(temp);
		WEBCFG_FREE(etag_header);
		transaction_uuid = strdup(generate_trans_uuid());
		if(data !=NULL)
		{
			WebcfgInfo("webConfigData fetched successfully\n");
			WebcfgDebug("parseMultipartDocument\n");
			test_file_status = parseMultipartDocument(data, ct, (size_t)test_dataSize, transaction_uuid);

			if(test_file_status == WEBCFG_SUCCESS)
			{
				WebcfgInfo("Test webConfigData applied successfully\n");
			}
			else
			{
				WebcfgError("Failed to apply Test root webConfigData received from server\n");
			}
		}
		else
		{
			WEBCFG_FREE(transaction_uuid);
			WebcfgError("webConfigData is empty, need to retry\n");
		}
		sprintf(command,"rm -rf %s",TEST_FILE_LOCATION);
		if(-1 != system(command))
		{
			WebcfgInfo("The %s file is removed successfully\n",TEST_FILE_LOCATION);
		}
		else
		{
			WebcfgError("Error in removing %s file\n",TEST_FILE_LOCATION);
		}
		return 1;
	}
	else
	{
		set_g_testfile(0);
		return 0;
	}
}
#endif

void JoinThread (pthread_t threadId)
{
	int ret = 0;
	ret = pthread_join (threadId, NULL);
	if(ret ==0)
	{
		WebcfgInfo("pthread_join returns success\n");
	}
	else
	{
		WebcfgError("Error joining thread threadId\n");
	}
}
