#ifndef _ALLJOYN_ALLJOYNCTYPES_H
#define _ALLJOYN_ALLJOYNCTYPES_H
/**
 * @file
 * This file provides definitions for AllJoyn C types.
 */

/******************************************************************************
 * Copyright 2009-2011, Qualcomm Innovation Center, Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 ******************************************************************************/

#include <qcc/platform.h>

typedef struct _alljoyn_busattachment_handle*               alljoyn_busattachment;
typedef struct _alljoyn_buslistener_handle*                 alljoyn_buslistener;
typedef struct _alljoyn_proxybusobject_handle*              alljoyn_proxybusobject;
typedef struct _alljoyn_interfacedescription_handle*        alljoyn_interfacedescription;
typedef struct _alljoyn_message_handle*                     alljoyn_message;
typedef struct _alljoyn_msgargs_handle*                     alljoyn_msgargs;
typedef struct _alljoyn_sessionopts_handle*                 alljoyn_sessionopts;
typedef struct _alljoyn_sessionlistener_handle*             alljoyn_sessionlistener;
typedef struct _alljoyn_sessionportlistener_handle*         alljoyn_sessionportlistener;
typedef struct _alljoyn_busobject_handle*                   alljoyn_busobject;
typedef struct _alljoyn_authlistener_handle*                alljoyn_authlistener;
typedef struct _alljoyn_credentials_handle*                 alljoyn_credentials;
typedef struct _alljoyn_keystore_handle*                    alljoyn_keystore;
typedef struct _alljoyn_keystorelistener_handle*            alljoyn_keystorelistener;

#endif