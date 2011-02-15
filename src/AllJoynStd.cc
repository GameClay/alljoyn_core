/**
 * @file
 *
 * This file provides definitions for standard AllJoyn interfaces
 *
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
#include <qcc/Debug.h>

#include <alljoyn/BusAttachment.h>
#include <alljoyn/AllJoynStd.h>
#include <alljoyn/InterfaceDescription.h>

#define QCC_MODULE  "ALLJOYN"

namespace ajn {

/** org.alljoyn.Bus interface definitions */
const char* org::alljoyn::Bus::ErrorName = "org.alljoyn.Bus.ErStatus";
const char* org::alljoyn::Bus::ObjectPath = "/org/alljoyn/Bus";
const char* org::alljoyn::Bus::InterfaceName = "org.alljoyn.Bus";
const char* org::alljoyn::Bus::WellKnownName = "org.alljoyn.Bus";
const char* org::alljoyn::Bus::Peer::ObjectPath = "/org/alljoyn/Bus/Peer";

/** org.alljoyn.Daemon interface definitions */
const char* org::alljoyn::Daemon::ErrorName = "org.alljoyn.Daemon.ErStatus";
const char* org::alljoyn::Daemon::ObjectPath = "/org/alljoyn/Bus";
const char* org::alljoyn::Daemon::InterfaceName = "org.alljoyn.Daemon";
const char* org::alljoyn::Daemon::WellKnownName = "org.alljoyn.Daemon";

/** org.alljoyn.Bus.Peer.* interface definitions */
const char* org::alljoyn::Bus::Peer::HeaderCompression::InterfaceName = "org.alljoyn.Bus.Peer.HeaderCompression";
const char* org::alljoyn::Bus::Peer::Authentication::InterfaceName = "org.alljoyn.Bus.Peer.Authentication";
const char* org::alljoyn::Bus::Peer::Session::InterfaceName = "org.alljoyn.Bus.Peer.Session";


QStatus org::alljoyn::CreateInterfaces(BusAttachment& bus)
{
    QStatus status;
    {
        /* Create the org.alljoyn.Bus interface */
        InterfaceDescription* ifc = NULL;
        status = bus.CreateInterface(org::alljoyn::Bus::InterfaceName, ifc);

        if (ER_OK != status) {
            QCC_LogError(status, ("Failed to create interface \"%s\"", org::alljoyn::Bus::InterfaceName));
            return status;
        }
        ifc->AddMethod("BusHello",                 "su",           "ssu",           "GUIDC,protoVerC,GUIDS,uniqueName,protoVerS", 0);
        ifc->AddMethod("CreateSession",            "s"QOSINFO_SIG, "uu",            "sessionName,qos,disposition,sessionId",      0);
        ifc->AddMethod("JoinSession",              "s"QOSINFO_SIG, "uu"QOSINFO_SIG, "sName,qos,disp,sessionId,qos",               0);
        ifc->AddMethod("LeaveSession",             "u",            "u",             "sessionId,disposition",                      0);
        ifc->AddMethod("AdvertiseName",            "s",            "u",             "name,disposition",                           0);
        ifc->AddMethod("CancelAdvertiseName",      "s",            "u",             "name,disposition",                           0);
        ifc->AddMethod("FindAdvertisedName",       "s",            "u",             "name,disposition",                           0);
        ifc->AddMethod("CancelFindAdvertisedName", "s",            "u",             "name,disposition",                           0);
        ifc->AddMethod("GetSessionFd",             "u",            "h",             "sessionId,handle",                           0);

        ifc->AddSignal("FoundAdvertisedName",      "s"QOSINFO_SIG"s",         "name,qos,prefix",                              0);
        ifc->AddSignal("LostAdvertisedName",       "s"QOSINFO_SIG"s",         "name,qos,prefix",                              0);
        ifc->AddSignal("BusConnectionLost",        "s",                       "busName",                                      0);

        ifc->Activate();
    }

    {
        /* Create the org.alljoyn.Daemon interface */
        InterfaceDescription* ifc = NULL;
        status = bus.CreateInterface(org::alljoyn::Daemon::InterfaceName, ifc);

        if (ER_OK != status) {
            QCC_LogError(status, ("Failed to create interface \"%s\"", org::alljoyn::Daemon::InterfaceName));
            return status;
        }
        ifc->AddMethod("AttachSession",  "ssss"QOSINFO_SIG, "uu"QOSINFO_SIG, "name,joiner,creator,b2b,qosIn,status,id,qosOut", 0);
        ifc->AddSignal("DetachSession",  "us",        "sessionId,joiner",                                          0);
        ifc->AddSignal("ExchangeNames",  "a(sas)",    "uniqueName,aliases",                                        0);
        ifc->AddSignal("NameChanged",    "sss",       "name,oldOwner,newOwner",                                    0);
        ifc->Activate();
    }
    {
        /* Create the org.alljoyn.Bus.Peer.HeaderCompression interface */
        InterfaceDescription* ifc = NULL;
        status = bus.CreateInterface(org::alljoyn::Bus::Peer::HeaderCompression::InterfaceName, ifc);
        if (ER_OK != status) {
            QCC_LogError(status, ("Failed to create %s interface", org::alljoyn::Bus::Peer::HeaderCompression::InterfaceName));
            return status;
        }
        ifc->AddMethod("GetExpansion", "u", "a(yv)", "token,headerFields");
        ifc->Activate();
    }
    {
        /* Create the org.alljoyn.Bus.Peer.Authentication interface */
        InterfaceDescription* ifc = NULL;
        status = bus.CreateInterface(org::alljoyn::Bus::Peer::Authentication::InterfaceName, ifc);
        if (ER_OK != status) {
            QCC_LogError(status, ("Failed to create %s interface", org::alljoyn::Bus::Peer::Authentication::InterfaceName));
            return status;
        }
        ifc->AddMethod("ExchangeGuids",     "s",   "s",  "localGuid,remoteGuid");
        ifc->AddMethod("GenSessionKey",     "sss", "ss", "localGuid,remoteGuid,localNonce,remoteNonce,verifier");
        ifc->AddMethod("ExchangeGroupKeys", "ay",  "ay", "localKeyMatter,remoteKeyMatter");
        ifc->AddMethod("AuthChallenge",     "s",   "s",  "challenge,response");
        ifc->AddProperty("Mechanisms",  "s", PROP_ACCESS_READ);
        ifc->Activate();
    }
    {
        /* Create the org.alljoyn.Bus.Peer.Session interface */
        InterfaceDescription* ifc = NULL;
        status = bus.CreateInterface(org::alljoyn::Bus::Peer::Session::InterfaceName, ifc);
        if (ER_OK != status) {
            QCC_LogError(status, ("Failed to create %s interface", org::alljoyn::Bus::Peer::Session::InterfaceName));
            return status;
        }
        ifc->AddMethod("AcceptSession",     "suss"QOSINFO_SIG, "b",  "name,id,src,dest,qos,accepted");
        ifc->Activate();
    }
    return status;
}


}


