/**
 * @file
 * @brief Sample implementation of an AllJoyn service.
 *
 * This sample will show how to set up an AllJoyn service that will registered with the
 * well-known name 'org.alljoyn.Bus.signal_sample'.  The service will register a signal method 'nameChanged'
 * as well as a property 'name'.
 *
 * When the property 'sampleName' is changed by any client this service will emit the new name using
 * the 'nameChanged' signal.
 *
 */

/******************************************************************************
 *
 *
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

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <vector>

#include <qcc/String.h>

#include <alljoyn/BusAttachment.h>
#include <alljoyn/BusObject.h>
#include <alljoyn/MsgArg.h>
#include <alljoyn/DBusStd.h>
#include <alljoyn/AllJoynStd.h>
#include <alljoyn/version.h>
#include <Status.h>

using namespace std;
using namespace qcc;
using namespace ajn;

/** Static top level message bus object */
static BusAttachment* g_msgBus = NULL;
static const char* SERVICE_NAME = "org.alljoyn.Bus.signal_sample";
static const char* SERVICE_PATH = "/";

/**
 * Signal handler
 * with out the signal handler the program will exit without stopping the bus when kill signal is received.  (i.e. [Ctrl + c] is pressed)
 * not using this may result in a memory leak if [ctrl + c] is used to end this program.
 */
static void SigIntHandler(int sig) {
    if (NULL != g_msgBus) {
        QStatus status = g_msgBus->Stop(false);
        if (ER_OK != status) {
            printf("BusAttachment::Stop() failed\n");
        }
    }
}

class BasicSampleObject : public BusObject {
  public:
    BasicSampleObject(BusAttachment& bus, const char* path) :
        BusObject(bus, path),
        nameChangedMember(NULL),
        prop_name("Default name")
    {
        /* Add org.alljoyn.Bus.signal_sample interface */
        InterfaceDescription* intf = NULL;
        QStatus status = bus.CreateInterface(SERVICE_NAME, intf);
        if (status == ER_OK) {
            intf->AddSignal("nameChanged", "s", "newName", 0);
            intf->AddProperty("name", "s", PROP_ACCESS_RW);
            intf->Activate();
        } else {
            printf("Failed to create interface %s\n", SERVICE_NAME);
        }

        status = AddInterface(*intf);

        if (status == ER_OK) {
            /* Register the signal handler 'nameChanged' with the bus*/
            nameChangedMember = intf->GetMember("nameChanged");
            assert(nameChangedMember);
        } else {
            printf("Failed to Add interface: %s", SERVICE_NAME);
        }
    }

    QStatus EmitNameChangedSignal(qcc::String newName)
    {
        printf("Emiting Name Changed Signal.\n");
        assert(nameChangedMember);
        MsgArg arg("s", newName.c_str());
        QStatus status = Signal(NULL, 0, *nameChangedMember, &arg, 1, 0, ALLJOYN_FLAG_GLOBAL_BROADCAST);
        return status;
    }

    /*
     * override virtual function from base BusObject class
     * this method will call the 'RequestName' from the daemon.
     * If successful this will register the name 'com.sample.test'
     * as the well-known name of this service.
     */
    void ObjectRegistered(void)
    {
        BusObject::ObjectRegistered();

        /* Request a well-known name */
        /* Note that you cannot make a blocking method call here */
        const ProxyBusObject& dbusObj = bus.GetDBusProxyObj();
        MsgArg args[2];
        args[0].Set("s", SERVICE_NAME);
        /* (DBUS_NAME_FLAG_REPLACE_EXISTING | DBUS_NAME_FLAG_DO_NOT_QUEUE) = 6 */
        args[1].Set("u", 6);
        QStatus status = dbusObj.MethodCallAsync("org.freedesktop.DBus",
                                                 "RequestName",
                                                 this,
                                                 static_cast<MessageReceiver::ReplyHandler> (&BasicSampleObject::RequestNameCB),
                                                 args, sizeof(args) / sizeof(args[0]));
        if (ER_OK != status) {
            printf("Failed to request name %s", SERVICE_NAME);
        }
    }

    /* The return value for the 'RequestName' call will be checked by this function
     * the ObjectRegistered function specified that this function should be used
     * @see { ObjectRegistered() } */
    void RequestNameCB(Message& msg, void* context)
    {
        if (msg->GetArg(0)->v_uint32 == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
            printf("Obtained the well-known name: %s\n", SERVICE_NAME);
            /* Begin Advertising the well known name to remote busses */
            const ProxyBusObject& proxyBusObj = bus.GetAllJoynProxyObj();
            MsgArg arg("s", SERVICE_NAME);
            QStatus status = proxyBusObj.MethodCallAsync(ajn::org::alljoyn::Bus::InterfaceName,
                                                         "AdvertiseName",
                                                         this,
                                                         static_cast<MessageReceiver::ReplyHandler>(&BasicSampleObject::AdvertiseRequestCB),
                                                         &arg,
                                                         1);
            if (ER_OK != status) {
                printf("Sending org.alljoyn.Bus.Advertise failed\n");
            }
        } else {
            printf("Failed to request interface name '%s'\n", SERVICE_NAME);
            exit(1);
        }
    }

    void AdvertiseRequestCB(Message& msg, void* context)
    {
        /* Make sure request was processed */
        size_t numArgs;
        const MsgArg* args;
        msg->GetArgs(numArgs, args);

        if ((MESSAGE_METHOD_RET != msg->GetType()) || (ALLJOYN_ADVERTISENAME_REPLY_SUCCESS != args[0].v_uint32)) {
            printf("Failed to advertise name \"%s\". org.alljoyn.Bus.Advertise returned %d\n",
                   SERVICE_NAME,
                   args[0].v_uint32);
        } else {
            printf("Advertising the well-known name: %s\n", SERVICE_NAME);
        }
    }

    QStatus Get(const char* ifcName, const char* propName, MsgArg& val)
    {
        printf("Get 'name' property was called returning: %s\n", prop_name.c_str());
        QStatus status = ER_OK;
        if (0 == strcmp("name", propName)) {
            val.typeId = ALLJOYN_STRING;
            val.v_string.str = prop_name.c_str();
            val.v_string.len = prop_name.length();
        } else {
            status = ER_BUS_NO_SUCH_PROPERTY;
        }
        return status;
    }

    QStatus Set(const char* ifcName, const char* propName, MsgArg& val)
    {
        QStatus status = ER_OK;
        if ((0 == strcmp("name", propName)) && (val.typeId == ALLJOYN_STRING)) {
            printf("Set 'name' property was called changing name to %s\n", val.v_string.str);
            prop_name = val.v_string.str;
            EmitNameChangedSignal(prop_name);
        } else {
            status = ER_BUS_NO_SUCH_PROPERTY;
        }
        return status;
    }
  private:
    const InterfaceDescription::Member* nameChangedMember;
    qcc::String prop_name;
};

/** Main entry point */
int main(int argc, char** argv, char** envArg) {
    QStatus status = ER_OK;

    printf("AllJoyn Library version: %s\n", ajn::GetVersion());

    /* Install SIGINT handler */
    signal(SIGINT, SigIntHandler);

    /* Create message bus */
    g_msgBus = new BusAttachment("myApp", true);

#ifdef _WIN32
    qcc::String connectArgs = "tcp:addr=127.0.0.1,port=9955";
#else
    qcc::String connectArgs = "unix:abstract=alljoyn";
#endif

    /* Start the msg bus */
    status = g_msgBus->Start();
    if (ER_OK == status) {

        /* Register objects */
        BasicSampleObject testObj(*g_msgBus, SERVICE_PATH);
        g_msgBus->RegisterBusObject(testObj);

        /* Create the client-side endpoint */
        status = g_msgBus->Connect(connectArgs.c_str());
        if (ER_OK != status) {
            printf("Failed to connect to \"%s\"\n", connectArgs.c_str());
        }

        if (ER_OK == status) {
            /*
             * Wait until bus is stopped
             */
            g_msgBus->WaitStop();
        }
    } else {
        printf("BusAttachment::Start failed\n");
    }
    /* Clean up msg bus */
    if (g_msgBus) {
        BusAttachment* deleteMe = g_msgBus;
        g_msgBus = NULL;
        delete deleteMe;
    }
    return (int) status;
}
