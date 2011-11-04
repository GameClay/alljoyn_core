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

#include "targetver.h"

#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers

// Windows Header Files:
#include <windows.h>

#include "AllJoynConnection.h"

extern "C" {

#define ALLJOYN_API __declspec(dllexport)

// AllJoynBus Properties
//----------------------------------------------------------------------------------------------
ALLJOYN_API void __stdcall SetJoinListener(FPJoinedCallback callback);
ALLJOYN_API void __stdcall SetLocalOutputStream(FPPrintCallback callback);
ALLJOYN_API void __stdcall GetNamePrefix(char*arg, int& maxchars);
// ALLJOYN_API void __stdcall SetNamePrefix(char *arg, int & maxchars);

// AllJoynBus API
//----------------------------------------------------------------------------------------------
ALLJOYN_API void __stdcall ConnectToAllJoyn(char* identity, bool& asAdvertiser);
ALLJOYN_API void __stdcall DisconnectFromAllJoyn(void);

// XferObject Bus Methods
ALLJOYN_API void __stdcall SetIncomingXferInterface(FPQueryCallback qcb, FPXferCallback xcb);
ALLJOYN_API void __stdcall QueryRemoteXfer(int index, char* filename, int& filesize, int& accept);
ALLJOYN_API void __stdcall InitiateXfer(int proxyIndex, int segmentSize, int nSegments, bool& success);
ALLJOYN_API void __stdcall TransferSegment(int proxyIndex, void* bytes, int segmentSize, int nSegments, bool& success);
ALLJOYN_API void __stdcall GetRemoteTransferStatus(int proxyIndex, int& state, int& errorCode);
// 0 - available 1 - busy -1 error
ALLJOYN_API void __stdcall SetPendingTransferIn(char* filename, bool& success);
ALLJOYN_API void __stdcall EndRemoteTransfer(int proxyIndex, bool& success);
// RemoteXferObject
ALLJOYN_API void __stdcall CreateXferProxyFor(char* name, int& index);
ALLJOYN_API void __stdcall ReleaseXferProxy(char* name, int& index);

// ChatObject Signal Interface
ALLJOYN_API void __stdcall MessageOut(char*arg, int& maxchars);

// RemoteChatObject




} // end extern "C"

