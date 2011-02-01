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

#include "org_alljoyn_bus_samples_simpleservice_Service.h"
#include <alljoyn/BusAttachment.h>
#include <alljoyn/ProxyBusObject.h>
#include <alljoyn/DBusStd.h>
#include <alljoyn/AllJoynStd.h>
#include <qcc/Log.h>
#include <new>
#include <assert.h>
#include <android/log.h>

#define LOG_TAG  "SimpleService"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

/* Missing (from NDK) log macros (use to be in cutils/log.h) */
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

/* Forward decl */
class ServiceObject;

/* Static data */
static BusAttachment* s_bus = NULL;
static ServiceObject* s_obj = NULL;

/* Bus object */
class ServiceObject : public BusObject {
  public:

    ServiceObject(BusAttachment& bus, const char* path, const char* serviceName, JavaVM* vm, jobject jobj)
        : BusObject(bus, path), vm(vm), jobj(jobj), isNameAcquired(false)
    {
        QStatus status;

        wellKnownName = SIMPLE_SERVICE_WELL_KNOWN_NAME_PREFIX;
        wellKnownName.append(serviceName);

        /* Add the service interface to this object */
        const InterfaceDescription* regTestIntf = bus.GetInterface(SIMPLE_SERVICE_INTERFACE_NAME);
        assert(regTestIntf);
        AddInterface(*regTestIntf);

        /* Register the method handlers with the object */
        const MethodEntry methodEntries[] = {
            { regTestIntf->GetMember("Ping"), static_cast<MessageReceiver::MethodHandler>(&ServiceObject::Ping) }
        };
        status = AddMethodHandlers(methodEntries, ARRAY_SIZE(methodEntries));
        if (ER_OK != status) {
            LOGE("Failed to register method handlers for ServiceObject (%s)", QCC_StatusText(status));
        }
    }

    void ObjectRegistered(void) {
        BusObject::ObjectRegistered();

        /* Request a well-known name */
        /* Note that you cannot make a blocking method call here */
        const ProxyBusObject& dbusObj = bus.GetDBusProxyObj();
        MsgArg args[2];
        args[0].Set("s", wellKnownName.c_str());
        args[1].Set("u", DBUS_NAME_FLAG_DO_NOT_QUEUE);
        QStatus status = dbusObj.MethodCallAsync(org::freedesktop::DBus::InterfaceName,
                                                 "RequestName",
                                                 this,
                                                 static_cast<MessageReceiver::ReplyHandler>(&ServiceObject::NameAcquiredCB),
                                                 args,
                                                 ARRAY_SIZE(args));
        if (ER_OK != status) {
            LOGE("Failed to request name %s (%s)", wellKnownName.c_str(), QCC_StatusText(status));
        }
    }

    void NameAcquiredCB(Message& msg, void* context)
    {
        /* Note you cannot make a blocking call here since we are in a callback */
        /* If name request was successful, then advertise the name */
        if ((msg->GetType() == MESSAGE_METHOD_RET) && (msg->GetArg(0)->v_uint32 == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER)) {
            isNameAcquired = true;
            const ProxyBusObject& alljoynObj = bus.GetAllJoynProxyObj();
            MsgArg args;
            args.Set("s", wellKnownName.c_str());
            LOGE("Advertising name \"%s\"", wellKnownName.c_str());
            QStatus status = alljoynObj.MethodCallAsync(org::alljoyn::Bus::InterfaceName,
                                                        "AdvertiseName",
                                                        this,
                                                        static_cast<MessageReceiver::ReplyHandler>(&ServiceObject::NameAdvertisedCB),
                                                        &args,
                                                        1);

            if (ER_OK != status) {
                LOGE("Failed to request name %s (%s)", wellKnownName.c_str(), QCC_StatusText(status));
            }
        } else {
            LOGE("Failed to request the name \"%s\"", wellKnownName.c_str());
        }
    }

    void NameAdvertisedCB(Message& msg, void* context) {
        if ((msg->GetType() != MESSAGE_METHOD_RET) || (msg->GetArg(0)->v_uint32 != ALLJOYN_ADVERTISENAME_REPLY_SUCCESS)) {
            LOGE("Failed to advertisze the name \"%s\"", wellKnownName.c_str());
        }
    }

    /** Release the well-known name if it was acquired */
    void ReleaseName() {
        if (s_bus && isNameAcquired) {
            uint32_t disposition = 0;
            isNameAcquired = false;
            const ProxyBusObject& dbusObj = bus.GetDBusProxyObj();
            Message reply(*s_bus);
            MsgArg arg;
            arg.Set("s", wellKnownName.c_str());
            QStatus status = dbusObj.MethodCall(org::freedesktop::DBus::InterfaceName,
                                                "ReleaseName",
                                                &arg,
                                                1,
                                                reply,
                                                5000);
            if (ER_OK == status) {
                disposition = reply->GetArg(0)->v_uint32;
            }
            if ((ER_OK != status) || (disposition != DBUS_RELEASE_NAME_REPLY_RELEASED)) {
                LOGE("Failed to release name %s (%s, disposition=%d)", wellKnownName.c_str(), QCC_StatusText(status), disposition);
            }
        }
    }


    void CancelAdvertise() {
        uint32_t disposition = 0;
        Message reply(*s_bus);
        const ProxyBusObject& alljoynObj = bus.GetAllJoynProxyObj();
        MsgArg args;
        args.Set("s", wellKnownName.c_str());
        QStatus status = alljoynObj.MethodCall(org::alljoyn::Bus::InterfaceName,
                                               "CancelAdvertiseName",
                                               &args,
                                               1,
                                               reply,
                                               5000);
        if (ER_OK == status) {
            disposition = reply->GetArg(0)->v_uint32;
        }
        if ((ER_OK != status) || (disposition != ALLJOYN_CANCELADVERTISENAME_REPLY_SUCCESS)) {
            LOGE("Failed to cancel advertise name %s (%s)", wellKnownName.c_str(), QCC_StatusText(status));
        }
    }

    /** Implement org.alljoyn.bus.samples.simple.Ping by returning the passed in string */
    void Ping(const InterfaceDescription::Member* member, Message& msg)
    {
        const char* pingStr = msg->GetArg(0)->v_string.str;

        LOGD("Pinged from %s with: %s\n", msg->GetSender(), pingStr);

        /* Inform Java GUI of this ping */
        JNIEnv* env;
        vm->AttachCurrentThread(&env, NULL);

        jclass jcls = env->GetObjectClass(jobj);
        jmethodID mid = env->GetMethodID(jcls, "PingCallback", "(Ljava/lang/String;Ljava/lang/String;)V");
        if (mid == 0) {
            LOGE("Failed to get Java PingCallback");
        } else {
            jstring jSender = env->NewStringUTF(msg->GetSender());
            jstring jPingStr = env->NewStringUTF(pingStr);
            env->CallVoidMethod(jobj, mid, jSender, jPingStr);
            env->DeleteLocalRef(jSender);
            env->DeleteLocalRef(jPingStr);
        }

        /* Reply with same string that was sent to us */
        MsgArg reply(*(msg->GetArg(0)));
        QStatus status = MethodReply(msg, &reply, 1);
        if (ER_OK != status) {
            LOGE("Ping: Error sending reply (%s)", QCC_StatusText(status));
        }


    }

  private:
    JavaVM* vm;
    jobject jobj;
    qcc::String wellKnownName;
    bool isNameAcquired;
};

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Class:     org_alljoyn_bus_samples_simpleservice_Service
 * Method:    simpleOnCreate
 * Signature: ()I
 */
JNIEXPORT jint JNICALL Java_org_alljoyn_bus_samples_simpleservice_Service_simpleOnCreate(JNIEnv* env, jobject jobj)
{
    QStatus status = ER_OK;
    const char* daemonAddr = "unix:abstract=alljoyn";
    jboolean iscopy;

    /* Set AllJoyn logging */
    // QCC_SetLogLevels("ALLJOYN=7;ALL=1");
    QCC_UseOSLogging(true);

    /* Create message bus */
    s_bus = new BusAttachment("bbservice", true);

    /* Add org.codeaurora.samples.simple interface */
    InterfaceDescription* testIntf = NULL;
    status = s_bus->CreateInterface(SIMPLE_SERVICE_INTERFACE_NAME, testIntf);
    if (ER_OK == status) {
        testIntf->AddMethod("Ping", "s",  "s", "inStr,outStr", 0);
        testIntf->Activate();
    } else {
        LOGE("Failed to create interface %s (%s)", SIMPLE_SERVICE_INTERFACE_NAME, QCC_StatusText(status));
    }

    /* Start the msg bus */
    if (ER_OK == status) {
        status = s_bus->Start();
    } else {
        LOGE("BusAttachment::Start failed (%s)", QCC_StatusText(status));
    }

    /* Connect to the daemon */
    if (ER_OK == status) {
        status = s_bus->Connect(daemonAddr);
        if (ER_OK != status) {
            LOGE("Connect to %s failed (%s)", daemonAddr, QCC_StatusText(status));
        }
    }

    return status;
}

/*
 * Class:     org_alljoyn_bus_samples_simpleservice_Service
 * Method:    startService
 * Signature: (Ljava/lang/String;)Z
 */
JNIEXPORT jboolean JNICALL Java_org_alljoyn_bus_samples_simpleservice_Service_startService(JNIEnv* env, jobject jobj, jstring jServiceName)
{
    if (s_obj) {
        return (jboolean) false;
    }

    jboolean iscopy;
    const char* serviceName = env->GetStringUTFChars(jServiceName, &iscopy);

    /* Register service object */
    JavaVM* vm;
    env->GetJavaVM(&vm);
    s_obj = new ServiceObject(*s_bus, SIMPLE_SERVICE_OBJECT_PATH, serviceName, vm, jobj);
    s_bus->RegisterBusObject(*s_obj);

    env->DeleteLocalRef(jServiceName);
    return (jboolean) true;
}

/*
 * Class:     org_alljoyn_bus_samples_simpleservice_Service
 * Method:    stopService
 * Signature: ()V
 */
JNIEXPORT void JNICALL Java_org_alljoyn_bus_samples_simpleservice_Service_stopService(JNIEnv* env, jobject jobj)
{
    /* Deregister the ServiceObject. */
    if (s_obj) {
        s_obj->ReleaseName();
        s_obj->CancelAdvertise();
        s_bus->DeregisterBusObject(*s_obj);
        delete s_obj;
        s_obj = NULL;
    }
}

/*
 * Class:     org_alljoyn_bus_samples_simpleservice_Service
 * Method:    simpleOnDestroy
 * Signature: ()V
 */
JNIEXPORT void JNICALL Java_org_alljoyn_bus_samples_simpleservice_Service_simpleOnDestroy(JNIEnv* env, jobject jobj)
{
    /* Unregister and deallocate service object */
    if (s_obj) {
        if (s_bus) {
            s_bus->DeregisterBusObject(*s_obj);
        }
        delete s_obj;
    }

    /* Deallocate bus */
    if (s_bus) {
        delete s_bus;
    }
}

#ifdef __cplusplus
}
#endif
