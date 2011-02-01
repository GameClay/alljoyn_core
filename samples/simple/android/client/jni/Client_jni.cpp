/*
 * Copyright 2010-2011, Qualcomm Innovation Center, Inc.
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

#include "org_alljoyn_bus_samples_simpleclient_Client.h"
#include <alljoyn/BusAttachment.h>
#include <alljoyn/ProxyBusObject.h>
#include <alljoyn/AllJoynStd.h>
#include <alljoyn/BusListener.h>
#include <qcc/Log.h>
#include <qcc/String.h>
#include <new>
#include <android/log.h>
#include <assert.h>

#define LOG_TAG  "SimpleClient"

/* Missing (from NDK) log macros (cutils/log.h) */
#ifndef LOGD
#define LOGD(...) (__android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__))
#endif

#ifndef LOGI
#define LOGI(...) (__android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__))
#endif

#ifndef LOGE
#define LOGE(...) (__android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__))
#endif

using namespace ajn;

/* constants */
static const char* SIMPLE_SERVICE_INTERFACE_NAME = "org.alljoyn.bus.samples.simple";
static const char* SIMPLE_SERVICE_WELL_KNOWN_NAME_PREFIX = "org.alljoyn.bus.samples.simple.";
static const char* SIMPLE_SERVICE_OBJECT_PATH = "/simpleService";

/* forward decl */
class MyBusListener;

/* Static data */
static BusAttachment* s_bus = NULL;
static ProxyBusObject s_remoteObj;
static MyBusListener* s_busListener = NULL;

/* Local types */
class MyBusListener : public BusListener {
  public:
    MyBusListener(JavaVM* vm, jobject& jobj) : vm(vm), jobj(jobj) { }

    void FoundName(const char* name, const char* guid, const char* namePrefix, const char* busAddress)
    {
        LOGE("ENTERED FoundName with name=%s, busaddr=%s", name, busAddress);
        size_t prefixLen = strlen(SIMPLE_SERVICE_WELL_KNOWN_NAME_PREFIX);
        if (0 == strncmp(SIMPLE_SERVICE_WELL_KNOWN_NAME_PREFIX, name, prefixLen)) {
            /* Found a name that matches service prefix. Inform Java GUI of this name */
            JNIEnv* env;
            vm->AttachCurrentThread(&env, NULL);
            jclass jcls = env->GetObjectClass(jobj);
            jmethodID mid = env->GetMethodID(jcls, "FoundNameCallback", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
            if (mid == 0) {
                LOGE("Failed to get Java FoundNameCallback");
            } else {
                jstring jName = env->NewStringUTF(name + prefixLen);
                jstring jBusAddr = env->NewStringUTF(busAddress);
                LOGE("SimpleClient", "Calling FoundNameCallback");
                env->CallVoidMethod(jobj, mid, jName, jBusAddr);
                env->DeleteLocalRef(jName);
                env->DeleteLocalRef(jBusAddr);
            }
        }
    }

    void NameOwnerChanged(const char* busName, const char* previousOwner, const char* newOwner)
    {
    }

  private:
    JavaVM* vm;
    jobject jobj;
};


#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize AllJoyn and connect to local daemon.
 */
JNIEXPORT jint JNICALL Java_org_alljoyn_bus_samples_simpleclient_Client_simpleOnCreate(JNIEnv* env,
                                                                                       jobject jobj)
{
    QStatus status = ER_OK;
    const char* daemonAddr = "unix:abstract=alljoyn";

    /* Set AllJoyn logging */
    // QCC_SetLogLevels("ALLJOYN=7;ALL=1");
    QCC_UseOSLogging(true);

    /* Create message bus */
    s_bus = new BusAttachment("client", true);

    /* Add org.alljoyn.bus.samples.simple interface */
    InterfaceDescription* testIntf = NULL;
    status = s_bus->CreateInterface(SIMPLE_SERVICE_INTERFACE_NAME, testIntf);
    if (ER_OK == status) {
        testIntf->AddMethod("Ping", "s",  "s", "outStr, inStr", 0);
        testIntf->Activate();
    } else {
        LOGE("Failed to create interface \"%s\" (%s)", SIMPLE_SERVICE_INTERFACE_NAME, QCC_StatusText(status));
    }

    /* Start the msg bus */
    if (ER_OK == status) {
        status = s_bus->Start();
        if (ER_OK != status) {
            LOGE("BusAttachment::Start failed (%s)", QCC_StatusText(status));
        }
    }

    /* Install discovery and name changed callbacks */
    if (ER_OK == status) {
        JavaVM* vm;
        env->GetJavaVM(&vm);
        s_busListener = new MyBusListener(vm, jobj);
        s_bus->RegisterBusListener(*s_busListener);
    }

    /* Connect to the daemon */
    if (ER_OK == status) {
        status = s_bus->Connect(daemonAddr);
        if (ER_OK != status) {
            LOGE("BusAttachment::Connect(\"%s\") failed (%s)", daemonAddr, QCC_StatusText(status));
        }
    }

    /* Discover names starting with SIMPLE_SERVICE_WELL_KNOWN_NAME_PREFIX */
    if (ER_OK == status) {
        uint32_t disposition = 0;
        Message reply(*s_bus);
        MsgArg prefixArg("s", SIMPLE_SERVICE_WELL_KNOWN_NAME_PREFIX);
        status = s_bus->GetAllJoynProxyObj().MethodCall(org::alljoyn::Bus::InterfaceName, "FindName", &prefixArg, 1, reply, 5000);
        if (ER_OK == status) {
            if (reply->GetType() != MESSAGE_METHOD_RET) {
                status = ER_BUS_REPLY_IS_ERROR_MESSAGE;
            } else if (reply->GetArg(0)->v_uint32 != ALLJOYN_FINDNAME_REPLY_SUCCESS) {
                status = ER_FAIL;
            }
        }
        if (ER_OK != status) {
            LOGE("org.alljoyn.bus.FindName failed (%s) (disposition=%d)", QCC_StatusText(status), disposition);
        }
    }

    return (int) status;
}

/**
 * Request the local AllJoyn daemon to connect to a remote daemon.
 */
JNIEXPORT jboolean JNICALL Java_org_alljoyn_bus_samples_simpleclient_Client_connect(JNIEnv* env,
                                                                                    jobject jobj,
                                                                                    jstring jConnectStr)
{
    if (NULL == s_bus) {
        return jboolean(false);
    }

    /* Send a connect message to the daemon */
    uint32_t disposition = 0;
    jboolean iscopy;
    Message reply(*s_bus);
    const ProxyBusObject& alljoynObj = s_bus->GetAllJoynProxyObj();
    const char* connectStr = env->GetStringUTFChars(jConnectStr, &iscopy);
    MsgArg connectArg("s", connectStr);
    QStatus status = alljoynObj.MethodCall(org::alljoyn::Bus::InterfaceName, "Connect", &connectArg, 1, reply, 20000);
    if (ER_OK == status) {
        disposition = reply->GetArg(0)->v_uint32;
        status = (disposition == ALLJOYN_CONNECT_REPLY_SUCCESS) ? ER_OK : ER_FAIL;
    }
    if (ER_OK != status) {
        LOGE("%s.Connect(%s) failed (dispostion=%d)", org::alljoyn::Bus::InterfaceName, connectStr, QCC_StatusText(status), disposition);
    }
    env->ReleaseStringUTFChars(jConnectStr, connectStr);
    return jboolean(ER_OK == status);
}

/**
 * Request the local daemon to disconnect from the remote daemon.
 */
JNIEXPORT jboolean JNICALL Java_org_alljoyn_bus_samples_simpleclient_Client_disconnect(JNIEnv* env,
                                                                                       jobject jobj,
                                                                                       jstring jConnectStr)
{
    if (NULL == s_bus) {
        return jboolean(false);
    }

    /* Send a disconnect message to the daemon */
    jboolean iscopy;
    Message reply(*s_bus);
    const ProxyBusObject& alljoynObj = s_bus->GetAllJoynProxyObj();
    const char* connectStr = env->GetStringUTFChars(jConnectStr, &iscopy);
    MsgArg disconnectArg("s", connectStr);
    QStatus status = alljoynObj.MethodCall(org::alljoyn::Bus::InterfaceName, "Disconnect", &disconnectArg, 1, reply, 4000);
    if (ER_OK != status) {
        LOGE("%s.Disonnect(%s) failed", org::alljoyn::Bus::InterfaceName, connectStr, QCC_StatusText(status));
    }
    return jboolean(ER_OK == status);
}

/**
 * Called when SimpleClient Java application exits. Performs AllJoyn cleanup.
 */
JNIEXPORT void JNICALL Java_org_alljoyn_bus_samples_simpleclient_Client_simpleOnDestroy(JNIEnv* env, jobject jobj)
{
    /* Deallocate bus */
    if (NULL != s_bus) {
        delete s_bus;
        s_bus = NULL;
    }

    if (NULL != s_busListener) {
        delete s_busListener;
        s_busListener = NULL;
    }
}


/**
 * Invoke the remote method org.alljoyn.bus.samples.simple.Ping on the /simpleService object
 * located within the bus attachment named org.alljoyn.bus.samples.simple.
 */
JNIEXPORT jstring JNICALL Java_org_alljoyn_bus_samples_simpleclient_Client_simplePing(JNIEnv* env,
                                                                                      jobject jobj,
                                                                                      jstring jNamePrefix,
                                                                                      jstring jPingStr)
{
    /* Call the remote method */
    jboolean iscopy;
    const char* pingStr = env->GetStringUTFChars(jPingStr, &iscopy);
    const char* namePrefix = env->GetStringUTFChars(jNamePrefix, &iscopy);
    Message reply(*s_bus);
    MsgArg pingStrArg("s", pingStr);

    qcc::String nameStr(SIMPLE_SERVICE_WELL_KNOWN_NAME_PREFIX);
    nameStr.append(namePrefix);

    ProxyBusObject remoteObj = ProxyBusObject(*s_bus, nameStr.c_str(), SIMPLE_SERVICE_OBJECT_PATH);
    QStatus status = remoteObj.AddInterface(SIMPLE_SERVICE_INTERFACE_NAME);
    if (ER_OK == status) {
        status = remoteObj.MethodCall(SIMPLE_SERVICE_INTERFACE_NAME, "Ping", &pingStrArg, 1, reply, 5000);
        if (ER_OK == status) {
            LOGI("%s.%s (path=%s) returned \"%s\"", nameStr.c_str(), "Ping", SIMPLE_SERVICE_OBJECT_PATH, reply->GetArg(0)->v_string.str);
        } else {
            LOGE("MethodCall on %s.%s failed (%s)", nameStr.c_str(), "Ping", QCC_StatusText(status));
        }
    } else {
        LOGE("Failed to add interface %s to remote bus obj", SIMPLE_SERVICE_INTERFACE_NAME);
    }

    if (ER_OK == status) {
        LOGI("Ping reply is: \"%s\"", reply->GetArg(0)->v_string.str);
    }
    env->ReleaseStringUTFChars(jPingStr, pingStr);
    return env->NewStringUTF((ER_OK == status) ? reply->GetArg(0)->v_string.str : "");
}


#ifdef __cplusplus
}

#endif
