/**
 * @file
 * @brief Sample implementation of an AllJoyn client.
 *
 * This client will subscribe to the nameChanged signal sent from the 'org.alljoyn.Bus.signal_sample'
 * service.  When a name change signal is sent this will print out the new value for the
 * 'name' property that was sent by the service.
 *
 */

/******************************************************************************
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
 ******************************************************************************/

#include <qcc/platform.h>

#include <signal.h>
#include <stdio.h>
#include <assert.h>
#include <vector>

#include <qcc/String.h>

#include <alljoyn/BusAttachment.h>
#include <alljoyn/BusObject.h>
#include <alljoyn/version.h>
#include <alljoyn/AllJoynStd.h>

#include <Status.h>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#define QCC_MODULE "ALLJOYN"

using namespace std;
using namespace qcc;
using namespace ajn;

/*constants*/
static const char* SERVICE_NAME = "org.alljoyn.Bus.signal_sample";
static const char* SERVICE_PATH = "/";

/** Static top level message bus object */
static BusAttachment* g_msgBus = NULL;

/** Signal handler */
static void SigIntHandler(int sig)
{
    if (NULL != g_msgBus) {
        QStatus status = g_msgBus->Stop(false);
        if (ER_OK != status) {
            printf("BusAttachment::Stop() failed\n");
        }
    }
    exit(0);
}

/** AllJoynListener receives discovery events from AllJoyn */
class MyBusListener : public BusListener {
  public:

    MyBusListener() : BusListener(), sessionId(0) { }

    void FoundName(const char* name, const char* guid, const char* namePrefix, const char* busAddress)
    {
        if (0 == strcmp(name, SERVICE_NAME)) {
            printf("FoundName(name=%s, guid=%s, addr=%s)\n", name, guid, busAddress);
            /* We found a remote bus that is advertising bbservice's well-known name so connect to it */
            uint32_t disposition;
            SessionId sessionId;
            QosInfo qos;
            QStatus status = g_msgBus->JoinSession(name, disposition, sessionId, qos);
            if ((ER_OK != status) || (ALLJOYN_JOINSESSION_REPLY_SUCCESS != disposition)) {
                printf("JoinSession failed (status=%s, disposition=%d)", QCC_StatusText(status), disposition);
            }
        }
    }

    void NameOwnerChanged(const char* name, const char* previousOwner, const char* newOwner)
    {
        if (newOwner && (0 == strcmp(name, SERVICE_NAME))) {
            printf("NameOwnerChanged(%s, %s, %s)\n",
                   name,
                   previousOwner ? previousOwner : "null",
                   newOwner ? newOwner : "null");
        }
    }

    SessionId GetSessionId() const { return sessionId; }

  private:
    SessionId sessionId;
};

/** Static bus listener */
static MyBusListener g_busListener;

class SignalListeningObject : public BusObject {
  public:
    SignalListeningObject(BusAttachment& bus, const char* path) :
        BusObject(bus, path)
    {
        /* Empty constructor */
    }

    QStatus SubscribeNameChangedSignal(void) {

        QStatus status;

        /* Register a bus listener in order to get discovery indications */
        g_msgBus->RegisterBusListener(g_busListener);
        printf("BusListener Registered.\n");

        /* Begin discovery on the well-known name of the service to be called */
        Message reply(bus);
        const ProxyBusObject& alljoynProxyObj = g_msgBus->GetAllJoynProxyObj();

        MsgArg serviceName("s", SERVICE_NAME);
        status = alljoynProxyObj.MethodCall(ajn::org::alljoyn::Bus::InterfaceName,
                                            "FindName",
                                            &serviceName,
                                            1,
                                            reply,
                                            5000);
        if (ER_OK != status) {
            printf("%s.FindName failed\n", ajn::org::alljoyn::Bus::InterfaceName);
        } else {
            printf("%s.FindName method called.\n", ajn::org::alljoyn::Bus::InterfaceName);
        }

        ProxyBusObject remoteObj;
        if (ER_OK == status) {
            remoteObj = ProxyBusObject(bus, SERVICE_NAME, SERVICE_PATH, g_busListener.GetSessionId());
            status = remoteObj.IntrospectRemoteObject();
            if (ER_OK != status) {
                printf("Introspection of %s (path=%s) failed\n", SERVICE_NAME, SERVICE_PATH);
                printf("Make sure the service is running before launching the client.\n");
            }
            ;
        }

        const InterfaceDescription* intf = remoteObj.GetInterface(SERVICE_NAME);
        assert(intf);
        status = remoteObj.AddInterface(*intf);
        if (ER_OK == status) {
            printf("the %s interface has been added to the ProxyBusObject.\n", SERVICE_NAME);
        } else {
            printf("Error adding %s interface to the ProxyBusObject.\n", SERVICE_NAME);
        }

        const InterfaceDescription::Member* nameChangedMember = intf->GetMember("nameChanged");
        assert("nameChangedMember");



        /* register the signal handler for the the 'nameChanged' signal */
        status =  bus.RegisterSignalHandler(this,
                                            static_cast<MessageReceiver::SignalHandler>(&SignalListeningObject::NameChangedSignalHandler),
                                            nameChangedMember,
                                            NULL);
        if (status != ER_OK) {
            printf("Failed to register signal handler for %s.nameChanged\n", SERVICE_NAME);
        } else {
            printf("Registered signal handler for %s.nameChanged\n", SERVICE_NAME);
        }

        /* add the match rules */
        const ProxyBusObject& dbusObj = bus.GetDBusProxyObj();

        MsgArg arg("s", "type='signal',interface='org.alljoyn.Bus.signal_sample',member='nameChanged'");
        status = dbusObj.MethodCall("org.freedesktop.DBus", "AddMatch", &arg, 1, reply);

        if (status != ER_OK) {
            printf("Failed to register Match rule for 'org.alljoyn.Bus.signal_sample.nameChanged\n");
            printf("reply msg: %s\n", reply->ToString().c_str());
            printf("Status %d (%s)\n", status, QCC_StatusText(status));
        } else {
            printf("Registered Match rule for 'org.alljoyn.Bus.signal_sample.nameChanged signal\n");
        }
        return status;
    }

    void ObjectRegistered(void)
    {
        BusObject::ObjectRegistered();
    }

    void NameChangedSignalHandler(const InterfaceDescription::Member* member,
                                  const char* sourcePath,
                                  Message& msg)
    {
        printf("--==## signalConsumer: Name Changed signal Received ##==--\n");
        printf("\tNew name: %s\n", msg->GetArg(0)->v_string.str);

    }

};

/** Main entry point */
int main(int argc, char** argv, char** envArg)
{
    QStatus status = ER_OK;

    printf("AllJoyn Library version: %s\n", ajn::GetVersion());

    /* Install SIGINT handler */
    signal(SIGINT, SigIntHandler);

#ifdef _WIN32
    qcc::String connectArgs = "tcp:addr=127.0.0.1,port=9955";
#else
    qcc::String connectArgs = "unix:abstract=alljoyn";
#endif

    g_msgBus = new BusAttachment("myApp", true);
    status = g_msgBus->Start();
    if (status == ER_OK) {
        printf("BusAttachment started\n");
        /* Register object */
        SignalListeningObject object(*g_msgBus, SERVICE_PATH);

        g_msgBus->RegisterBusObject(object);

        /* Create the client-side endpoint */
        status = g_msgBus->Connect(connectArgs.c_str());

        if (status != ER_OK) {
            printf("failed to connect to '%s'\n", connectArgs.c_str());
        } else {
            printf("BusAttachement connected to %s\n", connectArgs.c_str());
        }
        if (ER_OK == status) {
            status = object.SubscribeNameChangedSignal();
            if (status != ER_OK) {
                printf("Failed to Subscribe to the Name Changed Signal.\n");
            } else {
                printf("Successfully Subscribed to the Name Changed Signal.\n");
            }

        }
    }

    if (status == ER_OK) {
        /*
         * Wait until bus is stopped
         */
        g_msgBus->WaitStop();
    } else {
        printf("BusAttachment::Start failed\n");
    }

    /* Deallocate bus */
    if (g_msgBus) {
        BusAttachment* deleteMe = g_msgBus;
        g_msgBus = NULL;
        delete deleteMe;
    }

    printf("Exiting with status %d (%s)\n", status, QCC_StatusText(status));

    return (int) status;
}
