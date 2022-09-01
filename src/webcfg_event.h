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
#ifndef ERRHANDLE_H
#define ERRHANDLE_H

#include <stdint.h>
#include "webcfg.h"
#include "webcfg_db.h"

#define MAX_APPLY_RETRY_COUNT 3

typedef struct _event_data
{
	char * data;
	struct _event_data *next;
} event_data_t;

typedef struct _event_params
{
	char *subdoc_name;
	uint16_t trans_id;
	uint32_t version;
	char* status;
	uint32_t timeout;
	char *process_name;
	uint16_t err_code;
	char *failure_reason;
} event_params_t;


typedef struct expire_timer_list
{
	int running;
	uint32_t timeout;
	char *subdoc_name;
	uint16_t txid;
	struct expire_timer_list *next;
} expire_timer_t;

void initEventHandlingTask();
void processWebcfgEvents();

void webcfgCallback(char *Info, void* user_data);
WEBCFG_STATUS retryMultipartSubdoc(webconfig_tmp_data_t *docNode, char *docName);
WEBCFG_STATUS checkAndUpdateTmpRetryCount(webconfig_tmp_data_t *temp, char *docname);
uint32_t getDocVersionFromTmpList(webconfig_tmp_data_t *temp, char *docname);
void free_event_params_struct(event_params_t *param);
pthread_cond_t *get_global_event_con(void);
pthread_mutex_t *get_global_event_mut(void);
pthread_t get_global_event_threadid();
pthread_t get_global_process_threadid();
pthread_cond_t *get_global_event_con(void);
pthread_mutex_t *get_global_event_mut(void);
int checkTmpNACKstatus(webconfig_tmp_data_t *temp, char *docname);

#endif
