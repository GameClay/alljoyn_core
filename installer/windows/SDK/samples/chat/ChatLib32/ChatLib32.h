
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
