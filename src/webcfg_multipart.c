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
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "webcfg_multipart.h"
#include "webcfg_param.h"
#include "webcfg_log.h"
#include "webcfg_auth.h"
#include "webcfg_db.h"
#include "webcfg_generic.h"
#include "webcfg_db.h"
#include "webcfg_notify.h"
#include "webcfg_blob.h"
#include "webcfg_event.h"
#include "webcfg_metadata.h"
#include "webcfg_timer.h"

#ifdef FEATURE_SUPPORT_AKER
#include "webcfg_aker.h"
#endif

#include <pthread.h>
#include <uuid/uuid.h>
#include <math.h>
#include <unistd.h>

#if defined(WEBCONFIG_BIN_SUPPORT)
#include "webcfg_rbus.h"
#endif

/*----------------------------------------------------------------------------*/
/*                                   Macros                                   */
/*----------------------------------------------------------------------------*/
#define MAX_HEADER_LEN			4096
#define ETAG_HEADER 		       "Etag:"
#define CONTENT_LENGTH_HEADER 	       "Content-Length:"
#define CURL_TIMEOUT_SEC	   25L
#if ! defined(DEVICE_EXTENDER)
#define CA_CERT_PATH 		   "/etc/ssl/certs/ca-certificates.crt"
#else
#define CA_CERT_PATH               "/usr/opensync/certs/ca.crt"
#endif
#define CCSP_CRASH_STATUS_CODE      192
#define MAX_PARAMETERNAME_LEN		4096
#define SUBDOC_TAG_COUNT            4
/*----------------------------------------------------------------------------*/
/*                               Data Structures                              */
/*----------------------------------------------------------------------------*/
struct token_data {
    size_t size;
    char* data;
};

/*----------------------------------------------------------------------------*/
/*                            File Scoped Variables                           */
/*----------------------------------------------------------------------------*/
static char g_ETAG[64]={'\0'};
char webpa_aut_token[4096]={'\0'};
static char g_interface[32]={'\0'};
static char g_systemReadyTime[64]={'\0'};
static char g_FirmwareVersion[64]={'\0'};
static char g_bootTime[64]={'\0'};
static char g_productClass[64]={'\0'};
static char g_ModelName[64]={'\0'};
static char g_PartnerID[64]={'\0'};
static char g_AccountID[64]={'\0'};
char g_RebootReason[64]={'\0'};
static char g_transID[64]={'\0'};
static char * g_contentLen = NULL;
static char *supportedVersion_header=NULL;
static char *supportedDocs_header=NULL;
static char *supplementaryDocs_header=NULL;
static multipartdocs_t *g_mp_head = NULL;
pthread_mutex_t multipart_t_mut =PTHREAD_MUTEX_INITIALIZER;
static int eventFlag = 0;
char * get_global_transID(void)
{
    return g_transID;
}

void set_global_transID(char *id)
{
	strncpy(g_transID, id, sizeof(g_transID)-1);
}

multipartdocs_t * get_global_mp(void)
{
    multipartdocs_t *tmp = NULL;
    pthread_mutex_lock (&multipart_t_mut);
    tmp = g_mp_head;
    pthread_mutex_unlock (&multipart_t_mut);
    return tmp;
}

void set_global_mp(multipartdocs_t *new)
{
	pthread_mutex_lock (&multipart_t_mut);
	g_mp_head = new;
	pthread_mutex_unlock (&multipart_t_mut);
}

char * get_global_contentLen(void)
{
	return g_contentLen;
}

void set_global_contentLen(char * value)
{
	g_contentLen = value;
}

int get_global_eventFlag(void)
{
    return eventFlag;
}

void reset_global_eventFlag()
{
	eventFlag = 0;
}

void set_global_eventFlag()
{
	eventFlag = 1;
}

void set_global_ETAG(char *etag)
{
	webcfgStrncpy(g_ETAG, etag, sizeof(g_ETAG));
}

char *get_global_ETAG(void)
{
    return g_ETAG;
}

/*----------------------------------------------------------------------------*/
/*                             Function Prototypes                            */
/*----------------------------------------------------------------------------*/
size_t writer_callback_fn(void *buffer, size_t size, size_t nmemb, struct token_data *data);
size_t headr_callback(char *buffer, size_t size, size_t nitems);
void stripspaces(char *str, char **final_str);
void line_parser(char *ptr, int no_of_bytes, char **name_space, uint32_t *etag, char **data, size_t *data_size);
void subdoc_parser(char *ptr, int no_of_bytes);
void addToDBList(webconfig_db_data_t *webcfgdb);
char* generate_trans_uuid();
WEBCFG_STATUS processMsgpackSubdoc(char *transaction_id);
void loadInitURLFromFile(char **url);
static void get_webCfg_interface(char **interface);

#ifdef FEATURE_SUPPORT_AKER
WEBCFG_STATUS checkAkerDoc();
#endif

/*----------------------------------------------------------------------------*/
/*                             External Functions                             */
/*----------------------------------------------------------------------------*/
/*
* @brief Initialize curl object with required options. create configData using libcurl.
* @param[out] configData
* @param[in] r_count Number of curl retries on ipv4 and ipv6 mode during failure
* @param[in] doc name to detect force sync
* @param[in] status device operational status
* @param[out] code curl response code
* @param[out] transaction_id use webpa transaction_id for webpa force sync, else generate new id.
* @param[out] contentType config data contentType 
* @return returns 0 if success, otherwise failed to fetch auth token and will be retried.
*/
WEBCFG_STATUS webcfg_http_request(char **configData, int r_count, int status, long *code, char **transaction_id, char* contentType, size_t *dataSize, char* docname)
{
	CURL *curl;
	CURLcode res;
	CURLcode time_res;
	struct curl_slist *list = NULL;
	struct curl_slist *headers_list = NULL;
	double total;
	long response_code = 0;
	char *interface = NULL;
	char *ct = NULL;
	char *webConfigURL = NULL;
	char *transID = NULL;
	char docList[512] = {'\0'};
	char configURL[256] = { 0 };
	char c[] = "{mac}";
	int rv = 0;

	int content_res=0;
	struct token_data data;
	data.size = 0;
	void * dataVal = NULL;
	char syncURL[256]={'\0'};
	char docname_upper[64]={'\0'};

	curl = curl_easy_init();
	if(curl)
	{
		//this memory will be dynamically grown by write call back fn as required
		data.data = (char *) malloc(sizeof(char) * 1);
		if(NULL == data.data)
		{
			WebcfgError("Failed to allocate memory.\n");
			return WEBCFG_FAILURE;
		}
		data.data[0] = '\0';
		createCurlHeader(list, &headers_list, status, &transID);
		if(transID !=NULL)
		{
			*transaction_id = strdup(transID);
			WEBCFG_FREE(transID);
		}
		WebcfgInfo("The get_global_supplementarySync() is %d\n", get_global_supplementarySync());
		if(get_global_supplementarySync() == 0)
		{
			//loadInitURLFromFile(&webConfigURL);
			Get_Webconfig_URL(configURL);
			WebcfgDebug("primary sync url fetched is %s\n", configURL);
		}
		else
		{
			if(docname != NULL && strlen(docname)>0)
			{
				WebcfgDebug("Supplementary sync for %s\n",docname);
				strncpy(docname_upper , docname,(sizeof(docname_upper)-1));
				docname_upper[0] = toupper(docname_upper[0]);
				WebcfgDebug("docname is %s and in uppercase is %s\n", docname, docname_upper);
				Get_Supplementary_URL(docname_upper, configURL);
				WebcfgDebug("Supplementary sync url fetched is %s\n", configURL);
				if( strcmp(configURL, "NULL") == 0)
				{
					WebcfgInfo("Supplementary sync with cloud is disabled as configURL is NULL\n");
					WEBCFG_FREE(data.data);
					curl_slist_free_all(headers_list);
					curl_easy_cleanup(curl);
					return WEBCFG_FAILURE;
				}
			}
		}
		if(strlen(configURL)>0)
		{
			//Replace {mac} string from default init url with actual deviceMAC
			WebcfgDebug("replaceMacWord to actual device mac\n");
			webConfigURL = replaceMacWord(configURL, c, get_deviceMAC());
			if(get_global_supplementarySync() == 0)
			{
				Set_Webconfig_URL(webConfigURL);
			}
			else
			{
				Set_Supplementary_URL(docname_upper, webConfigURL);
			}
		}
		else
		{
			WebcfgError("Failed to get configURL\n");
			WEBCFG_FREE(data.data);			
			curl_slist_free_all(headers_list);
			curl_easy_cleanup(curl);
			return WEBCFG_FAILURE;
		}
		WebcfgDebug("ConfigURL fetched is %s\n", webConfigURL);

		if(!get_global_supplementarySync())
		{
			//Update query param in the URL based on the existing doc names from db
			getConfigDocList(docList);
		}

		if(strlen(docList) > 0)
		{
			WebcfgInfo("docList is %s\n", docList);
			snprintf(syncURL, MAX_BUF_SIZE, "%s?group_id=%s", webConfigURL, docList);
			WEBCFG_FREE(webConfigURL);
			WebcfgDebug("syncURL is %s\n", syncURL);
			webConfigURL =strdup( syncURL);
		}

		if(webConfigURL !=NULL)
		{
			WebcfgInfo("Webconfig root ConfigURL is %s\n", webConfigURL);
			res = curl_easy_setopt(curl, CURLOPT_URL, webConfigURL );
		}
		else
		{
			WebcfgError("Failed to get webconfig configURL\n");
			WEBCFG_FREE(data.data);			
			curl_slist_free_all(headers_list);
			curl_easy_cleanup(curl);
			return WEBCFG_FAILURE;
		}
		res = curl_easy_setopt(curl, CURLOPT_TIMEOUT, CURL_TIMEOUT_SEC);
		WebcfgDebug("fetching interface from device.properties\n");
		if(strlen(g_interface) == 0)
		{
			//get_webCfg_interface(&interface);
		        interface = getParamValue(WEBCFG_INTERFACE_PARAM);
			if(interface !=NULL)
		        {
		               strncpy(g_interface, interface, sizeof(g_interface)-1);
		               WebcfgDebug("g_interface copied is %s\n", g_interface);
		               WEBCFG_FREE(interface);
		        }
		}
		WebcfgInfo("g_interface fetched is %s\n", g_interface);
		if(strlen(g_interface) > 0)
		{
			WebcfgDebug("setting interface %s\n", g_interface);
			res = curl_easy_setopt(curl, CURLOPT_INTERFACE, g_interface);
		}

		// set callback for writing received data
		dataVal = &data;
		res = curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writer_callback_fn);
		res = curl_easy_setopt(curl, CURLOPT_WRITEDATA, dataVal);

		res = curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers_list);

		res = curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headr_callback);

		// setting curl resolve option as default mode.
		//If any failure, retry with v4 first and then v6 mode.
		if(r_count == 1)
		{
			WebcfgInfo("curl Ip resolve option set as V4 mode\n");
			res = curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
		}
		else if(r_count == 2)
		{
			WebcfgInfo("curl Ip resolve option set as V6 mode\n");
			res = curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V6);
		}
		else
		{
			WebcfgInfo("curl Ip resolve option set as default mode\n");
			res = curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_WHATEVER);
		}
		res = curl_easy_setopt(curl, CURLOPT_CAINFO, CA_CERT_PATH);
		// disconnect if it is failed to validate server's cert
		res = curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
		// Verify the certificate's name against host
  		res = curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
		// To use TLS version 1.2 or later
  		res = curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
		// To follow HTTP 3xx redirections
  		res = curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		// Perform the request, res will get the return code
		res = curl_easy_perform(curl);
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
		WebcfgInfo("webConfig curl response %d http_code %ld\n", res, response_code);
		*code = response_code;
		time_res = curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &total);
		if(time_res == 0)
		{
			WebcfgInfo("curl response Time: %.1f seconds\n", total);
		}
		curl_slist_free_all(headers_list);
		WEBCFG_FREE(webConfigURL);
		if(res != 0)
		{
			WebcfgError("curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
		}
		else
		{
			if(response_code == 200)
                        {
				WebcfgDebug("checking content type\n");
				content_res = curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct);
				WebcfgInfo("ct is %s, content_res is %d\n", ct, content_res);

				if(ct !=NULL)
				{
					if(strncmp(ct, "multipart/mixed", 15) !=0)
					{
						WebcfgError("Content-Type is not multipart/mixed. Invalid\n");

						uint16_t err = 0;
						char* result = NULL;

						uint32_t version = strtoul(g_ETAG,NULL,0);
						err = getStatusErrorCodeAndMessage(INVALID_CONTENT_TYPE, &result);
						WebcfgDebug("The error_details is %s and err_code is %d\n", result, err);
						addWebConfgNotifyMsg("root", version, "failed", result, *transaction_id ,0, "status", err, NULL, 200);
						WEBCFG_FREE(result);
					}
					else
					{
						WebcfgInfo("Content-Type is multipart/mixed. Valid\n");
						strcpy(contentType, ct);

						*configData=data.data;
						*dataSize = data.size;
						WebcfgDebug("Data size is %d\n",(int)data.size);
						rv = 1;
					}
				}
			}
		}
		if(rv != 1)
		{
			WEBCFG_FREE(data.data);
		}
		curl_easy_cleanup(curl);
		return WEBCFG_SUCCESS;
	}
	else
	{
		WebcfgError("curl init failure\n");
	}
	return WEBCFG_FAILURE;
}

WEBCFG_STATUS parseMultipartDocument(void *config_data, char *ct , size_t data_size, char* trans_uuid)
{
	char *boundary = NULL;
	char *str=NULL;
	char *line_boundary = NULL;
	char *last_line_boundary = NULL;
	char *str_body = NULL;
	int boundary_len =0;
	int count =0;
	int status =0;
	uint16_t err = 0;
	char* result = NULL;
	
	WebcfgDebug("ct is %s\n", ct );
	// fetch boundary
	str = strtok(ct,";");
	str = strtok(NULL, ";");
	boundary= strtok(str,"=");
	boundary= strtok(NULL,"=");
	WebcfgDebug( "boundary %s\n", boundary );
	
	if(boundary !=NULL)
	{
		boundary_len= strlen(boundary);
		line_boundary  = (char *)malloc(sizeof(char) * (boundary_len +5));
		snprintf(line_boundary,boundary_len+5,"--%s\r\n",boundary);
		WebcfgDebug( "line_boundary %s, len %zu\n", line_boundary, strlen(line_boundary) );

		last_line_boundary  = (char *)malloc(sizeof(char) * (boundary_len + 5));
		snprintf(last_line_boundary,boundary_len+5,"--%s--",boundary);
		WebcfgDebug( "last_line_boundary %s, len %zu\n", last_line_boundary, strlen(last_line_boundary) );
		// Use --boundary to split
		str_body = malloc(sizeof(char) * data_size + 1);
		str_body = memcpy(str_body, config_data, data_size + 1);
		WEBCFG_FREE(config_data);
		int num_of_parts = 0;
		char *ptr_lb=str_body;
		char *ptr_lb1=str_body;
		char *ptr_count = str_body;
		int index1=0, index2 =0;

		/* For Subdocs count */
		while((ptr_count - str_body) < (int)data_size )
		{
			ptr_count = memchr(ptr_count, '-', data_size - (ptr_count - str_body));
			if(0 == memcmp(ptr_count, last_line_boundary, strlen(last_line_boundary)))
			{
				num_of_parts++;
				break;
			}
			else if(0 == memcmp(ptr_count, line_boundary, strlen(line_boundary)))
			{
				num_of_parts++;
			}
			ptr_count++;
		}
		WebcfgInfo("Size of the docs is :%d\n", (num_of_parts-1));

		///Subdoc contents are retrieved with boundary as delimiter
		delete_mp_doc();
		while((ptr_lb - str_body) < (int)data_size)
		{
			ptr_lb = memchr(ptr_lb, '-', data_size - (ptr_lb - str_body));
			if(0 == memcmp(ptr_lb, last_line_boundary, strlen(last_line_boundary)))
			{
				WebcfgDebug("last line boundary \n");
				break;
			}
			else if(0 == memcmp(ptr_lb, line_boundary, strlen(line_boundary)))
			{
				ptr_lb = ptr_lb+(strlen(line_boundary))-1;
				ptr_lb1 = ptr_lb+1;
				num_of_parts = 1;
				while(0 != num_of_parts % 2)
				{
					ptr_lb1 = memchr(ptr_lb1, '-', data_size - (ptr_lb1 - str_body));
					if(0 == memcmp(ptr_lb1, last_line_boundary, strlen(last_line_boundary)))
					{
						index2 = ptr_lb1-str_body;
						index1 = ptr_lb-str_body;
						subdoc_parser(str_body+index1,index2 - index1 - 2);
						break;
					}
					else if(0 == memcmp(ptr_lb1, line_boundary, strlen(line_boundary)))
					{
						index2 = ptr_lb1-str_body;
						index1 = ptr_lb-str_body;
						subdoc_parser(str_body+index1,index2 - index1 - 2);
						num_of_parts++;
						count++;
					}
					ptr_lb1 = memchr(ptr_lb1, '\n', data_size - (ptr_lb1 - str_body));
					ptr_lb1++;
				}
			}
			ptr_lb = memchr(ptr_lb, '\n', data_size - (ptr_lb - str_body));
			ptr_lb++;
		}
		WEBCFG_FREE(str_body);
		WEBCFG_FREE(line_boundary);
		WEBCFG_FREE(last_line_boundary);

		if(get_multipartdoc_count() == 0)
		{
			WebcfgError("Multipart list is empty\n");
			return WEBCFG_FAILURE;
		}

		status = processMsgpackSubdoc(trans_uuid);
		if(status ==0)
		{
			WebcfgInfo("processMsgpackSubdoc success\n");
			return WEBCFG_SUCCESS;
		}
		else
		{
			WebcfgDebug("processMsgpackSubdoc done,docs are sent for apply\n");
		}
		return WEBCFG_FAILURE;
	}
    else
    {
		WebcfgError("Multipart Boundary is NULL\n");
		uint32_t version = strtoul(g_ETAG, NULL, 0);
		err = getStatusErrorCodeAndMessage(MULTIPART_BOUNDARY_NULL, &result);
		addWebConfgNotifyMsg("root", version, "failed", result, trans_uuid ,0, "status", err, NULL, 200);
		WEBCFG_FREE(result);
		return WEBCFG_FAILURE;
    }
}

WEBCFG_STATUS processMsgpackSubdoc(char *transaction_id)
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
	webcfgparam_t *pm = NULL;
	int success_count = 0;
	WEBCFG_STATUS addStatus =0;
	WEBCFG_STATUS subdocStatus = 0;
	uint16_t doc_transId = 0;

#ifdef FEATURE_SUPPORT_AKER
	int backoffRetryTime = 0;
	int max_retry_sleep = 0;
	int backoff_max_time = 6;
	int c=4;
	multipartdocs_t *akerIndex = NULL;
	int akerSet = 0;
#endif
	int mp_count = 0;
	int current_doc_count = 0;
	int err = 0;
	char * errmsg = NULL;

	int sendMsgSize = 0;
#ifdef WEBCONFIG_BIN_SUPPORT
	void *buff = NULL;
#endif

	mp_count = get_multipartdoc_count();
	if(transaction_id !=NULL)
	{
		strncpy(g_transID, transaction_id, sizeof(g_transID)-1);
		WebcfgDebug("g_transID is %s\n", g_transID);
		WEBCFG_FREE(transaction_id);
	}
        
	WebcfgDebug("Add mp entries to tmp list\n");
	addStatus = addToTmpList();
	if(addStatus == WEBCFG_SUCCESS)
	{
		WebcfgInfo("Added %d mp entries To tmp List\n", get_numOfMpDocs());
		print_tmp_doc_list(mp_count+1);
	}
	else
	{
		WebcfgError("addToTmpList failed\n");
		uint32_t version = strtoul(g_ETAG,NULL,0);
		err = getStatusErrorCodeAndMessage(ADD_TO_CACHE_LIST_FAILURE, &errmsg);
		WebcfgDebug("The error_details is %s and err_code is %d\n", errmsg, err);
		addWebConfgNotifyMsg("root", version, "failed", errmsg, get_global_transID() ,0, "status", err, NULL, 200);
		WEBCFG_FREE(errmsg);
		err = 0;
	}

	WebcfgDebug("mp->entries_count is %d\n",mp_count);

	multipartdocs_t *mp = NULL;
	mp = get_global_mp();

	while(mp != NULL)
	{
		ret = WDMP_FAILURE;
		err = 0;

		webconfig_tmp_data_t * subdoc_node = NULL;
		subdoc_node = getTmpNode(mp->name_space);

		if(subdoc_node == NULL)
		{
			WebcfgDebug("Failed to get subdoc_node from tmp list\n");
			mp = mp->next;
			continue;
		}

		WebcfgDebug("check for current docs\n");
		//Process subdocs with status "pending_apply" which indicates docs from current sync, skip all others.
		if(strcmp(subdoc_node->status, "pending_apply") != 0)
		{
			WebcfgDebug("skipped setValues for doc %s as it is already processed\n", mp->name_space);
			mp = mp->next;
			continue;
		}
		else
		{
			updateTmpList(subdoc_node, mp->name_space, subdoc_node->version, "pending", "none", 0, 0, 0);
			//current_doc_count indicates current primary docs count.
			if(subdoc_node->isSupplementarySync == 0)
			{
				current_doc_count++;
				WebcfgDebug("current_doc_count incremented to %d\n", current_doc_count);
			}
		}
		WebcfgDebug("mp->name_space %s\n", mp->name_space);
		WebcfgDebug("mp->etag %lu\n" , (long)mp->etag);
		WebcfgDebug("mp->data %s\n" , mp->data);

		WebcfgDebug("mp->data_size is %zu\n", mp->data_size);
#ifdef FEATURE_SUPPORT_AKER
		if(strcmp(mp->name_space, "aker") == 0)
		{
			akerIndex = mp;
			akerSet = 1;
			WebcfgDebug("skip aker doc and process at the end\n");
			mp = mp->next;
			continue;
		}
#endif
		WebcfgDebug("--------------decode root doc-------------\n");
		pm = webcfgparam_convert( mp->data, mp->data_size+1 );
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
						appended_doc = webcfg_appendeddoc( mp->name_space, mp->etag, pm->entries[i].value, pm->entries[i].value_size, &doc_transId, &sendMsgSize);
						if(appended_doc != NULL)
						{
							WebcfgDebug("webcfg_appendeddoc doc_transId : %hu\n", doc_transId);
							if(pm->entries[i].name !=NULL)
							{
								reqParam[i].name = strdup(pm->entries[i].name);
							}
							#ifdef WEBCONFIG_BIN_SUPPORT
								if(isRbusEnabled() && isRbusListener(mp->name_space))
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
						}
						//Update doc trans_id to validate events.
						WebcfgDebug("Update doc trans_id to validate events.\n");
						updateTmpList(subdoc_node, mp->name_space, mp->etag, "pending", "none", 0, doc_transId, 0);
						//If request type is BLOB, start event handler thread to process various error handling operations based on the events received from components.
						if(eventFlag == 0)
						{
							WebcfgInfo("starting initEventHandlingTask\n");
							initEventHandlingTask();
							processWebcfgEvents();
							eventFlag = 1;
						}
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
				WebcfgDebug("Request:> param[%d].type = %d\n",i,reqParam[i].type);
			}

			if(reqParam !=NULL && validate_request_param(reqParam, paramCount) == WEBCFG_SUCCESS)
			{
				WebcfgDebug("Proceed to setValues..\n");
				if((checkAndUpdateTmpRetryCount(subdoc_node, mp->name_space))== WEBCFG_SUCCESS)
				{
					WebcfgDebug("WebConfig SET Request\n");
					#ifdef WEBCONFIG_BIN_SUPPORT
						// rbus_enabled and rbus_listener_supported, rbus_set direct API used to send binary data to component.
						if(isRbusEnabled() && isRbusListener(mp->name_space))
						{
							char topic[64] = {0};
					        	get_destination(mp->name_space, topic);
							blobSet_rbus(topic, buff, sendMsgSize, &ret, &ccspStatus);
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
					
					
					if(ret == WDMP_SUCCESS)
					{
						WebcfgInfo("setValues success. ccspStatus : %d\n", ccspStatus);
						WebcfgDebug("reqParam[0].type is %d WDMP_BASE64 %d\n", reqParam[0].type, WDMP_BASE64);
						if(reqParam[0].type  == WDMP_BASE64)
						{
							WebcfgDebug("blob subdoc set is success\n");
						}
						else
						{
							WebcfgDebug("update doc status for %s\n", mp->name_space);
							updateTmpList(subdoc_node, mp->name_space, mp->etag, "success", "none", 0, 0, 0);
							//send success notification to cloud
							WebcfgDebug("send notify for mp->name_space %s\n", mp->name_space);
							if(subdoc_node !=NULL && subdoc_node->cloud_trans_id !=NULL)
							{
								addWebConfgNotifyMsg(mp->name_space, mp->etag, "success", "none", subdoc_node->cloud_trans_id,0, "status",0, NULL, 200);
							}
							WebcfgDebug("deleteFromTmpList as doc is applied\n");
							if(mp->isSupplementarySync == 0)
							{
								checkDBList(mp->name_space,mp->etag, NULL);
								success_count++;
							}
						}

						WebcfgDebug("The mp->entries_count %d\n",mp_count);
						WebcfgDebug("The current doc  count in primary sync is %d\n",current_doc_count);
						WebcfgDebug("The count %d\n",success_count);
						if(success_count == current_doc_count && get_global_supplementarySync() == 0)
						{
							char * temp = strdup(g_ETAG);
							uint32_t version=0;
							if(temp)
							{
								version = strtoul(temp,NULL,0);
								WEBCFG_FREE(temp);
							}
							if(version != 0)
							{
								checkDBList("root",version, NULL);
								success_count++;
							}

							WebcfgInfo("The Etag is %lu\n",(long)version );

							if(checkRootDelete() == WEBCFG_SUCCESS)
							{
								//Delete tmp queue root as all docs are applied
								WebcfgInfo("Delete tmp queue root as all docs are applied\n");
								WebcfgDebug("root version to delete is %lu\n", (long)version);
								reset_numOfMpDocs();
								delete_tmp_list(); 
							}
							WebcfgDebug("processMsgpackSubdoc is success as all the docs are applied\n");
							rv = WEBCFG_SUCCESS;
						}
					}
					else
					{
						if(ccspStatus == 9005)
						{
							subdocStatus = isSubDocSupported(mp->name_space);
							if(subdocStatus != WEBCFG_SUCCESS)
							{
								WebcfgDebug("The ccspstatus is %d\n",ccspStatus);
								ccspStatus = 204;
							}
						}
						WebcfgError("setValues Failed. ccspStatus : %d\n", ccspStatus);
						errd = mapStatus(ccspStatus);
						WebcfgDebug("The errd value is %d\n",errd);

						mapWdmpStatusToStatusMessage(errd, errDetails);
						WebcfgDebug("The errDetails value is %s\n",errDetails);

						WebcfgInfo("subdoc_name and err_code : %s %d\n",mp->name_space,ccspStatus);
						WebcfgInfo("failure_reason %s\n",errDetails);
						//Update error_details to tmp list and send failure notification to cloud.
						if((ccspStatus == CCSP_CRASH_STATUS_CODE) || (ccspStatus == 204) || (ccspStatus == 191) || (ccspStatus == 193) || (ccspStatus == 190))
						{
							subdocStatus = isSubDocSupported(mp->name_space);
							WebcfgDebug("ccspStatus is %d\n", ccspStatus);
							if(ccspStatus == 204 && subdocStatus != WEBCFG_SUCCESS)
							{
								snprintf(result,MAX_VALUE_LEN,"doc_unsupported:%s", errDetails);
							}
							else
							{
								long long expiry_time = 0;
								expiry_time = getRetryExpiryTimeout();
								set_doc_fail(1);

								updateFailureTimeStamp(subdoc_node, mp->name_space, expiry_time);
								WebcfgDebug("The retry_timer is %d and timeout generated is %lld\n", get_retry_timer(), expiry_time);
								//To get the exact time diff for retry from present time.
								updateRetryTimeDiff(expiry_time);
								snprintf(result,MAX_VALUE_LEN,"failed_retrying:%s", errDetails);
							}
							WebcfgDebug("The result is %s\n",result);
							updateTmpList(subdoc_node, mp->name_space, mp->etag, "failed", result, ccspStatus, 0, 1);
							if(subdoc_node !=NULL && subdoc_node->cloud_trans_id !=NULL)
							{
								addWebConfgNotifyMsg(mp->name_space, mp->etag, "failed", result, subdoc_node->cloud_trans_id, 0,"status",ccspStatus, NULL, 200);
							}
							WebcfgDebug("checkRootUpdate\n");
							//No root update for supplementary sync
							if(!get_global_supplementarySync() && (ccspStatus == 204 && subdocStatus != WEBCFG_SUCCESS) && (checkRootUpdate() == WEBCFG_SUCCESS))
							{
								WebcfgDebug("updateRootVersionToDB\n");
								updateRootVersionToDB();
								WebcfgDebug("check deleteRootAndMultipartDocs\n");
								deleteRootAndMultipartDocs();
								addNewDocEntry(get_successDocCount());
								if(NULL != reqParam)
								{
									reqParam_destroy(paramCount, reqParam);
								}
								webcfgparam_destroy( pm );
								break;
							}

							WebcfgDebug("the retry flag value is %d\n", get_doc_fail());
						}
						else
						{
							snprintf(result,MAX_VALUE_LEN,"doc_rejected:%s", errDetails);
							WebcfgDebug("The result is %s\n",result);
							updateTmpList(subdoc_node, mp->name_space, mp->etag, "failed", result, ccspStatus, 0, 0);
							if(subdoc_node !=NULL && subdoc_node->cloud_trans_id !=NULL)
							{
								addWebConfgNotifyMsg(mp->name_space, mp->etag, "failed", result, subdoc_node->cloud_trans_id,0, "status", ccspStatus, NULL, 200);
							}
						}
						//print_tmp_doc_list(mp_count);
					}
				}
				else
				{
					WebcfgError("Update retry count failed for doc %s\n", mp->name_space);
					err = getStatusErrorCodeAndMessage(FAILED_TO_SET_BLOB, &errmsg);
					WebcfgDebug("The error_details is %s and err_code is %d\n", errmsg, err);
					updateTmpList(subdoc_node, mp->name_space, mp->etag, "failed", errmsg, err, 0, 0);
					if(subdoc_node !=NULL && subdoc_node->cloud_trans_id !=NULL)
					{
						addWebConfgNotifyMsg(mp->name_space, mp->etag, "failed", errmsg,  subdoc_node->cloud_trans_id ,0, "status", err, NULL, 200);
					}
					WEBCFG_FREE(errmsg);
				}

				if(NULL != reqParam)
				{
					reqParam_destroy(paramCount, reqParam);
				}
			}
			else
			{
				err = getStatusErrorCodeAndMessage(BLOB_PARAM_VALIDATION_FAILURE, &errmsg);
				WebcfgDebug("The error_details is %s and err_code is %d\n", errmsg, err);
				updateTmpList(subdoc_node, mp->name_space, mp->etag, "failed", errmsg, err, 0, 0);
				addWebConfgNotifyMsg(mp->name_space, mp->etag, "failed", errmsg, get_global_transID() ,0, "status", err, NULL, 200);
				WEBCFG_FREE(errmsg);
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

			updateTmpList(subdoc_node, mp->name_space, mp->etag, "failed", result, err, 0, 0);
			if(subdoc_node !=NULL && subdoc_node->cloud_trans_id !=NULL)
			{
				addWebConfgNotifyMsg(mp->name_space, mp->etag, "failed", result, subdoc_node->cloud_trans_id,0, "status", err, NULL, 200);
			}
			WEBCFG_FREE(errmsg);
		}
		mp = mp->next;
	}
	WebcfgDebug("The current_doc_count is %d\n",current_doc_count);

#ifdef FEATURE_SUPPORT_AKER
	//Apply aker doc at the end when all other docs are processed.
	if(akerSet)
	{
		AKER_STATUS akerStatus = AKER_FAILURE;
		webconfig_tmp_data_t * subdoc_node = NULL;
		subdoc_node = getTmpNode(akerIndex->name_space);
		max_retry_sleep = (int) pow(2, backoff_max_time) -1;
		WebcfgDebug("max_retry_sleep is %d\n", max_retry_sleep );

		while(1)
		{
			//check aker ready and is registered to parodus using RETRIEVE request to parodus.
			WebcfgDebug("AkerStatus to check service ready\n");
			if(checkAkerStatus() != WEBCFG_SUCCESS)
			{
				WebcfgError("Aker is not ready to process requests, retry is required\n");
				updateTmpList(subdoc_node, akerIndex->name_space, akerIndex->etag, "pending", "aker_service_unavailable", 0, 0, 0);
				if(backoffRetryTime >= max_retry_sleep)
				{
					WebcfgError("aker doc max retry reached\n");
					updateAkerMaxRetry(subdoc_node, "aker");
					break;
				}

				if(backoffRetryTime < max_retry_sleep)
				{
					backoffRetryTime = (int) pow(2, c) -1;
				}
				WebcfgError("aker doc is pending, retrying in backoff interval %dsec\n", backoffRetryTime);
				if (akerwait__ (backoffRetryTime))
				{
					WebcfgInfo("g_shutdown true, break checkAkerStatus\n");
					break;
				}
				c++;
			}
			else
			{
				WebcfgDebug("process aker sub doc\n");
				akerStatus = processAkerSubdoc(subdoc_node, akerIndex);
				WebcfgInfo("Aker doc processed. akerStatus %d\n", akerStatus);
				break;
			}
		}
		akerSet= 0;
	}
#endif

	if(success_count) //No DB update when all docs failed.
	{

		webconfig_db_data_t* temp1 = NULL;
		temp1 = get_global_db_node();

		while(temp1 )
		{
			WebcfgInfo("DB wd->name: %s, version: %lu\n",  temp1->name, (long)temp1->version);
			temp1 = temp1->next;
		}
		WebcfgDebug("addNewDocEntry\n");
		addNewDocEntry(get_successDocCount());
	}

	/*WebcfgDebug("Proceed to generateBlob\n");
	if(generateBlob() == WEBCFG_SUCCESS)
	{
		blob_data = get_DB_BLOB_base64(&blob_len);
		if(blob_data !=NULL)
		{
			WebcfgDebug("The b64 encoded blob is : %s\n",blob_data);
			WebcfgInfo("The b64 encoded blob_length is : %zu\n",strlen(blob_data));
			WEBCFG_FREE(blob_data);
		}
		else
		{
			WebcfgError("Failed in blob base64 encode\n");
		}
	}
	else
	{
		WebcfgError("Failed in Blob generation\n");
	}*/

	return rv;
}
/*----------------------------------------------------------------------------*/
/*                             Internal functions                             */
/*----------------------------------------------------------------------------*/

/* @brief callback function for writing libcurl received data
 * @param[in] buffer curl delivered data which need to be saved.
 * @param[in] size size is always 1
 * @param[in] nmemb size of delivered data
 * @param[out] data curl response data saved.
*/
size_t writer_callback_fn(void *buffer, size_t size, size_t nmemb, struct token_data *data)
{
    size_t index = data->size;
    size_t n = (size * nmemb);
    char* tmp; 
    data->size += (size * nmemb);

    tmp = realloc(data->data, data->size + 1); // +1 for '\0' 

    if(tmp) {
        data->data = tmp;
    } else {
        if(data->data) {
            free(data->data);
        }
        WebcfgError("Failed to allocate memory for data\n");
        return 0;
    }
    memcpy((data->data + index), buffer, n);
    data->data[data->size] = '\0';
    WebcfgDebug("size * nmemb is %zu\n", size * nmemb);
    return size * nmemb;
}

/* @brief callback function to extract response header data.
   This is to get multipart root version which is received as header.
*/
size_t headr_callback(char *buffer, size_t size, size_t nitems)
{
	size_t etag_len = 0;
	char* header_value = NULL;
	char* final_header = NULL;
	char header_str[64] = {'\0'};
	size_t content_len = 0;

	etag_len = strlen(ETAG_HEADER);
	content_len = strlen(CONTENT_LENGTH_HEADER);
	if( nitems > etag_len )
	{
		if( strncasecmp(ETAG_HEADER, buffer, etag_len) == 0 )
		{
			header_value = strtok(buffer, ":");
			while( header_value != NULL )
			{
				header_value = strtok(NULL, ":");
				if(header_value !=NULL)
				{
					strncpy(header_str, header_value, sizeof(header_str)-1);
					stripspaces(header_str, &final_header);
					//g_ETAG should be updated only for primary sync.
					if(!get_global_supplementarySync())
					{
						strncpy(g_ETAG, final_header, sizeof(g_ETAG)-1);
						WebcfgInfo("g_ETAG updated for primary sync is %s\n", g_ETAG);
					}
				}
			}
		}

		if( strncasecmp(CONTENT_LENGTH_HEADER, buffer, content_len) == 0 )
		{
			header_value = strtok(buffer, ":");
			while( header_value != NULL )
			{
				header_value = strtok(NULL, ":");
				if(header_value != NULL)
				{
					strncpy(header_str, header_value, sizeof(header_str)-1);
					stripspaces(header_str, &final_header);
					if(g_contentLen != NULL)
					{
						WEBCFG_FREE(g_contentLen);
					}
					g_contentLen = strdup(final_header);
				}
			}
			WebcfgDebug("g_contentLen is %s\n", g_contentLen);
		}
	}
	WebcfgDebug("header_callback size %zu\n", size);
	return nitems;
}

//Convert string root version from multipart header to uint32
uint32_t get_global_root()
{
	char * temp = NULL;
	uint32_t g_version = 0;

	temp = strdup(g_ETAG);
	if(temp)
	{
		g_version = strtoul(temp,NULL,0);
		WEBCFG_FREE(temp);
	}
	WebcfgDebug("g_version is %lu\n", (long)g_version);
	return g_version;
}

//To strip all spaces , new line & carriage return characters from header output
void stripspaces(char *str, char **final_str)
{
	int i=0, j=0;

	for(i=0;str[i]!='\0';++i)
	{
		if(str[i]!=' ')
		{
			if(str[i]!='\n')
			{
				if(str[i]!='\r')
				{
					str[j++]=str[i];
				}
			}
		}
	}
	str[j]='\0';
	*final_str = str;
}

static void get_webCfg_interface(char **interface)
{
#if ! defined(DEVICE_EXTENDER)
        FILE *fp = fopen(DEVICE_PROPS_FILE, "r");

        if (NULL != fp)
        {
                char str[255] = {'\0'};
                while (fgets(str, sizeof(str), fp) != NULL)
                {
                    char *value = NULL;

                    if(NULL != (value = strstr(str, "WEBCONFIG_INTERFACE=")))
                    {
                        value = value + strlen("WEBCONFIG_INTERFACE=");
                        value[strlen(value)-1] = '\0';
                        *interface = strdup(value);
                        break;
                    }

                }
                fclose(fp);
        }
        else
        {
                WebcfgError("Failed to open device.properties file:%s\n", DEVICE_PROPS_FILE);
        }

        if (NULL == *interface)
        {
                WebcfgError("WebConfig interface is not present in device.properties\n");

        }
        else
        {
                WebcfgDebug("interface fetched is %s\n", *interface);
        }
#endif
}

void loadInitURLFromFile(char **url)
{
	FILE *fp = fopen(DEVICE_PROPS_FILE, "r");

	if (NULL != fp)
	{
		char str[255] = {'\0'};
		while (fgets(str, sizeof(str), fp) != NULL)
		{
		    char *value = NULL;
                    if(NULL != (value = strstr(str, "WEBCONFIG_INIT_URL=")))
		    {
			value = value + strlen("WEBCONFIG_INIT_URL=");
			value[strlen(value)-1] = '\0';
			*url = strdup(value);
			break;
		    }

		}
		fclose(fp);
	}
	else
	{
		WebcfgError("Failed to open device.properties file:%s\n", DEVICE_PROPS_FILE);
	}
	if (NULL == *url)
	{
		WebcfgError("WebConfig url is not present in device.properties\n");
	}
	else
	{
		WebcfgDebug("url fetched is %s\n", *url);
	}
}

int readFromFile(char *filename, char **data, int *len)
{
	FILE *fp;
	size_t sz;
	int ch_count = 0;
	fp = fopen(filename, "r+");
	if (fp == NULL)
	{
	#ifdef MULTIPART_UTILITY
		WebcfgDebug("Failed to open file %s\n", filename);
	#else
		WebcfgError("Failed to open file %s\n", filename);
	#endif
		return 0;
	}
	fseek(fp, 0, SEEK_END);
	ch_count = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	*data = (char *) malloc(sizeof(char) * (ch_count + 1));
	sz = fread(*data, 1, ch_count-1,fp);
	if (!sz) 
	{	
		fclose(fp);
		WebcfgError("fread failed.\n");
		WEBCFG_FREE(*data);
		return WEBCFG_FAILURE;
	}
	*len = ch_count;
	(*data)[ch_count] ='\0';
	fclose(fp);
	return 1;
}

/* Traverse through db list to get docnames of all docs with root.
e.g. root,ble,lan,mesh,moca. */
void getConfigDocList(char *docList)
{
	char *docList_tmp = NULL;
	webconfig_db_data_t *temp = NULL;
	temp = get_global_db_node();

	if( NULL != temp)
	{
		sprintf(docList, "%s", "root");
		WebcfgDebug("docList is %s\n", docList);

		while (NULL != temp)
		{
			if( temp->name != NULL)
			{
				if( strcmp(temp->name,"root") !=0 )
				{
					docList_tmp = strdup(docList);
					sprintf(docList, "%s,%s",docList_tmp, temp->name);
					WEBCFG_FREE(docList_tmp);
				}
			}
			temp= temp->next;
		}
		WebcfgDebug("Final docList is %s len %zu\n", docList, strlen(docList));
	}
}

void getRootDocVersionFromDBCache(uint32_t *rt_version, char **rt_string, int *subdoclist)
{
	webconfig_db_data_t *temp = NULL;
	temp = get_global_db_node();

	while (NULL != temp)
	{
		if( strcmp(temp->name, "root") == 0)
		{
			*rt_version = temp->version;
			if(temp->root_string !=NULL)
			{
				*rt_string = strdup(temp->root_string);
			}
			WebcfgDebug("rt_version %lu rt_string %s from DB list\n", (long)*rt_version, *rt_string);
		}
		temp= temp->next;
		*subdoclist = *subdoclist+1;
	}
	WebcfgDebug("*subdoclist is %d\n", *subdoclist);
}

void derive_root_doc_version_string(char **rootVersion, uint32_t *root_ver, int status)
{
	FILE *fp = NULL;
	char *reason = NULL;
	uint32_t db_root_version = 0;
	char *db_root_string = NULL;
	int subdocList = 0;
	static int reset_once = 0;

	if(strlen(g_RebootReason) == 0)
	{
		reason = getRebootReason();
		if(reason !=NULL)
		{
		       strncpy(g_RebootReason, reason, sizeof(g_RebootReason)-1);
		       WebcfgDebug("g_RebootReason fetched is %s\n", g_RebootReason);
		       WEBCFG_FREE(reason);
		}
		else
		{
			WebcfgError("Failed to get reboot reason\n");
		}
	}

	if(strlen(g_RebootReason)> 0)
	{
		WebcfgInfo("reboot reason is %s\n", g_RebootReason);

		fp = fopen(WEBCFG_DB_FILE,"rb");
		if (fp == NULL)
		{
			if(strncmp(g_RebootReason,FACTORY_RESET_REBOOT_REASON,strlen(FACTORY_RESET_REBOOT_REASON))==0)
			{
				*rootVersion = strdup("NONE");
			}
			else if((strncmp(g_RebootReason,FW_UPGRADE_REBOOT_REASON,strlen(FW_UPGRADE_REBOOT_REASON))==0) || (strncmp(g_RebootReason,FORCED_FW_UPGRADE_REBOOT_REASON,strlen(FORCED_FW_UPGRADE_REBOOT_REASON))==0))
			{
				*rootVersion = strdup("NONE-MIGRATION");
			}
			else
			{
				*rootVersion = strdup("NONE-REBOOT");
			}
		}
		else
		{
			fclose(fp);
			//get existing root version from DB
			getRootDocVersionFromDBCache(&db_root_version, &db_root_string, &subdocList);
			WebcfgDebug("db_root_version %lu db_root_string %s subdocList %d\n", (long)db_root_version, db_root_string, subdocList);

			if(db_root_string !=NULL)
			{
				if((strcmp(db_root_string,"POST-NONE")==0) && (strcmp(g_RebootReason,FW_UPGRADE_REBOOT_REASON)!=0) && (strcmp(g_RebootReason,FORCED_FW_UPGRADE_REBOOT_REASON)!=0) && (strcmp(g_RebootReason,FACTORY_RESET_REBOOT_REASON)!=0))
				{
					*rootVersion = strdup("NONE-REBOOT");
					WEBCFG_FREE(db_root_string);
					return;
				}
				else if(status == 404 && ((strcmp(db_root_string, "NONE") == 0) || (strcmp(db_root_string, "NONE-MIGRATION") == 0) || (strcmp(db_root_string, "NONE-REBOOT") == 0)))
				{
					*rootVersion = strdup("POST-NONE");
					WEBCFG_FREE(db_root_string);
					return;
				}
				else if(status == 200 && (strcmp(db_root_string, "NONE") == 0))
				{
					WebcfgInfo("Received factory reset Ack from server, set root to POST-NONE\n");
					*rootVersion = strdup("POST-NONE");
					WEBCFG_FREE(db_root_string);
					return;
				}
			}

			//check existing root version
			WebcfgDebug("check existing root version\n");
			if(db_root_version)
			{
			//when subdocs are applied and reboot due to software migration/rollback, root reset to 0.
				if( (reset_once == 0) && (subdocList > 1) && ( (strcmp(g_RebootReason,FW_UPGRADE_REBOOT_REASON)==0) || (strcmp(g_RebootReason,FORCED_FW_UPGRADE_REBOOT_REASON)==0) ) )
				{
					WebcfgInfo("reboot due to software migration/rollback, root reset to 0\n");
					*root_ver = 0;
					reset_once = 1;
					return;
				}
				WebcfgDebug("Update rootVersion with db_root_version %lu\n", (long)db_root_version);
			}
			else
			{
				if(db_root_string !=NULL)
				{
					WebcfgDebug("Update rootVersion with db_root_string %s\n", db_root_string);
					*rootVersion = strdup(db_root_string);
					WEBCFG_FREE(db_root_string);
				}
			}
			*root_ver = db_root_version;
		}
	}
}

/* Traverse through db list to get versions of all docs with root.
e.g. IF-NONE-MATCH: 123,44317,66317,77317 where 123 is root version.
Currently versionsList length is fixed to 512 which can support up to 45 docs.
This can be increased if required. */
void refreshConfigVersionList(char *versionsList, int http_status)
{
	char *versionsList_tmp = NULL;
	char *root_str = NULL;
	uint32_t root_version = 0;

	//initialize to default value "0".
	snprintf(versionsList, 512, "%s", "0");

	derive_root_doc_version_string(&root_str, &root_version, http_status);
	WebcfgDebug("update root_version %lu rootString %s to DB\n", (long)root_version, root_str);
	checkDBList("root", root_version, root_str);
	WebcfgDebug("addNewDocEntry. get_successDocCount %d\n", get_successDocCount());
	addNewDocEntry(get_successDocCount());

	webconfig_db_data_t *temp = NULL;
	temp = get_global_db_node();

	if(NULL != temp)
	{
		if(root_str!=NULL && strlen(root_str) >0)
		{
			WebcfgDebug("update root_str %s to versionsList\n", root_str);
			snprintf(versionsList, 512, "%s", root_str);
			WEBCFG_FREE(root_str);
		}
		else
		{
			WebcfgDebug("update root_version %lu to versionsList\n", (long)root_version);
			sprintf(versionsList, "%lu", (long)root_version);
		}
		WebcfgInfo("versionsList is %s\n", versionsList);

		while (NULL != temp)
		{
			if( temp->name != NULL)
			{
				if( strcmp(temp->name,"root") !=0 )
				{
					versionsList_tmp = strdup(versionsList);
					sprintf(versionsList, "%s,%lu",versionsList_tmp,(long)temp->version);
					WEBCFG_FREE(versionsList_tmp);
				}
			}
			temp= temp->next;
		}
		WebcfgDebug("Final versionsList is %s len %zu\n", versionsList, strlen(versionsList));
	}
}

/* @brief Function to create curl header options
 * @param[in] list temp curl header list
 * @param[in] device status value
 * @param[out] trans_uuid for sync
 * @param[out] header_list output curl header list
*/
void createCurlHeader( struct curl_slist *list, struct curl_slist **header_list, int status, char ** trans_uuid)
{
	char *version_header = NULL;
	char *auth_header = NULL;
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
	struct timespec cTime;
	char currentTime[32];
	char *currentTime_header=NULL;
	char *uuid_header = NULL;
	char *transaction_uuid = NULL;
	char version[512]={'\0'};
	char* syncTransID = NULL;
	char* ForceSyncDoc = NULL;
	size_t supported_doc_size = 0;
	size_t supported_version_size = 0;
	size_t supplementary_docs_size = 0;

	WebcfgDebug("Start of createCurlheader\n");
	//Fetch auth JWT token from cloud.
	getAuthToken();

	WebcfgDebug("get_global_auth_token() is %s\n", get_global_auth_token());

	auth_header = (char *) malloc(sizeof(char)*MAX_HEADER_LEN);
	if(auth_header !=NULL)
	{
		snprintf(auth_header, MAX_HEADER_LEN, "Authorization:Bearer %s", (0 < strlen(get_global_auth_token()) ? get_global_auth_token() : NULL));
		list = curl_slist_append(list, auth_header);
		WEBCFG_FREE(auth_header);
	}

	if(!get_global_supplementarySync())
	{
		version_header = (char *) malloc(sizeof(char)*MAX_BUF_SIZE);
		if(version_header !=NULL)
		{
			refreshConfigVersionList(version, 0);
			snprintf(version_header, MAX_BUF_SIZE, "IF-NONE-MATCH:%s", ((strlen(version)!=0) ? version : "0"));
			WebcfgInfo("version_header formed %s\n", version_header);
			list = curl_slist_append(list, version_header);
			WEBCFG_FREE(version_header);
		}
	}
	list = curl_slist_append(list, "Accept: application/msgpack");

	schema_header = (char *) malloc(sizeof(char)*MAX_BUF_SIZE);
	if(schema_header !=NULL)
	{
		snprintf(schema_header, MAX_BUF_SIZE, "Schema-Version: %s", "v1.0");
		WebcfgInfo("schema_header formed %s\n", schema_header);
		list = curl_slist_append(list, schema_header);
		WEBCFG_FREE(schema_header);
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
				snprintf(supportedVersion_header, supported_version_size+1, "X-System-Schema-Version: %s", supportedVersion);
				WebcfgInfo("supportedVersion_header formed %s\n", supportedVersion_header);
				list = curl_slist_append(list, supportedVersion_header);
			}
			else
			{
				WebcfgInfo("supportedVersion fetched is NULL\n");
			}
		}
		else
		{
			WebcfgInfo("supportedVersion_header formed %s\n", supportedVersion_header);
			list = curl_slist_append(list, supportedVersion_header);
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
				snprintf(supportedDocs_header, supported_doc_size+1, "X-System-Supported-Docs: %s", supportedDocs);
				WebcfgInfo("supportedDocs_header formed %s\n", supportedDocs_header);
				list = curl_slist_append(list, supportedDocs_header);
			}
			else
			{
				WebcfgInfo("SupportedDocs fetched is NULL\n");
			}
		}
		else
		{
			WebcfgInfo("supportedDocs_header formed %s\n", supportedDocs_header);
			list = curl_slist_append(list, supportedDocs_header);
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
				snprintf(supplementaryDocs_header, supplementary_docs_size+1, "X-System-SupplementaryService-Sync: %s", supplementaryDocs);
				WebcfgInfo("supplementaryDocs_header formed %s\n", supplementaryDocs_header);
				list = curl_slist_append(list, supplementaryDocs_header);
			}
			else
			{
				WebcfgInfo("supplementaryDocs fetched is NULL\n");
			}
		}
		else
		{
			WebcfgInfo("supplementaryDocs_header formed %s\n", supplementaryDocs_header);
			list = curl_slist_append(list, supplementaryDocs_header);
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
			snprintf(bootTime_header, MAX_BUF_SIZE, "X-System-Boot-Time: %s", g_bootTime);
			WebcfgInfo("bootTime_header formed %s\n", bootTime_header);
			list = curl_slist_append(list, bootTime_header);
			WEBCFG_FREE(bootTime_header);
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
			snprintf(FwVersion_header, MAX_BUF_SIZE, "X-System-Firmware-Version: %s", g_FirmwareVersion);
			WebcfgInfo("FwVersion_header formed %s\n", FwVersion_header);
			list = curl_slist_append(list, FwVersion_header);
			WEBCFG_FREE(FwVersion_header);
		}
	}
	else
	{
		WebcfgError("Failed to get FwVersion\n");
	}

	status_header = (char *) malloc(sizeof(char)*MAX_BUF_SIZE);
	if(status_header !=NULL)
	{
		if(status !=0)
		{
			snprintf(status_header, MAX_BUF_SIZE, "X-System-Status: %s", "Non-Operational");
		}
		else
		{
			snprintf(status_header, MAX_BUF_SIZE, "X-System-Status: %s", "Operational");
		}
		WebcfgInfo("status_header formed %s\n", status_header);
		list = curl_slist_append(list, status_header);
		WEBCFG_FREE(status_header);
	}

	memset(currentTime, 0, sizeof(currentTime));
	getCurrent_Time(&cTime);
	snprintf(currentTime,sizeof(currentTime),"%d",(int)cTime.tv_sec);
	currentTime_header = (char *) malloc(sizeof(char)*MAX_BUF_SIZE);
	if(currentTime_header !=NULL)
	{
		snprintf(currentTime_header, MAX_BUF_SIZE, "X-System-Current-Time: %s", currentTime);
		WebcfgInfo("currentTime_header formed %s\n", currentTime_header);
		list = curl_slist_append(list, currentTime_header);
		WEBCFG_FREE(currentTime_header);
	}

        if(strlen(g_systemReadyTime) ==0)
        {
                systemReadyTime = get_global_systemReadyTime();
                if(systemReadyTime !=NULL)
                {
                       strncpy(g_systemReadyTime, systemReadyTime, sizeof(g_systemReadyTime)-1);
                       WebcfgDebug("g_systemReadyTime fetched is %s\n", g_systemReadyTime);
                       WEBCFG_FREE(systemReadyTime);
                }
        }

        if(strlen(g_systemReadyTime))
        {
                systemReadyTime_header = (char *) malloc(sizeof(char)*MAX_BUF_SIZE);
                if(systemReadyTime_header !=NULL)
                {
	                snprintf(systemReadyTime_header, MAX_BUF_SIZE, "X-System-Ready-Time: %s", g_systemReadyTime);
	                WebcfgInfo("systemReadyTime_header formed %s\n", systemReadyTime_header);
	                list = curl_slist_append(list, systemReadyTime_header);
	                WEBCFG_FREE(systemReadyTime_header);
                }
        }
        else
        {
                WebcfgDebug("Failed to get systemReadyTime\n");
        }

	getForceSync(&ForceSyncDoc, &syncTransID);

	if(syncTransID !=NULL)
	{
		if(ForceSyncDoc !=NULL)
		{
			if (strlen(syncTransID)>0)
			{
				WebcfgInfo("updating transaction_uuid with force syncTransID\n");
				transaction_uuid = strdup(syncTransID);
			}
			WEBCFG_FREE(ForceSyncDoc);
		}
		WEBCFG_FREE(syncTransID);
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
			snprintf(uuid_header, MAX_BUF_SIZE, "Transaction-ID: %s", transaction_uuid);
			WebcfgInfo("uuid_header formed %s\n", uuid_header);
			list = curl_slist_append(list, uuid_header);
			*trans_uuid = strdup(transaction_uuid);
			WEBCFG_FREE(transaction_uuid);
			WEBCFG_FREE(uuid_header);
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
			snprintf(productClass_header, MAX_BUF_SIZE, "X-System-Product-Class: %s", g_productClass);
			WebcfgInfo("productClass_header formed %s\n", productClass_header);
			list = curl_slist_append(list, productClass_header);
			WEBCFG_FREE(productClass_header);
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
			snprintf(ModelName_header, MAX_BUF_SIZE, "X-System-Model-Name: %s", g_ModelName);
			WebcfgInfo("ModelName_header formed %s\n", ModelName_header);
			list = curl_slist_append(list, ModelName_header);
			WEBCFG_FREE(ModelName_header);
		}
	}
	else
	{
		WebcfgError("Failed to get ModelName\n");
	}

	//Addtional headers for telemetry sync
	if(get_global_supplementarySync())
	{
		telemetryVersion_header = (char *) malloc(sizeof(char)*MAX_BUF_SIZE);
		if(telemetryVersion_header !=NULL)
		{
			snprintf(telemetryVersion_header, MAX_BUF_SIZE, "X-System-Telemetry-Profile-Version: %s", "2.0");
			WebcfgInfo("telemetryVersion_header formed %s\n", telemetryVersion_header);
			list = curl_slist_append(list, telemetryVersion_header);
			WEBCFG_FREE(telemetryVersion_header);
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
				snprintf(PartnerID_header, MAX_BUF_SIZE, "X-System-PartnerID: %s", g_PartnerID);
				WebcfgInfo("PartnerID_header formed %s\n", PartnerID_header);
				list = curl_slist_append(list, PartnerID_header);
				WEBCFG_FREE(PartnerID_header);
			}
		}
		else
		{
			WebcfgError("Failed to get PartnerID\n");
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
				snprintf(AccountID_header, MAX_BUF_SIZE, "X-System-AccountID: %s", g_AccountID);
				WebcfgInfo("AccountID_header formed %s\n", AccountID_header);
				list = curl_slist_append(list, AccountID_header);
				WEBCFG_FREE(AccountID_header);
			}
		}
		else
		{
			WebcfgError("Failed to get AccountID\n");
		}
	}
	*header_list = list;
}

char* generate_trans_uuid()
{
	char *transID = NULL;
	uuid_t transaction_Id;
	char *trans_id = NULL;
	trans_id = (char *)malloc(37);
	uuid_generate_random(transaction_Id);
	uuid_unparse(transaction_Id, trans_id);

	if(trans_id !=NULL)
	{
		transID = trans_id;
	}
	return transID;
}

void delete_multipart()
{
	multipartdocs_t *temp = NULL;
	multipartdocs_t *head = NULL;
	head = get_global_mp();

	while(head != NULL)
	{
		temp = head;
		head = head->next;
		WebcfgDebug("Deleted mp node: temp->name_space:%s\n", temp->name_space);
		free(temp);
		temp = NULL;
	}
	pthread_mutex_lock (&multipart_t_mut);
	g_mp_head = NULL;
	pthread_mutex_unlock (&multipart_t_mut);
}

//Segregation of each subdoc elements line by line
void subdoc_parser(char *ptr, int no_of_bytes)
{
	char *name_space = NULL;
	char *data = NULL;
	uint32_t  etag = 0;
	size_t data_size = 0;

	char* str_body = NULL;
	str_body = malloc(sizeof(char) * no_of_bytes + 1);
	str_body = memcpy(str_body, ptr, no_of_bytes + 1);

	char *ptr_lb=str_body;
	char *ptr_lb1=str_body;
	int index1=0, index2 =0;
	int count = 0;

	while((ptr_lb - str_body) < no_of_bytes)
	{
		if(count < SUBDOC_TAG_COUNT)
		{
			ptr_lb1 =  memchr(ptr_lb+1, '\n', no_of_bytes - (ptr_lb - str_body));
			if(0 != memcmp(ptr_lb1-1, "\r",1 ))
			{
				ptr_lb1 = memchr(ptr_lb1+1, '\n', no_of_bytes - (ptr_lb - str_body));
			}
			index2 = ptr_lb1-str_body;
			index1 = ptr_lb-str_body;
			line_parser(str_body+index1+1,index2 - index1 - 2, &name_space, &etag, &data, &data_size);
			ptr_lb++;
			ptr_lb = memchr(ptr_lb, '\n', no_of_bytes - (ptr_lb - str_body));
			count++;
		}
		else             //For data bin segregation
		{
			index2 = no_of_bytes+1;
			index1 = ptr_lb-str_body;
			line_parser(str_body+index1+1,index2 - index1 - 2, &name_space, &etag, &data, &data_size);
			break;
		}
	}

	if(etag != 0 && name_space != NULL && data != NULL && data_size != 0 )
	{
		addToMpList(etag, name_space, data, data_size);
	}
	else
	{
		uint16_t err = 0;
		char* result = NULL;
		if(name_space != NULL)
		{
			err = getStatusErrorCodeAndMessage(MULTIPART_CACHE_NULL, &result);
			WebcfgDebug("The error_details is %s and err_code is %d\n", result, err);
			addWebConfgNotifyMsg(name_space, 0, "failed", result, get_global_transID(),0, "status", err, NULL, 200);
			WEBCFG_FREE(result);
		}
	}

	if(name_space != NULL)
	{
		WEBCFG_FREE(name_space);
	}

	if(data != NULL)
	{
		WEBCFG_FREE(data);
	}

        if(str_body != NULL)
	{
		WEBCFG_FREE(str_body);
	}
}

void line_parser(char *ptr, int no_of_bytes, char **name_space, uint32_t *etag, char **data, size_t *data_size)
{
	char *content_type = NULL;

	/*for storing respective values */
	if(0 == strncmp(ptr,"Content-type: ",strlen("Content-type")))
	{
		content_type = strndup(ptr+(strlen("Content-type: ")),no_of_bytes-((strlen("Content-type: "))));
		if(strncmp(content_type, "application/msgpack",strlen("application/msgpack")) !=0)
		{
			WebcfgError("Content-type not msgpack: %s", content_type);
		}
		WEBCFG_FREE(content_type);
	}
	else if(0 == strncasecmp(ptr,"Namespace",strlen("Namespace")))
	{
	        *name_space = strndup(ptr+(strlen("Namespace: ")),no_of_bytes-((strlen("Namespace: "))));
	}
	else if(0 == strncasecmp(ptr,"Etag",strlen("Etag")))
	{
	        char * temp = strndup(ptr+(strlen("Etag: ")),no_of_bytes-((strlen("Etag: "))));
		if(temp)
		{
			*etag = strtoul(temp,0,0);
			WebcfgDebug("The Etag version is %lu\n",(long)*etag);
			WEBCFG_FREE(temp);
		}
	}
	else if(strstr(ptr,"parameters"))
	{
		*data = malloc(sizeof(char) * no_of_bytes );
		*data = memcpy(*data, ptr, no_of_bytes );
		//store doc size of each sub doc
		*data_size = no_of_bytes;
	}

}

void addToMpList(uint32_t etag, char *name_space, char *data, size_t data_size)
{
	multipartdocs_t *mp_node;
	mp_node = (multipartdocs_t *)malloc(sizeof(multipartdocs_t));

	if(mp_node)
	{
		memset(mp_node, 0, sizeof(multipartdocs_t));

		mp_node->etag = etag;
		mp_node->name_space = strdup(name_space);
		mp_node->data = malloc(sizeof(char) * data_size);
		mp_node->data = memcpy(mp_node->data, data, data_size );
		mp_node->data_size = data_size;
		mp_node->isSupplementarySync = get_global_supplementarySync();
		mp_node->next = NULL;

		WebcfgDebug("mp_node->etag is %ld\n",(long)mp_node->etag);
		WebcfgDebug("mp_node->name_space is %s mp_node->etag is %lu mp_node->isSupplementarySync %d\n", mp_node->name_space, (long)mp_node->etag, mp_node->isSupplementarySync);
		WebcfgDebug("mp_node->data is %s\n", mp_node->data);
		WebcfgDebug("mp_node->data_size is %zu\n", mp_node->data_size);
		WebcfgDebug("mp_node->isSupplementarySync is %d\n", mp_node->isSupplementarySync);

		if(get_global_mp() == NULL)
		{
			set_global_mp(mp_node);
		}
		else
		{
			multipartdocs_t *temp = NULL;
			temp = get_global_mp();
			while( temp->next != NULL)
			{
				WebcfgDebug("The temp->name_space is %s\n", temp->name_space);
				temp = temp->next;
			}
			temp->next = mp_node;
		}
	}

}

void delete_mp_doc()
{
	multipartdocs_t *temp = NULL;
	temp = get_global_mp();

	while(temp != NULL)
	{
		if(temp->isSupplementarySync == get_global_supplementarySync())
		{
			WebcfgDebug("Delete mp node--> mp_node->name_space is %s mp_node->etag is %lu mp_node->isSupplementarySync %d\n", temp->name_space, (long)temp->etag, temp->isSupplementarySync);
			deleteFromMpList(temp->name_space);
		}
		temp = temp->next;
	}

}

//delete doc from multipart list
WEBCFG_STATUS deleteFromMpList(char* doc_name)
{
	multipartdocs_t *prev_node = NULL, *curr_node = NULL;

	if( NULL == doc_name )
	{
		WebcfgError("Invalid value for mp doc\n");
		return WEBCFG_FAILURE;
	}
	WebcfgDebug("mp doc to be deleted: %s\n", doc_name);

	prev_node = NULL;
	pthread_mutex_lock (&multipart_t_mut);	
	curr_node = g_mp_head;

	// Traverse to get the doc to be deleted
	while( NULL != curr_node )
	{
		if(strcmp(curr_node->name_space, doc_name) == 0)
		{
			WebcfgDebug("Found the node to delete\n");
			if( NULL == prev_node )
			{
				WebcfgDebug("need to delete first doc\n");
				g_mp_head = curr_node->next;
			}
			else
			{
				WebcfgDebug("Traversing to find node\n");
				prev_node->next = curr_node->next;
			}

			WebcfgDebug("Deleting the node entries\n");
			WEBCFG_FREE( curr_node->name_space );
			WEBCFG_FREE( curr_node->data );
			WEBCFG_FREE( curr_node );
			curr_node = NULL;
			WebcfgDebug("Deleted successfully and returning..\n");
			pthread_mutex_unlock (&multipart_t_mut);
			return WEBCFG_SUCCESS;
		}

		prev_node = curr_node;
		curr_node = curr_node->next;
	}
	pthread_mutex_unlock (&multipart_t_mut);
	WebcfgError("Could not find the entry to delete from mp list\n");
	return WEBCFG_FAILURE;
}

void print_tmp_doc_list(size_t mp_count)
{
	int count =0;
	webconfig_tmp_data_t *temp = NULL;
	temp = get_global_tmp_node();
	while (NULL != temp)
	{
		count = count+1;
		WebcfgInfo("node is pointing to temp->name %s temp->version %lu temp->status %s temp->error_details %s\n",temp->name, (long)temp->version, temp->status, temp->error_details);
		temp= temp->next;
		WebcfgDebug("count %d mp_count %zu\n", count, mp_count);
		if(count == (int)mp_count)
		{
			WebcfgDebug("Found all docs in the list\n");
			break;
		}
	}
	return;
}

char *replaceMacWord(const char *s, const char *macW, const char *deviceMACW)
{
	char *result = NULL;
	int i, cnt = 0;

	if(deviceMACW != NULL)
	{
		int deviceMACWlen = strlen(deviceMACW);
		int macWlen = strlen(macW);
		// Counting the number of times mac word occur in the string
		for (i = 0; s[i] != '\0'; i++)
		{
			if (strstr(&s[i], macW) == &s[i])
			{
			    cnt++;
			    // Jumping to index after the mac word.
			    i += macWlen - 1;
			}
		}

		result = (char *)malloc(i + cnt * (deviceMACWlen - macWlen) + 1);
		i = 0;
		while (*s)
		{
			if (strstr(s, macW) == s)
			{
				strcpy(&result[i], deviceMACW);
				i += deviceMACWlen;
				s += macWlen;
			}
			else
			    result[i++] = *s++;
		}
		result[i] = '\0';
	}
	return result;
}

void reqParam_destroy( int paramCnt, param_t *reqObj )
{
	int i = 0;
	if(reqObj)
	{
		for (i = 0; i < paramCnt; i++)
		{
			if(reqObj[i].name)
			{
				free(reqObj[i].name);
			}
			if(reqObj[i].value)
			{
				free(reqObj[i].value);
			}
		}
		free(reqObj);
	}
}

//Root will be the first doc always in tmp list and will be deleted when all the docs are success. Delete root doc from tmp list when all primary and supplementary docs are success.
WEBCFG_STATUS checkRootDelete()
{
	webconfig_tmp_data_t *temp = NULL;
	temp = get_global_tmp_node();
	int docSuccess =0;
	while (NULL != temp)
	{
		WebcfgDebug("Root check ====> temp->name %s\n", temp->name);
		if( strcmp("root", temp->name) != 0)
		{
			if(strcmp("success", temp->status) == 0)
			{
				docSuccess = 1;
			}
			else
			{
				docSuccess = 0;
				break;
			}
		}
		temp= temp->next;
	}
	if( docSuccess == 1)
	{
		WebcfgInfo("Tmp list root doc delete is required\n");
		return WEBCFG_SUCCESS;
	}
	else
	{
		WebcfgDebug("Tmp list root doc delete is not required\n");
	}
	return WEBCFG_FAILURE;
}

//Update root version to DB when all primary docs are success.
WEBCFG_STATUS checkRootUpdate()
{
	webconfig_tmp_data_t *temp = NULL;
	temp = get_global_tmp_node();
	int docSuccess =0;
	while (NULL != temp)
	{
		WebcfgDebug("Root check ====> temp->name %s\n", temp->name);
		if((temp->error_code == 204 && (temp->error_details != NULL && strstr(temp->error_details, "doc_unsupported") != NULL)) || (temp->isSupplementarySync == 1)) //skip supplementary docs
		{
			if(temp->isSupplementarySync)
			{
				WebcfgDebug("Skipping supplementary sub doc %s\n", temp->name);
			}
			else
			{
				WebcfgDebug("Skipping unsupported sub doc %s\n",temp->name);
			}
			WebcfgDebug("Error details: %s\n",temp->error_details);
		}
		else if( strcmp("root", temp->name) != 0)
		{
			if(strcmp("success", temp->status) == 0)
			{
				docSuccess = 1;
			}			
			else
			{
				docSuccess = 0;
				break;
			}
		}
		else
		{
			WebcfgDebug("docs not found in templist\n");
		}	
		temp= temp->next;
	}
	if( docSuccess == 1)
	{
		WebcfgDebug("root DB update is required\n");
		return WEBCFG_SUCCESS;
	}
	else
	{
		WebcfgInfo("root DB update is not required\n");
	}
	return WEBCFG_FAILURE;
}

//Update root version to DB.
void updateRootVersionToDB()
{
	char * temp = strdup(g_ETAG);
	uint32_t version=0;

	if(temp)
	{
		version = strtoul(temp,NULL,0);
		WEBCFG_FREE(temp);
	}
	if(version != 0)
	{
		WebcfgDebug("Updating root version to DB\n");
		checkDBList("root",version, NULL);
		//Update tmp list root as root doc from tmp list is not deleted until all docs are success.
		webconfig_tmp_data_t * root_node = NULL;
		root_node = getTmpNode("root");
		WebcfgDebug("Updating root version to tmp list\n");
		updateTmpList(root_node, "root", version, "success", "none", 0, 0, 0);
	}

	WebcfgDebug("The Etag is %lu\n",(long)version );
}

//Delete root doc from tmp list and mp cache list when all the docs are success.
void deleteRootAndMultipartDocs()
{
	//Delete root only when all the primary and supplementary docs are applied .
	if(checkRootDelete() == WEBCFG_SUCCESS)
	{
		//Delete tmp queue root as all docs are applied
		WebcfgDebug("Delete tmp queue root as all docs are applied\n");
		//WebcfgInfo("root version to delete is %lu\n", (long)version);
		reset_numOfMpDocs();
		delete_tmp_list();
		delete_multipart();
		WebcfgDebug("After free mp\n");
	}
}

void failedDocsRetry()
{
	webconfig_tmp_data_t *temp = NULL;
	temp = get_global_tmp_node();

	while (NULL != temp)
	{
		if((temp->error_code == CCSP_CRASH_STATUS_CODE) || (temp->error_code == 204 && (temp->error_details != NULL && strstr(temp->error_details, "doc_unsupported") == NULL)) || (temp->error_code == 191) || (temp->error_code == 193) || (temp->error_code == 190))
		{
			if(checkRetryTimer(temp->retry_timestamp))
			{
				WebcfgInfo("Retrying for subdoc %s error_code %lu\n", temp->name, (long)temp->error_code);
				if(retryMultipartSubdoc(temp, temp->name) == WEBCFG_SUCCESS)
				{
					WebcfgDebug("The subdoc %s set is success\n", temp->name);
				}
				else
				{
					WebcfgDebug("The subdoc %s set is failed\n", temp->name);
				}
			}
			else
			{
				int time_diff = 0;

				//To get the exact time diff for retry from present time do the below
				time_diff = updateRetryTimeDiff(temp->retry_timestamp);
				WebcfgDebug("The docname is %s and diff is %d retry time stamp is %s\n", temp->name, time_diff, printTime(temp->retry_timestamp));
				set_doc_fail(1);
			}
		}
		else
		{
			WebcfgDebug("Retry skipped for %s (%s)\n",temp->name,temp->error_details);
		}
		temp= temp->next;
	}
}

WEBCFG_STATUS validate_request_param(param_t *reqParam, int paramCount)
{
	int i = 0;
	WEBCFG_STATUS ret = WEBCFG_SUCCESS;
	WebcfgDebug("------------ validate_request_param ----------\n");
	for (i = 0; i < paramCount; i++)
	{
		WebcfgDebug("reqParam[%d].name: %s\n",i,reqParam[i].name);
		if(reqParam[i].name == NULL || strcmp(reqParam[i].name, "") == 0 || reqParam[i].value == NULL || strcmp(reqParam[i].value, "") == 0)
		{
			WebcfgError("Parameter name/value is null\n");
			ret = WEBCFG_FAILURE;
			break;
		}

		if(strlen(reqParam[i].name) >= MAX_PARAMETERNAME_LEN)
		{
			ret = WEBCFG_FAILURE;
			break;
		}

	}
	if(ret != WEBCFG_SUCCESS)
	{
		reqParam_destroy(paramCount, reqParam);
	}
	return ret;
}

int get_multipartdoc_count()
{
	int count = 0;
	multipartdocs_t *temp = NULL;
	temp = get_global_mp();

	while(temp != NULL)
	{
		count++;
		temp = temp->next;
	}
	return count;
}
