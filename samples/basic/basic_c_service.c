/**
 * @file
 * @brief Sample implementation of an AllJoyn service in C.
 *
 * This sample will show how to set up an AllJoyn service that will registered with the
 * wellknown name 'org.alljoyn.Bus.method_sample'.  The service will register a method call
 * with the name 'cat'  this method will take two input strings and return a
 * Concatenated version of the two strings.
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

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <alljoyn/BusAttachment.h>
#include <alljoyn/DBusStd.h>
#include <alljoyn/AllJoynStd.h>
#include <alljoyn/BusObject.h>
#include <alljoyn/MsgArg.h>
#include <alljoyn/version.h>

#include <Status.h>

/** Static top level message bus object */
static alljoyn_busattachment g_msgBus = NULL;

static alljoyn_buslistener s_busListener = NULL;
static alljoyn_sessionportlistener s_sessionPortListener = NULL;

/* Static BusListener */
static alljoyn_buslistener g_busListener = NULL;

/*constants*/
static const char* INTERFACE_NAME = "org.alljoyn.Bus.method_sample";
static const char* SERVICE_NAME = "org.alljoyn.Bus.method_sample";
static const char* SERVICE_PATH = "/method_sample";
static const alljoyn_sessionport SERVICE_PORT = 25;

/** Signal handler
 * with out the signal handler the program will exit without stoping the bus
 * when kill signal is received.  (i.e. [Ctrl + c] is pressed) not using this
 * may result in a memory leak if [cont + c] is used to end this program.
 */
static void SigIntHandler(int sig)
{
    if (NULL != g_msgBus) {
        QStatus status = alljoyn_busattachment_stop(g_msgBus, QC_FALSE);
        if (ER_OK != status) {
            printf("BusAttachment::Stop() failed\n");
        }
    }
}
#if 0
class BasicSampleObject : public BusObject {
  public:
    BasicSampleObject(BusAttachment & bus, const char* path) : BusObject(bus, path)
    {
        /** Add the test interface to this object */
        const InterfaceDescription* exampleIntf = bus.GetInterface(INTERFACE_NAME);
        assert(exampleIntf);
        AddInterface(*exampleIntf);

        /** Register the method handlers with the object */
        const MethodEntry methodEntries[] = {
            { exampleIntf->GetMember("cat"), static_cast<MessageReceiver::MethodHandler>(&BasicSampleObject::Cat) }
        };
        QStatus status = AddMethodHandlers(methodEntries, sizeof(methodEntries) / sizeof(methodEntries[0]));
        if (ER_OK != status) {
            printf("Failed to register method handlers for BasicSampleObject");
        }
    }

    void ObjectRegistered()
    {
        BusObject::ObjectRegistered();
        printf("ObjectRegistered has been called\n");
    }


    void Cat(const InterfaceDescription::Member* member, Message& msg)
    {
        /* Concatenate the two input strings and reply with the result. */
        qcc::String inStr1 = msg->GetArg(0)->v_string.str;
        qcc::String inStr2 = msg->GetArg(1)->v_string.str;
        qcc::String outStr = inStr1 + inStr2;

        MsgArg outArg("s", outStr.c_str());
        QStatus status = MethodReply(msg, &outArg, 1);
        if (ER_OK != status) {
            printf("Ping: Error sending reply\n");
        }
    }
};
#endif

/* NameOwnerChanged callback */
void name_owner_changed(const void* context, const char* busName, const char* previousOwner, const char* newOwner)
{
    if (newOwner && (0 == strcmp(busName, SERVICE_NAME))) {
        printf("NameOwnerChanged: name=%s, oldOwner=%s, newOwner=%s\n",
               busName,
               previousOwner ? previousOwner : "<none>",
               newOwner ? newOwner : "<none>");
    }
}

/* AcceptSessionJoiner callback */
QC_BOOL accept_session_joiner(const void* context, alljoyn_sessionport sessionPort,
                              const char* joiner,  alljoyn_sessionopts_const opts)
{
    QC_BOOL ret = QC_FALSE;
    if (sessionPort != SERVICE_PORT) {
        printf("Rejecting join attempt on unexpected session port %d\n", sessionPort);
    } else {
        printf("Accepting join session request from %s (opts.proximity=%x, opts.traffic=%x, opts.transports=%x)\n",
               joiner, alljoyn_sessionopts_proximity(opts), alljoyn_sessionopts_traffic(opts), alljoyn_sessionopts_transports(opts));
        ret = QC_TRUE;
    }
    return ret;
}

/** Main entry point */
int main(int argc, char** argv, char** envArg)
{
    QStatus status = ER_OK;

    printf("AllJoyn Library version: %s\n", alljoyn_getversion());
    printf("AllJoyn Library build info: %s\n", alljoyn_getbuildinfo());

    /* Install SIGINT handler */
    signal(SIGINT, SigIntHandler);

    const char* connectArgs = getenv("BUS_ADDRESS");
    if (connectArgs == NULL) {
#ifdef _WIN32
        connectArgs = "tcp:addr=127.0.0.1,port=9955";
#else
        connectArgs = "unix:abstract=alljoyn";
#endif
    }

    /* Create message bus */
    g_msgBus = alljoyn_busattachment_create("myApp", QC_TRUE);

    /* Add org.alljoyn.Bus.method_sample interface */
    alljoyn_interfacedescription testIntf = NULL;
    status = alljoyn_busattachment_createinterface(g_msgBus, INTERFACE_NAME, &testIntf, QC_FALSE);
    if (status == ER_OK) {
        printf("Interface Created.\n");
        alljoyn_interfacedescription_addmethod(testIntf, "cat", "ss",  "s", "inStr1,inStr2,outStr", 0);
        alljoyn_interfacedescription_activate(testIntf);
    } else {
        printf("Failed to create interface 'org.alljoyn.Bus.method_sample'\n");
    }

    /* Register a bus listener */
    if (ER_OK == status) {
        /* Create a bus listener */
        alljoyn_buslistener_callbacks callbacks = {
            NULL,
            NULL,
            NULL,
            NULL,
            &name_owner_changed,
            NULL,
            NULL
        };
        g_busListener = alljoyn_buslistener_create(&callbacks, NULL);
        alljoyn_busattachment_registerbuslistener(g_msgBus, g_busListener);
    }

    /* Set up bus object */
    alljoyn_interfacedescription_const exampleIntf = alljoyn_busattachment_getinterface(g_msgBus, INTERFACE_NAME);
    assert(exampleIntf);

#if 0
    BasicSampleObject testObj(*g_msgBus, SERVICE_PATH);

    /* Start the msg bus */
    status = g_msgBus->Start();
    if (ER_OK == status) {
        printf("BusAttachement started.\n");
        /* Register  local objects and connect to the daemon */
        g_msgBus->RegisterBusObject(testObj);

        /* Create the client-side endpoint */
        status = g_msgBus->Connect(connectArgs);
        if (ER_OK != status) {
            printf("Failed to connect to \"%s\"\n", connectArgs);
            exit(1);
        } else {
            printf("Connected to '%s'\n", connectArgs);
        }
    } else {
        printf("BusAttachment::Start failed\n");
    }

    /*
     * Advertise this service on the bus
     * There are three steps to advertising this service on the bus
     * 1) Request a well-known name that will be used by the client to discover
     *    this service
     * 2) Create a session
     * 3) Advertise the well-known name
     */
    /* Request name */
    if (ER_OK == status) {
        uint32_t flags = DBUS_NAME_FLAG_REPLACE_EXISTING | DBUS_NAME_FLAG_DO_NOT_QUEUE;
        QStatus status = g_msgBus->RequestName(SERVICE_NAME, flags);
        if (ER_OK != status) {
            printf("RequestName(%s) failed (status=%s)\n", SERVICE_NAME, QCC_StatusText(status));
        }
    }

    /* Create session */
    SessionOpts opts(SessionOpts::TRAFFIC_MESSAGES, false, SessionOpts::PROXIMITY_ANY, TRANSPORT_ANY);
    if (ER_OK == status) {
        SessionPort sp = SERVICE_PORT;
        status = g_msgBus->BindSessionPort(sp, opts, *s_busListener);
        if (ER_OK != status) {
            printf("BindSessionPort failed (%s)\n", QCC_StatusText(status));
        }
    }

    /* Advertise name */
    if (ER_OK == status) {
        status = g_msgBus->AdvertiseName(SERVICE_NAME, opts.transports);
        if (status != ER_OK) {
            printf("Failed to advertise name %s (%s)\n", SERVICE_NAME, QCC_StatusText(status));
        }
    }

    if (ER_OK == status) {
        /*
         * Wait until bus is stopped
         */
        g_msgBus->WaitStop();
    }
#endif
    /* Deallocate bus */
    if (g_msgBus) {
        alljoyn_busattachment deleteMe = g_msgBus;
        g_msgBus = NULL;
        alljoyn_busattachment_destroy(deleteMe);
    }

    /* Deallocate bus listener */
    if (g_busListener) {
        alljoyn_buslistener_destroy(g_busListener);
    }

    return (int) status;
}
