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

typedef void* alljoyn_busattachment;
typedef void* alljoyn_buslistener;
typedef void* alljoyn_proxybusobject;
typedef void* alljoyn_interfacedescription;
typedef const void* alljoyn_interfacedescription_const;
typedef struct _alljoyn_message_handle* alljoyn_message;
typedef void* alljoyn_msgargs;
typedef const void* alljoyn_msgargs_const;
typedef void* alljoyn_sessionopts;
typedef const void* alljoyn_sessionopts_const;
typedef void* alljoyn_sessionlistener;
typedef void* alljoyn_sessionportlistener;
typedef void* alljoyn_busobject;
typedef const void* alljoyn_interfacedescription_member_const;

#endif