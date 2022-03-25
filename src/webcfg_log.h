#ifndef _WEBCONFIG_LOG_H_
#define _WEBCONFIG_LOG_H_

#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>

#if ! defined(DEVICE_EXTENDER)
#include <cimplog.h>
#endif

#define WEBCFG_LOGGING_MODULE                     "WEBCONFIG"
/**
 * @brief Enables or disables debug logs.
 */
#if defined(BUILD_YOCTO) && ! defined(DEVICE_EXTENDER)

#define WEBCFG_RDK_LOGGING_MODULE                 "LOG.RDK.WEBCONFIG"

#define WebConfigLog(...)       __cimplog_rdk_generic(WEBCFG_RDK_LOGGING_MODULE, WEBCFG_LOGGING_MODULE, LEVEL_INFO, __VA_ARGS__)

/*#ifdef WEBCONFIG_BIN_SUPPORT

#define WebcfgError(...)	cimplog_error(WEBCFG_LOGGING_MODULE, __VA_ARGS__)
#define WebcfgInfo(...)		cimplog_info(WEBCFG_LOGGING_MODULE, __VA_ARGS__)
#define WebcfgDebug(...)	cimplog_debug(WEBCFG_LOGGING_MODULE, __VA_ARGS__)

#else*/

#define WebcfgError(...)	__cimplog_rdk_generic(WEBCFG_RDK_LOGGING_MODULE, WEBCFG_LOGGING_MODULE, LEVEL_ERROR, __VA_ARGS__)
#define WebcfgInfo(...)		__cimplog_rdk_generic(WEBCFG_RDK_LOGGING_MODULE, WEBCFG_LOGGING_MODULE, LEVEL_INFO, __VA_ARGS__)
#define WebcfgDebug(...)	__cimplog_rdk_generic(WEBCFG_RDK_LOGGING_MODULE, WEBCFG_LOGGING_MODULE, LEVEL_DEBUG, __VA_ARGS__)

//#endif

#else
#define WebConfigLog(...)       printf(__VA_ARGS__)

#define WebcfgError(...)	printf(__VA_ARGS__)
#define WebcfgInfo(...)		printf(__VA_ARGS__)
#define WebcfgDebug(...)	printf(__VA_ARGS__)

#endif



#endif /* _WEBCONFIG_LOG_H_ */
