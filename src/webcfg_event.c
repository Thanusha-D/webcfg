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

#include "webcfg_multipart.h"
#include "webcfg_notify.h"
#include "webcfg_generic.h"
#include "webcfg.h"
#include <pthread.h>
#include <cJSON.h>
#include <unistd.h>
#include "webcfg_event.h"
#include "webcfg_db.h"
#include "webcfg_param.h"
#include "webcfg_blob.h"
#include "webcfg_timer.h"
#include <errno.h>
#include "webcfg_metadata.h"
#if defined(WEBCONFIG_BIN_SUPPORT)
#include "webcfg_rbus.h"
#endif
/*----------------------------------------------------------------------------*/
/*                                   Macros                                   */
/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/
/*                               Data Structures                              */
/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
/*                            File Scoped Variables                           */
/*----------------------------------------------------------------------------*/
static pthread_t EventThreadId=0;
static pthread_t processThreadId = 0;
pthread_mutex_t event_mut=PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t event_con=PTHREAD_COND_INITIALIZER;
pthread_mutex_t expire_timer_mut=PTHREAD_MUTEX_INITIALIZER;
event_data_t *eventDataQ = NULL;
static expire_timer_t * g_timer_head = NULL;
static int numOfEvents = 0;
/*----------------------------------------------------------------------------*/
/*                             Function Prototypes                            */
/*----------------------------------------------------------------------------*/
void* blobEventHandler();
int parseEventData();
void* processSubdocEvents();

int checkWebcfgTimer();
int addToEventQueue(char *buf);
void sendSuccessNotification(webconfig_tmp_data_t *subdoc_node, char *name, uint32_t version, uint16_t txid);
WEBCFG_STATUS startWebcfgTimer(expire_timer_t *timer_node, char *name, uint16_t transID, uint32_t timeout);
WEBCFG_STATUS stopWebcfgTimer(expire_timer_t *temp, char *name, uint16_t trans_id);
int checkTimerExpired (char **exp_doc);
void createTimerExpiryEvent(char *docName, uint16_t transid);
WEBCFG_STATUS updateTimerList(expire_timer_t *temp, int status, char *docname, uint16_t transid, uint32_t timeout);
WEBCFG_STATUS deleteFromTimerList(char* doc_name);
WEBCFG_STATUS checkDBVersion(char *docname, uint32_t version);
WEBCFG_STATUS validateEvent(webconfig_tmp_data_t *temp, char *docname, uint16_t txid);
expire_timer_t * getTimerNode(char *docname);
void handleConnectedClientNotify(char *status);
/*----------------------------------------------------------------------------*/
/*                             External Functions                             */
/*----------------------------------------------------------------------------*/
pthread_t get_global_event_threadid()
{
    return EventThreadId;
}

pthread_t get_global_process_threadid()
{
    return processThreadId;
}

pthread_cond_t *get_global_event_con(void)
{
    return &event_con;
}

pthread_mutex_t *get_global_event_mut(void)
{
    return &event_mut;
}
expire_timer_t * get_global_timer_node(void)	
{
    expire_timer_t * tmp = NULL;
    pthread_mutex_lock (&expire_timer_mut);
    tmp = g_timer_head;
    pthread_mutex_unlock (&expire_timer_mut);
    return tmp;
}	

void free_event_params_struct(event_params_t *param)
{
	if(param != NULL)
	{
		WEBCFG_FREE(param->subdoc_name);
		WEBCFG_FREE(param->status);
		WEBCFG_FREE(param->process_name);
		WEBCFG_FREE(param->failure_reason);
		WEBCFG_FREE(param);
	}
}

//Webcfg thread listens for blob events from respective components.
void initEventHandlingTask()
{
	int err = 0;
	err = pthread_create(&EventThreadId, NULL, blobEventHandler, NULL);
	if (err != 0)
	{
		WebcfgError("Error creating Webconfig event handling thread :[%s]\n", strerror(err));
	}
	else
	{
		WebcfgInfo("Webconfig event handling thread created Successfully\n");
	}

}

void* blobEventHandler()
{

	int ret = 0;
	char *expired_doc= NULL;
	uint16_t tx_id = 0;
	expire_timer_t *expired_node = NULL;

	ret = registerWebcfgEvent(webcfgCallback);
	if(ret)
	{
		WebcfgInfo("registerWebcfgEvent success\n");
	}
	else
	{
		WebcfgError("registerWebcfgEvent failed\n");
	}

	/* Loop to check timer expiry. When timer is not running, loop is active but wont check expiry until next timer starts. */
	while(FOREVER())
	{
		if (checkTimerExpired (&expired_doc))
		{
			if(expired_doc !=NULL)
			{
				WebcfgError("Timer expired_doc %s. No event received within timeout period\n", expired_doc);
				//reset timer. Generate internal EXPIRE event with new trans_id and retry.
				tx_id = generateRandomId(1001,3000);
				WebcfgDebug("EXPIRE event tx_id generated is %lu\n", (long)tx_id);
				expired_node = getTimerNode(expired_doc);
				updateTimerList(expired_node, false, expired_doc, tx_id, 0);
				createTimerExpiryEvent(expired_doc, tx_id);
				WebcfgDebug("After createTimerExpiryEvent\n");
			}
			else
			{
				WebcfgError("Failed to get expired_doc\n");
			}
		}
		else
		{
			WebcfgDebug("Waiting at timer loop of 5s\n");
			sleep(5);
			if (get_global_shutdown())
			{
				WebcfgDebug("g_shutdown true, break timer expire events\n");
				break;
			}
		}
	}
	ret = unregisterWebcfgEvent();
	if(ret)
	{
		WebcfgDebug("unregisterWebcfgEvent success\n");
	}
	else
	{
		WebcfgError("unregisterWebcfgEvent failed\n");
	}
	return NULL;
}

//Call back function to be executed when webconfigSignal signal is received from component.
void webcfgCallback(char *Info, void* user_data)
{
	char *buff = NULL;
	WebcfgInfo("Received webconfig event signal Info %s\n", Info);
	WebcfgDebug("user_data %s\n", (char*) user_data);

	buff = strdup(Info);
	addToEventQueue(buff);
}

//Producer adds event data to queue
int addToEventQueue(char *buf)
{
	event_data_t *Data;

	WebcfgDebug ("Add data to event queue\n");
        Data = (event_data_t *)malloc(sizeof(event_data_t));

	if(Data)
        {
            Data->data =buf;
            Data->next=NULL;
            pthread_mutex_lock (&event_mut);
            if(eventDataQ == NULL)
            {
                eventDataQ = Data;

                WebcfgDebug("Producer added Data\n");
                pthread_cond_signal(&event_con);
                pthread_mutex_unlock (&event_mut);
                WebcfgDebug("mutex unlock in producer event thread\n");
            }
            else
            {
                event_data_t *temp = eventDataQ;
                while(temp->next)
                {
                    temp = temp->next;
                }
                temp->next = Data;
                pthread_mutex_unlock (&event_mut);
            }
        }
        else
        {
            WebcfgError("failure in allocation for event data\n");
        }

	return 0;
}


//Webcfg consumer thread to process the events.
void processWebcfgEvents()
{
	int err = 0;
	err = pthread_create(&processThreadId, NULL, processSubdocEvents, NULL);
	if (err != 0)
	{
		WebcfgError("Error creating processWebcfgEvents thread :[%s]\n", strerror(err));
	}
	else
	{
		WebcfgInfo("processWebcfgEvents thread created Successfully\n");
	}

}

//Parse and process sub doc event data.
void* processSubdocEvents()
{
	event_params_t *eventParam = NULL;
	int rv = WEBCFG_FAILURE;
	WEBCFG_STATUS rs = WEBCFG_FAILURE;
	uint32_t docVersion = 0;
	char err_details[512] = {0};
	webconfig_tmp_data_t * subdoc_node = NULL;
	expire_timer_t * doctimer_node = NULL;
	char * cloud_trans_id = NULL;
	uint16_t err = 0;
	char* errmsg = NULL;

	while(FOREVER())
	{
		pthread_mutex_lock (&event_mut);
		WebcfgDebug("mutex lock in event consumer thread\n");
		if(eventDataQ != NULL)
		{
			event_data_t *Data = eventDataQ;
			eventDataQ = eventDataQ->next;
			pthread_mutex_unlock (&event_mut);
			WebcfgDebug("mutex unlock in event consumer thread\n");

			WebcfgDebug("Data->data is %s\n", Data->data);
			rv = parseEventData(Data->data, &eventParam);
			if(rv == WEBCFG_SUCCESS)
			{
				subdoc_node = getTmpNode(eventParam->subdoc_name);
				doctimer_node = getTimerNode(eventParam->subdoc_name);

				WebcfgDebug("Event detection\n");
				if (((eventParam->status !=NULL) && (strcmp(eventParam->status, "ACK")==0 || (strcmp(eventParam->status, "ACK;enabled")==0) || (strcmp(eventParam->status, "ACK;disabled")==0))) && (eventParam->timeout == 0))
				{
					//Based on ACK event if mesh/cujo is enabled then connected client notification need to be turned OFF
					if ((eventParam->subdoc_name !=NULL) &&((strcmp(eventParam->subdoc_name, "mesh") ==0) || (strcmp(eventParam->subdoc_name, "advsecurity") ==0)))
					{
						WebcfgInfo("ACK for mesh/cujo received: %s,%lu,%lu,%s,%lu\n", eventParam->subdoc_name,(long)eventParam->trans_id, (long)eventParam->version, eventParam->status, (long)eventParam->timeout);
						handleConnectedClientNotify(eventParam->status);
					}
				
					WebcfgInfo("ACK EVENT: %s,%lu,%lu,ACK,%lu %s\n", eventParam->subdoc_name,(long)eventParam->trans_id, (long)eventParam->version, (long)eventParam->timeout, "(doc apply success)");
					WebcfgInfo("doc apply success, proceed to add to DB\n");
					if( validateEvent(subdoc_node, eventParam->subdoc_name, eventParam->trans_id) == WEBCFG_SUCCESS)
					{
						//version in event &tmp are not same indicates latest doc is not yet applied
						if((getDocVersionFromTmpList(subdoc_node, eventParam->subdoc_name))== eventParam->version)
						{
							stopWebcfgTimer(doctimer_node, eventParam->subdoc_name, eventParam->trans_id);

							//add to DB, update tmp list and notification based on success ack.
							sendSuccessNotification(subdoc_node, eventParam->subdoc_name, eventParam->version, eventParam->trans_id);
							//No DB update for supplementary sync as version is not required to be stored.
							if(subdoc_node->isSupplementarySync == 0)
							{
								WebcfgDebug("AddToDB subdoc_name %s version %lu\n", eventParam->subdoc_name, (long)eventParam->version);
								checkDBList(eventParam->subdoc_name,eventParam->version, NULL);
								WebcfgDebug("checkRootUpdate\n");
								if(checkRootUpdate() == WEBCFG_SUCCESS)
								{
									WebcfgDebug("updateRootVersionToDB\n");
									updateRootVersionToDB();
								}
								addNewDocEntry(get_successDocCount());
							}
							else
							{
								WebcfgInfo("No DB update for supplementary sync as version is not required to be stored.\n");
							}
							//root doc delete from tmp list and mp docs destroy can be done irrespective of primary/supplementary checks as all docs success can be reached during any sync.
							WebcfgDebug("check for deleteRootAndMultipartDocs\n");
							deleteRootAndMultipartDocs();
						}
						else
						{
							WebcfgError("ACK event version and tmp cache version are not same\n");
						}
					}
				}
				else if (((eventParam->status !=NULL)&&(strcmp(eventParam->status, "NACK")==0)) && (eventParam->timeout == 0))
				{
					WebcfgInfo("NACK EVENT: %s,%lu,%lu,NACK,%lu %s\n", eventParam->subdoc_name,(long)eventParam->trans_id, (long)eventParam->version, (long)eventParam->timeout, "(doc apply failed)");
					WebcfgError("doc apply failed for %s\n", eventParam->subdoc_name);
					if( validateEvent(subdoc_node, eventParam->subdoc_name, eventParam->trans_id) == WEBCFG_SUCCESS)
					{
						if((getDocVersionFromTmpList(subdoc_node, eventParam->subdoc_name))== eventParam->version)
						{
							stopWebcfgTimer(doctimer_node, eventParam->subdoc_name, eventParam->trans_id);
							snprintf(err_details, sizeof(err_details),"NACK:%s,%s",((NULL != eventParam->process_name) ? eventParam->process_name : "unknown"), ((NULL != eventParam->failure_reason) ? eventParam->failure_reason : "unknown"));
							WebcfgDebug("err_details : %s, err_code : %lu\n", err_details, (long) eventParam->err_code);
							WebcfgInfo("subdoc_name and err_code : %s %lu\n", eventParam->subdoc_name, (long) eventParam->err_code);
							WebcfgInfo("failure_reason %s\n", err_details);
							updateTmpList(subdoc_node, eventParam->subdoc_name, eventParam->version, "failed", err_details, eventParam->err_code, eventParam->trans_id, 0);
							WebcfgDebug("get_global_transID is %s\n", get_global_transID());
							if(subdoc_node !=NULL && subdoc_node->cloud_trans_id !=NULL)
							{
								cloud_trans_id = subdoc_node->cloud_trans_id;
							}
							else
							{
								WebcfgInfo("subdoc_node is NULL, cloud_trans_id is unknown\n");
								cloud_trans_id = "unknown";
							}
							WebcfgDebug("cloud_trans_id is %s\n", cloud_trans_id);
							addWebConfgNotifyMsg(eventParam->subdoc_name, eventParam->version, "failed", err_details, cloud_trans_id, eventParam->timeout, "status", eventParam->err_code, NULL, 200);
						}
						else
						{
							WebcfgError("NACK event version and tmp cache version are not same\n");
						}
					}
				}
				else if ((eventParam->status !=NULL)&&(strcmp(eventParam->status, "EXPIRE")==0))
				{
					WebcfgInfo("EXPIRE EVENT: %s,%lu,%lu,EXPIRE,%lu\n", eventParam->subdoc_name,(long)eventParam->trans_id, (long)eventParam->version, (long)eventParam->timeout);
					WebcfgInfo("doc apply timeout expired, need to retry\n");
					WebcfgDebug("get_global_transID is %s\n", get_global_transID());
					if(eventParam->version !=0)
					{
						docVersion = eventParam->version;
					}
					else
					{
						docVersion = getDocVersionFromTmpList(subdoc_node, eventParam->subdoc_name);
					}
					WebcfgDebug("docVersion %lu\n", (long) docVersion);
					if(subdoc_node !=NULL && subdoc_node->cloud_trans_id !=NULL)
					{
						addWebConfgNotifyMsg(eventParam->subdoc_name, docVersion, "pending", "timer_expired", subdoc_node->cloud_trans_id, eventParam->timeout, "status", 0, NULL, 200);
					}
					//To avoid NACK blob re-apply on EXPIRE event after 304
					if(checkTmpNACKstatus(subdoc_node, eventParam->subdoc_name))
					{
						WebcfgInfo("Skip retry for %s as blob apply status is NACK\n", eventParam->subdoc_name);
					}
					else
					{
					WebcfgDebug("retryMultipartSubdoc for EXPIRE case\n");
					rs = retryMultipartSubdoc(subdoc_node, eventParam->subdoc_name);
					if(rs == WEBCFG_SUCCESS)
					{
						WebcfgDebug("retryMultipartSubdoc success\n");
					}
					else
					{
						WebcfgError("retryMultipartSubdoc failed\n");
						if(subdoc_node !=NULL)
						{
							if(subdoc_node->retry_count < 3)
							{
								err = getStatusErrorCodeAndMessage(SUBDOC_RETRY_FAILED, &errmsg);
								WebcfgDebug("The error_details is %s and err_code is %d\n", errmsg, err);
								updateTmpList(subdoc_node, eventParam->subdoc_name, docVersion, "failed", errmsg, err, subdoc_node->trans_id, subdoc_node->retry_count);
								if(subdoc_node->cloud_trans_id !=NULL)
								{
									addWebConfgNotifyMsg(eventParam->subdoc_name, docVersion, "failed", errmsg, subdoc_node->cloud_trans_id ,0, "status", err, NULL, 200);
								}
								WEBCFG_FREE(errmsg);
							}
						}
					}
					}
				}
				else if (eventParam->timeout != 0)
				{
					WebcfgInfo("TIMEOUT EVENT: %s,%lu,%lu,ACK,%lu %s\n", eventParam->subdoc_name,(long)eventParam->trans_id, (long)eventParam->version, (long)eventParam->timeout,"(doc apply need time)");
					WebcfgInfo("doc apply need time, start timer.\n");
					if( validateEvent(subdoc_node, eventParam->subdoc_name, eventParam->trans_id) == WEBCFG_SUCCESS)
					{
						if((getDocVersionFromTmpList(subdoc_node, eventParam->subdoc_name))== eventParam->version)
						{
							startWebcfgTimer(doctimer_node, eventParam->subdoc_name, eventParam->trans_id, eventParam->timeout);
							if(subdoc_node !=NULL && subdoc_node->cloud_trans_id !=NULL)
							{
								cloud_trans_id = subdoc_node->cloud_trans_id;
							}
							else
							{
								WebcfgInfo("subdoc_node is NULL, cloud_trans_id is unknown\n");
								cloud_trans_id = "unknown";
							}
							WebcfgDebug("cloud_trans_id is %s\n", cloud_trans_id);
							addWebConfgNotifyMsg(eventParam->subdoc_name, eventParam->version, "pending", NULL, cloud_trans_id,eventParam->timeout, "ack", 0, NULL, 200);
						}
						else
						{
							WebcfgError("Timeout event version and tmp cache version are not same\n");
						}
					}
				}
				else
				{
					if ((eventParam->status !=NULL)&&(strcmp(eventParam->status, "COMP_INIT")==0))
					{
						WebcfgInfo("COMP_INIT EVENT: %s,%d,%lu\n", eventParam->subdoc_name,0, (long)eventParam->version);
						WebcfgInfo("Component initialized, check and re-send blob.\n");
					}
					else
					{
						WebcfgInfo("Crash EVENT: %s,%d,%lu\n", eventParam->subdoc_name,0, (long)eventParam->version);
						WebcfgInfo("Component restarted after crash, re-send blob.\n");
					}
					uint32_t tmpVersion = 0;

					//If version in event and tmp are not matching, re-send blob to retry.
					if(checkDBVersion(eventParam->subdoc_name, eventParam->version) !=WEBCFG_SUCCESS)
					{
						WebcfgDebug("DB and event version are not same, check tmp list\n");
						tmpVersion = getDocVersionFromTmpList(subdoc_node, eventParam->subdoc_name);
						if (tmpVersion == 0)
						{
							//tmpVersion=0 indicate already doc is applied & doc is not available in tmp list
							WebcfgDebug("tmpVersion is 0, DB already in latest version\n");
						}
						else if(tmpVersion != eventParam->version)
						{
							//To avoid NACK blob re-apply on CRASH event after 304
							if(checkTmpNACKstatus(subdoc_node, eventParam->subdoc_name))
							{
								WebcfgInfo("Skip retry for %s as blob apply status is NACK\n", eventParam->subdoc_name);
							}
							else
							{
							WebcfgInfo("tmp list has new version %lu for doc %s, retry\n", (long)tmpVersion, eventParam->subdoc_name);
							rs = retryMultipartSubdoc(subdoc_node, eventParam->subdoc_name);
							if(rs == WEBCFG_SUCCESS)
							{
								WebcfgDebug("retryMultipartSubdoc success\n");
							}
							else
							{
								WebcfgError("retryMultipartSubdoc failed\n");
								if(subdoc_node != NULL)
								{
									if(subdoc_node->retry_count < 3)
									{
										err = getStatusErrorCodeAndMessage(SUBDOC_RETRY_FAILED, &errmsg);
										WebcfgDebug("The error_details is %s and err_code is %d\n", errmsg, err);
										updateTmpList(subdoc_node, eventParam->subdoc_name, tmpVersion, "failed", errmsg, err, subdoc_node->trans_id, subdoc_node->retry_count);
									        if(subdoc_node->cloud_trans_id != NULL)
										{
											addWebConfgNotifyMsg(eventParam->subdoc_name, tmpVersion, "failed", errmsg, subdoc_node->cloud_trans_id ,0, "status", err, NULL, 200);
										}
										WEBCFG_FREE(errmsg);
									}
								}
							}
							}
						}
						else
						{
							//already in tmp latest version,send success notify, updateDB
							WebcfgInfo("tmp version %lu same as event version %lu\n",(long)tmpVersion, (long)eventParam->version); 
							sendSuccessNotification(subdoc_node, eventParam->subdoc_name, eventParam->version, eventParam->trans_id);
							//No DB update for supplementary docs
							if(subdoc_node->isSupplementarySync == 0)
							{
								WebcfgInfo("AddToDB subdoc_name %s version %lu\n", eventParam->subdoc_name, (long)eventParam->version);
								checkDBList(eventParam->subdoc_name,eventParam->version, NULL);
								WebcfgDebug("checkRootUpdate\n");
								if(checkRootUpdate() == WEBCFG_SUCCESS)
								{
									WebcfgDebug("updateRootVersionToDB\n");
									updateRootVersionToDB();
								}
								addNewDocEntry(get_successDocCount());
							}
							WebcfgDebug("check for deleteRootAndMultipartDocs\n");
							deleteRootAndMultipartDocs();
						}
					}
					else
					{
						WebcfgDebug("DB and event version are same, check tmp list\n");
						tmpVersion = getDocVersionFromTmpList(subdoc_node, eventParam->subdoc_name);
						//tmpVersion=0 indicate already doc is applied & deleted frm tmp list
						if((tmpVersion !=0) && (tmpVersion != eventParam->version))
						{
							//To avoid NACK blob re-apply on CRASH event after 304
							if(checkTmpNACKstatus(subdoc_node, eventParam->subdoc_name))
							{
								WebcfgInfo("Skip retry for %s as blob apply status is NACK\n", eventParam->subdoc_name);
							}
							else
							{
							WebcfgInfo("tmp list has new version %lu for doc %s, retry\n", (long)tmpVersion, eventParam->subdoc_name);
							//retry with latest tmp version
							rs = retryMultipartSubdoc(subdoc_node, eventParam->subdoc_name);
							//wait for ACK to send success notification and Update DB
							if(rs == WEBCFG_SUCCESS)
							{
								WebcfgDebug("retryMultipartSubdoc success\n");
							}
							else
							{
								WebcfgError("retryMultipartSubdoc failed\n");
								if(subdoc_node != NULL)
								{
									if(subdoc_node->retry_count < 3)
									{
										err = getStatusErrorCodeAndMessage(SUBDOC_RETRY_FAILED, &errmsg);
										WebcfgDebug("The error_details is %s and err_code is %d\n", errmsg, err);
										updateTmpList(subdoc_node, eventParam->subdoc_name, tmpVersion, "failed", errmsg, err, subdoc_node->trans_id, subdoc_node->retry_count);
										if(subdoc_node->cloud_trans_id != NULL)
										{
											addWebConfgNotifyMsg(eventParam->subdoc_name, tmpVersion, "failed", errmsg, subdoc_node->cloud_trans_id ,0, "status", err, NULL, 200);
										}
										WEBCFG_FREE(errmsg);
									}
								}
							}
							}
						}
						else
						{
							WebcfgInfo("Already in latest version, no need to retry\n");
						}
					}
				}
				free_event_params_struct(eventParam);
			}
			else
			{
				WebcfgError("Failed to parse event Data\n");
				err = getStatusErrorCodeAndMessage(COMPONENT_EVENT_PARSE_FAILURE, &errmsg);
				WebcfgDebug("The error_details is %s and err_code is %d\n", errmsg, err);
				addWebConfgNotifyMsg(NULL, 0, "failed", errmsg, NULL ,0, "status", err, NULL, 200);
				WEBCFG_FREE(errmsg);
			}
			WEBCFG_FREE(Data);
		}
		else
		{
			if (get_global_shutdown())
			{
				WebcfgDebug("g_shutdown in event consumer thread\n");
				pthread_mutex_unlock (&event_mut);
				break;
			}
			WebcfgDebug("Before pthread cond wait in event consumer thread\n");
			pthread_cond_wait(&event_con, &event_mut);
			pthread_mutex_unlock (&event_mut);
			WebcfgDebug("mutex unlock in event consumer thread after cond wait\n");
		}
	}
	return NULL;
}

//Extract values from comma separated string and add to event_params_t structure.
int parseEventData(char* str, event_params_t **val)
{
	event_params_t *param = NULL;
	char *tmpStr, *token =  NULL;
	char * trans_id = NULL;
	char *version = NULL;
	char * timeout = NULL;
	char *err_code = NULL;
	char *subdoc_name = NULL, *status = NULL;
	char *process_name = NULL, *failure_reason = NULL;

	if(str !=NULL)
	{
		param = (event_params_t *)malloc(sizeof(event_params_t));
		if(param)
		{
			memset(param, 0, sizeof(event_params_t));
		        token = strdup(str);
				tmpStr = token;
			WEBCFG_FREE(str);

		        subdoc_name = strsep(&tmpStr, ",");
				if(subdoc_name != NULL)
				{
					param->subdoc_name = strdup(subdoc_name);
				}
		        trans_id = strsep(&tmpStr,",");
		        version = strsep(&tmpStr,",");
				status = strsep(&tmpStr,",");
				if(status != NULL)
				{
		        	param->status = strdup(status);
				}
		        timeout = strsep(&tmpStr, ",");

			if(tmpStr !=NULL)
			{
				WebcfgInfo("For NACK event: tmpStr with error_details is %s\n", tmpStr);
				process_name = strsep(&tmpStr, ",");
				if(process_name != NULL)
				{
					param->process_name = strdup(process_name);
				}
				err_code = strsep(&tmpStr, ",");
				failure_reason = strsep(&tmpStr, ",");
				if(failure_reason != NULL)
				{
					param->failure_reason = strdup(failure_reason);
				}
				WebcfgInfo("process_name %s err_code %s failure_reason %s\n", param->process_name, err_code, param->failure_reason);
			}

			WebcfgDebug("convert string to uint type\n");
			if(trans_id !=NULL)
			{
				param->trans_id = strtoul(trans_id,NULL,0);
			}

			if(version !=NULL)
			{
				param->version = strtoul(version,NULL,0);
			}

			if(timeout !=NULL)
			{
				param->timeout = strtoul(timeout,NULL,0);
			}

			if(err_code !=NULL)
			{
				param->err_code = strtoul(err_code,NULL,0);
				WebcfgDebug("param->err_code %lu\n", (long)param->err_code);
			}

			WebcfgDebug("param->subdoc_name %s param->trans_id %lu param->version %lu param->status %s param->timeout %lu\n", param->subdoc_name, (long)param->trans_id, (long)param->version, param->status, (long)param->timeout);
			*val = param;
			WEBCFG_FREE(token);
			return WEBCFG_SUCCESS;
		}		
	}

	return WEBCFG_FAILURE;

}


//To generate custom timer EXPIRE event for expired doc and add to event queue.
void createTimerExpiryEvent(char *docName, uint16_t transid)
{
	char *expiry_event_data = NULL;
	char data[128] = {0};

	snprintf(data,sizeof(data),"%s,%hu,%u,EXPIRE,%u",docName,transid,0,0);
	expiry_event_data = strdup(data);
	WebcfgDebug("expiry_event_data formed %s\n", expiry_event_data);
	if(expiry_event_data)
	{
		addToEventQueue(expiry_event_data);
		WebcfgDebug("Added EXPIRE event queue\n");
	}
	else
	{
		WebcfgError("Failed to generate timer EXPIRE event\n");
	}
}

//Update Tmp list and send success notification to cloud .
void sendSuccessNotification(webconfig_tmp_data_t *subdoc_node, char *name, uint32_t version, uint16_t txid)
{
	char *cloud_trans_id = NULL;
	updateTmpList(subdoc_node, name, version, "success", "none", 0, txid, 0);
	if(subdoc_node !=NULL && subdoc_node->cloud_trans_id !=NULL)
	{
		cloud_trans_id = subdoc_node->cloud_trans_id;
	}
	else
	{
		WebcfgInfo("subdoc_node is NULL, cloud_trans_id is unknown\n");
		cloud_trans_id = "unknown";
	}
	addWebConfgNotifyMsg(name, version, "success", "none", cloud_trans_id,0, "status",0, NULL, 200);
}

//start internal timer for required doc when timeout value is received
WEBCFG_STATUS startWebcfgTimer(expire_timer_t *timer_node, char *name, uint16_t transID, uint32_t timeout)
{
	if(updateTimerList(timer_node, true, name, transID, timeout) != WEBCFG_SUCCESS)
	{
		//add docs to timer list
		expire_timer_t *new_node = NULL;
		new_node=(expire_timer_t *)malloc(sizeof(expire_timer_t));

		if(new_node)
		{
			memset( new_node, 0, sizeof( expire_timer_t ) );
			WebcfgDebug("Adding events to list\n");
			new_node->running = true;
			new_node->subdoc_name = strdup(name);
			new_node->txid = transID;
			new_node->timeout = timeout;
			WebcfgDebug("started webcfg internal timer\n");

			new_node->next=NULL;

			pthread_mutex_lock (&expire_timer_mut);
			if (g_timer_head == NULL)
			{
				g_timer_head = new_node;
				pthread_mutex_unlock (&expire_timer_mut);
			}
			else
			{
				expire_timer_t *temp = NULL;
				WebcfgDebug("Adding next events to list\n");
				temp = g_timer_head;
				while(temp->next !=NULL)
				{
					temp=temp->next;
				}
				temp->next=new_node;
				pthread_mutex_unlock (&expire_timer_mut);
			}

			WebcfgInfo("new_node->subdoc_name %s new_node->txid %lu new_node->timeout %lu status %d added to list\n", new_node->subdoc_name, (long)new_node->txid, (long)new_node->timeout, new_node->running);
			numOfEvents = numOfEvents + 1;
		}
		else
		{
			WebcfgError("Failed in timer allocation\n");
			return WEBCFG_FAILURE;
		}
	}
	WebcfgInfo("startWebcfgTimer success\n");
	return WEBCFG_SUCCESS;
}

//update name, transid, timeout for each doc event
WEBCFG_STATUS updateTimerList(expire_timer_t *temp, int status, char *docname, uint16_t transid, uint32_t timeout)
{
	if (NULL != temp)
	{
		WebcfgDebug("node is pointing to temp->subdoc_name %s \n",temp->subdoc_name);
		if( strcmp(docname, temp->subdoc_name) == 0)
		{
			pthread_mutex_lock (&expire_timer_mut);
			temp->running = status;
			temp->txid = transid;
			temp->timeout = timeout;
			if(strcmp(temp->subdoc_name, docname) !=0)
			{
				WEBCFG_FREE(temp->subdoc_name);
				temp->subdoc_name = NULL;
				temp->subdoc_name = strdup(docname);
			}
			WebcfgInfo("doc timer %s is updated with txid %lu timeout %lu\n", docname, (long)temp->txid, (long)temp->timeout);
			pthread_mutex_unlock (&expire_timer_mut);
			return WEBCFG_SUCCESS;
		}
	}
	WebcfgInfo("Timer list is empty\n");
	return WEBCFG_FAILURE;
}


//delete doc from webcfg timer list
WEBCFG_STATUS deleteFromTimerList(char* doc_name)
{
	expire_timer_t *prev_node = NULL, *curr_node = NULL;

	if( NULL == doc_name )
	{
		WebcfgError("Invalid value for timer doc\n");
		return WEBCFG_FAILURE;
	}
	WebcfgDebug("timer doc to be deleted: %s\n", doc_name);

	prev_node = NULL;
	pthread_mutex_lock (&expire_timer_mut);
	curr_node = g_timer_head ;

	// Traverse to get the doc to be deleted
	while( NULL != curr_node )
	{
		if(strcmp(curr_node->subdoc_name, doc_name) == 0)
		{
			WebcfgDebug("Found the node to delete\n");
			if( NULL == prev_node )
			{
				WebcfgDebug("need to delete first doc\n");
				g_timer_head = curr_node->next;
			}
			else
			{
				WebcfgDebug("Traversing to find node\n");
				prev_node->next = curr_node->next;
			}

			WebcfgDebug("Deleting the node entries\n");
			curr_node->running = false;
			curr_node->txid = 0;
			curr_node->timeout = 0;
			WEBCFG_FREE( curr_node->subdoc_name );
			WEBCFG_FREE( curr_node );
			curr_node = NULL;
			WebcfgDebug("Deleted timer successfully and returning..\n");
			numOfEvents =numOfEvents - 1;
			WebcfgDebug("numOfEvents after delete is %d\n", numOfEvents);
			pthread_mutex_unlock (&expire_timer_mut);
			return WEBCFG_SUCCESS;
		}

		prev_node = curr_node;
		curr_node = curr_node->next;
	}
	pthread_mutex_unlock (&expire_timer_mut);
	WebcfgError("Could not find the entry to delete from timer list\n");
	return WEBCFG_FAILURE;
}

//Stop the required doc timer for which ack/nack events received
WEBCFG_STATUS stopWebcfgTimer(expire_timer_t *temp, char *name, uint16_t trans_id)
{
	WebcfgDebug("stopWebcfgTimer trans_id %lu\n", (long)trans_id);
	if (NULL != temp)
	{
		WebcfgDebug("stopWebcfgTimer: node is pointing to temp->subdoc_name %s\n",temp->subdoc_name);
		if( strcmp(name, temp->subdoc_name) == 0)
		{
			if (temp->running)
			{
				if( trans_id ==temp->txid)
				{
					WebcfgDebug("delete timer for sub doc %s\n", name);
					if(deleteFromTimerList(name) == WEBCFG_SUCCESS)
					{
						WebcfgDebug("stopped timer for doc %s\n", name);
						return WEBCFG_SUCCESS;
					}
					else
					{
						WebcfgError("Failed to stop timer for doc %s\n", name);
					}
				}
				else
				{
					//wait for next event with latest txid T2.
					WebcfgError("temp->txid %lu for doc %s is not matching.\n", (long)temp->txid, name);
				}
			}
			else
			{
				WebcfgError("timer is not running for doc %s!!!\n", temp->subdoc_name);
			}
		}
	}
	return WEBCFG_FAILURE;
}

//check timer expiry by decrementing timeout by 5 for 5s sleep.
int checkTimerExpired (char **exp_doc)
{
	expire_timer_t *temp = NULL;
	temp = get_global_timer_node();

	//Traverse through all docs in list, decrement timer and check if any doc expired.
	while (NULL != temp)
	{
		WebcfgDebug("checking expiry for temp->subdoc_name %s\n",temp->subdoc_name);
		if (temp->running)
		{
			WebcfgDebug("timer running for doc %s temp->timeout: %d\n",temp->subdoc_name, (int)temp->timeout);
			if((int)temp->timeout <= 0)
			{
				WebcfgInfo("Timer Expired for doc %s, doc apply failed\n", temp->subdoc_name);
				*exp_doc = strdup(temp->subdoc_name);
				WebcfgDebug("*exp_doc is %s\n", *exp_doc);
				return true;
			}
			pthread_mutex_lock (&expire_timer_mut);
			temp->timeout = temp->timeout - 5;
			pthread_mutex_unlock (&expire_timer_mut);
			WebcfgDebug("temp->timeout %d for doc %s\n", temp->timeout, temp->subdoc_name);
		}
		temp= temp->next;
	}
	return false;
}

WEBCFG_STATUS retryMultipartSubdoc(webconfig_tmp_data_t *docNode, char *docName)
{
	int i =0;
	WEBCFG_STATUS rv = WEBCFG_FAILURE;
	param_t *reqParam = NULL;
	WDMP_STATUS ret = WDMP_FAILURE;
	WDMP_STATUS errd = WDMP_FAILURE;
	char errDetails[MAX_VALUE_LEN]={0};
	char result[MAX_VALUE_LEN]={0};
	int ccspStatus=0;
	int paramCount = 0;
	uint16_t doc_transId = 0;
	webcfgparam_t *pm = NULL;
	multipartdocs_t *gmp = NULL;
	uint16_t err = 0;
	char * errmsg = NULL;

	int sendMsgsize = 0;
#ifdef WEBCONFIG_BIN_SUPPORT
	void *buff = NULL;
#endif

	gmp = get_global_mp();

	if(gmp ==NULL)
	{
		WebcfgError("Multipart mp cache is NULL\n");
		return rv;
	}

	if(checkAndUpdateTmpRetryCount(docNode, docName) !=WEBCFG_SUCCESS)
	{
		WebcfgError("checkAndUpdateTmpRetryCount failed\n");
		return rv;
	}

	while( gmp != NULL)
	{
		multipartdocs_t *temp_mp = NULL;
		temp_mp = get_global_mp();

		if(temp_mp == NULL)
		{
			WebcfgInfo("mp cache list is empty. Exiting from subdoc retry.\n");
			break;
		}

		if(strcmp(gmp->name_space, docName) == 0)
		{
			WebcfgDebug("gmp->name_space %s\n", gmp->name_space);
			WebcfgDebug("gmp->etag %lu\n" , (long)gmp->etag);
			WebcfgDebug("gmp->data %s\n" , gmp->data);
			WebcfgDebug("gmp->data_size is %zu\n", gmp->data_size);

			WebcfgDebug("--------------decode root doc-------------\n");
			pm = webcfgparam_convert( gmp->data, gmp->data_size+1 );
			err = errno;
			if ( NULL != pm)
			{
				paramCount = (int)pm->entries_count;

				reqParam = (param_t *) malloc(sizeof(param_t) * paramCount);
				memset(reqParam,0,(sizeof(param_t) * paramCount));

				WebcfgDebug("paramCount is %d\n", paramCount);
				for (i = 0; i < paramCount; i++)
				{
			                if(pm->entries[i].value != NULL)
			                {
						if(pm->entries[i].type == WDMP_BLOB)
						{
							char *appended_doc = NULL;
							WebcfgDebug("B4 webcfg_appendeddoc\n");
							appended_doc = webcfg_appendeddoc( gmp->name_space, gmp->etag, pm->entries[i].value, pm->entries[i].value_size, &doc_transId, &sendMsgsize);
							
							WebcfgDebug("webcfg_appendeddoc doc_transId is %hu\n", doc_transId);
							reqParam[i].name = strdup(pm->entries[i].name);
							#ifdef WEBCONFIG_BIN_SUPPORT
								if(isRbusEnabled() && isRbusListener(gmp->name_space))
								{
								        //Setting reqParam struct to avoid validate_request_param() failure
									//disable string operation as it is binary data.
									reqParam[i].value = appended_doc;
									//setting reqParam type as base64 to indicate blob
									reqParam[i].type = WDMP_BASE64;
									buff = reqParam[i].value;
								}
								else
								{
									reqParam[i].value = strdup(appended_doc);
									reqParam[i].type = WDMP_BASE64;
									WEBCFG_FREE(appended_doc);
								}
							#else
								reqParam[i].value = strdup(appended_doc);
								reqParam[i].type = WDMP_BASE64;
								WEBCFG_FREE(appended_doc);
							#endif
							//update doc_transId only for blob docs, not for scalars.
							updateTmpList(docNode, gmp->name_space, gmp->etag, "pending", "failed_retrying", ccspStatus, doc_transId, 1);
						}
						else
						{
							if(pm->entries[i].name !=NULL)
							{
								reqParam[i].name = strdup(pm->entries[i].name);
							}
							if(pm->entries[i].value !=NULL)
							{
								reqParam[i].value = strdup(pm->entries[i].value);
							}
							reqParam[i].type = pm->entries[i].type;
						}
			                }
					WebcfgInfo("Request:> param[%d].name = %s, type = %d\n",i,reqParam[i].name,reqParam[i].type);
					WebcfgDebug("Request:> param[%d].value = %s\n",i,reqParam[i].value);
				}

				if(reqParam !=NULL && validate_request_param(reqParam, paramCount) == WEBCFG_SUCCESS)
				{
					WebcfgDebug("Proceed to setValues..\n");
					WebcfgDebug("retryMultipartSubdoc WebConfig SET Request\n");
					#ifdef WEBCONFIG_BIN_SUPPORT
						// rbus_enabled and rbus_listener_supported, rbus_set direct API used to send binary data to component.
						if(isRbusEnabled() && isRbusListener(gmp->name_space))
						{
							char topic[64] = {0};
					        	get_destination(gmp->name_space, topic);
							blobSet_rbus(topic, buff, sendMsgsize, &ret, &ccspStatus);
						}
						//rbus_enabled and rbus_listener_not_supported, rbus_set api used to send b64 encoded data to component.
						else 
						{
							setValues_rbus(reqParam, paramCount, ATOMIC_SET_WEBCONFIG, NULL, NULL, &ret, &ccspStatus);
						}
					#else
					        //dbus_enabled, ccsp common library set api used to send data to component.
						setValues(reqParam, paramCount, ATOMIC_SET_WEBCONFIG, NULL, NULL, &ret, &ccspStatus);
					#endif
					//setValues(reqParam, paramCount, ATOMIC_SET_WEBCONFIG, NULL, NULL, &ret, &ccspStatus);
					if(ret == WDMP_SUCCESS)
					{
						WebcfgInfo("retryMultipartSubdoc setValues success. ccspStatus : %d\n", ccspStatus);
						if(reqParam[0].type  != WDMP_BASE64)
						{
							WebcfgDebug("For scalar docs, update trans_id as 0\n");
							updateTmpList(docNode, gmp->name_space, gmp->etag, "success", "none", 0, 0, 0);
							//send scalar success notification, delete tmp, updateDB
							if(docNode !=NULL && docNode->cloud_trans_id !=NULL)
							{
								addWebConfgNotifyMsg(gmp->name_space, gmp->etag, "success", "none", docNode->cloud_trans_id, 0, "status", 0, NULL, 200);
							}
							WebcfgDebug("deleteFromTmpList as scalar doc is applied\n");
							WebcfgDebug("docNode->isSupplementarySync is %d\n", docNode->isSupplementarySync);
							if(docNode->isSupplementarySync == 0)
							{
								checkDBList(gmp->name_space,gmp->etag, NULL);
								WebcfgInfo("checkRootUpdate scalar doc case\n");
								if(checkRootUpdate() == WEBCFG_SUCCESS)
								{
									WebcfgDebug("updateRootVersionToDB\n");
									updateRootVersionToDB();
								}
								addNewDocEntry(get_successDocCount());
							}
							else
							{
								WebcfgInfo("retryMultipartSubdoc. No DB update is required for supplementary sync\n");
							}
							WebcfgDebug("check for deleteRootAndMultipartDocs\n");
							deleteRootAndMultipartDocs();
						}
						rv = WEBCFG_SUCCESS;
					}
					else
					{

						WebcfgError("retryMultipartSubdoc setValues Failed. ccspStatus : %d\n", ccspStatus);
						errd = mapStatus(ccspStatus);
						WebcfgDebug("The errd value is %d\n",errd);

						mapWdmpStatusToStatusMessage(errd, errDetails);
						WebcfgDebug("The errDetails value is %s\n",errDetails);

						if((ccspStatus == 192) || (ccspStatus == 204) || (ccspStatus == 191) || (ccspStatus == 193) || (ccspStatus == 190))
						{
							long long expiry_time = 0;
							WebcfgDebug("ccspStatus is crash %d\n", ccspStatus);
							snprintf(result,MAX_VALUE_LEN,"failed_retrying:%s", errDetails);
							WebcfgDebug("The result is %s\n",result);
							updateTmpList(docNode, gmp->name_space, gmp->etag, "pending", result, ccspStatus, 0, 1);
							if(docNode !=NULL && docNode->cloud_trans_id !=NULL)
							{
								addWebConfgNotifyMsg(gmp->name_space, gmp->etag, "pending", result, docNode->cloud_trans_id, 0,"status",ccspStatus, NULL, 200);
							}
							expiry_time = getRetryExpiryTimeout();
							set_doc_fail(1);
							updateFailureTimeStamp(docNode, gmp->name_space, expiry_time);

							//To get the exact time diff for retry from present time do the below
							updateRetryTimeDiff(expiry_time);

							WebcfgDebug("the retry flag value is %d\n", get_doc_fail());
						}
						else
						{
							snprintf(result,MAX_VALUE_LEN,"doc_rejected:%s", errDetails);
							WebcfgDebug("The result is %s\n",result);
							updateTmpList(docNode, gmp->name_space, gmp->etag, "failed", result, ccspStatus, 0, 0);
							if(docNode !=NULL && docNode->cloud_trans_id !=NULL)
							{
								addWebConfgNotifyMsg(gmp->name_space, gmp->etag, "failed", result, docNode->cloud_trans_id, 0, "status", ccspStatus, NULL, 200);
							}
						}
					}
					reqParam_destroy(paramCount, reqParam);
				}
				webcfgparam_destroy( pm );
			}
			else
			{
				WebcfgError("--------------decode root doc failed-------------\n");
				char * msg = NULL;
				msg = (char *)webcfgparam_strerror(err);
				err = getStatusErrorCodeAndMessage(DECODE_ROOT_FAILURE, &errmsg);
				snprintf(result,MAX_VALUE_LEN,"%s:%s", errmsg, msg);
				updateTmpList(docNode, gmp->name_space, gmp->etag, "failed", result, err, 0, 0);
				if(docNode !=NULL && docNode->cloud_trans_id !=NULL)
				{
					addWebConfgNotifyMsg(gmp->name_space, gmp->etag, "failed", result, docNode->cloud_trans_id,0, "status", err, NULL, 200);
				}
				WEBCFG_FREE(errmsg);
			}
			break;
		}
		else
		{
			WebcfgDebug("docName %s not found in mp list\n", docName);
			gmp = gmp->next;
		}
	}
	return rv;
}

WEBCFG_STATUS checkDBVersion(char *docname, uint32_t version)
{
	webconfig_db_data_t *webcfgdb = NULL;
	webcfgdb = get_global_db_node();

	//Traverse through doc list & check version for required doc
	while (NULL != webcfgdb)
	{
		WebcfgDebug("node is pointing to webcfgdb->name %s, docname %s, webcfgdb->version %lu, version %lu \n",webcfgdb->name, docname, (long)webcfgdb->version, (long)version);
		if( strcmp(docname, webcfgdb->name) == 0)
		{
			if(webcfgdb->version == version)
			{
				WebcfgInfo("webcfgdb version %lu is same for doc %s\n", (long)webcfgdb->version, docname);
				return WEBCFG_SUCCESS;
			}
		}
		webcfgdb= webcfgdb->next;
	}
	return WEBCFG_FAILURE;
}

WEBCFG_STATUS checkAndUpdateTmpRetryCount(webconfig_tmp_data_t *temp, char *docname)
{
	if (NULL != temp)
	{
		WebcfgDebug("checkAndUpdateTmpRetryCount: temp->name %s, temp->version %lu, temp->retry_count %d\n",temp->name, (long)temp->version, temp->retry_count);
		if( strcmp(docname, temp->name) == 0)
		{
			if(temp->retry_count >= MAX_APPLY_RETRY_COUNT)
			{
				WebcfgInfo("Apply retry_count %d has reached max limit for doc %s\n", temp->retry_count, docname);
				//send max retry notification to cloud only one time on the max retry attempt.
				if((temp->retry_count == MAX_APPLY_RETRY_COUNT) && (strcmp(temp->error_details, "max_retry_reached") !=0))
				{
					addWebConfgNotifyMsg(temp->name, temp->version, "failed", "max_retry_reached", temp->cloud_trans_id, 0,"status",0, NULL, 200);
					WebcfgDebug("update max_retry_reached to tmp list: ccsp error code %hu\n", temp->error_code);
					updateTmpList(temp, temp->name, temp->version, "failed", "max_retry_reached", temp->error_code, 0, 1);
				}
				return WEBCFG_FAILURE;
			}
			temp->retry_count++;
			WebcfgDebug("temp->retry_count updated to %d for docname %s\n",temp->retry_count, docname);
			return WEBCFG_SUCCESS;
		}
	}

	WebcfgError("checkAndUpdateTmpRetryCount failed for doc %s\n", docname);
	return WEBCFG_FAILURE;
}

uint32_t getDocVersionFromTmpList(webconfig_tmp_data_t *temp, char *docname)
{
	if (NULL != temp)
	{
		WebcfgDebug("getDocVersionFromTmpList: temp->name %s, temp->version %lu\n",temp->name, (long)temp->version);
		if( strcmp(docname, temp->name) == 0)
		{
			WebcfgDebug("return temp->version %lu for docname %s\n", (long)temp->version, docname);
			return temp->version;
		}
	}
	WebcfgDebug("getDocVersionFromTmpList failed for doc %s\n", docname);
	return 0;
}

//validate each event based on exact doc txid in tmp list.
WEBCFG_STATUS validateEvent(webconfig_tmp_data_t *temp, char *docname, uint16_t txid)
{
	if ((NULL != temp) && (docname != NULL))
	{
		WebcfgDebug("validateEvent: temp->name %s, temp->trans_id %hu\n",temp->name, temp->trans_id);
		if( strcmp(docname, temp->name) == 0)
		{
			if(txid == temp->trans_id)
			{
				WebcfgDebug("Valid event. Event txid %hu matches with temp trans_id %hu doc %s\n", txid, temp->trans_id, temp->name);
				return WEBCFG_SUCCESS;
			}
			else
			{
				WebcfgError("Not a valid event. Event txid %hu does not match with temp trans_id %hu doc %s\n", txid, temp->trans_id, temp->name);
			}
		}
	}
	WebcfgError("validateEvent failed for doc %s\n", docname);
	return WEBCFG_FAILURE;
}

//To get individual subdoc details from timer list.
expire_timer_t * getTimerNode(char *docname)
{
	expire_timer_t *temp = NULL;
	temp = get_global_timer_node();

	//Traverse through timer list & fetch required doc timer node.
	while (NULL != temp)
	{
		WebcfgDebug("getTimerNode: subdoc_name %s txid %hu timeout %lu running %d\n",temp->subdoc_name, temp->txid, (long)temp->timeout, temp->running);
		if( strcmp(docname, temp->subdoc_name) == 0)
		{
			return temp;
		}
		temp= temp->next;
	}
	WebcfgDebug("getTimerNode failed for doc %s\n", docname);
	return NULL;
}


void handleConnectedClientNotify(char *status)
{
	char tmpStr[32] = {'\0'};
	char *token = NULL;
	char *apply_status = NULL;
	const char s[2] = ";";
	char notif[20] = "";
	WDMP_STATUS ret = WDMP_FAILURE;
	char * paramName = NULL;
	param_t *attArr = NULL;

	if(status != NULL)
	{
		strncpy(tmpStr , status,(sizeof(tmpStr)-1));
		token = strtok(tmpStr, s);
		if( token != NULL )
		{
			apply_status = strtok(NULL, s);
			if(apply_status != NULL)
			{
				WebcfgInfo("apply_status is %s\n", apply_status);

				if(strcmp(apply_status, "enabled")==0)
				{
					snprintf(notif, sizeof(notif), "%d", 0);
				}
				else if(strcmp(apply_status, "disabled")==0)
				{
					snprintf(notif, sizeof(notif), "%d", 1);
				}
				else
				{
					WebcfgError("Invalid apply status in ACK event\n");
					return;
				}

				paramName = getConnClientParamName();
				if(paramName == NULL)
				{
					WebcfgError("Failed to get connected client paramName\n");
					return;
				}
				attArr = (param_t *) malloc(sizeof(param_t));
				if(attArr !=NULL)
				{
					memset(attArr,0,sizeof(param_t));
					attArr[0].name = paramName;
					#ifdef WEBCONFIG_BIN_SUPPORT
						attArr[0].value = strdup(apply_status);
						attArr[0].type = WDMP_STRING;
						int ccspStatus=0;
						setValues_rbus(attArr, 1, ATOMIC_SET_WEBCONFIG, NULL, NULL, &ret, &ccspStatus);
					#else
						attArr[0].value = (char *) malloc(sizeof(char) * 20);
						webcfgStrncpy(attArr[0].value, notif, 20);
						attArr[0].type = WDMP_INT;
						WebcfgDebug("Notify paramName %s\n", paramName);
						setAttributes(attArr, 1, NULL, &ret);
					#endif
					if (ret != WDMP_SUCCESS)
					{
						WebcfgError("setAttributes failed for parameter : %s notif:%s ret: %d\n", paramName, notif, ret);
					}
					else
					{
						WebcfgInfo("setAttributes success for parameter : %s notif:%s ret: %d\n",paramName, notif, ret);
					}
					WEBCFG_FREE(attArr[0].value);
					WEBCFG_FREE(attArr);
				}
			}
		}

	}
	return;
}

int checkTmpNACKstatus(webconfig_tmp_data_t *temp, char *docname)
{
	if (NULL != temp)
	{
		WebcfgDebug("checkTmpNACKstatus: temp->name %s, temp->error_details %s\n",temp->name, temp->error_details);
		if( strcmp(docname, temp->name) == 0)
		{
			if(strncmp(temp->error_details, "NACK", 4) ==0)
			{
				WebcfgInfo("Tmp list error_details is NACK for subdoc %s\n", docname);
				return 1;
			}
		}
	}
	WebcfgDebug("Tmp list is empty or error_details is not NACK\n");
	return 0;
}
