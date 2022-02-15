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
#ifndef REQUEST_H
#define REQUEST_H

#include <stdint.h>
#include <curl/curl.h>
#include "webcfg.h"
#include <wdmp-c.h>

#define WEBCFG_FREE(__x__) if(__x__ != NULL) { free((void*)(__x__)); __x__ = NULL;} else {printf("Trying to free null pointer\n");}

#define ATOMIC_SET_WEBCONFIG	    3
#define MAX_VALUE_LEN		128

#if ! defined(DEVICE_EXTENDER)
#define FACTORY_RESET_REBOOT_REASON      "factory-reset"
#define FW_UPGRADE_REBOOT_REASON         "Software_upgrade"
#define FORCED_FW_UPGRADE_REBOOT_REASON  "Forced_Software_upgrade"
#else
#define FACTORY_RESET_REBOOT_REASON      "UNKNOWN"
#define FW_UPGRADE_REBOOT_REASON         "UPGRADE"
#define FORCED_FW_UPGRADE_REBOOT_REASON  "UPGRADE"
#endif

typedef struct multipartdocs
{
    uint32_t  etag;
    char  *name_space;
    char  *data;
    size_t data_size;
    int isSupplementarySync; 
    struct multipartdocs *next;
} multipartdocs_t;

int readFromFile(char *filename, char **data, int *len);
WEBCFG_STATUS parseMultipartDocument(void *config_data, char *ct , size_t data_size, char* trans_uuid);
void getConfigDocList(char *docList);
void print_tmp_doc_list(size_t mp_count);
void loadInitURLFromFile(char **url);
uint32_t get_global_root();
WEBCFG_STATUS checkRootUpdate();
WEBCFG_STATUS checkRootDelete();
void updateRootVersionToDB();
void deleteRootAndMultipartDocs();
char * get_global_transID(void);
char* generate_trans_uuid();
void set_global_transID(char *id);
multipartdocs_t * get_global_mp(void);
void set_global_mp(multipartdocs_t *new);
void reqParam_destroy( int paramCnt, param_t *reqObj );
void failedDocsRetry();
WEBCFG_STATUS validate_request_param(param_t *reqParam, int paramCount);
void refreshConfigVersionList(char *versionsList, int http_status);
char * get_global_contentLen(void);
void set_global_contentLen(char * value);
void getRootDocVersionFromDBCache(uint32_t *rt_version, char **rt_string, int *subdoclist);
void derive_root_doc_version_string(char **rootVersion, uint32_t *root_ver, int status);
void reset_global_eventFlag();
int get_global_eventFlag(void);
void set_global_eventFlag();
void set_global_ETAG(char *etag);
char *get_global_ETAG(void);
pthread_t get_global_process_threadid();
void delete_multipart();
int get_multipartdoc_count();
WEBCFG_STATUS deleteFromMpList(char* doc_name);
void addToMpList(uint32_t etag, char *name_space, char *data, size_t data_size);
void delete_mp_doc();
void createCurlHeader( struct curl_slist *list, struct curl_slist **header_list, int status, char ** trans_uuid);
char *replaceMacWord(const char *s, const char *macW, const char *deviceMACW);
#endif
