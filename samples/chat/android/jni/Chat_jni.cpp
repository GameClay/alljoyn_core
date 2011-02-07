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

#include "org_alljoyn_bus_samples_chat_Chat.h"
#include <alljoyn/BusAttachment.h>
#include <alljoyn/ProxyBusObject.h>
#include <alljoyn/InterfaceDescription.h>
#include <alljoyn/DBusStd.h>
#include <alljoyn/AllJoynStd.h>
#include <qcc/Log.h>
#include <qcc/String.h>
#include <new>
#include <android/log.h>
#include <assert.h>


#include <qcc/platform.h>

#define LOG_TAG  "AllJoynChat"

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
static const char* CHAT_SERVICE_INTERFACE_NAME = "org.alljoyn.bus.samples.chat";
static const char* CHAT_SERVICE_WELL_KNOWN_NAME = "org.alljoyn.bus.samples.chat";
static const char* CHAT_SERVICE_OBJECT_PATH = "/chatService";
static const char* NAME_PREFIX = "org.alljoyn.bus.samples.chat";

/* Forward declaration */
class ChatObject;
class MyBusListener;

/* Static data */
static BusAttachment* s_bus = NULL;
static ChatObject* s_chatObj = NULL;
static qcc::String s_connectName;
static qcc::String s_advertisedName;
static MyBusListener* s_busListener = NULL;


class MyBusListener : public BusListener {
  public:
    MyBusListener(JavaVM* vm, jobject& jobj) : vm(vm), jobj(jobj) { }

    void FoundName(const char* name, const char* guid, const char* namePrefix, const char* busAddress)
    {
        LOGE("Chat", "FoundName signal received from %s", busAddress);

        /* We found a remote bus that is advertising bbservice's well-known name so connect to it */
        uint32_t disposition;
        QStatus status = s_bus->ConnectToRemoteBus(busAddress, disposition);
        if ((ER_OK == status) && (ALLJOYN_CONNECT_REPLY_SUCCESS == disposition)) {
            LOGE("\n Connected to bus %s having well known name %s ", busAddress, name);
        } else {
            LOGE("ConnectToRemoteBus failed (status=%s, disposition=%d)", QCC_StatusText(status), disposition);
        }

    }
    void NameOwnerChanged(const char* busName, const char* previousOwner, const char* newOwner)
    {
    }
  private:
    JavaVM* vm;
    jobject jobj;
};


/* Bus object */
class ChatObject : public BusObject {
  public:

    ChatObject(BusAttachment& bus, const char* path, JavaVM* vm, jobject jobj) : BusObject(bus, path), vm(vm), jobj(jobj)
    {
        QStatus status;

        /* Add the chat interface to this object */
        const InterfaceDescription* chatIntf = bus.GetInterface(CHAT_SERVICE_INTERFACE_NAME);
        assert(chatIntf);
        AddInterface(*chatIntf);

        /* Store the Chat signal member away so it can be quickly looked up when signals are sent */
        chatSignalMember = chatIntf->GetMember("Chat");
        assert(chatSignalMember);

        /* Register signal handler */
        status =  bus.RegisterSignalHandler(this,
                                            static_cast<MessageReceiver::SignalHandler>(&ChatObject::ChatSignalHandler),
                                            chatSignalMember,
                                            NULL);

        if (ER_OK != status) {
            LOGE("Failed to register s_advertisedNamesignal handler for ChatObject::Chat (%s)", QCC_StatusText(status));
        }
    }

    /** Send a Chat signal */
    QStatus SendChatSignal(const char* msg) {
        MsgArg chatArg("s", msg);

        uint8_t flags = 0;
        flags |= ALLJOYN_FLAG_GLOBAL_BROADCAST;
        return Signal(NULL, *chatSignalMember, &chatArg, 1, 0, flags);
    }

    /** Receive a signal from another Chat client */
    void ChatSignalHandler(const InterfaceDescription::Member* member, const char* srcPath, Message& msg)
    {
        /* Inform Java GUI of this message */
        JNIEnv* env;
        vm->AttachCurrentThread(&env, NULL);
        jclass jcls = env->GetObjectClass(jobj);
        jmethodID mid = env->GetMethodID(jcls, "ChatCallback", "(Ljava/lang/String;Ljava/lang/String;)V");
        if (mid == 0) {
            LOGE("Failed to get Java ChatCallback");
        } else {

            jstring jSender = env->NewStringUTF(msg->GetSender());
            jstring jChatStr = env->NewStringUTF(msg->GetArg(0)->v_string.str);
            env->CallVoidMethod(jobj, mid, jSender, jChatStr);
            env->DeleteLocalRef(jSender);
            env->DeleteLocalRef(jChatStr);
        }
    }



    void NameAcquiredCB(Message& msg, void* context)
    {
        /* Check name acquired result */
        size_t numArgs;
        const MsgArg* args;
        msg->GetArgs(numArgs, args);

        if (args[0].v_uint32 == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
            /* Begin Advertising the well known name to remote busses */
            const ProxyBusObject& alljoynObj = s_bus->GetAllJoynProxyObj();
            MsgArg arg("s", s_advertisedName.c_str());
            QStatus status = alljoynObj.MethodCallAsync(org::alljoyn::Bus::InterfaceName,
                                                        "AdvertiseName",
                                                        this,
                                                        static_cast<MessageReceiver::ReplyHandler>(&ChatObject::AdvertiseRequestCB),
                                                        &arg,
                                                        1);
            if (ER_OK != status) {
                LOGE("Sending org.alljoyn.bus.Advertise failed");
            }
        } else {
            LOGE("Failed to obtain name \"%s\". RequestName returned %d", s_advertisedName.c_str(), args[0].v_uint32);
        }



    }


    void AdvertiseRequestCB(Message& msg, void* context)
    {
        /* Make sure request was processed */
        size_t numArgs;
        const MsgArg* args;
        msg->GetArgs(numArgs, args);

        if ((MESSAGE_METHOD_RET != msg->GetType()) || (ALLJOYN_ADVERTISENAME_REPLY_SUCCESS != args[0].v_uint32)) {
            LOGE("Failed to advertise name \"%s\". org.alljoyn.bus.Advertise returned %d", s_advertisedName.c_str(), args[0].v_uint32);
        }

    }


    void ObjectRegistered(void) {

        BusObject::ObjectRegistered();

        /* Request a well-known name */
        /* Note that you cannot make a blocking method call here */
        const ProxyBusObject& dbusObj = s_bus->GetDBusProxyObj();
        MsgArg args[2];
        args[0].Set("s", s_advertisedName.c_str());
        args[1].Set("u", 6);
        QStatus status = dbusObj.MethodCallAsync(org::freedesktop::DBus::InterfaceName,
                                                 "RequestName",
                                                 s_chatObj,
                                                 static_cast<MessageReceiver::ReplyHandler>(&ChatObject::NameAcquiredCB),
                                                 args,
                                                 2);
        if (ER_OK != status) {
            LOGE("Failed to request name %s ", s_advertisedName.c_str());
        }
    }

    /** Release the well-known name if it was acquired */
    void ReleaseName() {
        if (s_bus) {
            uint32_t disposition = 0;

            const ProxyBusObject& dbusObj = s_bus->GetDBusProxyObj();
            Message reply(*s_bus);
            MsgArg arg;
            arg.Set("s", s_advertisedName.c_str());
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
                LOGE("Failed to release name %s (%s, disposition=%d)", s_advertisedName.c_str(), QCC_StatusText(status), disposition);
            }
        }
    }


  private:
    JavaVM* vm;
    jobject jobj;
    const InterfaceDescription::Member* chatSignalMember;
};

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize AllJoyn and connect to local daemon.
 */
JNIEXPORT jint JNICALL Java_org_alljoyn_bus_samples_chat_Chat_jniOnCreate(JNIEnv* env, jobject jobj)
{
    QStatus status = ER_OK;
    const char* daemonAddr = "unix:abstract=alljoyn";

    /* Set AllJoyn logging */
    QCC_SetLogLevels("ALLJOYN=7;ALL=1");
    QCC_UseOSLogging(true);

    /* Create message bus */
    s_bus = new BusAttachment("chat", true);

    /* Create org.alljoyn.bus.samples.chat interface */
    InterfaceDescription* chatIntf = NULL;
    status = s_bus->CreateInterface(CHAT_SERVICE_INTERFACE_NAME, chatIntf);
    if (ER_OK == status) {
        chatIntf->AddSignal("Chat", "s",  "str", 0);
        chatIntf->Activate();
    } else {
        LOGE("Failed to create interface \"%s\" (%s)", CHAT_SERVICE_INTERFACE_NAME, QCC_StatusText(status));
    }

    /* Start the msg bus */
    if (ER_OK == status) {
        status = s_bus->Start();
        if (ER_OK != status) {
            LOGE("BusAttachment::Start failed (%s)", QCC_StatusText(status));
        }
    }

    /* Register a bus listener in order to get discovery indications */
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

    /* Add a rule to allow org.codeaurora.samples.chat.Chat signals to be routed here */
    if (ER_OK == status) {
        MsgArg arg("s", "type='signal',interface='org.alljoyn.bus.samples.chat',member='Chat'");
        Message reply(*s_bus);
        const ProxyBusObject& dbusObj = s_bus->GetDBusProxyObj();
        status = dbusObj.MethodCall(org::freedesktop::DBus::InterfaceName,
                                    "AddMatch",
                                    &arg,
                                    1,
                                    reply);
        if (status != ER_OK) {
            LOGE("Failed to register Match rule for 'org.alljoyn.bus.samples.chat.Chat': %s\n",
                 QCC_StatusText(status));
        }
    }


    return (int) status;
}

/**
 * Request the local daemon to disconnect from the remote daemon.
 */
JNIEXPORT jboolean JNICALL Java_org_alljoyn_bus_samples_chat_Chat_disconnect(JNIEnv* env,
                                                                             jobject jobj)
{
    if ((NULL == s_bus) || s_connectName.empty()) {
        return jboolean(false);
    }
    /* Send a disconnect message to the daemon */
    Message reply(*s_bus);
    const ProxyBusObject& alljoynObj = s_bus->GetAllJoynProxyObj();
    MsgArg disconnectArg("s", s_connectName.c_str());
    QStatus status = alljoynObj.MethodCall(org::alljoyn::Bus::InterfaceName, "Disconnect", &disconnectArg, 1, reply, 4000);
    if (ER_OK != status) {
        LOGE("%s.Disonnect(%s) failed", org::alljoyn::Bus::InterfaceName, s_connectName.c_str(), QCC_StatusText(status));
    }
    s_connectName.clear();
    return jboolean(ER_OK == status);
}

/**
 * Called when SimpleClient Java application exits. Performs AllJoyn cleanup.
 */
JNIEXPORT void JNICALL Java_org_alljoyn_bus_samples_chat_Chat_jniOnDestroy(JNIEnv* env, jobject jobj)
{
    /* Deallocate bus */
    if (NULL != s_bus) {
        delete s_bus;
        s_bus = NULL;
    }

    /* Deregister the ServiceObject. */
    if (s_chatObj) {
        s_chatObj->ReleaseName();
        s_bus->DeregisterBusObject(*s_chatObj);
        delete s_chatObj;
        s_chatObj = NULL;
    }

    if (NULL != s_busListener) {
        delete s_busListener;
        s_busListener = NULL;
    }
}


/**
 * Send a broadcast ping message to all handlers registered for org.codeauora.alljoyn.samples.chat.Chat signal.
 */
JNIEXPORT jint JNICALL Java_org_alljoyn_bus_samples_chat_Chat_sendChatMsg(JNIEnv* env,
                                                                          jobject jobj,
                                                                          jstring chatMsgObj)
{
    /* Send a signal */
    jboolean iscopy;
    const char* chatMsg = env->GetStringUTFChars(chatMsgObj, &iscopy);
    QStatus status = s_chatObj->SendChatSignal(chatMsg);
    if (ER_OK != status) {
        LOGE("Chat", "Sending signal failed (%s)", QCC_StatusText(status));
    }
    env->ReleaseStringUTFChars(chatMsgObj, chatMsg);
    return (jint) status;
}

JNIEXPORT jboolean JNICALL Java_org_alljoyn_bus_samples_chat_Chat_advertise(JNIEnv* env,
                                                                            jobject jobj,
                                                                            jstring advertiseStrObj)
{

    if (NULL == s_bus) {
        return jboolean(false);
    }

    jboolean iscopy;

    const char* advertisedNameStr = env->GetStringUTFChars(advertiseStrObj, &iscopy);
    s_advertisedName = "";
    s_advertisedName += NAME_PREFIX;
    s_advertisedName += ".";
    s_advertisedName += advertisedNameStr;


    /* Create and register the bus object that will be used to send out signals */
    JavaVM* vm;
    env->GetJavaVM(&vm);
    s_chatObj = new ChatObject(*s_bus, CHAT_SERVICE_OBJECT_PATH, vm, jobj);
    s_bus->RegisterBusObject(*s_chatObj);

    LOGE("Chat", "---------- Registered Bus Object -----------");



    Message reply(*s_bus);
    const ProxyBusObject& alljoynObj = s_bus->GetAllJoynProxyObj();

    // Look for the prefix
    MsgArg serviceName("s", NAME_PREFIX);
    QStatus status = alljoynObj.MethodCall(::org::alljoyn::Bus::InterfaceName,
                                           "FindName",
                                           &serviceName,
                                           1,
                                           reply,
                                           5000);
    if (ER_OK == status) {
        if (reply->GetType() != MESSAGE_METHOD_RET) {
            status = ER_BUS_REPLY_IS_ERROR_MESSAGE;
        } else if (reply->GetArg(0)->v_uint32 != ALLJOYN_FINDNAME_REPLY_SUCCESS) {
            status = ER_FAIL;
        }
    } else {
        LOGE("%s.FindName failed", ::org::alljoyn::Bus::InterfaceName);
    }


    env->ReleaseStringUTFChars(advertiseStrObj, advertisedNameStr);
    return (jboolean) true;
}



#ifdef __cplusplus
}

#endif
