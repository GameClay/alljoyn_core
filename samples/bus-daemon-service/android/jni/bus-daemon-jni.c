/******************************************************************************
 * Copyright 2010 - 2011, Qualcomm Innovation Center, Inc.
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
 *
 ******************************************************************************/
#include <string.h>
#include <jni.h>
#include <android/log.h>

#define LOG_TAG "bus-daemon-jni"

//
// The AllJoyn test program bbdaemon has an alternate personality in that it is
// built as a static library, somewhat redundantly, libbbdaemon-lib.a.  In
// this case, the entry point main() is replaced by the function DaemonMain
// that takes the same parameters.  Calling DaemonMain() here essentially
// runs the bbdaemon program with its default parameters.
//
// The fact that the daemon is run with its default parameters is very
// important as this defines how services or clients must connect to it
// (the unix domain sockets) and which TCP port it uses to communicate with
// other daemons.  Neither of these is a shared resource, so this ulitmately
// means that only one of these services can run on a phone at any given
// time.
//
extern int DaemonMain(int argc, char** argv);

void Java_org_alljoyn_bus_daemonservice_DaemonService_runDaemon(JNIEnv* env, jobject thiz)
{
    char const* name = "bus-daemon-jni";

    //
    // Make a log entry saying that the daemon was run.
    //
    __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, "runDaemon(): calling DaemonMain()\n");

    //
    // Run the bbdaemon test program with no arguments.
    //
    int rc = DaemonMain(1, (char**)&name);

    //
    // Make a log entry saying that the daemon has returned.  We don't expect
    // this to happen unless bbdaemon detects an error and shuts down, so we
    // take care to log the return code.  If Android decides to kill the
    // service, we expect it will do so via a SIGKILL, and we will never know
    // it happened.
    //
    __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, "runDaemon(): back from DaemonMain(): returned %d\n", rc);
}
