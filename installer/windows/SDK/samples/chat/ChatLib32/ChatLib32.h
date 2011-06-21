/*
 * Copyright 2011, Qualcomm Innovation Center, Inc.
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
 */
//----------------------------------------------------------------------------------------------

#pragma once

#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers

#include <windows.h>
#include <stdio.h>

#include "ChatClasses.h"

extern "C" {

#define ALLJOYN_API __declspec(dllexport)

//----------------------------------------------------------------------------------------------
ALLJOYN_API void __stdcall SetOutStream(FPPrintCallBack callback);
ALLJOYN_API void __stdcall SetListener(FPJoinedCallBack callback);

//----------------------------------------------------------------------------------------------
ALLJOYN_API void __stdcall GetInterfaceName(char*arg, int& maxchars);
ALLJOYN_API void __stdcall GetNamePrefix(char*arg, int& maxchars);
ALLJOYN_API void __stdcall GetObjectPath(char*arg, int& maxchars);
ALLJOYN_API void __stdcall Connect(void);

//----------------------------------------------------------------------------------------------
ALLJOYN_API void __stdcall MessageOut(char*arg, int& maxchars);
ALLJOYN_API void __stdcall SetupChat(char* chatName, bool asAdvertiser, int& maxchars);

}
