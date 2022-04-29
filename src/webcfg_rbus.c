/*
 * If not stated otherwise in this file or this component's Licenses.txt file the
 * following copyright and licenses apply:
 *
 * Copyright 2021 RDK Management
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
#include <wdmp-c.h>
#include "webcfg_rbus.h"
#include "webcfg_metadata.h"
#ifdef WAN_FAILOVER_SUPPORTED
#include "webcfg_multipart.h"
#endif

static rbusHandle_t rbus_handle;

static bool  RfcVal = false ;
static char* URLVal = NULL ;
static char* forceSyncVal = NULL ;
static char* SupportedDocsVal = NULL ;
static char* SupportedVersionVal = NULL ;
static char* SupplementaryURLVal = NULL ;
static bool isRbus = false ;
static char* BinDataVal = NULL ;
static char *paramRFCEnable = "eRT.com.cisco.spvtg.ccsp.webpa.WebConfigRfcEnable";

static char ForceSync[256]={'\0'};
static char ForceSyncTransID[256]={'\0'};

static int subscribed = 0;

#ifdef WAN_FAILOVER_SUPPORTED
static void eventReceiveHandler(
    rbusHandle_t rbus_handle,
    rbusEvent_t const* event,
    rbusEventSubscription_t* subscription);

static void subscribeAsyncHandler(
    rbusHandle_t rbus_handle,
    rbusEventSubscription_t* subscription,
    rbusError_t error);
#endif

rbusDataElement_t eventDataElement[1] = {
		{WEBCFG_EVENT_NAME, RBUS_ELEMENT_TYPE_PROPERTY, {NULL, rbusWebcfgEventHandler, NULL, NULL, NULL, NULL}}
	};

typedef struct
{
    char *paramName;
    char *paramValue;
    rbusValueType_t type;
} rbusParamVal_t;

bool get_global_isRbus(void)
{
    return isRbus;
}

rbusHandle_t get_global_rbus_handle(void)
{
     return rbus_handle;
}

bool isRbusEnabled() 
{
	if(RBUS_ENABLED == rbus_checkStatus()) 
	{
		isRbus = true;
	}
	else
	{
		isRbus = false;
	}
	WebcfgInfo("Webconfig RBUS mode active status = %s\n", isRbus ? "true":"false");
	return isRbus;
}

bool isRfcEnabled() 
{
        bool isRfc = false;
	if(get_rbus_RfcEnable()) 
	{
		isRfc = true;
	}
	else
	{
		isRfc = false;
	}
	WebcfgDebug("Webconfig RFC  = %s\n", isRfc ? "true":"false");
	return isRfc;
}


bool isRbusInitialized( ) 
{
    return rbus_handle != NULL ? true : false;
}

WEBCFG_STATUS webconfigRbusInit(const char *pComponentName) 
{
	int ret = RBUS_ERROR_SUCCESS;   

	WebcfgDebug("rbus_open for component %s\n", pComponentName);
	ret = rbus_open(&rbus_handle, pComponentName);
	if(ret != RBUS_ERROR_SUCCESS)
	{
		WebcfgError("webconfigRbusInit failed with error code %d\n", ret);
		return WEBCFG_FAILURE;
	}
	WebcfgInfo("webconfigRbusInit is success. ret is %d\n", ret);
	return WEBCFG_SUCCESS;
}

void webpaRbus_Uninit()
{
    rbus_close(rbus_handle);
}

/**
 * Data set handler for parameters owned by Webconfig
 */
rbusError_t webcfgRfcSetHandler(rbusHandle_t handle, rbusProperty_t prop, rbusSetHandlerOptions_t* opts)
{
    (void) handle;
    (void) opts;
    char const* paramName = rbusProperty_GetName(prop);
    if(strncmp(paramName,  WEBCFG_RFC_PARAM, maxParamLen) != 0) 
    {
        WebcfgError("Unexpected parameter = %s\n", paramName);
        return RBUS_ERROR_ELEMENT_DOES_NOT_EXIST;
    }

    rbusError_t retPsmSet = RBUS_ERROR_BUS_ERROR;
    WebcfgDebug("Parameter name is %s \n", paramName);
    rbusValueType_t type_t;
    rbusValue_t paramValue_t = rbusProperty_GetValue(prop);
    if(paramValue_t) {
        type_t = rbusValue_GetType(paramValue_t);
    } else {
	WebcfgError("Invalid input to set\n");
        return RBUS_ERROR_INVALID_INPUT;
    }

    if(strncmp(paramName, WEBCFG_RFC_PARAM, maxParamLen) == 0)
    {
        if(type_t == RBUS_BOOLEAN) {
	    bool paramval = rbusValue_GetBoolean(paramValue_t);
	    WebcfgDebug("paramval is %d\n", paramval);
	    if(paramval == true || paramval == false)
            {
                    retPsmSet = set_rbus_RfcEnable(paramval);

                    if (retPsmSet != RBUS_ERROR_SUCCESS)
                    {
                      WebcfgError("RFC set failed\n");
                      return retPsmSet;
                    }
                    else
                    {
                      RfcVal = paramval;
                      WebcfgDebug("RfcVal after processing %s\n", (1==RfcVal)?"true":"false");
                    }
	    }
	    else
	    {
		WebcfgError("set value type is invalid\n");
                return RBUS_ERROR_INVALID_INPUT;
	    }
        } else {
            WebcfgError("Unexpected value type for property %s \n", paramName);
	    return RBUS_ERROR_INVALID_INPUT;
        }
    }
    return RBUS_ERROR_SUCCESS;
}

rbusError_t webcfgUrlSetHandler(rbusHandle_t handle, rbusProperty_t prop, rbusSetHandlerOptions_t* opts) 
{
    (void) handle;
    (void) opts;
    char const* paramName = rbusProperty_GetName(prop);

    if(strncmp(paramName, WEBCFG_URL_PARAM, maxParamLen) != 0)
    {
        WebcfgError("Unexpected parameter = %s\n", paramName);
        return RBUS_ERROR_ELEMENT_DOES_NOT_EXIST;
    }

    rbusError_t retPsmSet = RBUS_ERROR_BUS_ERROR;
    WebcfgDebug("Parameter name is %s \n", paramName);
    rbusValueType_t type_t;
    rbusValue_t paramValue_t = rbusProperty_GetValue(prop);
    if(paramValue_t) {
        type_t = rbusValue_GetType(paramValue_t);
    } else {
	WebcfgError("Invalid input to set\n");
        return RBUS_ERROR_INVALID_INPUT;
    }

    if(strncmp(paramName, WEBCFG_URL_PARAM, maxParamLen) == 0) {

	if (!isRfcEnabled())
	{
		WebcfgError("RfcEnable is disabled so, %s SET failed\n",paramName);
		return RBUS_ERROR_ACCESS_NOT_ALLOWED;
	}
        if(type_t == RBUS_STRING) {
            char* data = rbusValue_ToString(paramValue_t, NULL, 0);
            if(data) {
                WebcfgDebug("Call datamodel function  with data %s\n", data);

                if(URLVal) {
                    free(URLVal);
                    URLVal = NULL;
                }
                URLVal = strdup(data);
                free(data);
		WebcfgDebug("URLVal after processing %s\n", URLVal);
		retPsmSet = rbus_StoreValueIntoDB( WEBCFG_URL_PARAM, URLVal );
		if (retPsmSet != RBUS_ERROR_SUCCESS)
		{
			WebcfgError("psm_set failed ret %d for parameter %s and value %s\n", retPsmSet, paramName, URLVal);
			return retPsmSet;
		}
		else
		{
			WebcfgInfo("psm_set success ret %d for parameter %s and value %s\n", retPsmSet, paramName, URLVal);
		}
            }
        } else {
            WebcfgError("Unexpected value type for property %s\n", paramName);
	    return RBUS_ERROR_INVALID_INPUT;
        }
    }
    return RBUS_ERROR_SUCCESS;
}

rbusError_t webcfgDataSetHandler(rbusHandle_t handle, rbusProperty_t prop, rbusSetHandlerOptions_t* opts)
 {
    (void) handle;
    (void) opts;
    char const* paramName = rbusProperty_GetName(prop);

    rbusError_t retPsmSet = RBUS_ERROR_BUS_ERROR;
    WebcfgDebug("Parameter name is %s \n", paramName);
   
    if(strncmp(paramName, WEBCFG_DATA_PARAM, maxParamLen) == 0)
    {
        WebcfgError("Data Set is not allowed\n");
        retPsmSet = RBUS_ERROR_ACCESS_NOT_ALLOWED;
        return retPsmSet;
    }
    return RBUS_ERROR_SUCCESS;
}

rbusError_t webcfgSupportedDocsSetHandler(rbusHandle_t handle, rbusProperty_t prop, rbusSetHandlerOptions_t* opts) {

    (void) handle;
    (void) opts;
    char const* paramName = rbusProperty_GetName(prop);

    rbusError_t retPsmSet = RBUS_ERROR_BUS_ERROR;
    WebcfgDebug("Parameter name is %s \n", paramName);
 
    if(strncmp(paramName, WEBCFG_SUPPORTED_DOCS_PARAM, maxParamLen) == 0)
    {
        WebcfgError("SupportedDocs Set is not allowed\n");
        retPsmSet = RBUS_ERROR_ACCESS_NOT_ALLOWED;
        return retPsmSet;
    }
    return RBUS_ERROR_SUCCESS;
}

rbusError_t webcfgSupportedVersionSetHandler(rbusHandle_t handle, rbusProperty_t prop, rbusSetHandlerOptions_t* opts) {

    (void) handle;
    (void) opts;
    char const* paramName = rbusProperty_GetName(prop);

    rbusError_t retPsmSet = RBUS_ERROR_BUS_ERROR;
    WebcfgDebug("Parameter name is %s \n", paramName);

    if(strncmp(paramName, WEBCFG_SUPPORTED_VERSION_PARAM, maxParamLen) == 0)
    {
        WebcfgError("SupportedSchemaVersion Set is not allowed\n");
        retPsmSet = RBUS_ERROR_ACCESS_NOT_ALLOWED;
        return retPsmSet;
    }
    return RBUS_ERROR_SUCCESS;
}

rbusError_t webcfgTelemetrySetHandler(rbusHandle_t handle, rbusProperty_t prop, rbusSetHandlerOptions_t* opts) {

    (void) handle;
    (void) opts;
    char const* paramName = rbusProperty_GetName(prop);
    if(strncmp(paramName, WEBCFG_SUPPLEMENTARY_TELEMETRY_PARAM, maxParamLen) != 0) 
    {
        WebcfgError("Unexpected parameter = %s\n", paramName);
        return RBUS_ERROR_ELEMENT_DOES_NOT_EXIST;
    }

    rbusError_t retPsmSet = RBUS_ERROR_BUS_ERROR;
    WebcfgDebug("Parameter name is %s \n", paramName);
    rbusValueType_t type_t;
    rbusValue_t paramValue_t = rbusProperty_GetValue(prop);
    if(paramValue_t) {
        type_t = rbusValue_GetType(paramValue_t);
    } else {
	WebcfgError("Invalid input to set\n");
        return RBUS_ERROR_INVALID_INPUT;
    }
   
    if(strncmp(paramName, WEBCFG_SUPPLEMENTARY_TELEMETRY_PARAM, maxParamLen) == 0){

	if (!isRfcEnabled())
	{
		WebcfgError("RfcEnable is disabled so, %s SET failed\n",paramName);
		return RBUS_ERROR_ACCESS_NOT_ALLOWED;
	}
        if(type_t == RBUS_STRING) {
            char* data = rbusValue_ToString(paramValue_t, NULL, 0);
            if(data)
            {
                WebcfgDebug("Call datamodel function  with data %s\n", data);

                if(SupplementaryURLVal) {
                    free(SupplementaryURLVal);
                    SupplementaryURLVal = NULL;
                }
                SupplementaryURLVal = strdup(data);
                free(data);
		WebcfgDebug("SupplementaryURLVal after processing %s\n", SupplementaryURLVal);
		retPsmSet = rbus_StoreValueIntoDB( WEBCFG_SUPPLEMENTARY_TELEMETRY_PARAM, SupplementaryURLVal );
		if (retPsmSet != RBUS_ERROR_SUCCESS)
		{
			WebcfgError("psm_set failed ret %d for parameter %s and value %s\n", retPsmSet, paramName, SupplementaryURLVal);
			return retPsmSet;
		}
		else
		{
			WebcfgInfo("psm_set success ret %d for parameter %s and value %s\n", retPsmSet, paramName, SupplementaryURLVal);
		}
            }
        }
         else {
            WebcfgError("Unexpected value type for property %s\n", paramName);
	    return RBUS_ERROR_INVALID_INPUT;
        }
    }   
     return RBUS_ERROR_SUCCESS;
}
      

rbusError_t webcfgFrSetHandler(rbusHandle_t handle, rbusProperty_t prop, rbusSetHandlerOptions_t* opts) {

    (void) handle;
    (void) opts;
    char const* paramName = rbusProperty_GetName(prop);
    if(strncmp(paramName, WEBCFG_FORCESYNC_PARAM, maxParamLen) != 0)
    {
        WebcfgError("Unexpected parameter = %s\n", paramName); 
        return RBUS_ERROR_ELEMENT_DOES_NOT_EXIST;
    }

    WebcfgDebug("Parameter name is %s \n", paramName);
    rbusValueType_t type_t;
    rbusValue_t paramValue_t = rbusProperty_GetValue(prop);
    if(paramValue_t) {
        type_t = rbusValue_GetType(paramValue_t);
    } else {
	WebcfgError("Invalid input to set\n");
        return RBUS_ERROR_INVALID_INPUT;
    }
    if(strncmp(paramName, WEBCFG_FORCESYNC_PARAM, maxParamLen) == 0) {
	if (!isRfcEnabled())
	{
		WebcfgError("RfcEnable is disabled so, %s SET failed\n",paramName);
		return RBUS_ERROR_ACCESS_NOT_ALLOWED;
	}
        if(type_t == RBUS_STRING) {
            char* data = rbusValue_ToString(paramValue_t, NULL, 0);
            if(data) {
                WebcfgDebug("Call datamodel function  with data %s \n", data);

		if(strlen(data) == strlen("telemetry"))
		{
			if(0 == strncmp(data,"telemetry",strlen("telemetry")))
			{
				char telemetryUrl[256] = {0};
				Get_Supplementary_URL("Telemetry", telemetryUrl);
				if(strncmp(telemetryUrl,"NULL",strlen("NULL")) == 0)
				{
					WebcfgError("Telemetry url is null so, force sync SET failed\n");
					return RBUS_ERROR_BUS_ERROR;
				}
			}
		}

                if (forceSyncVal){
                    free(forceSyncVal);
                    forceSyncVal = NULL;
                }
		int session_status = 0;
		int ret = set_rbus_ForceSync(data, &session_status);
		WebcfgDebug("set_rbus_ForceSync ret %d\n", ret);
		if(session_status)
		{
			WebcfgInfo("session_status is 1\n");
			WEBCFG_FREE(data);
			return RBUS_ERROR_SESSION_ALREADY_EXIST;
		}
		if(!ret)
		{
			WebcfgError("setForceSync failed\n");
			WEBCFG_FREE(data);
			return RBUS_ERROR_BUS_ERROR;
		}
                WEBCFG_FREE(data);
		WebcfgDebug("forceSyncVal after processing %s\n", forceSyncVal);
            }
        } else {
            WebcfgError("Unexpected value type for property %s \n", paramName);
            return RBUS_ERROR_INVALID_INPUT;
        }

    }
    return RBUS_ERROR_SUCCESS;
}

/**
 * Data get handler for parameters owned by Webconfig
 */
rbusError_t webcfgRfcGetHandler(rbusHandle_t handle, rbusProperty_t property, rbusGetHandlerOptions_t* opts) 
{

    (void) handle;
    (void) opts;
    char const* propertyName;
    rbusError_t retPsmGet = RBUS_ERROR_BUS_ERROR;

    propertyName = rbusProperty_GetName(property);
    if(propertyName) {
        WebcfgDebug("Property Name is %s \n", propertyName);
    } else {
        WebcfgError("Unable to handle get request for property \n");
        return RBUS_ERROR_INVALID_INPUT;
    }

    if(strncmp(propertyName, WEBCFG_RFC_PARAM, maxParamLen) == 0)
    {
        rbusValue_t value;
        rbusValue_Init(&value);
	char *tmpchar = NULL;
	retPsmGet = rbus_GetValueFromDB( paramRFCEnable, &tmpchar );
	if (retPsmGet != RBUS_ERROR_SUCCESS){
		WebcfgError("psm_get failed ret %d for parameter %s and value %s\n", retPsmGet, propertyName, tmpchar);
                if(value)
                {
                	rbusValue_Release(value);
 		}
		return retPsmGet;
	}
	else
	{
		WebcfgInfo("psm_get success ret %d for parameter %s and value %s\n", retPsmGet, propertyName, tmpchar);
		if(tmpchar != NULL)
		{
			WebcfgDebug("B4 char to bool conversion\n");
			if(((strcmp (tmpchar, "true") == 0)) || (strcmp (tmpchar, "TRUE") == 0))
			{
				RfcVal = true;
			}
			free(tmpchar);
		}
		WebcfgDebug("RfcVal fetched %d\n", RfcVal);
	}

	rbusValue_SetBoolean(value, RfcVal);
        rbusProperty_SetValue(property, value);
	WebcfgDebug("Rfc value fetched is %s\n", (rbusValue_GetBoolean(value)==true)?"true":"false");
        rbusValue_Release(value);
   }
   return RBUS_ERROR_SUCCESS;
}

rbusError_t webcfgUrlGetHandler(rbusHandle_t handle, rbusProperty_t property, rbusGetHandlerOptions_t* opts) 
{
    
    (void) handle;
    (void) opts;
    char const* propertyName;
    rbusError_t retPsmGet = RBUS_ERROR_BUS_ERROR;

    propertyName = rbusProperty_GetName(property);
    if(propertyName) {
        WebcfgDebug("Property Name is %s \n", propertyName);
    } else {
        WebcfgError("Unable to handle get request for property \n");
        return RBUS_ERROR_INVALID_INPUT;
    }
   if(strncmp(propertyName, WEBCFG_URL_PARAM, maxParamLen) == 0)
   {

	rbusValue_t value;
        rbusValue_Init(&value);

	if(!isRfcEnabled())
	{
		WebcfgError("RfcEnable is disabled so, %s Get from DB failed\n",propertyName);
		rbusValue_SetString(value, "");
		rbusProperty_SetValue(property, value);
		rbusValue_Release(value);
		return 0;
	}
        if(URLVal){
            rbusValue_SetString(value, URLVal);
	}
        else{
		retPsmGet = rbus_GetValueFromDB( WEBCFG_URL_PARAM, &URLVal );
		if (retPsmGet != RBUS_ERROR_SUCCESS){
			WebcfgError("psm_get failed ret %d for parameter %s and value %s\n", retPsmGet, propertyName, URLVal);
                        if(value)
                	{
                		rbusValue_Release(value);
 			}
			return retPsmGet;
		}
		else{
			WebcfgInfo("psm_get success ret %d for parameter %s and value %s\n", retPsmGet, propertyName, URLVal);
			if(URLVal)
			{
				rbusValue_SetString(value, URLVal);
			}
			else
			{
				WebcfgError("URL is empty\n");
				rbusValue_SetString(value, "");
			}
		}
	}
        rbusProperty_SetValue(property, value);
        rbusValue_Release(value);

    }
    return RBUS_ERROR_SUCCESS;
}

rbusError_t webcfgDataGetHandler(rbusHandle_t handle, rbusProperty_t property, rbusGetHandlerOptions_t* opts) {
    
    (void) handle;
    (void) opts;
    char const* propertyName;

    propertyName = rbusProperty_GetName(property);
    if(propertyName) {
        WebcfgDebug("Property Name is %s \n", propertyName);
    } else {
        WebcfgError("Unable to handle get request for property \n");
        return RBUS_ERROR_INVALID_INPUT;
    }
    if(strncmp(propertyName, WEBCFG_DATA_PARAM , maxParamLen) == 0)
    {
        rbusValue_t value;
        rbusValue_Init(&value);

	if(!isRfcEnabled())
	{
		WebcfgError("RfcEnable is disabled so, %s Get from DB failed\n",propertyName);
		rbusValue_SetString(value, "");
		rbusProperty_SetValue(property, value);
		rbusValue_Release(value);
		return 0;
	}

        BinDataVal = get_DB_BLOB_base64();

        if(BinDataVal)
        {
            rbusValue_SetString(value, BinDataVal);
        }
        else
        {
            rbusValue_SetString(value, "");
        }
        rbusProperty_SetValue(property, value);
        rbusValue_Release(value);
    }
    return RBUS_ERROR_SUCCESS;
}

rbusError_t webcfgSupportedDocsGetHandler(rbusHandle_t handle, rbusProperty_t property, rbusGetHandlerOptions_t* opts) {
    
    (void) handle;
    (void) opts;
    char const* propertyName;

    propertyName = rbusProperty_GetName(property);
    if(propertyName) {
        WebcfgDebug("Property Name is %s \n", propertyName);
    } else {
        WebcfgError("Unable to handle get request for property \n");
        return RBUS_ERROR_INVALID_INPUT;
    }
    if(strncmp(propertyName, WEBCFG_SUPPORTED_DOCS_PARAM, maxParamLen) == 0)
    {
        rbusValue_t value;
        rbusValue_Init(&value);

	if(!isRfcEnabled())
	{
		WebcfgError("RfcEnable is disabled so, %s Get from DB failed\n",propertyName);
		rbusValue_SetString(value, "");
		rbusProperty_SetValue(property, value);
		rbusValue_Release(value);
		return 0;
	}

        SupportedDocsVal = getsupportedDocs();

        if(SupportedDocsVal)
        {
            rbusValue_SetString(value, SupportedDocsVal);
        }
        else
        {
            rbusValue_SetString(value, "");
        }
        rbusProperty_SetValue(property, value);
        WebcfgDebug("SupportedDocs value fetched is %s\n", rbusValue_GetString(value, NULL));
        rbusValue_Release(value);

    }
    return RBUS_ERROR_SUCCESS;
}

rbusError_t webcfgSupportedVersionGetHandler(rbusHandle_t handle, rbusProperty_t property, rbusGetHandlerOptions_t* opts) {
    
    (void) handle;
    (void) opts;
    char const* propertyName;

    propertyName = rbusProperty_GetName(property);
    if(propertyName) {
        WebcfgDebug("Property Name is %s \n", propertyName);
    } else {
        WebcfgError("Unable to handle get request for property \n");
        return RBUS_ERROR_INVALID_INPUT;
    }
    if (strncmp(propertyName, WEBCFG_SUPPORTED_VERSION_PARAM, maxParamLen) == 0)
    {
        rbusValue_t value;
        rbusValue_Init(&value);

	if(!isRfcEnabled())
	{
		WebcfgError("RfcEnable is disabled so, %s Get from DB failed\n",propertyName);
		rbusValue_SetString(value, "");
		rbusProperty_SetValue(property, value);
		rbusValue_Release(value);
		return 0;
	}

        SupportedVersionVal = getsupportedVersion();

        if(SupportedVersionVal)
        {
            rbusValue_SetString(value, SupportedVersionVal);
        }
        else
        {
            rbusValue_SetString(value, "");
        }
        rbusProperty_SetValue(property, value);
        WebcfgDebug("SupportedVersion value fetched is %s\n", rbusValue_GetString(value, NULL));
        rbusValue_Release(value);
    }
    return RBUS_ERROR_SUCCESS;	
}

rbusError_t webcfgTelemetryGetHandler(rbusHandle_t handle, rbusProperty_t property, rbusGetHandlerOptions_t* opts) {
    
    (void) handle;
    (void) opts;
    char const* propertyName;
    rbusError_t retPsmGet = RBUS_ERROR_BUS_ERROR;

    propertyName = rbusProperty_GetName(property);
    if(propertyName) {
        WebcfgDebug("Property Name is %s \n", propertyName);
    } else {
        WebcfgError("Unable to handle get request for property \n");
        return RBUS_ERROR_INVALID_INPUT;
    }
    if(strncmp(propertyName, WEBCFG_SUPPLEMENTARY_TELEMETRY_PARAM, maxParamLen)==0)
    {

	rbusValue_t value;
        rbusValue_Init(&value);

	if(!isRfcEnabled())
	{
		WebcfgError("RfcEnable is disabled so, %s Get from DB failed\n",propertyName);
		rbusValue_SetString(value, "");
		rbusProperty_SetValue(property, value);
		rbusValue_Release(value);
		return 0;
	}
        if(SupplementaryURLVal){
            rbusValue_SetString(value, SupplementaryURLVal);
	}
        else{
		retPsmGet = rbus_GetValueFromDB( WEBCFG_SUPPLEMENTARY_TELEMETRY_PARAM, &SupplementaryURLVal );
		if (retPsmGet != RBUS_ERROR_SUCCESS){
			WebcfgError("psm_get failed ret %d for parameter %s and value %s\n", retPsmGet, propertyName, SupplementaryURLVal);
                        if(value)
                	{
                		rbusValue_Release(value);
 			}
			return retPsmGet;
		}
		else{
			WebcfgInfo("psm_get success ret %d for parameter %s and value %s\n", retPsmGet, propertyName, SupplementaryURLVal);
			if(SupplementaryURLVal)
			{
				rbusValue_SetString(value, SupplementaryURLVal);
			}
			else
			{
				WebcfgError("SupplementaryURL is empty\n");
				rbusValue_SetString(value, "");
			}
		}
	}
        rbusProperty_SetValue(property, value);
        rbusValue_Release(value);

    }
    return RBUS_ERROR_SUCCESS;	
}

rbusError_t webcfgFrGetHandler(rbusHandle_t handle, rbusProperty_t property, rbusGetHandlerOptions_t* opts) {
    
    (void) handle;
    (void) opts;
    char const* propertyName;

    propertyName = rbusProperty_GetName(property);
    if(propertyName) {
        WebcfgDebug("Property Name is %s \n", propertyName);
    } else {
        WebcfgError("Unable to handle get request for property \n");
        return RBUS_ERROR_INVALID_INPUT;
    }
    if(strncmp(propertyName, WEBCFG_FORCESYNC_PARAM, maxParamLen) == 0) {

	rbusValue_t value;
        rbusValue_Init(&value);
        rbusValue_SetString(value, "");
        rbusProperty_SetValue(property, value);
        rbusValue_Release(value);

	if(!isRfcEnabled())
	{
		WebcfgError("RfcEnable is disabled so, %s Get from DB failed\n",propertyName);
		return 0;
	}
    }
    return RBUS_ERROR_SUCCESS;
}

/**
 *Event subscription handler to track the subscribers for the event
 */
rbusError_t eventSubHandler(rbusHandle_t handle, rbusEventSubAction_t action, const char* eventName, rbusFilter_t filter, int32_t interval, bool* autoPublish)
{
    (void)handle;
    (void)filter;
    (void)autoPublish;
    (void)interval;
    WebcfgInfo("eventSubHandler: action=%s eventName=%s\n", action == RBUS_EVENT_ACTION_SUBSCRIBE ? "subscribe" : "unsubscribe", eventName);
    if(!strcmp(WEBCFG_UPSTREAM_EVENT, eventName))
    {
        subscribed = action == RBUS_EVENT_ACTION_SUBSCRIBE ? 1 : 0;
    }
    else
    {
        WebcfgError("provider: eventSubHandler unexpected eventName %s\n", eventName);
    }
    return RBUS_ERROR_SUCCESS;
}

/**
 * Register data elements for dataModel implementation using rbus.
 * Data element over bus will be Device.X_RDK_WebConfig.RfcEnable, Device.X_RDK_WebConfig.ForceSync,
 * Device.X_RDK_WebConfig.URL
 */
WEBCFG_STATUS regWebConfigDataModel()
{
	rbusError_t ret = RBUS_ERROR_SUCCESS;
	rbusError_t retPsmGet = RBUS_ERROR_BUS_ERROR;
	WEBCFG_STATUS status = WEBCFG_SUCCESS;

	WebcfgInfo("Registering parameters %s, %s, %s %s\n", WEBCFG_RFC_PARAM, WEBCFG_FORCESYNC_PARAM, WEBCFG_URL_PARAM, WEBCFG_SUPPLEMENTARY_TELEMETRY_PARAM);
	if(!rbus_handle)
	{
		WebcfgError("regWebConfigDataModel Failed in getting bus handles\n");
		return WEBCFG_FAILURE;
	}

	rbusDataElement_t dataElements[NUM_WEBCFG_ELEMENTS] = {

		{WEBCFG_RFC_PARAM, RBUS_ELEMENT_TYPE_PROPERTY, {webcfgRfcGetHandler, webcfgRfcSetHandler, NULL, NULL, NULL, NULL}},
		{WEBCFG_URL_PARAM, RBUS_ELEMENT_TYPE_PROPERTY, {webcfgUrlGetHandler, webcfgUrlSetHandler, NULL, NULL, NULL, NULL}},
		{WEBCFG_FORCESYNC_PARAM, RBUS_ELEMENT_TYPE_PROPERTY, {webcfgFrGetHandler, webcfgFrSetHandler, NULL, NULL, NULL, NULL}},
		{WEBCFG_SUPPLEMENTARY_TELEMETRY_PARAM, RBUS_ELEMENT_TYPE_PROPERTY, {webcfgTelemetryGetHandler, webcfgTelemetrySetHandler, NULL, NULL, NULL, NULL}},
		{WEBCFG_DATA_PARAM, RBUS_ELEMENT_TYPE_PROPERTY, {webcfgDataGetHandler, webcfgDataSetHandler, NULL, NULL, NULL, NULL}},
		{WEBCFG_SUPPORTED_DOCS_PARAM, RBUS_ELEMENT_TYPE_PROPERTY, {webcfgSupportedDocsGetHandler, webcfgSupportedDocsSetHandler, NULL, NULL, NULL, NULL}},
		{WEBCFG_SUPPORTED_VERSION_PARAM, RBUS_ELEMENT_TYPE_PROPERTY, {webcfgSupportedVersionGetHandler, webcfgSupportedVersionSetHandler, NULL, NULL, NULL, NULL}},
		{WEBCFG_UPSTREAM_EVENT, RBUS_ELEMENT_TYPE_EVENT, {NULL, NULL, NULL, NULL, eventSubHandler, NULL}}
	};
	ret = rbus_regDataElements(rbus_handle, NUM_WEBCFG_ELEMENTS, dataElements);
	if(ret == RBUS_ERROR_SUCCESS)
	{
		WebcfgDebug("Registered data element %s with rbus \n ", WEBCFG_RFC_PARAM);
		memset(ForceSync, 0, 256);
		webcfgStrncpy( ForceSync, "", sizeof(ForceSync));

		memset(ForceSyncTransID, 0, 256);
		webcfgStrncpy( ForceSyncTransID, "", sizeof(ForceSyncTransID));

		// Initialise rfc enable global variable value
		char *tmpchar = NULL;
		retPsmGet = rbus_GetValueFromDB(paramRFCEnable, &tmpchar);
		if (retPsmGet != RBUS_ERROR_SUCCESS){
			WebcfgError("psm_get failed ret %d for parameter %s and value %s\n", retPsmGet, WEBCFG_RFC_PARAM, tmpchar);
		}
		else{
			WebcfgInfo("psm_get success ret %d for parameter %s and value %s\n", retPsmGet, WEBCFG_RFC_PARAM, tmpchar);
			if(tmpchar != NULL)
			{
				if(((strcmp (tmpchar, "true") == 0)) || (strcmp (tmpchar, "TRUE") == 0))
				{
					RfcVal = true;
				}
				free(tmpchar);
			}
		}
	}
	else
	{
		WebcfgError("Failed in registering data element %s \n", WEBCFG_RFC_PARAM);
		status = WEBCFG_FAILURE;
	}

	WebcfgInfo("rbus reg status returned is %d\n", status);
	return status;
}

//maps rbus rbusValueType_t to WDMP datatype
DATA_TYPE mapRbusToWdmpDataType(rbusValueType_t rbusType)
{
	DATA_TYPE wdmp_type = WDMP_NONE;

	switch (rbusType)
	{
		case RBUS_INT16:
		case RBUS_INT32:
			wdmp_type = WDMP_INT;
			break;
		case RBUS_UINT16:
		case RBUS_UINT32:
			wdmp_type = WDMP_UINT;
			break;
		case RBUS_INT64:
			wdmp_type = WDMP_LONG;
			break;
		case RBUS_UINT64:
			wdmp_type = WDMP_ULONG;
			break;
		case RBUS_SINGLE:
			wdmp_type = WDMP_FLOAT;
			break;
		case RBUS_DOUBLE:
			wdmp_type = WDMP_DOUBLE;
			break;
		case RBUS_DATETIME:
			wdmp_type = WDMP_DATETIME;
			break;
		case RBUS_BOOLEAN:
			wdmp_type = WDMP_BOOLEAN;
			break;
		case RBUS_CHAR:
		case RBUS_INT8:
			wdmp_type = WDMP_INT;
			break;
		case RBUS_UINT8:
		case RBUS_BYTE:
			wdmp_type = WDMP_UINT;
			break;
		case RBUS_STRING:
			wdmp_type = WDMP_STRING;
			break;
		case RBUS_BYTES:
			wdmp_type = WDMP_BYTE;
			break;
		case RBUS_PROPERTY:
		case RBUS_OBJECT:
		case RBUS_NONE:
		default:
			wdmp_type = WDMP_NONE;
			break;
	}
	WebcfgDebug("mapRbusToWdmpDataType : wdmp_type is %d\n", wdmp_type);
	return wdmp_type;
}

static rbusValueType_t mapWdmpToRbusDataType(DATA_TYPE wdmpType)
{
	DATA_TYPE rbusType = RBUS_NONE;

	switch (wdmpType)
	{
		case WDMP_INT:
			rbusType = RBUS_INT32;
			break;
		case WDMP_UINT:
			rbusType = RBUS_UINT32;
			break;
		case WDMP_LONG:
			rbusType = RBUS_INT64;
			break;
		case WDMP_ULONG:
			rbusType = RBUS_UINT64;
			break;
		case WDMP_FLOAT:
			rbusType = RBUS_SINGLE;
			break;
		case WDMP_DOUBLE:
			rbusType = RBUS_DOUBLE;
			break;
		case WDMP_DATETIME:
			rbusType = RBUS_DATETIME;
			break;
		case WDMP_BOOLEAN:
			rbusType = RBUS_BOOLEAN;
			break;
		case WDMP_STRING:
			rbusType = RBUS_STRING;
			break;
		case WDMP_BASE64:
			rbusType = RBUS_STRING;
			break;
		case WDMP_BYTE:
			rbusType = RBUS_BYTES;
			break;
		case WDMP_NONE:
		default:
			rbusType = RBUS_NONE;
			break;
	}

	WebcfgDebug("mapWdmpToRbusDataType : rbusType is %d\n", rbusType);
	return rbusType;
}

//To map Rbus error code to Ccsp error codes.
int mapRbusToCcspStatus(int Rbus_error_code)
{
    int CCSP_error_code = CCSP_Msg_Bus_ERROR;
    switch (Rbus_error_code)
    {
        case  RBUS_ERROR_SUCCESS  : CCSP_error_code = CCSP_Msg_Bus_OK; break;
        case  RBUS_ERROR_BUS_ERROR  : CCSP_error_code = CCSP_Msg_Bus_ERROR; break;
        case  RBUS_ERROR_INVALID_INPUT  : CCSP_error_code = CCSP_ERR_INVALID_PARAMETER_VALUE; break;
        case  RBUS_ERROR_NOT_INITIALIZED  : CCSP_error_code = CCSP_Msg_Bus_ERROR; break;
        case  RBUS_ERROR_OUT_OF_RESOURCES  : CCSP_error_code = CCSP_Msg_Bus_OOM; break;
        case  RBUS_ERROR_DESTINATION_NOT_FOUND  : CCSP_error_code = CCSP_Msg_BUS_CANNOT_CONNECT; break;
        case  RBUS_ERROR_DESTINATION_NOT_REACHABLE  : CCSP_error_code = CCSP_ERR_UNSUPPORTED_NAMESPACE; break;
        case  RBUS_ERROR_DESTINATION_RESPONSE_FAILURE  : CCSP_error_code = CCSP_Msg_BUS_TIMEOUT; break;
        case  RBUS_ERROR_INVALID_RESPONSE_FROM_DESTINATION  : CCSP_error_code = CCSP_ERR_UNSUPPORTED_PROTOCOL; break;
        case  RBUS_ERROR_INVALID_OPERATION  : CCSP_error_code = CCSP_Msg_BUS_NOT_SUPPORT; break;
        case  RBUS_ERROR_INVALID_EVENT : CCSP_error_code = CCSP_Msg_BUS_NOT_SUPPORT; break;
        case  RBUS_ERROR_INVALID_HANDLE : CCSP_error_code = CCSP_Msg_BUS_NOT_SUPPORT; break;
        case  RBUS_ERROR_SESSION_ALREADY_EXIST : CCSP_error_code = CCSP_Msg_BUS_NOT_SUPPORT; break;
    }
    WebcfgDebug("mapRbusToCcspStatus. CCSP_error_code is %d\n", CCSP_error_code);
    return CCSP_error_code;
}

void blobSet_rbus(char *name, void *bufVal, int len, WDMP_STATUS *retStatus, int *ccspRetStatus)
{
	rbusError_t ret = RBUS_ERROR_BUS_ERROR;
	*retStatus = WDMP_FAILURE;
	rbusValue_t value;
        rbusSetOptions_t opts;
        opts.commit = true;
	
	rbusValue_Init(&value);
	rbusValue_SetBytes(value, (uint8_t*)bufVal, len);
	
	if(!rbus_handle)
	{
		WebcfgError("blobSet_rbus Failed as rbus_handle is not initialized\n");
		return;
	}
	ret = rbus_set(rbus_handle, name, value, &opts);
	if (ret)
	{
		WebcfgError("rbus_set failed:%s\n", rbusError_ToString(ret));
	}
	else
	{
		WebcfgInfo("rbus_set success\n");
	}
	rbusValue_Release(value);
	*ccspRetStatus = mapRbusToCcspStatus((int)ret);
	WebcfgInfo("ccspRetStatus is %d\n", *ccspRetStatus);
       *retStatus = mapStatus(*ccspRetStatus);
}

void setValues_rbus(const param_t paramVal[], const unsigned int paramCount, const int setType,char *transactionId, money_trace_spans *timeSpan, WDMP_STATUS *retStatus, int *ccspRetStatus)
{
	unsigned int cnt = 0;
	int isInvalid = 0;
	bool isCommit = true;
	int sessionId = 0;
	rbusError_t ret = RBUS_ERROR_BUS_ERROR;
	rbusProperty_t properties = NULL, last = NULL;
	rbusValue_t setVal[paramCount];
	char const* setNames[paramCount];
	*retStatus = WDMP_FAILURE;

	if(!rbus_handle)
	{
		WebcfgError("setValues_rbus Failed as rbus_handle is not initialized\n");
		return;
	}

	WebcfgDebug("setValues_rbus setType %d\n", setType);
	WebcfgDebug("setValues_rbus transactionId %s\n",transactionId);
	WebcfgDebug("setValues_rbus timeSpan %p\n",timeSpan);

	for(cnt=0; cnt<paramCount; cnt++)
	{
		rbusValue_Init(&setVal[cnt]);

		setNames[cnt] = paramVal[cnt].name;
		WebcfgDebug("paramName to be set is %s paramCount %d\n", paramVal[cnt].name, paramCount);
		WebcfgDebug("paramVal is %s\n", paramVal[cnt].value);

		rbusValueType_t type = mapWdmpToRbusDataType(paramVal[cnt].type);

		if (type == RBUS_NONE)
		{
			WebcfgError("Invalid data type\n");
			isInvalid = 1;
			break;
		}

		rbusValue_SetFromString(setVal[cnt], type, paramVal[cnt].value);

		rbusProperty_t next;
		rbusProperty_Init(&next, setNames[cnt], setVal[cnt]);

		WebcfgDebug("Property Name[%d] is %s\n", cnt, rbusProperty_GetName(next));
		WebcfgDebug("Value type[%d] is %d\n", cnt, rbusValue_GetType(setVal[cnt]));

		if(properties == NULL)
		{
			properties = last = next;
		}
		else
		{
			rbusProperty_SetNext(last, next);
			last=next;
		}
	}

	if(!isInvalid)
	{
		isCommit = true;
		sessionId = 0;

		rbusSetOptions_t opts = {isCommit,sessionId};

		ret = rbus_setMulti(rbus_handle, paramCount, properties, &opts);
		WebcfgInfo("The ret status for rbus_setMulti is %d\n", ret);
	}
	else
	{
		ret = RBUS_ERROR_INVALID_INPUT;
		WebcfgError("Invalid input. ret %d\n", ret);
	}

	*ccspRetStatus = mapRbusToCcspStatus((int)ret);
	WebcfgInfo("ccspRetStatus is %d\n", *ccspRetStatus);

        *retStatus = mapStatus(*ccspRetStatus);

	WebcfgDebug("paramCount is %d\n", paramCount);
        for (cnt = 0; cnt < paramCount; cnt++)
        {
            rbusValue_Release(setVal[cnt]);
        }
        rbusProperty_Release(properties);
}

void getValues_rbus(const char *paramName[], const unsigned int paramCount, int index, money_trace_spans *timeSpan, param_t ***paramArr, int *retValCount, int *retStatus)
{
	int resCount = 0;
	rbusError_t rc;
	rbusProperty_t props = NULL;
	char* paramValue = NULL;
	char *pName = NULL;
	int i =0;
	unsigned int val_size = 0;
	rbusValue_t paramValue_t = NULL;
	rbusValueType_t type_t;
	unsigned int cnt=0;
	*retStatus = WDMP_FAILURE;

	for(cnt = 0; cnt < paramCount; cnt++)
	{
		WebcfgDebug("rbus_getExt paramName[%d] : %s paramCount %d\n",cnt,paramName[cnt], paramCount);
	}

	WebcfgDebug("setValues_rbus index %d\n", index);
	WebcfgDebug("getValues_rbus timeSpan %p\n",timeSpan);

	if(!rbus_handle)
	{
		WebcfgError("getValues_rbus Failed as rbus_handle is not initialized\n");
		return;
	}
	rc = rbus_getExt(rbus_handle, paramCount, paramName, &resCount, &props);

	WebcfgDebug("rbus_getExt rc=%d resCount=%d\n", rc, resCount);

	if(RBUS_ERROR_SUCCESS != rc)
	{
		WebcfgError("Failed to get value rbus_getExt rc=%d resCount=%d\n", rc, resCount);
		rbusProperty_Release(props);
		return;
	}
	if(props)
	{
		rbusProperty_t next = props;
		val_size = resCount;
		WebcfgDebug("val_size : %d\n",val_size);
		if(val_size > 0)
		{
			if(paramCount == val_size)
			{
				for (i = 0; i < resCount; i++)
				{
					WebcfgDebug("Response Param is %s\n", rbusProperty_GetName(next));
					paramValue_t = rbusProperty_GetValue(next);

					if(paramValue_t)
					{
						type_t = rbusValue_GetType(paramValue_t);
						paramValue = rbusValue_ToString(paramValue_t, NULL, 0);
						WebcfgDebug("Response paramValue is %s\n", paramValue);
						pName = strdup(rbusProperty_GetName(next));
						(*paramArr)[i] = (param_t *) malloc(sizeof(param_t));

						WebcfgDebug("Framing paramArr\n");
						(*paramArr)[i][0].name = strdup(pName);
						(*paramArr)[i][0].value = strdup(paramValue);
						(*paramArr)[i][0].type = mapRbusToWdmpDataType(type_t);
						WebcfgDebug("success: %s %s %d \n",(*paramArr)[i][0].name,(*paramArr)[i][0].value, (*paramArr)[i][0].type);
						*retValCount = resCount;
						*retStatus = WDMP_SUCCESS;
						if(paramValue !=NULL)
						{
							WEBCFG_FREE(paramValue);
						}
						if(pName !=NULL)
						{
							WEBCFG_FREE(pName);
						}
					}
					else
					{
						WebcfgError("Parameter value from rbus_getExt is empty\n");
					}
					next = rbusProperty_GetNext(next);
				}
			}
		}
		else if(val_size == 0 && rc == RBUS_ERROR_SUCCESS)
		{
			WebcfgInfo("No child elements found\n");
		}
		rbusProperty_Release(props);
	}
}

rbusError_t rbusWebcfgEventHandler(rbusHandle_t handle, rbusProperty_t prop, rbusSetHandlerOptions_t* opts)
{
	(void) handle;
	(void) opts;
	char const* paramName = NULL;

	paramName = rbusProperty_GetName(prop);

	if((paramName !=NULL) && (strncmp(paramName, WEBCFG_EVENT_NAME, maxParamLen) == 0))
	{
		rbusValue_t paramValue_t = rbusProperty_GetValue(prop);

		char* data = rbusValue_ToString(paramValue_t, NULL, 0);
		WebcfgDebug("Event data received from rbus_set is %s\n", data);
		if(data !=NULL)
		{
			char eventMsg[128]= {'\0'};
			webcfgStrncpy(eventMsg, data, sizeof(eventMsg));
			WEBCFG_FREE(data);
			WebcfgInfo("Received msg %s from topic webconfigSignal\n", eventMsg);

			webcfgCallback(eventMsg, NULL);
		}
	}
	return RBUS_ERROR_SUCCESS;
}

/* API to register RBUS dataElement to receive messages from components */
rbusError_t registerRBUSEventElement()
{
	rbusError_t rc = RBUS_ERROR_BUS_ERROR;
	if(!rbus_handle)
	{
		WebcfgError("registerRBUSEventElement failed as rbus_handle is not initialized\n");
		return rc;
	}

	rc = rbus_regDataElements(rbus_handle, 1, eventDataElement);
	if(rc != RBUS_ERROR_SUCCESS)
	{
		WebcfgError("registerRBUSEventElement failed err: %s\n", rbusError_ToString(rc));
	}
	else
	{
		WebcfgDebug("registerRBUSEventElement success\n");
	}
	return rc;
}

/* API to unregister RBUS dataElement events */
rbusError_t removeRBUSEventElement()
{
	rbusError_t rc = RBUS_ERROR_BUS_ERROR;
	if(!rbus_handle)
	{
		WebcfgError("rbus_unregDataElements failed as rbus_handle is not initialized\n");
		return rc;
	}

	rc = rbus_unregDataElements(rbus_handle, 1, eventDataElement);
	if(rc != RBUS_ERROR_SUCCESS)
	{
		WebcfgError("rbus_unregDataElements failed err: %s\n", rbusError_ToString(rc));
	}
	else
	{
		WebcfgDebug("rbus_unregDataElements success\n");
	}
	return rc;
}

bool get_rbus_RfcEnable()
{
    return RfcVal;
}

int set_rbus_RfcEnable(bool bValue)
{
	char* buf = NULL;
	int retPsmSet = RBUS_ERROR_SUCCESS;

	if(bValue == true)
	{
		buf = strdup("true");
		WebcfgDebug("Received RFC enable. updating g_shutdown\n");
		if(RfcVal == false)
		{
			pthread_mutex_lock (get_global_sync_mutex());
			set_global_shutdown(false);
			pthread_mutex_unlock(get_global_sync_mutex());
			WebcfgInfo("RfcEnable dynamic change from false to true. start initWebConfigMultipartTask.\n");
			if(get_global_mpThreadId() == NULL)
			{
				initWebConfigMultipartTask(0);
			}
			else
			{
				WebcfgInfo("Webconfig is already started, so not starting again for dynamic RFC change.\n");
			}
		}
	}
	else
	{
		buf = strdup("false");
		WebcfgDebug("Received RFC disable. updating g_shutdown\n");
		if(RfcVal == true)
		{
			/* sending signal to kill initWebConfigMultipartTask thread*/
			pthread_mutex_lock (get_global_sync_mutex());
			set_global_shutdown(true);
			pthread_cond_signal(get_global_sync_condition());
			pthread_mutex_unlock(get_global_sync_mutex());
		}
	}
	retPsmSet = rbus_StoreValueIntoDB( paramRFCEnable, buf );
	if (retPsmSet != RBUS_ERROR_SUCCESS)
	{
		WebcfgError("psm_set failed ret %d for parameter %s and value %s\n", retPsmSet, paramRFCEnable, buf);
		return 1;
	}
	else
	{
		WebcfgDebug("psm_set success ret %d for parameter %s and value %s\n", retPsmSet, paramRFCEnable, buf);
		RfcVal = bValue;
	}
	return 0;
}

int parseForceSyncJson(char *jsonpayload, char **forceSyncVal, char **forceSynctransID)
{
	cJSON *json = NULL;
	cJSON *forceSyncValObj = NULL;
	char *force_sync_str = NULL;
	char *force_sync_transid = NULL;

	json = cJSON_Parse( jsonpayload );
	if( !json )
	{
		WebcfgInfo( "Force sync json parsed: [%s]\n", cJSON_GetErrorPtr() );
	}
	else
	{
		forceSyncValObj = cJSON_GetObjectItem( json, "value" );
		if( forceSyncValObj != NULL)
		{
			force_sync_str = cJSON_GetObjectItem( json, "value" )->valuestring;
			force_sync_transid = cJSON_GetObjectItem( json, "transaction_id" )->valuestring;
			if ((force_sync_str != NULL) && strlen(force_sync_str) > 0)
			{
				if(strlen(force_sync_str) == strlen("telemetry"))
				{
					if(0 == strncmp(force_sync_str,"telemetry",strlen("telemetry")))
					{
						char telemetryUrl[256] = {0};
						Get_Supplementary_URL("Telemetry", telemetryUrl);
						if(strncmp(telemetryUrl,"NULL",strlen("NULL")) == 0)
						{
							WebcfgError("Telemetry url is null so, force sync SET failed\n");
							cJSON_Delete(json);
							return -1;
						}
					}
				}
				*forceSyncVal = strdup(force_sync_str);
				WebcfgDebug("*forceSyncVal value parsed from payload is %s\n", *forceSyncVal);
			}
			else
			{
				WebcfgError("*forceSyncVal string is empty\n");
			}
			if ((force_sync_transid != NULL) && strlen(force_sync_transid) > 0)
			{
				*forceSynctransID = strdup(force_sync_transid);
				WebcfgDebug("*forceSynctransID value parsed from json is %s\n", *forceSynctransID);
			}
			else
			{
				WebcfgError("*forceSynctransID is empty\n");
			}
		}
		else
		{
			WebcfgError("forceSyncValObj is NULL\n");
		}
		cJSON_Delete(json);
	}
	return 0;
}

int set_rbus_ForceSync(char* pString, int *pStatus)
{
    char *transactionId = NULL;
    char *value = NULL;
    int parseJsonRet = 0;

    memset( ForceSync, 0, sizeof( ForceSync ));

    if(pString !=NULL)
    {
	webcfgStrncpy(ForceSync, pString,  sizeof(ForceSync));
	if(strlen(pString)>0)
	{
		WebcfgInfo("Received poke request, proceed to parseForceSyncJson\n");
		parseJsonRet = parseForceSyncJson(pString, &value, &transactionId);
		if(-1 == parseJsonRet)
		{
			return 0; // 0 corresponds to indicate error or failure
		}
		if(value !=NULL)
		{
			WebcfgDebug("After parseForceSyncJson. value %s transactionId %s\n", value, transactionId);
			webcfgStrncpy(ForceSync, value, sizeof(ForceSync));
		}
	}
	WebcfgDebug("set_rbus_ForceSync . ForceSync string is %s\n", ForceSync);
       
        if(value !=NULL)
	{
		WEBCFG_FREE(value);
	}
    }

    if((ForceSync[0] !='\0') && (strlen(ForceSync)>0))
    {
        if(get_bootSync())
        {
            WebcfgInfo("Bootup sync is already in progress, Ignoring this request.\n");
            *pStatus = 1;
            return 0;
        }
	else if(get_maintenanceSync())
	{
		WebcfgInfo("Maintenance window sync is in progress, Ignoring this request.\n");
		*pStatus = 1;
		return 0;
	}
	else if(strlen(ForceSyncTransID)>0)
        {
            WebcfgInfo("Force sync is already in progress, Ignoring this request.\n");
            *pStatus = 1;
            return 0;
        }
        else
        {
            /* sending signal to initWebConfigMultipartTask to trigger sync */
            pthread_mutex_lock (get_global_sync_mutex());

            //Update ForceSyncTransID to access poke transactionId in webConfig sync.
            if(transactionId !=NULL && (strlen(transactionId)>0))
            {
                webcfgStrncpy(ForceSyncTransID , transactionId, sizeof(ForceSyncTransID));
                WebcfgInfo("ForceSyncTransID is %s\n", ForceSyncTransID);
            }
            WebcfgInfo("Trigger force sync\n");
            pthread_cond_signal(get_global_sync_condition());
            pthread_mutex_unlock(get_global_sync_mutex());

            if( transactionId != NULL)
	    {
		WEBCFG_FREE(transactionId);
	    }
        }
    }
    else
    {
        WebcfgDebug("Force sync param set with empty value\n");
	memset(ForceSyncTransID,0,sizeof(ForceSyncTransID));
    }
    return 1;
}

int get_rbus_ForceSync(char** pString, char **transactionId )
{

	if(((ForceSync)[0] != '\0') && strlen(ForceSync)>0)
	{
		*pString = strdup(ForceSync);
		*transactionId = strdup(ForceSyncTransID);
	}
	else
	{
		WebcfgDebug("setting NULL to pString and transactionId\n");
		*pString = NULL;
		*transactionId = NULL;
		return 0;
	}
	WebcfgDebug("*transactionId is %s\n",*transactionId);
	return 1;
}

void sendNotification_rbus(char *payload, char *source, char *destination)
{
	wrp_msg_t *notif_wrp_msg = NULL;
	char *contentType = NULL;
	int rc = RBUS_ERROR_SUCCESS;
	ssize_t msg_len;
	void *msg_bytes;

	if(source != NULL && destination != NULL)
	{
		notif_wrp_msg = (wrp_msg_t *)malloc(sizeof(wrp_msg_t));
		if(notif_wrp_msg != NULL)
		{
			memset(notif_wrp_msg, 0, sizeof(wrp_msg_t));
			notif_wrp_msg->msg_type = WRP_MSG_TYPE__EVENT;
			WebcfgDebug("source: %s\n",source);
			notif_wrp_msg->u.event.source = source;
			WebcfgDebug("destination: %s\n", destination);
			notif_wrp_msg->u.event.dest = strdup(destination);
			contentType = strdup("application/json");
			if(contentType != NULL)
			{
				notif_wrp_msg->u.event.content_type = contentType;
				WebcfgDebug("content_type is %s\n",notif_wrp_msg->u.event.content_type);
			}
			if(payload != NULL)
			{
				WebcfgDebug("Notification payload: %s\n",payload);
				notif_wrp_msg->u.event.payload = (void *)payload;
				notif_wrp_msg->u.event.payload_size = strlen(notif_wrp_msg ->u.event.payload);
			}

			msg_len = wrp_struct_to (notif_wrp_msg, WRP_BYTES, &msg_bytes);

			// 30s wait interval for subscription 	
			if(!subscribed)
			{
				waitForUpstreamEventSubscribe(30);
	    		}
			if(subscribed)
			{
				rbusValue_t value;
				rbusObject_t data;
				rbusValue_Init(&value);
				rbusValue_SetBytes(value, msg_bytes, msg_len);
				rbusObject_Init(&data, NULL);
				rbusObject_SetValue(data, "value", value);
				rbusEvent_t event;
				event.name = WEBCFG_UPSTREAM_EVENT;
				event.data = data;
				event.type = RBUS_EVENT_GENERAL;
				rc = rbusEvent_Publish(rbus_handle, &event);
				rbusValue_Release(value);
				rbusObject_Release(data);
				if(rc != RBUS_ERROR_SUCCESS)
					WebcfgError("Failed to send Notification : %d, %s\n", rc, rbusError_ToString(rc));
				else
					WebcfgInfo("Notification successfully sent to %s\n", WEBCFG_UPSTREAM_EVENT);
			}
			else
				WebcfgError("Failed to send Notification as no subscription\n");

			wrp_free_struct (notif_wrp_msg );
                        
                        if(msg_bytes)
			{
				WEBCFG_FREE(msg_bytes);
			}
		}
	}
}

// wait for upstream subscriber
void waitForUpstreamEventSubscribe(int wait_time)
{
	int count=0;
	if(!subscribed)
		WebcfgError("Waiting for %s event subscription for %ds\n", WEBCFG_UPSTREAM_EVENT, wait_time);
	while(!subscribed)
	{
		sleep(5);
		count++;
		if(count >= wait_time/5)
		{
			WebcfgError("Waited for %s event subscription for %ds, proceeding\n", WEBCFG_UPSTREAM_EVENT, wait_time);
			break; 
		}
	}
}

#ifdef WAN_FAILOVER_SUPPORTED
static void eventReceiveHandler(
    rbusHandle_t rbus_handle,
    rbusEvent_t const* event,
    rbusEventSubscription_t* subscription)
{
    (void)rbus_handle;
    char * interface = NULL;
    rbusValue_t newValue = rbusObject_GetValue(event->data, "value");
    rbusValue_t oldValue = rbusObject_GetValue(event->data, "oldValue");
    WebcfgInfo("Consumer receiver ValueChange event for param %s\n", event->name);

    if(newValue) {    
        interface = (char *) rbusValue_GetString(newValue, NULL);
	set_global_interface(interface);
    }	
    if(newValue !=NULL && oldValue!=NULL && get_global_interface()!=NULL) {
            WebcfgInfo("New Value: %s Old Value: %s New Interface Value: %s\n", rbusValue_GetString(newValue, NULL), rbusValue_GetString(oldValue, NULL), get_global_interface());
    }    
}

static void subscribeAsyncHandler(
    rbusHandle_t rbus_handle,
    rbusEventSubscription_t* subscription,
    rbusError_t error)
{
  (void)rbus_handle;

  WebcfgInfo("subscribeAsyncHandler event %s, error %d - %s\n", subscription->eventName, error, rbusError_ToString(error));
}


//Subscribe for Device.X_RDK_WanManager.CurrentActiveInterface
int subscribeTo_CurrentActiveInterface_Event()
{
      int rc = RBUS_ERROR_SUCCESS;
      WebcfgDebug("Subscribing to %s Event\n", WEBCFG_INTERFACE_PARAM);
      rc = rbusEvent_SubscribeAsync (
        rbus_handle,
        WEBCFG_INTERFACE_PARAM,
        eventReceiveHandler,
	subscribeAsyncHandler,
        "Webcfg_Interface",
        10*20);
      if(rc != RBUS_ERROR_SUCCESS) {
	      WebcfgError("%s subscribe failed : %d - %s\n", WEBCFG_INTERFACE_PARAM, rc, rbusError_ToString(rc));
      }  
      return rc;
}      
#endif
