/**
 * @file
 * Sample implementation of an AllJoyn service that provides a raw stream.
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

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <vector>

#include <qcc/Debug.h>
#include <qcc/Environ.h>
#include <qcc/Mutex.h>
#include <qcc/String.h>
#include <qcc/Thread.h>
#include <qcc/time.h>
#include <qcc/Util.h>

#include <alljoyn/BusAttachment.h>
#include <alljoyn/BusObject.h>
#include <alljoyn/DBusStd.h>
#include <alljoyn/AllJoynStd.h>
#include <alljoyn/MsgArg.h>
#include <alljoyn/version.h>

#include <Status.h>


#define QCC_MODULE "ALLJOYN"

using namespace std;
using namespace qcc;
using namespace ajn;

namespace org {
namespace alljoyn {
namespace stream_test {
const char* InterfaceName = "org.alljoyn.stream_test";
const char* DefaultWellKnownName = "org.alljoyn.stream_test";
const char* ObjectPath = "/org/alljoyn/stream_test";
}
}
}

/** Static top level message bus object */
static BusAttachment* g_msgBus = NULL;
static String g_wellKnownName = ::org::alljoyn::stream_test::DefaultWellKnownName;

/** Signal handler */
static void SigIntHandler(int sig)
{
    if (NULL != g_msgBus) {
        QStatus status = g_msgBus->Stop(false);
        if (ER_OK != status) {
            QCC_LogError(status, ("BusAttachment::Stop() failed"));
        }
    }
}

class MyBusListener : public BusListener {
    bool AcceptSession(const char* sessionName, SessionId id, const char* joiner, const QosInfo& qos)
    {
        QCC_SyncPrintf("Accepting JoinSession request from %s\n", joiner);

        /* Get the streaming socket */
        if (qos.traffic == QosInfo::TRAFFIC_STREAM_RELIABLE) {
            MsgArg arg;
            arg.Set("u", id);
            const ProxyBusObject& ajObj = g_msgBus->GetAllJoynProxyObj();
            Message reply(*g_msgBus);
            QStatus status = ajObj.MethodCall(ajn::org::alljoyn::Bus::InterfaceName,
                                              "GetSessionFd",
                                              &arg,
                                              1,
                                              reply);
            if ((status == ER_OK) && (reply->GetType() == MESSAGE_METHOD_RET)) {
                size_t na;
                const MsgArg* args;
                reply->GetArgs(na, args);
                SocketFd sockFd;
                status = args[0].Get("h", &sockFd);
                if (status == ER_OK) {
                    /* Attempt to write test string to fd */
                    const char* testBytes = "Test Streaming Bytes";
                    size_t testBytesLen = ::strlen(testBytes);
                    int ret = write(sockFd, testBytes, testBytesLen);
                    if (ret > 0) {
                        QCC_SyncPrintf("Wrote %d of %d bytes to streaming fd\n", ret, testBytesLen);
                    } else {
                        QCC_SyncPrintf("Write to streaming fd failed (%d)\n", ret);
                    }
                } else {
                    QCC_SyncPrintf("Failed to get socket from GetSessionFd args\n");
                }
                
            } else {
                QCC_SyncPrintf("GetSessionFd failed: %s\n", reply->ToString().c_str());
            }
        }
        /* Allow the join attempt */
        return true;
    }
};

class LocalTestObject : public BusObject {

  public:

    LocalTestObject(BusAttachment& bus, const char* path) :
        BusObject(bus, path),
        sessionId(0)
    {
    }

    void ObjectRegistered(void)
    {
        Message reply(bus);

        /* Request a well-known name */
        const ProxyBusObject& dbusObj = bus.GetDBusProxyObj();
        MsgArg args[2];
        args[0].Set("s", g_wellKnownName.c_str());
        args[1].Set("u", 6);
        QStatus status = dbusObj.MethodCall(ajn::org::freedesktop::DBus::InterfaceName,
                                            "RequestName",
                                            args,
                                            ArraySize(args),
                                            reply);
        if ((status != ER_OK) || (reply->GetType() != MESSAGE_METHOD_RET)) {
            status = (status == ER_OK) ? ER_BUS_ERROR_RESPONSE : status;
            QCC_LogError(status, ("Failed to request name %s", g_wellKnownName.c_str()));
            return;
        } else {
            size_t numArgs;
            const MsgArg* replyArgs;
            reply->GetArgs(numArgs, replyArgs);
            if (replyArgs[0].v_uint32 != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
                status = ER_FAIL;
                QCC_LogError(status, ("RequestName(%s) returned failed status %d", g_wellKnownName.c_str(), replyArgs[0].v_uint32));
                return;
            }
        }

        /* Create a session for incoming client connections */
        const ProxyBusObject& alljoynObj = bus.GetAllJoynProxyObj();
        MsgArg createSessionArgs[2];
        createSessionArgs[0].Set("s", g_wellKnownName.c_str());
        createSessionArgs[1].Set(QOSINFO_SIG, QosInfo::TRAFFIC_STREAM_RELIABLE, QosInfo::PROXIMITY_ANY, QosInfo::TRANSPORT_ANY);
        status = alljoynObj.MethodCall(ajn::org::alljoyn::Bus::InterfaceName,
                                       "CreateSession",
                                       createSessionArgs,
                                       ArraySize(createSessionArgs),
                                       reply);
        
        if ((status != ER_OK) || (reply->GetType() != MESSAGE_METHOD_RET)) {
            status = (status == ER_OK) ? ER_BUS_ERROR_RESPONSE : status;
            QCC_LogError(status, ("CreateSession(%s,<>) failed", g_wellKnownName.c_str()));
            return;
        } else {
            size_t numArgs;
            const MsgArg* replyArgs;
            reply->GetArgs(numArgs, replyArgs);
            if (replyArgs[0].v_uint32 == ALLJOYN_CREATESESSION_REPLY_SUCCESS) {
                sessionId = replyArgs[1].v_uint32;
            } else {
                status = ER_FAIL;
                QCC_LogError(status, ("CreateSession(%s) returned failed status %d", g_wellKnownName.c_str(), replyArgs[0].v_uint32));
                return;
            }
        }

        /* Begin Advertising the well-known name */
        MsgArg advArg("s", g_wellKnownName.c_str());
        status = alljoynObj.MethodCall(ajn::org::alljoyn::Bus::InterfaceName,
                                       "AdvertiseName",
                                       &advArg,
                                       1,
                                       reply);
        if ((ER_OK != status) || (reply->GetType() != MESSAGE_METHOD_RET)) {
            status = (status == ER_OK) ? ER_BUS_ERROR_RESPONSE : status;
            QCC_LogError(status, ("Sending org.alljoyn.Bus.Advertise failed"));
            return;
        } else {
            size_t numArgs;
            const MsgArg* replyArgs;
            reply->GetArgs(numArgs, replyArgs);
            if (replyArgs[0].v_uint32 != ALLJOYN_ADVERTISENAME_REPLY_SUCCESS) {
                QCC_LogError(ER_FAIL, ("AdvertiseName(%s) failed with %d", g_wellKnownName.c_str(), replyArgs[0].v_uint32));
                return;
            }
        }
    }

  private:

    SessionId sessionId;
};


static void usage(void)
{
    printf("Usage: streamservice [-h] [-n <name>]\n\n");
    printf("Options:\n");
    printf("   -h         = Print this help message\n");
    printf("   -n <name>  = Well-known name to advertise\n");
}

/** Main entry point */
int main(int argc, char** argv)
{
    QStatus status = ER_OK;

    printf("AllJoyn Library version: %s\n", ajn::GetVersion());
    printf("AllJoyn Library build info: %s\n", ajn::GetBuildInfo());

    /* Install SIGINT handler */
    signal(SIGINT, SigIntHandler);

    /* Parse command line args */
    for (int i = 1; i < argc; ++i) {
        if (0 == strcmp("-h", argv[i])) {
            usage();
            exit(0);
        } else if (0 == strcmp("-n", argv[i])) {
            ++i;
            if (i == argc) {
                printf("option %s requires a parameter\n", argv[i - 1]);
                usage();
                exit(1);
            } else {
                g_wellKnownName = argv[i];
            }
        } else {
            status = ER_FAIL;
            printf("Unknown option %s\n", argv[i]);
            usage();
            exit(1);
        }
    }

    /* Get env vars */
    Environ* env = Environ::GetAppEnviron();
    qcc::String clientArgs = env->Find("DBUS_STARTER_ADDRESS");

    if (clientArgs.empty()) {
#ifdef _WIN32
        clientArgs = env->Find("BUS_ADDRESS", "tcp:addr=127.0.0.1,port=9955");
#else
        clientArgs = env->Find("BUS_ADDRESS", "unix:abstract=alljoyn");
#endif
    }

    /* Create message bus */
    g_msgBus = new BusAttachment("streamservice", true);
    MyBusListener myBusListener;

    /* Start the msg bus */
    status = g_msgBus->Start();
    if (status != ER_OK) {
        QCC_LogError(status, ("BusAttachment::Start failed"));
    }

    /* Create a bus listener to be used to accept incoming session requests */
    if (status == ER_OK) {
        g_msgBus->RegisterBusListener(myBusListener);

        /* Register local objects and connect to the daemon */
        LocalTestObject testObj(*g_msgBus, ::org::alljoyn::stream_test::ObjectPath);
        g_msgBus->RegisterBusObject(testObj);

        /* Connect to the daemon */
        status = g_msgBus->Connect(clientArgs.c_str());
        if (status == ER_OK) {
            /* Wait until bus is stopped */
            g_msgBus->WaitStop();
        } else {
            QCC_LogError(status, ("Failed to connect to \"%s\"", clientArgs.c_str()));
        }
        /* Deregister the bus object */
        g_msgBus->DeregisterBusObject(testObj);
    }

    delete g_msgBus;

    printf("%s exiting with status %d (%s)\n", argv[0], status, QCC_StatusText(status));

    return (int) status;
}
