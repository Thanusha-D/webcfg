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
#ifndef __WEBCFGGENERIC_H__
#define __WEBCFGGENERIC_H__

#include <stdint.h>
#include <wdmp-c.h>

/***!!!! NOTE: This file includes Device specific override functions. Mock implementations are added in webcfg_generic.c. Actual implementation need to be provided by platform specific code. !!!!***/

/*----------------------------------------------------------------------------*/
/*                                   Macros                                   */
/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/
/*                               Data Structures                              */
/*----------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------*/
/*                             External Functions                             */
/*----------------------------------------------------------------------------*/

/* Device specific getter functions */
char * getDeviceBootTime();
char * getSerialNumber();
char * getProductClass();
char * getModelName();
char * getPartnerID();
char * getAccountID();
char * getConnClientParamName();
char * getFirmwareVersion();
char* get_deviceMAC();
char * getRebootReason();
char * getFirmwareUpgradeStartTime();
char * getFirmwareUpgradeEndTime();
/* Getter function to return systemReadyTime in UTC format */
char *get_global_systemReadyTime();
char * getParamValue(char *paramName);

/* Function that gets the values from TR181 dml layer */
int setForceSync(char* pString, char *transactionId,int *session_status);
int getForceSync(char** pString, char **transactionId);
int Get_Webconfig_URL( char *pString);
int Set_Webconfig_URL( char *pString);
int Get_Supplementary_URL( char *name, char *pString);
int Set_Supplementary_URL( char *name, char *pString);

/**
 * @brief setValues interface sets the parameter value.
 *
 * @param[in] paramVal List of Parameter name/value pairs.
 * @param[in] paramCount Number of parameters.
 * @param[in] setType enum to specify the type of set operation. 
 * WEBPA_SET = 0 for set values, 1 for atomic set, 2 for XPC atomic set.
 * @param[out] timeSpan timing_values for each component. 
 * @param[out] retStatus Returns status
 * @param[out] ccspStatus Returns ccsp set status
 */
void setValues(const param_t paramVal[], const unsigned int paramCount, const int setType, char *transactionId, money_trace_spans *timeSpan, WDMP_STATUS *retStatus, int *ccspStatus);

/**
 * @brief sendNotification sends event notification to cloud via parodus.
 *
 * @param[in] payload Notify payload to be send to cloud.
 * @param[in] source The source end from which notify event is triggered.
 * @param[in] destination The destination at which notification has to be reached.
 */
void sendNotification(char *payload, char *source, char *destination);

typedef void (*WebConfigEventCallback)(char* Info, void *user_data);
int registerWebcfgEvent(WebConfigEventCallback webcfgEventCB);
int unregisterWebcfgEvent();

WDMP_STATUS mapStatus(int ret);

/**
 * @brief setAttributes interface sets parameter notify attribute.
 *
 * @param[in] attArr parameter attributes Array.
 * @param[in] paramCount Number of parameters.
 * @param[out] timeSpan timing_values for each component.
 * @param[out] retStatus Returns status
 */
void setAttributes(param_t *attArr, const unsigned int paramCount, money_trace_spans *timeSpan, WDMP_STATUS *retStatus);
int rbus_GetValueFromDB( char* paramName, char** paramValue);
int rbus_StoreValueIntoDB(char *paramName, char *value);
int rbus_waitUntilSystemReady();
#endif
