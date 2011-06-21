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
// ChatLib32.cpp : Defines the exported functions for the DLL application.
//

#include "chatlib32.h"

using namespace ajn;

static FPPrintCallBack ManagedOutput = NULL;
static FPJoinedCallBack JoinNotifier = NULL;

static ChatConnection* s_connection = NULL;

static char BUFFER[2048];

void NotifyUser(NotifyType informType, const char* format, ...)
{
    va_list ap;
    va_start(ap, format);
    vsprintf_s(BUFFER, 2048, format, ap);
    va_end(ap);
    if (NULL != ManagedOutput) {
        int i = strlen(BUFFER);
        int t = (int) informType;
        ManagedOutput(BUFFER, i,  t);
    }
}

ALLJOYN_API void __stdcall MessageOut(char*arg, int& maxchars)
{
    const int bufsize = 1024;
    static char outbuf[bufsize];
    strcpy_s(outbuf, bufsize, arg);
    outbuf[maxchars] = 0;
    QStatus status = s_connection->chatObject->SendChatSignal(outbuf);
}

ALLJOYN_API void __stdcall SetupChat(char* chatName, bool asAdvertiser, int& maxchars)
{
    if (NULL == s_connection)
        s_connection = new ChatConnection(*ManagedOutput, *JoinNotifier);
    if (asAdvertiser) {
        s_connection->advertisedName = NAME_PREFIX;
        s_connection->advertisedName += chatName;
        s_connection->joinName = "";
        NotifyUser(MSG_STATUS, "%s is advertiser \n", s_connection->advertisedName.c_str());
    } else {
        s_connection->joinName = NAME_PREFIX;
        s_connection->joinName += chatName;
        s_connection->advertisedName = "";
        NotifyUser(MSG_STATUS, "%s is joiner\n", s_connection->joinName.c_str());
    }
}

ALLJOYN_API void __stdcall SetOutStream(FPPrintCallBack callback)
{
    ManagedOutput = callback;
}

ALLJOYN_API void __stdcall SetListener(FPJoinedCallBack callback)
{
    JoinNotifier = callback;
}

ALLJOYN_API void __stdcall GetInterfaceName(char*arg, int& maxchars)
{
    strcpy_s(arg, maxchars, CHAT_SERVICE_INTERFACE_NAME);
    maxchars = strlen(CHAT_SERVICE_INTERFACE_NAME);
}

ALLJOYN_API void __stdcall GetNamePrefix(char*arg, int& maxchars)
{
    strcpy_s(arg, maxchars, NAME_PREFIX);
    maxchars = strlen(NAME_PREFIX);
}

ALLJOYN_API void __stdcall GetObjectPath(char*arg, int& maxchars)
{
    strcpy_s(arg, maxchars, CHAT_SERVICE_OBJECT_PATH);
    maxchars = strlen(CHAT_SERVICE_OBJECT_PATH);
}

//
ALLJOYN_API void __stdcall Connect()
{
    s_connection->Connect();
}


BOOL APIENTRY DllMain(HMODULE hModule,
                      DWORD ul_reason_for_call,
                      LPVOID lpReserved
                      )
{
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}

