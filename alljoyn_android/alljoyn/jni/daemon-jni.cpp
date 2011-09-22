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
#define QCC_OS_GROUP_POSIX
#include <string.h>
#include <jni.h>
#include <android/log.h>
#define LOG_TAG "daemon-jni"

// The AllJoyn daemon has an alternate personality in that it is built as a
// static library, liballjoyn-daemon.a.  In this case, the entry point main() is
// replaced by a function called DaemonMain.  Calling DaemonMain() here
// essentially runs the AllJoyn daemon like it had been run on the command line.
//

namespace ajn {
extern const char* GetVersion();        /**< Gives the version of AllJoyn Library */
extern const char* GetBuildInfo();      /**< Gives build information of AllJoyn Library */
};

extern int DaemonMain(int argc, char** argv, char* serviceConfig);


void do_log(const char* format, ...)
{
    va_list arglist;

    va_start(arglist, format);
    __android_log_vprint(ANDROID_LOG_DEBUG, LOG_TAG, format, arglist);
    va_end(arglist);

    return;
}

extern "C" JNIEXPORT void JNICALL Java_org_alljoyn_bus_alljoyn_AllJoynDaemon_runDaemon(JNIEnv* env, jobject thiz, jobjectArray jargv, jstring jconfig)
{
    int i;
    jsize argc;
    do_log("runDaemon()\n");

    argc = env->GetArrayLength(jargv);
    do_log("runDaemon(): argc = %d\n", argc);
    char const** argv  = (char const**)malloc(argc * sizeof(char*));

    for (i = 0; i < argc; ++i) {
        jstring jstr = (jstring)env->GetObjectArrayElement(jargv, i);
        argv[i] = env->GetStringUTFChars(jstr, 0);
        do_log("runDaemon(): argv[%d] = %s\n", i, argv[i]);
    }

    char const* config = env->GetStringUTFChars(jconfig, 0);
    do_log("runDaemon(): config = %s\n", config);

    //
    // Run the alljoyn-daemon we have built as a library.
    //
    do_log("runDaemon(): calling DaemonMain()\n");
    int rc = DaemonMain(argc, (char**)argv, (char*)config);

    free(argv);
}

extern "C" {

JNIEXPORT jstring JNICALL Java_org_alljoyn_bus_alljoyn_AllJoynDaemon_getDaemonVersion(JNIEnv* env, jobject thiz)
{
    return env->NewStringUTF(ajn::GetVersion());
}

JNIEXPORT jstring JNICALL Java_org_alljoyn_bus_alljoyn_AllJoynDaemon_getDaemonBuildInfo(JNIEnv* env, jobject thiz)
{
    return env->NewStringUTF(ajn::GetBuildInfo());
}

}
