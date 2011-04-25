/**
 * @file
 *
 * This file implements the org.alljoyn.Bus interfaces
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

#include <algorithm>
#include <assert.h>
#include <limits>
#include <map>
#include <set>
#include <vector>
#include <errno.h>

#include <qcc/Debug.h>
#include <qcc/ManagedObj.h>
#include <qcc/String.h>
#include <qcc/Thread.h>
#include <qcc/Util.h>

#include <alljoyn/BusAttachment.h>
#include <alljoyn/DBusStd.h>
#include <alljoyn/InterfaceDescription.h>
#include <alljoyn/AllJoynStd.h>
#include <alljoyn/Message.h>
#include <alljoyn/MessageReceiver.h>
#include <alljoyn/ProxyBusObject.h>

#include "DaemonRouter.h"
#include "AllJoynObj.h"
#include "TransportList.h"
#include "BusUtil.h"
#include "SessionInternal.h"

#define QCC_MODULE "ALLJOYN_OBJ"

using namespace std;
using namespace qcc;

namespace ajn {


AllJoynObj::AllJoynObj(Bus& bus) :
    BusObject(bus, org::alljoyn::Bus::ObjectPath, false),
    bus(bus),
    router(reinterpret_cast<DaemonRouter&>(bus.GetInternal().GetRouter())),
    foundNameSignal(NULL),
    lostAdvNameSignal(NULL),
    busConnLostSignal(NULL),
    guid(bus.GetInternal().GetGlobalGUID()),
    exchangeNamesSignal(NULL),
    detachSessionSignal(NULL),
    nameMapReaper(this),
    isStopping(false)
{
}

AllJoynObj::~AllJoynObj()
{
    bus.UnregisterBusObject(*this);

    // TODO: Unregister signal handlers.
    // TODO: Unregister name listeners
    // TODO: Unregister transport listener
    // TODO: Unregister local object

    /* Wait for any outstanding JoinSessionThreads */
    joinSessionThreadsLock.Lock();
    isStopping = true;
    vector<JoinSessionThread*>::iterator it = joinSessionThreads.begin();
    while (it != joinSessionThreads.end()) {
        (*it)->Stop();
        ++it;
    }
    while (!joinSessionThreads.empty()) {
        joinSessionThreadsLock.Unlock();
        qcc::Sleep(50);
        joinSessionThreadsLock.Lock();
    }
    joinSessionThreadsLock.Unlock();
}

QStatus AllJoynObj::Init()
{
    QStatus status;

    /* Make this object implement org.alljoyn.Bus */
    const InterfaceDescription* alljoynIntf = bus.GetInterface(org::alljoyn::Bus::InterfaceName);
    if (!alljoynIntf) {
        status = ER_BUS_NO_SUCH_INTERFACE;
        QCC_LogError(status, ("Failed to get %s interface", org::alljoyn::Bus::InterfaceName));
        return status;
    }

    /* Hook up the methods to their handlers */
    const MethodEntry methodEntries[] = {
        { alljoynIntf->GetMember("AdvertiseName"),            static_cast<MessageReceiver::MethodHandler>(&AllJoynObj::AdvertiseName) },
        { alljoynIntf->GetMember("CancelAdvertiseName"),      static_cast<MessageReceiver::MethodHandler>(&AllJoynObj::CancelAdvertiseName) },
        { alljoynIntf->GetMember("FindAdvertisedName"),       static_cast<MessageReceiver::MethodHandler>(&AllJoynObj::FindAdvertisedName) },
        { alljoynIntf->GetMember("CancelFindAdvertisedName"), static_cast<MessageReceiver::MethodHandler>(&AllJoynObj::CancelFindAdvertisedName) },
        { alljoynIntf->GetMember("BindSessionPort"),          static_cast<MessageReceiver::MethodHandler>(&AllJoynObj::BindSessionPort) },
        { alljoynIntf->GetMember("UnbindSessionPort"),        static_cast<MessageReceiver::MethodHandler>(&AllJoynObj::UnbindSessionPort) },
        { alljoynIntf->GetMember("JoinSession"),              static_cast<MessageReceiver::MethodHandler>(&AllJoynObj::JoinSession) },
        { alljoynIntf->GetMember("LeaveSession"),             static_cast<MessageReceiver::MethodHandler>(&AllJoynObj::LeaveSession) },
        { alljoynIntf->GetMember("GetSessionFd"),             static_cast<MessageReceiver::MethodHandler>(&AllJoynObj::GetSessionFd) }
    };

    AddInterface(*alljoynIntf);
    status = AddMethodHandlers(methodEntries, ArraySize(methodEntries));
    if (ER_OK != status) {
        QCC_LogError(status, ("AddMethods for %s failed", org::alljoyn::Bus::InterfaceName));
    }

    foundNameSignal = alljoynIntf->GetMember("FoundAdvertisedName");
    lostAdvNameSignal = alljoynIntf->GetMember("LostAdvertisedName");
    busConnLostSignal = alljoynIntf->GetMember("BusConnectionLost");

    /* Make this object implement org.alljoyn.Daemon */
    daemonIface = bus.GetInterface(org::alljoyn::Daemon::InterfaceName);
    if (!daemonIface) {
        status = ER_BUS_NO_SUCH_INTERFACE;
        QCC_LogError(status, ("Failed to get %s interface", org::alljoyn::Daemon::InterfaceName));
        return status;
    }

    /* Hook up the methods to their handlers */
    const MethodEntry daemonMethodEntries[] = {
        { daemonIface->GetMember("AttachSession"),     static_cast<MessageReceiver::MethodHandler>(&AllJoynObj::AttachSession) },
        { daemonIface->GetMember("GetSessionInfo"),    static_cast<MessageReceiver::MethodHandler>(&AllJoynObj::GetSessionInfo) }
    };
    AddInterface(*daemonIface);
    status = AddMethodHandlers(daemonMethodEntries, ArraySize(daemonMethodEntries));
    if (ER_OK != status) {
        QCC_LogError(status, ("AddMethods for %s failed", org::alljoyn::Daemon::InterfaceName));
    }

    exchangeNamesSignal = daemonIface->GetMember("ExchangeNames");
    assert(exchangeNamesSignal);
    detachSessionSignal = daemonIface->GetMember("DetachSession");
    assert(detachSessionSignal);

    /* Register a signal handler for ExchangeNames */
    if (ER_OK == status) {
        status = bus.RegisterSignalHandler(this,
                                           static_cast<MessageReceiver::SignalHandler>(&AllJoynObj::ExchangeNamesSignalHandler),
                                           daemonIface->GetMember("ExchangeNames"),
                                           NULL);
    } else {
        QCC_LogError(status, ("Failed to register ExchangeNamesSignalHandler"));
    }

    /* Register a signal handler for NameChanged bus-to-bus signal */
    if (ER_OK == status) {
        status = bus.RegisterSignalHandler(this,
                                           static_cast<MessageReceiver::SignalHandler>(&AllJoynObj::NameChangedSignalHandler),
                                           daemonIface->GetMember("NameChanged"),
                                           NULL);
    } else {
        QCC_LogError(status, ("Failed to register NameChangedSignalHandler"));
    }

    /* Register a signal handler for DetachSession bus-to-bus signal */
    if (ER_OK == status) {
        status = bus.RegisterSignalHandler(this,
                                           static_cast<MessageReceiver::SignalHandler>(&AllJoynObj::DetachSessionSignalHandler),
                                           daemonIface->GetMember("DetachSession"),
                                           NULL);
    } else {
        QCC_LogError(status, ("Failed to register DetachSessionSignalHandler"));
    }

    /* Register a name table listener */
    router.AddBusNameListener(this);

    /* Register as a listener for all the remote transports */
    if (ER_OK == status) {
        TransportList& transList = bus.GetInternal().GetTransportList();
        status = transList.RegisterListener(this);
    }

    /* Start the name reaper */
    if (ER_OK == status) {
        status = nameMapReaper.Start();
    }

    if (ER_OK == status) {
        status = bus.RegisterBusObject(*this);
    }

    return status;
}

void AllJoynObj::ObjectRegistered(void)
{
    QStatus status;

    /* Must call base class */
    BusObject::ObjectRegistered();

    /* Acquire org.alljoyn.Bus name */
    uint32_t disposition = DBUS_REQUEST_NAME_REPLY_EXISTS;
    status = router.AddAlias(org::alljoyn::Bus::WellKnownName,
                             bus.GetInternal().GetLocalEndpoint().GetUniqueName(),
                             DBUS_NAME_FLAG_DO_NOT_QUEUE,
                             disposition,
                             NULL,
                             NULL);
    if ((ER_OK != status) || (DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER != disposition)) {
        status = (ER_OK == status) ? ER_FAIL : status;
        QCC_LogError(status, ("Failed to register well-known name \"%s\" (disposition=%d)", org::alljoyn::Bus::WellKnownName, disposition));
    }

    /* Acquire org.alljoyn.Daemon name */
    disposition = DBUS_REQUEST_NAME_REPLY_EXISTS;
    status = router.AddAlias(org::alljoyn::Daemon::WellKnownName,
                             bus.GetInternal().GetLocalEndpoint().GetUniqueName(),
                             DBUS_NAME_FLAG_DO_NOT_QUEUE,
                             disposition,
                             NULL,
                             NULL);
    if ((ER_OK != status) || (DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER != disposition)) {
        status = (ER_OK == status) ? ER_FAIL : status;
        QCC_LogError(status, ("Failed to register well-known name \"%s\" (disposition=%d)", org::alljoyn::Daemon::WellKnownName, disposition));
    }
}

void AllJoynObj::BindSessionPort(const InterfaceDescription::Member* member, Message& msg)
{
    uint32_t replyCode = ALLJOYN_BINDSESSIONPORT_REPLY_SUCCESS;
    size_t numArgs;
    const MsgArg* args;
    SessionOpts opts;

    msg->GetArgs(numArgs, args);
    SessionPort sessionPort = args[0].v_uint32;
    QStatus status = GetSessionOpts(args[1], opts);

    /* Get the sender */
    String sender = msg->GetSender();

    if (status != ER_OK) {
        QCC_DbgTrace(("AllJoynObj::BindSessionPort(<bad args>) from %s", sender.c_str()));
        replyCode = ALLJOYN_BINDSESSIONPORT_REPLY_FAILED;
    } else {
        QCC_DbgTrace(("AllJoynObj::BindSession(%s, %d, %s, <%x, %x, %x>)", sender.c_str(), sessionPort,
                      opts.isMultipoint ? "true" : "false", opts.traffic, opts.proximity, opts.transports));

        /* Assign or check uniqueness of sessionPort */
        sessionMapLock.Lock();
        if (sessionPort == SESSION_PORT_ANY) {
            sessionPort = 9999;
            while (++sessionPort) {
                map<pair<String, SessionId>, SessionMapEntry>::const_iterator it = sessionMap.lower_bound(pair<String, SessionId>(sender, 0));
                while ((it != sessionMap.end()) && (it->first.first == sender)) {
                    if (it->second.sessionPort == sessionPort) {
                        break;
                    }
                    ++it;
                }
                /* If no existing sessionMapEntry for sessionPort, then we are done */
                if ((it == sessionMap.end()) || (it->first.first != sender)) {
                    break;
                }
            }
            if (sessionPort == 0) {
                replyCode = ALLJOYN_BINDSESSIONPORT_REPLY_FAILED;
            }
        } else {
            map<pair<String, SessionId>, SessionMapEntry>::const_iterator it = sessionMap.lower_bound(pair<String, SessionId>(sender, 0));
            while ((it != sessionMap.end()) && (it->first.first == sender)) {
                if (it->second.sessionPort == sessionPort) {
                    replyCode = ALLJOYN_BINDSESSIONPORT_REPLY_ALREADY_EXISTS;
                    break;
                }
                ++it;
            }
        }

        if (replyCode == ALLJOYN_BINDSESSIONPORT_REPLY_SUCCESS) {
            /* Assign a session id and store the session information */
            SessionMapEntry entry;
            entry.sessionHost = sender;
            entry.sessionPort = sessionPort;
            entry.endpointName = sender;
            entry.fd = -1;
            entry.streamingEp = NULL;
            entry.opts = opts;
            do {
                entry.id = qcc::Rand32();
            } while (entry.id == 0);
            sessionMap[pair < String, SessionId > (entry.endpointName, 0)] = entry;
        }
        sessionMapLock.Unlock();
    }

    /* Reply to request */
    MsgArg replyArgs[2];
    replyArgs[0].Set("u", replyCode);
    replyArgs[1].Set("q", sessionPort);
    status = MethodReply(msg, replyArgs, ArraySize(replyArgs));
    QCC_DbgPrintf(("AllJoynObj::BindSessionPort(%s, %d) returned %d (status=%s)", sender.c_str(), sessionPort, replyCode, QCC_StatusText(status)));

    /* Log error if reply could not be sent */
    if (ER_OK != status) {
        QCC_LogError(status, ("Failed to respond to org.alljoyn.Bus.BindSessionPort"));
    }
}

void AllJoynObj::UnbindSessionPort(const InterfaceDescription::Member* member, Message& msg)
{
    uint32_t replyCode = ALLJOYN_UNBINDSESSIONPORT_REPLY_FAILED;
    size_t numArgs;
    const MsgArg* args;
    SessionOpts opts;

    msg->GetArgs(numArgs, args);
    SessionPort sessionPort = args[0].v_uint32;

    QCC_DbgTrace(("AllJoynObj::UnbindSession(%d)", sessionPort));

    /* Remove session map entry */
    String sender = msg->GetSender();
    sessionMapLock.Lock();
    map<pair<String, SessionId>, SessionMapEntry>::iterator it = sessionMap.lower_bound(pair<String, SessionId>(sender, 0));
    while ((it != sessionMap.end()) && (it->first.first == sender) && (it->first.second == 0)) {
        if (it->second.sessionPort == sessionPort) {
            sessionMap.erase(it);
            replyCode = ALLJOYN_UNBINDSESSIONPORT_REPLY_SUCCESS;
            break;
        }
        ++it;
    }
    sessionMapLock.Unlock();

    /* Reply to request */
    MsgArg replyArgs[1];
    replyArgs[0].Set("u", replyCode);
    QStatus status = MethodReply(msg, replyArgs, ArraySize(replyArgs));
    QCC_DbgPrintf(("AllJoynObj::UnbindSessionPort(%s, %d) returned %d (status=%s)", sender.c_str(), sessionPort, replyCode, QCC_StatusText(status)));

    /* Log error if reply could not be sent */
    if (ER_OK != status) {
        QCC_LogError(status, ("Failed to respond to org.alljoyn.Bus.UnbindSessionPort"));
    }
}

ThreadReturn STDCALL AllJoynObj::JoinSessionThread::Run(void* arg)
{
    uint32_t replyCode = ALLJOYN_JOINSESSION_REPLY_SUCCESS;
    SessionId id = 0;
    SessionOpts optsOut(SessionOpts::TRAFFIC_MESSAGES, false, SessionOpts::PROXIMITY_ANY, 0);
    size_t numArgs;
    const MsgArg* args;
    SessionMapEntry sme;

    /* Parse the message args */
    msg->GetArgs(numArgs, args);
    const char* sessionHost = NULL;
    SessionPort sessionPort;
    SessionOpts optsIn;
    QStatus status = MsgArg::Get(args, 2, "sq", &sessionHost, &sessionPort);
    if (status == ER_OK) {
        status = GetSessionOpts(args[2], optsIn);
    }

    if (status != ER_OK) {
        replyCode = ALLJOYN_JOINSESSION_REPLY_FAILED;
        QCC_DbgTrace(("JoinSession(<bad_args>"));
    } else {
        QCC_DbgTrace(("JoinSession(%d, <%d, 0x%x, 0x%x>)", sessionPort, optsIn.traffic, optsIn.proximity, optsIn.transports));

        /* Decide how to proceed based on the session endpoint existence/type */
        RemoteEndpoint* b2bEp = NULL;
        BusEndpoint* ep = sessionHost ? ajObj.router.FindEndpoint(sessionHost) : NULL;
        VirtualEndpoint* vSessionEp = (ep && (ep->GetEndpointType() == BusEndpoint::ENDPOINT_TYPE_VIRTUAL)) ? static_cast<VirtualEndpoint*>(ep) : NULL;
        RemoteEndpoint* rSessionEp = (ep && (ep->GetEndpointType() == BusEndpoint::ENDPOINT_TYPE_REMOTE)) ? static_cast<RemoteEndpoint*>(ep) : NULL;

        if (rSessionEp) {
            /* Session is with another locally connected attachment */

            /* Find creator in session map */
            ajObj.sessionMapLock.Lock();
            String creatorName = rSessionEp->GetUniqueName();
            bool foundSessionMapEntry = false;
            map<pair<String, SessionId>, SessionMapEntry>::iterator sit = ajObj.sessionMap.lower_bound(pair<String, SessionId>(creatorName, 0));
            while ((sit != ajObj.sessionMap.end()) && (creatorName == sit->first.first) && (sit->first.second == 0)) {
                if (sit->second.sessionPort == sessionPort) {
                    sme = sit->second;
                    foundSessionMapEntry = true;
                    break;
                }
                ++sit;
            }
            ajObj.sessionMapLock.Unlock();

            BusEndpoint* joinerEp = ajObj.router.FindEndpoint(msg->GetSender());
            if (joinerEp && foundSessionMapEntry) {
                bool isAccepted = false;
                SessionId newSessionId = sme.id;
                if (!sme.opts.IsCompatible(optsIn)) {
                    replyCode = ALLJOYN_JOINSESSION_REPLY_BAD_SESSION_OPTS;
                } else {
                    /* Create a new sessionId if this is not a mulit-point session */
                    if (!sme.opts.isMultipoint) {
                        do {
                            newSessionId = qcc::Rand32();
                        } while (newSessionId == 0);
                    }
                    /* Ask creator to accept session */
                    status = ajObj.SendAcceptSession(sme.sessionPort, newSessionId, sessionHost, msg->GetSender(), optsIn, isAccepted);
                    if (status != ER_OK) {
                        QCC_LogError(status, ("SendAcceptSession failed"));
                        replyCode = ALLJOYN_JOINSESSION_REPLY_FAILED;
                    }
                }
                if (replyCode == ALLJOYN_JOINSESSION_REPLY_SUCCESS) {
                    if (!isAccepted) {
                        replyCode = ALLJOYN_JOINSESSION_REPLY_REJECTED;
                    } else if (sme.opts.traffic == SessionOpts::TRAFFIC_MESSAGES) {
                        /* setup the forward and reverse routes through the local daemon */
                        RemoteEndpoint* tEp = NULL;
                        status = ajObj.router.AddSessionRoute(msg->GetSender(), newSessionId, *rSessionEp, tEp);
                        if (status != ER_OK) {
                            replyCode = ALLJOYN_JOINSESSION_REPLY_FAILED;
                            QCC_LogError(status, ("AddSessionRoute %s->%s failed", msg->GetSender(), rSessionEp->GetUniqueName().c_str()));
                        } else {
                            status = ajObj.router.AddSessionRoute(rSessionEp->GetUniqueName().c_str(), newSessionId, *joinerEp, tEp);
                            if (status != ER_OK) {
                                replyCode = ALLJOYN_JOINSESSION_REPLY_FAILED;
                                ajObj.router.RemoveSessionRoute(msg->GetSender(), newSessionId, *rSessionEp);
                                QCC_LogError(status, ("AddSessionRoute %s->%s failed", rSessionEp->GetUniqueName().c_str(), joinerEp->GetUniqueName().c_str()));
                            }
                        }
                        if (status == ER_OK) {
                            ajObj.sessionMapLock.Lock();
                            if (sme.opts.isMultipoint) {
                                /* Add (local) joiner to list of session members since no AttachSession will be sent */
                                map<pair<String, SessionId>, SessionMapEntry>::const_iterator sit = ajObj.sessionMap.find(pair<String, SessionId>(sme.endpointName, sme.id));
                                if (sit == ajObj.sessionMap.end()) {
                                    sit = ajObj.sessionMap.find(pair<String, SessionId>(sme.endpointName, 0));
                                }
                                if (sit != ajObj.sessionMap.end()) {
                                    sme = sit->second;
                                }
                                sme.memberNames.push_back(msg->GetSender());
                            } else {
                                /* Add the creator-side entry in sessionMap if session is not multiPoint */
                                sme.id = newSessionId;
                            }
                            ajObj.sessionMap[pair < String, SessionId > (sme.endpointName, sme.id)] = sme;

                            /* Create a joiner side entry in sessionMap */
                            SessionMapEntry joinerSme = sme;
                            joinerSme.endpointName = msg->GetSender();
                            joinerSme.id = newSessionId;
                            ajObj.sessionMap[pair < String, SessionId > (joinerSme.endpointName, joinerSme.id)] = joinerSme;
                            ajObj.sessionMapLock.Unlock();
                            id = joinerSme.id;
                            optsOut = sme.opts;
                        }
                    } else if ((sme.opts.traffic == SessionOpts::TRAFFIC_RAW_RELIABLE) && !sme.opts.isMultipoint) {
                        /* Create a raw socket pair for the two local  */
                        SocketFd fds[2];
                        status = SocketPair(fds);
                        if (status == ER_OK) {
                            /* Add the creator-side entry in sessionMap */
                            ajObj.sessionMapLock.Lock();
                            SessionMapEntry sme2 = sme;
                            sme2.id = newSessionId;
                            sme2.fd = fds[0];
                            ajObj.sessionMap[pair < String, SessionId > (sme2.endpointName, sme2.id)] = sme2;

                            /* Create a joiner side entry in sessionMap */
                            sme2.endpointName = msg->GetSender();
                            sme2.fd = fds[1];
                            ajObj.sessionMap[pair < String, SessionId > (sme2.endpointName, sme2.id)] = sme2;
                            ajObj.sessionMapLock.Unlock();
                            id = sme2.id;
                            optsOut = sme.opts;
                        } else {
                            QCC_LogError(status, ("SocketPair failed"));
                            replyCode = ALLJOYN_JOINSESSION_REPLY_FAILED;
                        }
                    } else {
                        /* QosInfo::TRAFFIC_RAW_UNRELIABLE is not currently supported */
                        replyCode = ALLJOYN_JOINSESSION_REPLY_BAD_SESSION_OPTS;
                    }
                }
            } else {
                replyCode = ALLJOYN_JOINSESSION_REPLY_NO_SESSION;
            }
        } else {
            /* Session is with a connected or unconnected remote device */
            MsgArg membersArg;

            /* Step 1: If there is a busAddr from advertsement use it to (possibly) create a physical connection */
            ajObj.router.LockNameTable();
            ajObj.discoverMapLock.Lock();
            ajObj.virtualEndpointsLock.Lock();
            String busAddr;
            multimap<String, NameMapEntry>::iterator nmit = ajObj.nameMap.lower_bound(sessionHost);
            while (nmit != ajObj.nameMap.end() && (nmit->first == sessionHost)) {
                if (nmit->second.transport & optsIn.transports) {
                    busAddr = nmit->second.busAddr;
                    break;
                }
                ++nmit;
            }
            ajObj.virtualEndpointsLock.Unlock();
            ajObj.discoverMapLock.Unlock();
            ajObj.router.UnlockNameTable();

#if 0
            /*
             * Step 1b: If no advertisment (busAddr) and we are connected to the sesionHost, then ask it directly
             * for the busAddr
             */
            if (vSessionEp && busAddr.empty()) {
                status = ajObj.SendGetSessionInfo(sessionHost, sessionPort, optsIn, busAddr);
                if (status != ER_OK) {
                    busAddr.clear();
                    QCC_LogError(status, ("GetSessionInfo failed"));
                }
            }
#endif
            String b2bEpName;
            if (!busAddr.empty()) {
                /* Ask the transport that provided the advertisement for an endpoint */
                TransportList& transList = ajObj.bus.GetInternal().GetTransportList();
                Transport* trans = transList.GetTransport(busAddr);
                if (trans == NULL) {
                    replyCode = ALLJOYN_JOINSESSION_REPLY_UNREACHABLE;
                } else {
                    status = trans->Connect(busAddr.c_str(), &b2bEp);
                    if (status == ER_OK) {
                        b2bEpName = b2bEp->GetUniqueName();
                    } else {
                        QCC_LogError(status, ("trans->Connect(%s) failed", busAddr.c_str()));
                        replyCode = ALLJOYN_JOINSESSION_REPLY_CONNECT_FAILED;
                    }
                }
            } else {
                /* No advertisment or existing route to session creator */
                replyCode = ALLJOYN_JOINSESSION_REPLY_NO_SESSION;
            }

            /* Step 2: Send a session attach */
            if (replyCode == ALLJOYN_JOINSESSION_REPLY_SUCCESS) {
                const String nextControllerName = b2bEp->GetRemoteName();
                status = ajObj.SendAttachSession(sessionPort, msg->GetSender(), sessionHost, b2bEpName.c_str(),
                                                 nextControllerName.c_str(), busAddr.c_str(), optsIn, replyCode,
                                                 id, optsOut, membersArg);
                if (status != ER_OK) {
                    QCC_LogError(status, ("AttachSession to %s failed", nextControllerName.c_str()));
                    replyCode = ALLJOYN_JOINSESSION_REPLY_FAILED;
                }
            }

            /* Step 3: Wait for the new b2b endpoint to have a virtual ep for our destination */
            uint32_t startTime = GetTimestamp();
            ajObj.router.LockNameTable();
            ajObj.discoverMapLock.Lock();
            ajObj.virtualEndpointsLock.Lock();
            b2bEp = static_cast<RemoteEndpoint*>(ajObj.router.FindEndpoint(b2bEpName));
            while (replyCode == ALLJOYN_JOINSESSION_REPLY_SUCCESS) {
                /* Does vSessionEp route through b2bEp? If so, we're done */
                ep = ajObj.router.FindEndpoint(sessionHost);
                vSessionEp = (ep && (ep->GetEndpointType() == BusEndpoint::ENDPOINT_TYPE_VIRTUAL)) ? static_cast<VirtualEndpoint*>(ep) : NULL;
                if (!b2bEp) {
                    QCC_LogError(ER_FAIL, ("B2B endpoint disappeared during JoinSession"));
                    replyCode = ALLJOYN_JOINSESSION_REPLY_FAILED;
                    break;
                } else if (vSessionEp && vSessionEp->CanUseRoute(*b2bEp)) {
                    break;
                }

                /* Otherwise wait */
                uint32_t now = GetTimestamp();
                if (now > (startTime + 10000)) {
                    replyCode = ALLJOYN_JOINSESSION_REPLY_FAILED;
                    QCC_LogError(ER_FAIL, ("JoinSession timed out waiting for destination to appear"));
                    break;
                } else {
                    /* Give up the locks while waiting */
                    ajObj.virtualEndpointsLock.Unlock();
                    ajObj.discoverMapLock.Unlock();
                    ajObj.router.UnlockNameTable();
                    qcc::Sleep(10);
                    ajObj.router.LockNameTable();
                    ajObj.discoverMapLock.Lock();
                    ajObj.virtualEndpointsLock.Lock();

                    /* Re-acquire b2bEp now that we have the lock again */
                    b2bEp = static_cast<RemoteEndpoint*>(ajObj.router.FindEndpoint(b2bEpName));
                }
            }

            /* If session was successful, Add two-way session routes to the table */
            if (replyCode == ALLJOYN_JOINSESSION_REPLY_SUCCESS) {
                BusEndpoint* joinerEp = ajObj.router.FindEndpoint(msg->GetSender());
                if (joinerEp) {
                    status = ajObj.router.AddSessionRoute(msg->GetSender(), id, *vSessionEp, b2bEp, b2bEp ? NULL : &optsOut);
                    if (status == ER_OK) {
                        RemoteEndpoint* tEp = NULL;
                        status = ajObj.router.AddSessionRoute(vSessionEp->GetUniqueName().c_str(), id, *joinerEp, tEp);
                        if (status != ER_OK) {
                            ajObj.router.RemoveSessionRoute(msg->GetSender(), id, *vSessionEp);
                            replyCode = ALLJOYN_JOINSESSION_REPLY_FAILED;
                            QCC_LogError(status, ("AddSessionRoute %s->%s failed", vSessionEp->GetUniqueName().c_str(), joinerEp->GetUniqueName().c_str()));
                        }
                    } else {
                        replyCode = ALLJOYN_JOINSESSION_REPLY_FAILED;
                        QCC_LogError(status, ("AddSessionRoute %s->%s failed", msg->GetSender(), vSessionEp->GetUniqueName().c_str()));
                    }
                } else {
                    replyCode = ALLJOYN_JOINSESSION_REPLY_FAILED;
                    QCC_LogError(ER_BUS_NO_ENDPOINT, ("Cannot find joiner endpoint %s", msg->GetSender()));
                }
            }

            /* Create session map entry */
            pair<String, SessionId> key(msg->GetSender(), id);
            bool sessionMapEntryCreated = false;
            if (replyCode == ALLJOYN_JOINSESSION_REPLY_SUCCESS) {
                const MsgArg* sessionMembers;
                size_t numSessionMembers = 0;
                membersArg.Get("as", &numSessionMembers, &sessionMembers);
                ajObj.sessionMapLock.Lock();
                sme.endpointName = msg->GetSender();
                sme.id = id;
                sme.sessionHost = sessionHost;
                sme.sessionPort = sessionPort;
                sme.opts = optsOut;
                for (size_t i = 0; i < numSessionMembers; ++i) {
                    sme.memberNames.push_back(sessionMembers[i].v_string.str);
                }
                ajObj.sessionMap[key] = sme;
                ajObj.sessionMapLock.Unlock();
                sessionMapEntryCreated = true;
            }

            /* If a raw sesssion was requested, then teardown the new b2bEp to use it for a raw stream */
            if ((replyCode == ALLJOYN_JOINSESSION_REPLY_SUCCESS) && (optsOut.traffic != SessionOpts::TRAFFIC_MESSAGES)) {
                ajObj.sessionMapLock.Lock();
                map<pair<String, SessionId>, SessionMapEntry>::iterator it = ajObj.sessionMap.find(key);
                if (it != ajObj.sessionMap.end()) {
                    status = ajObj.ShutdownEndpoint(*b2bEp, it->second.fd);
                    if (status != ER_OK) {
                        QCC_LogError(status, ("Failed to shutdown remote endpoint for raw usage"));
                        replyCode = ALLJOYN_JOINSESSION_REPLY_FAILED;
                    }
                } else {
                    QCC_LogError(ER_FAIL, ("Failed to find session id=%u for %s, %d", id, key.first.c_str(), key.second));
                    replyCode = ALLJOYN_JOINSESSION_REPLY_FAILED;
                }
                ajObj.sessionMapLock.Unlock();
            }

            /* If session was unsuccessful, cleanup sessionMap */
            if (sessionMapEntryCreated && (replyCode != ALLJOYN_JOINSESSION_REPLY_SUCCESS)) {
                ajObj.sessionMapLock.Lock();
                ajObj.sessionMap.erase(key);
                ajObj.sessionMapLock.Unlock();
            }

            /* If session was unsuccessful, we need to cleanup any b2b ep that was created */
            if ((replyCode != ALLJOYN_JOINSESSION_REPLY_SUCCESS) && b2bEp) {
                b2bEp->DecrementRef();
            }

            ajObj.virtualEndpointsLock.Unlock();
            ajObj.discoverMapLock.Unlock();
            ajObj.router.UnlockNameTable();
        }
    }

    /* Send AttachSession to all other members of the multicast session */
    if ((replyCode == ALLJOYN_JOINSESSION_REPLY_SUCCESS) && sme.opts.isMultipoint) {
        if (status == ER_OK) {
            for (size_t i = 0; i < sme.memberNames.size(); ++i) {
                const char* member = sme.memberNames[i].c_str();
                /* Skip this joiner since it is attached already */
                if (::strcmp(member, msg->GetSender()) == 0) {
                    continue;
                }
                ajObj.router.LockNameTable();
                ajObj.discoverMapLock.Lock();
                ajObj.virtualEndpointsLock.Lock();
                BusEndpoint* joinerEp = ajObj.router.FindEndpoint(msg->GetSender());
                BusEndpoint* memberEp = ajObj.router.FindEndpoint(member);
                RemoteEndpoint* memberB2BEp = NULL;
                if (memberEp && (memberEp->GetEndpointType() == BusEndpoint::ENDPOINT_TYPE_VIRTUAL)) {
                    /* Endpoint is not served directly by this daemon so forward the attach */
                    VirtualEndpoint* vMemberEp = static_cast<VirtualEndpoint*>(memberEp);
                    memberB2BEp = vMemberEp->GetSessionCompatibleB2B(sme.opts);
                    if (memberB2BEp) {
                        MsgArg tMembersArg;
                        SessionId tId;
                        SessionOpts tOpts;
                        const String nextControllerName = memberB2BEp->GetRemoteName();
                        ajObj.virtualEndpointsLock.Unlock();
                        ajObj.discoverMapLock.Unlock();
                        ajObj.router.UnlockNameTable();
                        status = ajObj.SendAttachSession(sessionPort, msg->GetSender(), member, memberB2BEp->GetUniqueName().c_str(),
                                                         nextControllerName.c_str(), "", sme.opts, replyCode, tId, tOpts, tMembersArg);
                        ajObj.router.LockNameTable();
                        ajObj.discoverMapLock.Lock();
                        ajObj.virtualEndpointsLock.Lock();
                        /* Reacquire endpoints since locks were given up */
                        joinerEp = ajObj.router.FindEndpoint(msg->GetSender());
                        memberEp = ajObj.router.FindEndpoint(member);
                        memberB2BEp = vMemberEp->GetSessionCompatibleB2B(sme.opts);
                        if (status != ER_OK) {
                            QCC_LogError(status, ("Failed to attach session %u to %s", id, member));
                        } else if (id != tId) {
                            status = ER_FAIL;
                            QCC_LogError(status, ("Session id mismatch (expected=%u, actual=%u)", id, tId));
                        }
                    } else {
                        status = ER_BUS_BAD_SESSION_OPTS;
                        QCC_LogError(status, ("Unable to add existing member %s to session %u", vMemberEp->GetUniqueName().c_str(), id));
                    }
                }
                /* Add session routing */
                if (memberEp && joinerEp && (status == ER_OK)) {
                    status = ajObj.router.AddSessionRoute(msg->GetSender(), id, *memberEp, memberB2BEp);
                    if (status == ER_OK) {
                        RemoteEndpoint* tEp = NULL;
                        status = ajObj.router.AddSessionRoute(memberEp->GetUniqueName().c_str(), id, *joinerEp, tEp);
                        if (status != ER_OK) {
                            replyCode = ALLJOYN_JOINSESSION_REPLY_FAILED;
                            ajObj.router.RemoveSessionRoute(msg->GetSender(), id, *memberEp);
                            QCC_LogError(status, ("AddSessionRoute %s->%s failed", memberEp->GetUniqueName().c_str(), joinerEp->GetUniqueName().c_str()));
                        }
                    } else {
                        replyCode = ALLJOYN_JOINSESSION_REPLY_FAILED;
                        QCC_LogError(status, ("AddSessionRoute %s->%s failed", msg->GetSender(), memberEp->GetUniqueName().c_str()));
                    }
                }
                ajObj.virtualEndpointsLock.Unlock();
                ajObj.discoverMapLock.Unlock();
                ajObj.router.UnlockNameTable();
            }
        }
    }

    /* Reply to request */
    MsgArg replyArgs[3];
    replyArgs[0].Set("u", replyCode);
    replyArgs[1].Set("u", id);
    SetSessionOpts(optsOut, replyArgs[2]);
    status = ajObj.MethodReply(msg, replyArgs, ArraySize(replyArgs));
    QCC_DbgPrintf(("AllJoynObj::JoinSession(%d) returned (%d,%u) (status=%s)", sessionPort, replyCode, id, QCC_StatusText(status)));

    /* Log error if reply could not be sent */
    if (ER_OK != status) {
        QCC_LogError(status, ("Failed to respond to org.alljoyn.Bus.JoinSession"));
    }
    return 0;
}

void AllJoynObj::JoinSessionThread::ThreadExit(Thread* thread)
{
    ajObj.joinSessionThreadsLock.Lock();
    vector<JoinSessionThread*>::iterator it = ajObj.joinSessionThreads.begin();
    bool isFound = false;
    while (it != ajObj.joinSessionThreads.end()) {
        if (*it == thread) {
            ajObj.joinSessionThreads.erase(it);
            isFound = true;
            break;
        }
        ++it;
    }
    if (!isFound) {
        QCC_LogError(ER_FAIL, ("Internal error: JoinSessionThread not found on list"));
    }
    ajObj.joinSessionThreadsLock.Unlock();
}

void AllJoynObj::JoinSession(const InterfaceDescription::Member* member, Message& msg)
{
    /* Handle JoinSession on another thread since JoinThread can block waiting for NameOwnerChanged */
    joinSessionThreadsLock.Lock();
    if (!isStopping) {
        JoinSessionThread* jst = new JoinSessionThread(*this, msg);
        QStatus status = jst->Start(NULL, jst);
        if (status == ER_OK) {
            joinSessionThreads.push_back(jst);
        } else {
            QCC_LogError(status, ("Failed to start JoinSessionThread"));
        }
    }
    joinSessionThreadsLock.Unlock();
}

void AllJoynObj::LeaveSession(const InterfaceDescription::Member* member, Message& msg)
{
    uint32_t replyCode = ALLJOYN_LEAVESESSION_REPLY_SUCCESS;

    size_t numArgs;
    const MsgArg* args;

    /* Parse the message args */
    msg->GetArgs(numArgs, args);
    assert(numArgs == 1);
    SessionId id = static_cast<SessionId>(args[0].v_uint32);

    QCC_DbgTrace(("AllJoynObj::LeaveSession(%d)", id));

    /* Find the session with that id */
    sessionMapLock.Lock();
    map<pair<String, SessionId>, SessionMapEntry>::iterator it = sessionMap.find(pair<String, SessionId>(msg->GetSender(), id));
    if ((it == sessionMap.end()) || (id == 0)) {
        sessionMapLock.Unlock();
        replyCode = ALLJOYN_LEAVESESSION_REPLY_NO_SESSION;
    } else {
        /* Close any open fd for this session */
        if (it->second.fd != -1) {
            qcc::Shutdown(it->second.fd);
            qcc::Close(it->second.fd);
        }
        /* Remove entry from sessionMap */
        sessionMap.erase(it);
        sessionMapLock.Unlock();

        /* Send DetachSession signal to all daemons */
        MsgArg detachSessionArgs[2];
        detachSessionArgs[0].Set("u", id);
        detachSessionArgs[1].Set("s", msg->GetSender());
        QStatus status = Signal(NULL, 0, *detachSessionSignal, detachSessionArgs, ArraySize(detachSessionArgs));
        if (status != ER_OK) {
            replyCode = ALLJOYN_LEAVESESSION_REPLY_FAILED;
            QCC_LogError(status, ("Error sending org.alljoyn.Daemon.DetachSession signal"));
        }

        /* Remove session routes */
        router.RemoveSessionRoutes(msg->GetSender(), id);
    }

    /* Reply to request */
    MsgArg replyArgs[1];
    replyArgs[0].Set("u", replyCode);
    QStatus status = MethodReply(msg, replyArgs, ArraySize(replyArgs));
    QCC_DbgPrintf(("AllJoynObj::LeaveSession(%d) returned (%u) (status=%s)", id, replyCode, QCC_StatusText(status)));

    /* Log error if reply could not be sent */
    if (ER_OK != status) {
        QCC_LogError(status, ("Failed to respond to org.alljoyn.Bus.LeaveSession"));
    }
}

void AllJoynObj::AttachSession(const InterfaceDescription::Member* member, Message& msg)
{
    SessionId id = 0;
    String creatorName;
    MsgArg replyArgs[4];
    SessionOpts optsOut;
    uint32_t replyCode = ALLJOYN_JOINSESSION_REPLY_FAILED;

    /* Default member list to empty */
    replyArgs[3].Set("as", 0, NULL);

    /* Received a daemon request to establish a session route */

    /* Parse message args */
    SessionPort sessionPort;
    const char* src;
    const char* dest;
    const char* srcB2B;
    const char* busAddr;
    SessionOpts optsIn;
    RemoteEndpoint* srcB2BEp = NULL;

    size_t na;
    const MsgArg* args;
    msg->GetArgs(na, args);
    assert(na == 6);
    QStatus status = MsgArg::Get(args, 5, "qssss", &sessionPort, &src, &dest, &srcB2B, &busAddr);
    if (status == ER_OK) {
        status = GetSessionOpts(args[5], optsIn);
    }

    if (status != ER_OK) {
        QCC_DbgTrace(("AllJoynObj::AttachSession(<bad args>)"));
        replyCode = ALLJOYN_JOINSESSION_REPLY_FAILED;
    } else {
        QCC_DbgTrace(("AllJoynObj::AttachSession(%d, %s, %s, %s, %s, <%x, %x, %x>)", sessionPort, src, dest, srcB2B, busAddr,
                      optsIn.traffic, optsIn.proximity, optsIn.transports));

        router.LockNameTable();
        discoverMapLock.Lock();
        virtualEndpointsLock.Lock();

        BusEndpoint* destEp = router.FindEndpoint(dest);

        /* Determine if the dest is local to this daemon */
        if (destEp && ((destEp->GetEndpointType() == BusEndpoint::ENDPOINT_TYPE_REMOTE) ||
                       (destEp->GetEndpointType() == BusEndpoint::ENDPOINT_TYPE_LOCAL))) {
            /* This daemon serves dest directly */

            /* Check for a session in the session map */
            SessionMapEntry sme;
            bool foundSessionMapEntry = false;
            String destUniqueName = destEp->GetUniqueName();
            sessionMapLock.Lock();
            map<pair<String, SessionId>, SessionMapEntry>::const_iterator sit = sessionMap.lower_bound(pair<String, SessionId>(destUniqueName, 0));
            while ((sit != sessionMap.end()) && (sit->first.first == destUniqueName)) {
                if (sit->second.sessionPort == sessionPort) {
                    sme = sit->second;
                    if (!sme.opts.isMultipoint) {
                        /* Session is not multipoint. Create a new session entry */
                        do {
                            sme.id = qcc::Rand32();
                        } while (sme.id == 0);
                    } else {
                        /* Session is multipoint. Look for an existing (already joined) session */
                        sit = sessionMap.find(pair<String, SessionId>(destUniqueName, sme.id));
                        if (sit != sessionMap.end()) {
                            sme = sit->second;
                        }
                    }
                    sessionMap[pair < String, SessionId > (sme.endpointName, sme.id)] = sme;
                    foundSessionMapEntry = true;
                    break;
                }
                ++sit;
            }
            sessionMapLock.Unlock();
            if (!foundSessionMapEntry) {
                /* No such session */
                replyCode = ALLJOYN_JOINSESSION_REPLY_NO_SESSION;
            } else if (!sme.opts.IsCompatible(optsIn)) {
                replyCode = ALLJOYN_JOINSESSION_REPLY_BAD_SESSION_OPTS;
                optsOut = sme.opts;
            } else {
                optsOut = sme.opts;
                BusEndpoint* ep = router.FindEndpoint(srcB2B);
                srcB2BEp = (ep && (ep->GetEndpointType() == BusEndpoint::ENDPOINT_TYPE_BUS2BUS)) ? static_cast<RemoteEndpoint*>(ep) : NULL;
                ep = router.FindEndpoint(src);
                VirtualEndpoint* srcEp = (ep && (ep->GetEndpointType() == BusEndpoint::ENDPOINT_TYPE_VIRTUAL)) ? static_cast<VirtualEndpoint*>(ep) : NULL;

                if (srcEp && srcB2BEp) {
                    if (status == ER_OK) {
                        /* Store ep for raw sessions (for future close and fd extract) */
                        if (optsOut.traffic != SessionOpts::TRAFFIC_MESSAGES) {
                            sessionMapLock.Lock();
                            sessionMap[pair < String, SessionId > (sme.endpointName, sme.id)].streamingEp = srcB2BEp;
                            sessionMapLock.Unlock();
                        }

                        /* If this node is the session creator, give it a chance to accept or reject the new member */
                        bool isAccepted = true;
                        BusEndpoint* creatorEp = router.FindEndpoint(sme.sessionHost);
                        if (creatorEp && (destEp == creatorEp)) {
                            virtualEndpointsLock.Unlock();
                            discoverMapLock.Unlock();
                            router.UnlockNameTable();
                            status = SendAcceptSession(sme.sessionPort, sme.id, dest, src, optsIn, isAccepted);
                            if (ER_OK != status) {
                                QCC_LogError(status, ("SendAcceptSession failed"));
                            }
                            router.LockNameTable();
                            discoverMapLock.Lock();
                            virtualEndpointsLock.Lock();
                        }

                        /* Add new joiner to members */
                        if (isAccepted && creatorEp) {
                            sessionMapLock.Lock();
                            map<pair<String, SessionId>, SessionMapEntry>::iterator smIt = sessionMap.find(pair<String, SessionId>(sme.endpointName, sme.id));
                            if (smIt != sessionMap.end()) {
                                id = sme.id;
                                creatorName = creatorEp->GetUniqueName();
                                replyCode = ALLJOYN_JOINSESSION_REPLY_SUCCESS;
                                smIt->second.memberNames.push_back(src);

                                /* Response will contain list of members */
                                vector<const char*> nameVec(smIt->second.memberNames.size());
                                for (size_t i = 0; i < nameVec.size(); ++i) {
                                    nameVec[i] = smIt->second.memberNames[i].c_str();
                                }
                                replyArgs[3].Set("as", nameVec.size(), &nameVec.front());
                            }
                            sessionMapLock.Unlock();

                            /* Add routes for new session */
                            if (optsOut.traffic == SessionOpts::TRAFFIC_MESSAGES) {
                                status = router.AddSessionRoute(destUniqueName.c_str(), id, *srcEp, srcB2BEp);
                                if (ER_OK == status) {
                                    RemoteEndpoint* tEp = NULL;
                                    status = router.AddSessionRoute(src, id, *destEp, tEp);
                                    if (ER_OK != status) {
                                        router.RemoveSessionRoute(destUniqueName.c_str(), id, *srcEp);
                                        QCC_LogError(status, ("AddSessionRoute %s->%s failed", src, destUniqueName.c_str()));
                                    }
                                } else {
                                    QCC_LogError(status, ("AddSessionRoute %s->%s failed", dest, srcEp->GetUniqueName().c_str()));
                                }
                            }
                        } else {
                            replyCode =  ALLJOYN_JOINSESSION_REPLY_REJECTED;
                        }
                    }
                } else {
                    status = ER_FAIL;
                    QCC_LogError(status, ("Cannot locate srcEp(%p, src=%s) or srcB2BEp(%p, src=%s)", srcEp, src, srcB2BEp, srcB2B));
                }
            }
        } else {
            /* This daemon will attempt to route indirectly to dest */
            virtualEndpointsLock.Unlock();
            discoverMapLock.Unlock();
            router.UnlockNameTable();
            RemoteEndpoint* b2bEp = NULL;
            String b2bEpName;
            if (busAddr[0] == '\0') {
                /* Reuse existing endpoints if busAddr is not given (multipoint sessions) */
                if (destEp && ((destEp->GetEndpointType() == BusEndpoint::ENDPOINT_TYPE_VIRTUAL))) {
                    b2bEp = static_cast<VirtualEndpoint*>(destEp)->GetSessionCompatibleB2B(optsIn);
                    if (b2bEp) {
                        b2bEpName = b2bEp->GetUniqueName();
                    } else {
                        replyCode = ALLJOYN_JOINSESSION_REPLY_BAD_SESSION_OPTS;
                    }
                } else {
                    replyCode = ALLJOYN_JOINSESSION_REPLY_NO_SESSION;
                }
            } else {
                /* Ask the transport for an endpoint */
                TransportList& transList = bus.GetInternal().GetTransportList();
                Transport* trans = transList.GetTransport(busAddr);
                if (trans == NULL) {
                    replyCode = ALLJOYN_JOINSESSION_REPLY_UNREACHABLE;
                } else {
                    status = trans->Connect(busAddr, &b2bEp);
                    if (status == ER_OK) {
                        b2bEpName = b2bEp->GetUniqueName();
                    } else {
                        QCC_LogError(status, ("trans->Connect(%s) failed", busAddr));
                        replyCode = ALLJOYN_JOINSESSION_REPLY_CONNECT_FAILED;
                    }
                }
            }

            if (!b2bEpName.empty()) {
                /* Forward AttachSession to next hop */
                SessionId tempId;
                SessionOpts tempOpts;
                const String nextControllerName = b2bEp->GetRemoteName();

                /* Unlock in preparation for sync method call. Ep pointers are unsafe after this operation */
                status = SendAttachSession(sessionPort, src, dest, b2bEpName.c_str(), nextControllerName.c_str(), busAddr,
                                           optsIn, replyCode, tempId, tempOpts, replyArgs[3]);

                /* If successful, add bi-directional session routes */
                router.LockNameTable();
                discoverMapLock.Lock();
                virtualEndpointsLock.Lock();
                if ((status == ER_OK) && (replyCode == ALLJOYN_JOINSESSION_REPLY_SUCCESS)) {

                    /* Wait for dest to appear with a route through b2bEp */
                    uint32_t startTime = GetTimestamp();
                    VirtualEndpoint* vDestEp = NULL;
                    while (replyCode == ALLJOYN_JOINSESSION_REPLY_SUCCESS) {
                        /* Does vSessionEp route through b2bEp? If so, we're done */
                        BusEndpoint* ep = router.FindEndpoint(dest);
                        vDestEp = (ep && (ep->GetEndpointType() == BusEndpoint::ENDPOINT_TYPE_VIRTUAL)) ? static_cast<VirtualEndpoint*>(ep) : NULL;
                        b2bEp = static_cast<RemoteEndpoint*>(router.FindEndpoint(b2bEpName));
                        if (!b2bEp) {
                            QCC_LogError(ER_FAIL, ("B2B endpoint disappeared during AttachSession"));
                            replyCode = ALLJOYN_JOINSESSION_REPLY_FAILED;
                            break;
                        } else if (vDestEp && vDestEp->CanUseRoute(*b2bEp)) {
                            break;
                        }
                        /* Otherwise wait */
                        uint32_t now = GetTimestamp();
                        if (now > (startTime + 10000)) {
                            replyCode = ALLJOYN_JOINSESSION_REPLY_FAILED;
                            QCC_LogError(ER_FAIL, ("AttachSession timed out waiting for destination to appear"));
                            break;
                        } else {
                            /* Give up the locks while waiting */
                            virtualEndpointsLock.Unlock();
                            discoverMapLock.Unlock();
                            router.UnlockNameTable();
                            qcc::Sleep(10);
                            router.LockNameTable();
                            discoverMapLock.Lock();
                            virtualEndpointsLock.Lock();
                        }
                    }

                    BusEndpoint* ep = router.FindEndpoint(srcB2B);
                    RemoteEndpoint* srcB2BEp2 = (ep && (ep->GetEndpointType() == BusEndpoint::ENDPOINT_TYPE_BUS2BUS)) ? static_cast<RemoteEndpoint*>(ep) : NULL;
                    ep = router.FindEndpoint(src);
                    VirtualEndpoint* srcEp = (ep && (ep->GetEndpointType() == BusEndpoint::ENDPOINT_TYPE_VIRTUAL)) ? static_cast<VirtualEndpoint*>(ep) : NULL;
                    /* Add bi-directional session routes */
                    if (srcB2BEp2 && srcEp && vDestEp && b2bEp) {
                        id = tempId;
                        optsOut = tempOpts;
                        status = router.AddSessionRoute(dest, id, *srcEp, srcB2BEp2);
                        if (status == ER_OK) {
                            status = router.AddSessionRoute(src, id, *vDestEp, b2bEp);
                            if (status != ER_OK) {
                                router.RemoveSessionRoute(dest, id, *srcEp);
                                QCC_LogError(status, ("AddSessionRoute(%s, %u) failed", src, id));
                            }
                        } else {
                            QCC_LogError(status, ("AddSessionRoute(%s, %u) failed", dest, id));
                        }
                    } else {
                        // TODO: Need to cleanup partially setup session
                        replyCode = ALLJOYN_JOINSESSION_REPLY_FAILED;
                    }
                } else {
                    if (status == ER_OK) {
                        status = ER_BUS_REPLY_IS_ERROR_MESSAGE;
                    }
                    replyCode = ALLJOYN_JOINSESSION_REPLY_FAILED;
                    QCC_LogError(status, ("AttachSession failed"));
                }
            } else {
                router.LockNameTable();
                discoverMapLock.Lock();
                virtualEndpointsLock.Lock();
            }
        }
    }

    virtualEndpointsLock.Unlock();
    discoverMapLock.Unlock();
    router.UnlockNameTable();

    /* Reply to request */
    replyArgs[0].Set("u", replyCode);
    replyArgs[1].Set("u", id);
    SetSessionOpts(optsOut, replyArgs[2]);

    /* On success, ensure that reply goes over the new b2b connection. Otherwise a race condition
     * related to shutting down endpoints that are to become raw will occur.
     */
    if (srcB2BEp) {
        status = msg->ReplyMsg(replyArgs, ArraySize(replyArgs));
        if (status == ER_OK) {
            status = srcB2BEp->PushMessage(msg);
        }
    } else {
        status = MethodReply(msg, replyArgs, ArraySize(replyArgs));
    }
    QCC_DbgPrintf(("AllJoynObj::AttachSession(%d) returned (%d,%u) (status=%s)", sessionPort, replyCode, id, QCC_StatusText(status)));

    /* Log error if reply could not be sent */
    if (ER_OK != status) {
        QCC_LogError(status, ("Failed to respond to org.alljoyn.Daemon.AttachSession"));
    }

    /* If the session is raw, then close the new ep and preserve the fd */
    if (srcB2BEp && !creatorName.empty() && (optsOut.traffic != SessionOpts::TRAFFIC_MESSAGES)) {
        sessionMapLock.Lock();
        map<pair<String, SessionId>, SessionMapEntry>::iterator it = sessionMap.find(pair<String, SessionId>(creatorName, id));
        if (it != sessionMap.end()) {
            if (it->second.streamingEp) {
                status = ShutdownEndpoint(*it->second.streamingEp, it->second.fd);
                if (status != ER_OK) {
                    QCC_LogError(status, ("Failed to shutdown raw endpoint\n"));
                }
                it->second.streamingEp = NULL;
            }
        } else {
            QCC_LogError(ER_FAIL, ("Failed to find SessionMapEntry \"%s\",%d", creatorName.c_str(), id));
        }
        sessionMapLock.Unlock();
    }
}

void AllJoynObj::GetSessionInfo(const InterfaceDescription::Member* member, Message& msg)
{
    /* Received a daemon request for session info */

    /* Parse message args */
    const char* creatorName;
    SessionPort sessionPort;
    SessionOpts optsIn;
    String busAddr;

    size_t na;
    const MsgArg* args;
    msg->GetArgs(na, args);
    QStatus status = MsgArg::Get(args, 2, "sq", &creatorName, &sessionPort);
    if (status == ER_OK) {
        status = GetSessionOpts(args[2], optsIn);
    }

    if (status != ER_OK) {
        QCC_DbgTrace(("AllJoynObj::GetSessionInfo(%s, %u, <%x, %x, %x>)", creatorName, sessionPort, optsIn.traffic, optsIn.proximity, optsIn.transports));

        /* Ask the appropriate transport for the listening busAddr */
        TransportList& transList = bus.GetInternal().GetTransportList();
        for (size_t i = 0; i < transList.GetNumTransports(); ++i) {
            Transport* trans = transList.GetTransport(i);
            if (trans && (trans->GetTransportMask() & optsIn.transports)) {
                busAddr = trans->GetListenAddress(optsIn);
                break;
            } else if (!trans) {
                QCC_LogError(ER_BUS_TRANSPORT_NOT_AVAILABLE, ("NULL transport pointer found in transportList"));
            }
        }
    } else {
        QCC_LogError(status, ("AllJoynObj::GetSessionInfo cannot parse args"));
    }

    if (busAddr.empty()) {
        status = MethodReply(msg, ER_BUS_NO_SESSION);
    } else {
        MsgArg replyArg("s", busAddr.c_str());
        status = MethodReply(msg, &replyArg, 1);
    }

    if (status != ER_OK) {
        QCC_LogError(status, ("GetSessionInfo failed"));
    }
}

QStatus AllJoynObj::SendAttachSession(SessionPort sessionPort,
                                      const char* src,
                                      const char* dest,
                                      const char* remoteB2BName,
                                      const char* remoteControllerName,
                                      const char* busAddr,
                                      const SessionOpts& optsIn,
                                      uint32_t& replyCode,
                                      SessionId& id,
                                      SessionOpts& optsOut,
                                      MsgArg& members)
{
    Message reply(bus);
    MsgArg attachArgs[6];
    attachArgs[0].Set("q", sessionPort);
    attachArgs[1].Set("s", src);
    attachArgs[2].Set("s", dest);
    attachArgs[3].Set("s", remoteB2BName);
    attachArgs[4].Set("s", busAddr);
    SetSessionOpts(optsIn, attachArgs[5]);
    ProxyBusObject controllerObj(bus, remoteControllerName, org::alljoyn::Daemon::ObjectPath, 0);
    controllerObj.AddInterface(*daemonIface);
    QStatus status = controllerObj.SetB2BEndpoint(remoteB2BName);

    /* If the new session is raw, then arm the endpoint's RX thread to stop after reading one more message */
    if ((status == ER_OK) && (optsIn.traffic != SessionOpts::TRAFFIC_MESSAGES)) {
        BusEndpoint* ep = router.FindEndpoint(remoteB2BName);
        RemoteEndpoint* b2bEp = (ep && ep->GetEndpointType() == BusEndpoint::ENDPOINT_TYPE_BUS2BUS) ?  static_cast<RemoteEndpoint*>(ep) : NULL;
        if (b2bEp) {
            status = b2bEp->PauseAfterRxReply();
        } else {
            status = ER_BUS_NO_ENDPOINT;
            QCC_LogError(status, ("Cannot find B2BEp for %s", remoteB2BName));
        }
    }

    /* Make the method call */
    if (status == ER_OK) {
        QCC_DbgPrintf(("Sending AttachSession(%u, %s, %s, %s, %s, <%x, %x, %x>) to %s",
                       attachArgs[0].v_uint16,
                       attachArgs[1].v_string.str,
                       attachArgs[2].v_string.str,
                       attachArgs[3].v_string.str,
                       attachArgs[4].v_string.str,
                       optsIn.proximity, optsIn.traffic, optsIn.transports,
                       remoteControllerName));

        status = controllerObj.MethodCall(org::alljoyn::Daemon::InterfaceName,
                                          "AttachSession",
                                          attachArgs,
                                          ArraySize(attachArgs),
                                          reply);
    }

    if (status != ER_OK) {
        replyCode = ALLJOYN_JOINSESSION_REPLY_FAILED;
        QCC_LogError(status, ("AttachSession failed"));
    } else {
        const MsgArg* replyArgs;
        size_t numReplyArgs;
        reply->GetArgs(numReplyArgs, replyArgs);
        replyCode = replyArgs[0].v_uint32;
        id = replyArgs[1].v_uint32;
        status = GetSessionOpts(replyArgs[2], optsOut);
        if (status == ER_OK) {
            members = *reply->GetArg(3);
            QCC_DbgPrintf(("Received AttachSession response: replyCode=%d, sessionId=0x%x, opts=<%x, %x, %x>",
                           replyCode, id, optsOut.proximity, optsOut.traffic, optsOut.transports));
        } else {
            QCC_DbgPrintf(("Received AttachSession response: <bad_args>"));
        }
    }

    return status;
}

QStatus AllJoynObj::SendAcceptSession(SessionPort sessionPort,
                                      SessionId sessionId,
                                      const char* creatorName,
                                      const char* joinerName,
                                      const SessionOpts& inOpts,
                                      bool& isAccepted)
{
    /* Give the receiver a chance to accept or reject the new member */
    Message reply(bus);
    MsgArg acceptArgs[4];
    acceptArgs[0].Set("q", sessionPort);
    acceptArgs[1].Set("u", sessionId);
    acceptArgs[2].Set("s", joinerName);
    SetSessionOpts(inOpts, acceptArgs[3]);
    ProxyBusObject peerObj(bus, creatorName, org::alljoyn::Bus::Peer::ObjectPath, 0);
    const InterfaceDescription* sessionIntf = bus.GetInterface(org::alljoyn::Bus::Peer::Session::InterfaceName);
    assert(sessionIntf);
    peerObj.AddInterface(*sessionIntf);
    QCC_DbgPrintf(("Calling AcceptSession(%d, %s, %s, <%x, %x, %x> to %s",
                   acceptArgs[0].v_uint16,
                   acceptArgs[1].v_string.str,
                   acceptArgs[2].v_string.str,
                   inOpts.proximity, inOpts.traffic, inOpts.transports,
                   creatorName));

    QStatus status = peerObj.MethodCall(org::alljoyn::Bus::Peer::Session::InterfaceName,
                                        "AcceptSession",
                                        acceptArgs,
                                        ArraySize(acceptArgs),
                                        reply);
    if (status == ER_OK) {
        size_t na;
        const MsgArg* replyArgs;
        reply->GetArgs(na, replyArgs);
        replyArgs[0].Get("b", &isAccepted);
    } else {
        isAccepted = false;
    }
    return status;
}

QStatus AllJoynObj::SendGetSessionInfo(const char* creatorName,
                                       SessionPort sessionPort,
                                       const SessionOpts& opts,
                                       String& busAddr)
{
    /* Send GetSessionInfo to creatorName */
    Message reply(bus);
    MsgArg sendArgs[3];
    sendArgs[0].Set("s", creatorName);
    sendArgs[1].Set("q", sessionPort);
    SetSessionOpts(opts, sendArgs[2]);

    String controllerName(creatorName);
    controllerName = controllerName.substr(0, controllerName.find_first_of('.') + 1);
    controllerName.append('1');

    ProxyBusObject rObj(bus, controllerName.c_str(), org::alljoyn::Daemon::ObjectPath, 0);
    const InterfaceDescription* intf = bus.GetInterface(org::alljoyn::Daemon::InterfaceName);
    assert(intf);
    rObj.AddInterface(*intf);
    QCC_DbgPrintf(("Calling GetSessionInfo(%s, %u, <%x, %x, %x>) on %s",
                   sendArgs[0].v_string.str,
                   sendArgs[1].v_uint16,
                   opts.proximity, opts.traffic, opts.transports,
                   controllerName.c_str()));

    QStatus status = rObj.MethodCall(org::alljoyn::Daemon::InterfaceName,
                                     "AcceptSession",
                                     sendArgs,
                                     ArraySize(sendArgs),
                                     reply);
    if (status == ER_OK) {
        size_t na;
        const MsgArg* replyArgs;
        const char* busAddrChars;
        reply->GetArgs(na, replyArgs);
        replyArgs[0].Get("s", &busAddrChars);
        busAddr = String(busAddrChars);
    }
    return status;
}

QStatus AllJoynObj::ShutdownEndpoint(RemoteEndpoint& b2bEp, SocketFd& sockFd)
{
    /* Grab the file descriptor for the B2B endpoint and close the endpoint */
    SocketFd epSockFd = b2bEp.GetSocketFd();
    QStatus status = SocketDup(epSockFd, sockFd);
    if (status == ER_OK) {
        status = b2bEp.StopAfterTxEmpty();
        if (status == ER_OK) {
            status = b2bEp.Join();
            if (status != ER_OK) {
                QCC_LogError(status, ("Failed to join RemoteEndpoint used for streaming"));
                sockFd = -1;
            }
        } else {
            QCC_LogError(status, ("Failed to stop RemoteEndpoint used for streaming"));
            sockFd = -1;
        }
    } else {
        QCC_LogError(status, ("Failed to dup remote endpoint's socket"));
        sockFd = -1;
    }
    return status;
}

void AllJoynObj::DetachSessionSignalHandler(const InterfaceDescription::Member* member, const char* sourcePath, Message& msg)
{
    size_t numArgs;
    const MsgArg* args;

    /* Parse message args */
    msg->GetArgs(numArgs, args);
    SessionId id = args[0].v_uint32;
    const char* src = args[1].v_string.str;

    /* Remove session info */
    router.RemoveSessionRoutes(src, id);
}

void AllJoynObj::GetSessionFd(const InterfaceDescription::Member* member, Message& msg)
{
    /* Parse args */
    size_t numArgs;
    const MsgArg* args;
    msg->GetArgs(numArgs, args);
    SessionId id = args[0].v_uint32;
    QStatus status;
    SocketFd sockFd = -1;

    QCC_DbgTrace(("AllJoynObj::GetSessionFd(%d)", id));

    /* Wait for any join related operations to complete before returning fd */
    sessionMapLock.Lock();
    map<pair<String, SessionId>, SessionMapEntry>::iterator it = sessionMap.find(pair<String, SessionId>(msg->GetSender(), id));
    if ((it != sessionMap.end()) && (it->second.opts.traffic != SessionOpts::TRAFFIC_MESSAGES)) {
        uint32_t ts = GetTimestamp();
        while ((it != sessionMap.end()) && ((sockFd = it->second.fd) == -1) && ((ts + 5000) > GetTimestamp())) {
            sessionMapLock.Unlock();
            qcc::Sleep(5);
            sessionMapLock.Lock();
            it = sessionMap.find(pair<String, SessionId>(msg->GetSender(), id));
        }
    }
    sessionMapLock.Unlock();

    if (sockFd != -1) {
        /* Send the fd and transfer ownership */
        MsgArg replyArg;
        replyArg.Set("h", sockFd);
        status = MethodReply(msg, &replyArg, 1);
        qcc::Close(sockFd);
    } else {
        /* Send an error */
        status = MethodReply(msg, ER_BUS_NO_SESSION);
    }

    if (status != ER_OK) {
        QCC_LogError(status, ("Failed to respond to org.alljoyn.Bus.GetSessionFd"));
    }
}

void AllJoynObj::AdvertiseName(const InterfaceDescription::Member* member, Message& msg)
{
    uint32_t replyCode = ALLJOYN_ADVERTISENAME_REPLY_SUCCESS;
    size_t numArgs;
    const MsgArg* args;
    MsgArg replyArg;
    const char* advertiseName;
    TransportMask transports = 0;

    /* Get AdvertiseName args */
    msg->GetArgs(numArgs, args);
    QStatus status = MsgArg::Get(args, numArgs, "sq", &advertiseName, &transports);
    QCC_DbgTrace(("AllJoynObj::AdvertiseName(%s, %x)", (status == ER_OK) ? advertiseName : "", transports));

    /* Get the sender name */
    qcc::String sender = msg->GetSender();

    /* Check to see if the advertise name is valid and well formed */
    if (IsLegalBusName(advertiseName)) {

        /* Check to see if advertiseName is already being advertised */
        advertiseMapLock.Lock();
        String advertiseNameStr = advertiseName;
        multimap<qcc::String, pair<TransportMask, qcc::String> >::iterator it = advertiseMap.find(advertiseNameStr);

        while ((it != advertiseMap.end()) && (it->first == advertiseNameStr)) {
            if (it->second.second == sender) {
                if ((it->second.first & transports) != 0) {
                    replyCode = ALLJOYN_ADVERTISENAME_REPLY_ALREADY_ADVERTISING;
                }
                break;
            }
            ++it;
        }

        if (ALLJOYN_ADVERTISENAME_REPLY_SUCCESS == replyCode) {
            /* Add to advertise map */
            if (it == advertiseMap.end()) {
                advertiseMap.insert(pair<qcc::String, pair<TransportMask, qcc::String> >(advertiseNameStr, pair<TransportMask, String>(transports, sender)));
            } else {
                it->second.first |= transports;
            }

            /* Advertise on transports specified */
            TransportList& transList = bus.GetInternal().GetTransportList();
            status = ER_BUS_BAD_SESSION_OPTS;
            for (size_t i = 0; i < transList.GetNumTransports(); ++i) {
                Transport* trans = transList.GetTransport(i);
                if (trans && (trans->GetTransportMask() & transports)) {
                    status = trans->EnableAdvertisement(advertiseNameStr);
                    if (status != ER_OK) {
                        QCC_LogError(status, ("EnableAdvertisment failed for mask=0x%x", transports));
                    }
                } else if (!trans) {
                    QCC_LogError(ER_BUS_TRANSPORT_NOT_AVAILABLE, ("NULL transport pointer found in transportList"));
                }
            }
        }
        advertiseMapLock.Unlock();

    } else {
        replyCode = ALLJOYN_ADVERTISENAME_REPLY_FAILED;
    }

    /* Reply to request */
    replyArg.Set("u", replyCode);
    status = MethodReply(msg, &replyArg, 1);

    QCC_DbgPrintf(("AllJoynObj::Advertise(%s) returned %d (status=%s)", advertiseName, replyCode, QCC_StatusText(status)));

    /* Add advertisement to local nameMap so local discoverers can see this advertisement */
    if ((replyCode == ALLJOYN_ADVERTISENAME_REPLY_SUCCESS) && (transports & TRANSPORT_LOCAL)) {
        vector<String> names;
        names.push_back(advertiseName);
        discoverMapLock.Lock();
        FoundNames("local:", bus.GetGlobalGUIDString(), transports, &names, numeric_limits<uint8_t>::max());
        discoverMapLock.Unlock();
    }

    /* Log error if reply could not be sent */
    if (ER_OK != status) {
        QCC_LogError(status, ("Failed to respond to org.alljoyn.Bus.Advertise"));
    }
}

void AllJoynObj::CancelAdvertiseName(const InterfaceDescription::Member* member, Message& msg)
{
    const MsgArg* args;
    size_t numArgs;

    /* Get the name being advertised */
    msg->GetArgs(numArgs, args);
    const char* advertiseName;
    TransportMask transports = 0;
    QStatus status = MsgArg::Get(args, numArgs, "sq", &advertiseName, &transports);
    if (status != ER_OK) {
        QCC_LogError(status, ("CancelAdvertiseName: bad arg types"));
        return;
    }

    QCC_DbgTrace(("AllJoynObj::CancelAdvertiseName(%s, 0x%x)", advertiseName, transports));

    /* Cancel advertisement */
    status = ProcCancelAdvertise(msg->GetSender(), advertiseName, transports);
    uint32_t replyCode = (ER_OK == status) ? ALLJOYN_CANCELADVERTISENAME_REPLY_SUCCESS : ALLJOYN_CANCELADVERTISENAME_REPLY_FAILED;

    /* Reply to request */
    MsgArg replyArg("u", replyCode);
    status = MethodReply(msg, &replyArg, 1);

    /* Remove advertisement from local nameMap so local discoverers are notified of advertisement going away */
    if ((replyCode == ALLJOYN_ADVERTISENAME_REPLY_SUCCESS) && (transports & TRANSPORT_LOCAL)) {
        vector<String> names;
        names.push_back(advertiseName);
        discoverMapLock.Lock();
        FoundNames("local:", bus.GetGlobalGUIDString(), TRANSPORT_LOCAL, &names, 0);
        discoverMapLock.Unlock();
    }

    /* Log error if reply could not be sent */
    if (ER_OK != status) {
        QCC_LogError(status, ("Failed to respond to org.alljoyn.Bus.CancelAdvertise"));
    }
}

QStatus AllJoynObj::ProcCancelAdvertise(const qcc::String& sender, const qcc::String& advertiseName, TransportMask transports)
{
    QCC_DbgTrace(("AllJoynObj::ProcCancelAdvertise(%s, %s, %x)",
                  sender.c_str(),
                  advertiseName.c_str(),
                  transports));

    QStatus status = ER_OK;

    /* Check to see if this advertised name exists and delete it */
    bool foundAdvert = false;
    bool advertHasRefs = false;

    advertiseMapLock.Lock();
    multimap<qcc::String, pair<TransportMask, qcc::String> >::iterator it = advertiseMap.find(advertiseName);
    while ((it != advertiseMap.end()) && (it->first == advertiseName)) {
        if (it->second.second == sender) {
            foundAdvert = true;
            it->second.first &= ~transports;
            if (it->second.first == 0) {
                advertiseMap.erase(it++);
            } else {
                ++it;
            }
        } else {
            advertHasRefs = true;
            ++it;
        }
    }

    /* Cancel transport advertisement if no other refs exist */
    if (foundAdvert && !advertHasRefs) {
        TransportList& transList = bus.GetInternal().GetTransportList();
        for (size_t i = 0; i < transList.GetNumTransports(); ++i) {
            Transport* trans = transList.GetTransport(i);
            if (trans && (trans->GetTransportMask() & transports)) {
                trans->DisableAdvertisement(advertiseName, advertiseMap.empty());
            } else if (!trans) {
                QCC_LogError(ER_BUS_TRANSPORT_NOT_AVAILABLE, ("NULL transport pointer found in transportList"));
            }
        }
    } else if (!foundAdvert) {
        status = ER_FAIL;
    }
    advertiseMapLock.Unlock();
    return status;
}

void AllJoynObj::GetAdvertisedNames(std::vector<qcc::String>& names)
{
    advertiseMapLock.Lock();
    multimap<qcc::String, pair<TransportMask, qcc::String> >::const_iterator it(advertiseMap.begin());
    while (it != advertiseMap.end()) {
        const qcc::String& name(it->first);
        QCC_DbgPrintf(("AllJoynObj::GetAdvertisedNames - Name[%u] = %s", names.size(), name.c_str()));
        names.push_back(name);
        // skip to next name
        it = advertiseMap.upper_bound(name);
    }
    advertiseMapLock.Unlock();
}

void AllJoynObj::FindAdvertisedName(const InterfaceDescription::Member* member, Message& msg)
{
    uint32_t replyCode;
    size_t numArgs;
    const MsgArg* args;

    /* Get the name prefix */
    msg->GetArgs(numArgs, args);
    assert((numArgs == 1) && (args[0].typeId == ALLJOYN_STRING));
    String namePrefix = args[0].v_string.str;

    QCC_DbgTrace(("AllJoynObj::FindAdvertisedName(%s)", namePrefix.c_str()));


    /* Check to see if this endpoint is already discovering this prefix */
    qcc::String sender = msg->GetSender();
    replyCode = ALLJOYN_FINDADVERTISEDNAME_REPLY_SUCCESS;
    router.LockNameTable();
    discoverMapLock.Lock();
    multimap<qcc::String, qcc::String>::const_iterator it = discoverMap.find(namePrefix);
    while ((it != discoverMap.end()) && (it->first == namePrefix)) {
        if (it->second == sender) {
            replyCode = ALLJOYN_FINDADVERTISEDNAME_REPLY_ALREADY_DISCOVERING;
            break;
        }
        ++it;
    }
    if (ALLJOYN_FINDADVERTISEDNAME_REPLY_SUCCESS == replyCode) {
        /* Add to discover map */
        discoverMap.insert(pair<String, String>(namePrefix, sender));

        /* Find name  on all remote transports */
        TransportList& transList = bus.GetInternal().GetTransportList();
        for (size_t i = 0; i < transList.GetNumTransports(); ++i) {
            Transport* trans = transList.GetTransport(i);
            if (trans) {
                trans->EnableDiscovery(namePrefix.c_str());
            } else {
                QCC_LogError(ER_BUS_TRANSPORT_NOT_AVAILABLE, ("NULL transport pointer found in transportList"));
            }
        }
    }

    /* Reply to request */
    MsgArg replyArg("u", replyCode);
    QStatus status = MethodReply(msg, &replyArg, 1);
    QCC_DbgPrintf(("AllJoynObj::Discover(%s) returned %d (status=%s)", namePrefix.c_str(), replyCode, QCC_StatusText(status)));

    /* Log error if reply could not be sent */
    if (ER_OK != status) {
        QCC_LogError(status, ("Failed to respond to org.alljoyn.Bus.Discover"));
    }

    /* Send FoundAdvertisedName signals if there are existing matches for namePrefix */
    if (ALLJOYN_FINDADVERTISEDNAME_REPLY_SUCCESS == replyCode) {
        multimap<String, NameMapEntry>::iterator it = nameMap.lower_bound(namePrefix);
        while ((it != nameMap.end()) && (0 == strncmp(it->first.c_str(), namePrefix.c_str(), namePrefix.size()))) {
            status = SendFoundAdvertisedName(sender, it->first, it->second.transport, namePrefix);
            if (ER_OK != status) {
                QCC_LogError(status, ("Cannot send FoundAdvertisedName to %s for name=%s", sender.c_str(), it->first.c_str()));
            }
            ++it;
        }
    }
    discoverMapLock.Unlock();
    router.UnlockNameTable();
}

void AllJoynObj::CancelFindAdvertisedName(const InterfaceDescription::Member* member, Message& msg)
{
    size_t numArgs;
    const MsgArg* args;

    /* Get the name prefix to be removed from discovery */
    msg->GetArgs(numArgs, args);
    assert((numArgs == 1) && (args[0].typeId == ALLJOYN_STRING));

    /* Cancel advertisement */
    QCC_DbgPrintf(("Calling ProcCancelFindName from CancelFindAdvertisedName [%s]", Thread::GetThread()->GetName().c_str()));
    QStatus status = ProcCancelFindName(msg->GetSender(), args[0].v_string.str);
    uint32_t replyCode = (ER_OK == status) ? ALLJOYN_CANCELFINDADVERTISEDNAME_REPLY_SUCCESS : ALLJOYN_CANCELFINDADVERTISEDNAME_REPLY_FAILED;

    /* Reply to request */
    MsgArg replyArg("u", replyCode);
    status = MethodReply(msg, &replyArg, 1);
    // QCC_DbgPrintf(("AllJoynObj::CancelDiscover(%s) returned %d (status=%s)", args[0].v_string.str, replyCode, QCC_StatusText(status)));

    /* Log error if reply could not be sent */
    if (ER_OK != status) {
        QCC_LogError(status, ("Failed to respond to org.alljoyn.Bus.CancelDiscover"));
    }
}

QStatus AllJoynObj::ProcCancelFindName(const qcc::String& sender, const qcc::String& namePrefix)
{
    QCC_DbgTrace(("AllJoynObj::ProcCancelFindName(sender = %s, namePrefix = %s)", sender.c_str(), namePrefix.c_str()));
    QStatus status = ER_OK;

    /* Check to see if this prefix exists and delete it */
    bool foundNamePrefix = false;
    discoverMapLock.Lock();
    multimap<qcc::String, qcc::String>::iterator it = discoverMap.find(namePrefix);
    while ((it != discoverMap.end()) && (it->first == namePrefix)) {
        if (it->second == sender) {
            discoverMap.erase(it);
            foundNamePrefix = true;
            break;
        }
        ++it;
    }

    /* Disable discovery if we found a name */
    if (foundNamePrefix) {
        TransportList& transList = bus.GetInternal().GetTransportList();
        for (size_t i = 0; i < transList.GetNumTransports(); ++i) {
            Transport* trans =  transList.GetTransport(i);
            if (trans) {
                trans->DisableDiscovery(namePrefix.c_str());
            } else {
                QCC_LogError(ER_BUS_TRANSPORT_NOT_AVAILABLE, ("NULL transport pointer found in transportList"));
            }
        }
    } else if (!foundNamePrefix) {
        status = ER_FAIL;
    }
    discoverMapLock.Unlock();
    return status;
}

QStatus AllJoynObj::AddBusToBusEndpoint(RemoteEndpoint& endpoint)
{
    QCC_DbgTrace(("AllJoynObj::AddBusToBusEndpoint(%s)", endpoint.GetUniqueName().c_str()));

    const qcc::String& shortGuidStr = endpoint.GetRemoteGUID().ToShortString();

    /* Add b2b endpoint */
    b2bEndpointsLock.Lock();
    b2bEndpoints[endpoint.GetUniqueName()] = &endpoint;
    b2bEndpointsLock.Unlock();

    /* Create a virtual endpoint for talking to the remote bus control object */
    /* This endpoint will also carry broadcast messages for the remote bus */
    String remoteControllerName(":", 1, 16);
    remoteControllerName.append(shortGuidStr);
    remoteControllerName.append(".1");
    AddVirtualEndpoint(remoteControllerName, endpoint);

    /* Exchange existing bus names if connected to another daemon */
    return ExchangeNames(endpoint);
}

void AllJoynObj::RemoveBusToBusEndpoint(RemoteEndpoint& endpoint)
{
    QCC_DbgTrace(("AllJoynObj::RemoveBusToBusEndpoint(%s)", endpoint.GetUniqueName().c_str()));

    /* Remove any virtual endpoints associated with a removed bus-to-bus endpoint */

    /* Be careful to lock the name table before locking the virtual endpoints since both locks are needed
     * and doing it in the opposite order invites deadlock
     */
    router.LockNameTable();
    virtualEndpointsLock.Lock();
    map<qcc::String, VirtualEndpoint*>::iterator it = virtualEndpoints.begin();
    while (it != virtualEndpoints.end()) {
        if (it->second->RemoveBusToBusEndpoint(endpoint)) {

            /* Remove virtual endpoint with no more b2b eps */
            String exitingEpName = it->second->GetUniqueName();
            RemoveVirtualEndpoint(*(it++->second));

            /* Let directly connected daemons know that this virtual endpoint is gone. */
            b2bEndpointsLock.Lock();
            map<qcc::StringMapKey, RemoteEndpoint*>::iterator it2 = b2bEndpoints.begin();
            while (it2 != b2bEndpoints.end()) {
                if (it2->second != &endpoint) {
                    Message sigMsg(bus);
                    MsgArg args[3];
                    args[0].Set("s", exitingEpName.c_str());
                    args[1].Set("s", exitingEpName.c_str());
                    args[2].Set("s", "");

                    QStatus status = sigMsg->SignalMsg("sss",
                                                       org::alljoyn::Daemon::WellKnownName,
                                                       0,
                                                       org::alljoyn::Daemon::ObjectPath,
                                                       org::alljoyn::Daemon::InterfaceName,
                                                       "NameChanged",
                                                       args,
                                                       ArraySize(args),
                                                       0,
                                                       0);
                    if (ER_OK == status) {
                        status = it2->second->PushMessage(sigMsg);
                    }
                    if (ER_OK != status) {
                        QCC_LogError(status, ("Failed to send NameChanged to %s", it2->second->GetUniqueName().c_str()));
                    }
                }
                ++it2;
            }
            b2bEndpointsLock.Unlock();
        } else {
            ++it;
        }
    }
    virtualEndpointsLock.Unlock();
    router.UnlockNameTable();

    /* Remove the B2B endpoint itself */
    b2bEndpointsLock.Lock();
    b2bEndpoints.erase(endpoint.GetUniqueName());
    b2bEndpointsLock.Unlock();
}

QStatus AllJoynObj::ExchangeNames(RemoteEndpoint& endpoint)
{
    vector<pair<qcc::String, vector<qcc::String> > > names;
    QStatus status;
    const qcc::String& shortGuidStr = endpoint.GetRemoteGUID().ToShortString();
    const size_t shortGuidLen = shortGuidStr.size();

    /* Send local name table info to remote bus controller */
    router.LockNameTable();
    router.GetUniqueNamesAndAliases(names);

    MsgArg argArray(ALLJOYN_ARRAY);
    MsgArg* entries = new MsgArg[names.size()];
    size_t numEntries = 0;
    vector<pair<qcc::String, vector<qcc::String> > >::const_iterator it = names.begin();

    /* Send all endpoint info except for endpoints related to destination */
    while (it != names.end()) {
        if ((it->first.size() <= shortGuidLen) || (0 != it->first.compare(1, shortGuidLen, shortGuidStr))) {
            MsgArg* aliasNames = new MsgArg[it->second.size()];
            vector<qcc::String>::const_iterator ait = it->second.begin();
            size_t numAliases = 0;
            while (ait != it->second.end()) {
                /* Send exportable endpoints */
                // if ((ait->size() > guidLen) && (0 == ait->compare(ait->size() - guidLen, guidLen, guidStr))) {
                if (true) {
                    aliasNames[numAliases++].Set("s", ait->c_str());
                }
                ++ait;
            }
            if (0 < numAliases) {
                entries[numEntries].Set("(sa*)", it->first.c_str(), numAliases, aliasNames);
                /*
                 * Set ownwership flag so entries array destructor will free inner message args.
                 */
                entries[numEntries].SetOwnershipFlags(MsgArg::OwnsArgs, true);
            } else {
                entries[numEntries].Set("(sas)", it->first.c_str(), 0, NULL);
                delete[] aliasNames;
            }
            ++numEntries;
        }
        ++it;
    }
    status = argArray.Set("a(sas)", numEntries, entries);
    if (ER_OK == status) {
        Message exchangeMsg(bus);
        status = exchangeMsg->SignalMsg("a(sas)",
                                        org::alljoyn::Daemon::WellKnownName,
                                        0,
                                        org::alljoyn::Daemon::ObjectPath,
                                        org::alljoyn::Daemon::InterfaceName,
                                        "ExchangeNames",
                                        &argArray,
                                        1,
                                        0,
                                        0);
        if (ER_OK == status) {
            status = endpoint.PushMessage(exchangeMsg);
        }
    }
    /*
     * This will also free the inner MsgArgs.
     */
    delete [] entries;

    router.UnlockNameTable();

    return status;
}

void AllJoynObj::ExchangeNamesSignalHandler(const InterfaceDescription::Member* member, const char* sourcePath, Message& msg)
{
    QCC_DbgTrace(("AllJoynObj::ExchangeNamesSignalHandler(msg sender = \"%s\")", msg->GetSender()));

    bool madeChanges = false;
    size_t numArgs;
    const MsgArg* args;
    msg->GetArgs(numArgs, args);
    assert((1 == numArgs) && (ALLJOYN_ARRAY == args[0].typeId));
    const MsgArg* items = args[0].v_array.GetElements();
    const String& shortGuidStr = guid.ToShortString();

    /* Create a virtual endpoint for each unique name in args */
    /* Be careful to lock the name table before locking the virtual endpoints since both locks are needed
     * and doing it in the opposite order invites deadlock
     */
    router.LockNameTable();
    virtualEndpointsLock.Lock();
    b2bEndpointsLock.Lock();
    map<qcc::StringMapKey, RemoteEndpoint*>::iterator bit = b2bEndpoints.find(msg->GetRcvEndpointName());
    const size_t numItems = args[0].v_array.GetNumElements();
    if (bit != b2bEndpoints.end()) {
        for (size_t i = 0; i < numItems; ++i) {
            assert(items[i].typeId == ALLJOYN_STRUCT);
            qcc::String uniqueName = items[i].v_struct.members[0].v_string.str;
            if (!IsLegalUniqueName(uniqueName.c_str())) {
                QCC_LogError(ER_FAIL, ("Invalid unique name \"%s\" in ExchangeNames message", uniqueName.c_str()));
                continue;
            } else if (0 == ::strncmp(uniqueName.c_str() + 1, shortGuidStr.c_str(), shortGuidStr.size())) {
                /* Cant accept a request to change a local name */
                continue;
            } else if (uniqueName == msg->GetSender()) {
                /* Ignore all bus controller object that we recieved this from since its virtual endpoint is preset (assumed) */
                continue;
            }

            bool madeChange;
            VirtualEndpoint& vep = AddVirtualEndpoint(uniqueName, *(bit->second), &madeChange);
            if (madeChange) {
                madeChanges = true;
            }

            /* Add virtual aliases (remote well-known names) */
            const MsgArg* aliasItems = items[i].v_struct.members[1].v_array.GetElements();
            const size_t numAliases = items[i].v_struct.members[1].v_array.GetNumElements();
            for (size_t j = 0; j < numAliases; ++j) {
                assert(ALLJOYN_STRING == aliasItems[j].typeId);
                bool madeChange = router.SetVirtualAlias(aliasItems[j].v_string.str, &vep, vep);
                if (madeChange) {
                    madeChanges = true;
                }
            }
        }
        b2bEndpointsLock.Unlock();
        virtualEndpointsLock.Unlock();
        router.UnlockNameTable();
    } else {
        b2bEndpointsLock.Unlock();
        virtualEndpointsLock.Unlock();
        router.UnlockNameTable();
        QCC_LogError(ER_BUS_NO_ENDPOINT, ("Cannot find b2b endpoint %s", msg->GetRcvEndpointName()));
    }

    /* If there were changes, forward message to all directly connected controllers except the one that
     * sent us this ExchangeNames
     */
    if (madeChanges) {
        set<String> sendSet;
        router.LockNameTable();
        b2bEndpointsLock.Lock();
        bool isRemarshaled = false;
        map<qcc::StringMapKey, RemoteEndpoint*>::const_iterator bit = b2bEndpoints.find(msg->GetRcvEndpointName());
        map<qcc::StringMapKey, RemoteEndpoint*>::iterator it = b2bEndpoints.begin();
        while (it != b2bEndpoints.end()) {
            if ((bit == b2bEndpoints.end()) || (bit->second->GetRemoteGUID() != it->second->GetRemoteGUID())) {
                msg->ReMarshal(bus.GetInternal().GetLocalEndpoint().GetUniqueName().c_str(), true);
                QStatus status = it->second->PushMessage(msg);
                if (ER_OK != status) {
                    QCC_LogError(status, ("Failed to forward NameChanged to %s", it->second->GetUniqueName().c_str()));
                }
            }
            ++it;
        }
        b2bEndpointsLock.Unlock();
        router.UnlockNameTable();
    }
}

void AllJoynObj::NameChangedSignalHandler(const InterfaceDescription::Member* member, const char* sourcePath, Message& msg)
{
    size_t numArgs;
    const MsgArg* args;
    msg->GetArgs(numArgs, args);

    assert(daemonIface);

    const qcc::String alias = args[0].v_string.str;
    const qcc::String oldOwner = args[1].v_string.str;
    const qcc::String newOwner = args[2].v_string.str;

    const String& shortGuidStr = guid.ToShortString();
    bool madeChanges = false;

    QCC_DbgPrintf(("AllJoynObj::NameChangedSignalHandler: alias = \"%s\"   oldOwner = \"%s\"   newOwner = \"%s\"  sent from \"%s\"",
                   alias.c_str(), oldOwner.c_str(), newOwner.c_str(), msg->GetSender()));

    /* Don't allow a NameChange that attempts to change a local name */
    if ((!oldOwner.empty() && (0 == ::strncmp(oldOwner.c_str() + 1, shortGuidStr.c_str(), shortGuidStr.size()))) ||
        (!newOwner.empty() && (0 == ::strncmp(newOwner.c_str() + 1, shortGuidStr.c_str(), shortGuidStr.size())))) {
        return;
    }

    if (alias[0] == ':') {
        router.LockNameTable();
        b2bEndpointsLock.Lock();
        map<qcc::StringMapKey, RemoteEndpoint*>::iterator bit = b2bEndpoints.find(msg->GetRcvEndpointName());
        if (bit != b2bEndpoints.end()) {
            /* Change affects a remote unique name (i.e. a VirtualEndpoint) */
            if (newOwner.empty()) {
                VirtualEndpoint* vep = FindVirtualEndpoint(oldOwner.c_str());
                if (vep) {
                    madeChanges = vep->CanUseRoute(*(bit->second));
                    if (vep->RemoveBusToBusEndpoint(*(bit->second))) {
                        RemoveVirtualEndpoint(*vep);
                    }
                }
            } else {
                /* Add a new virtual endpoint */
                if (bit != b2bEndpoints.end()) {
                    AddVirtualEndpoint(alias, *(bit->second), &madeChanges);
                }
            }
        } else {
            QCC_LogError(ER_BUS_NO_ENDPOINT, ("Cannot find bus-to-bus endpoint %s", msg->GetRcvEndpointName()));
        }
        b2bEndpointsLock.Unlock();
        router.UnlockNameTable();
    } else {
        /* Change affects a well-known name (name table only) */
        VirtualEndpoint* remoteController = FindVirtualEndpoint(msg->GetSender());
        if (remoteController) {
            VirtualEndpoint* newOwnerEp = newOwner.empty() ? NULL : FindVirtualEndpoint(newOwner.c_str());
            madeChanges = router.SetVirtualAlias(alias, newOwnerEp, *remoteController);
        } else {
            QCC_LogError(ER_BUS_NO_ENDPOINT, ("Cannot find virtual endpoint %s", msg->GetSender()));
        }
    }

    if (madeChanges) {
        /* Forward message to all directly connected controllers except the one that sent us this NameChanged */
        router.LockNameTable();
        b2bEndpointsLock.Lock();
        map<qcc::StringMapKey, RemoteEndpoint*>::const_iterator bit = b2bEndpoints.find(msg->GetRcvEndpointName());
        map<qcc::StringMapKey, RemoteEndpoint*>::iterator it = b2bEndpoints.begin();
        bool isReMarshaled = false;
        while (it != b2bEndpoints.end()) {
            if ((bit == b2bEndpoints.end()) || (bit->second->GetRemoteGUID() != it->second->GetRemoteGUID())) {
                if (!isReMarshaled) {
                    isReMarshaled = true;
                    msg->ReMarshal(bus.GetInternal().GetLocalEndpoint().GetUniqueName().c_str(), true);
                }
                QStatus status = it->second->PushMessage(msg);
                if (ER_OK != status) {
                    QCC_LogError(status, ("Failed to forward NameChanged to %s", it->second->GetUniqueName().c_str()));
                }
            }
            ++it;
        }
        b2bEndpointsLock.Unlock();
        router.UnlockNameTable();
    }

}

VirtualEndpoint& AllJoynObj::AddVirtualEndpoint(const qcc::String& uniqueName, RemoteEndpoint& busToBusEndpoint, bool* wasAdded)
{
    QCC_DbgTrace(("AllJoynObj::AddVirtualEndpoint(name=%s, b2b=%s)", uniqueName.c_str(), busToBusEndpoint.GetUniqueName().c_str()));

    bool added = false;
    VirtualEndpoint* vep = NULL;

    virtualEndpointsLock.Lock();
    map<qcc::String, VirtualEndpoint*>::iterator it = virtualEndpoints.find(uniqueName);
    if (it == virtualEndpoints.end()) {
        /* Add new virtual endpoint */
        pair<map<qcc::String, VirtualEndpoint*>::iterator, bool> ret =

            virtualEndpoints.insert(pair<qcc::String, VirtualEndpoint*>(uniqueName,
                                                                        new VirtualEndpoint(uniqueName.c_str(), busToBusEndpoint)));
        vep = ret.first->second;
        added = true;
    } else {
        /* Add the busToBus endpoint to the existing virtual endpoint */
        vep = it->second;
        added = vep->AddBusToBusEndpoint(busToBusEndpoint);
    }
    virtualEndpointsLock.Unlock();

    /* Register the endpoint with the router */
    router.RegisterEndpoint(*vep, false);

    if (wasAdded) {
        *wasAdded = added;
    }

    return *vep;
}

void AllJoynObj::RemoveVirtualEndpoint(VirtualEndpoint& vep)
{
    QCC_DbgTrace(("RemoveVirtualEndpoint: %s", vep.GetUniqueName().c_str()));

    /* Remove virtual endpoint along with any aliases that exist for this uniqueName */
    /* Be careful to lock the name table before locking the virtual endpoints since both locks are needed
     * and doing it in the opposite order invites deadlock
     */
    router.LockNameTable();
    virtualEndpointsLock.Lock();
    router.RemoveVirtualAliases(vep);
    router.UnregisterEndpoint(vep);
    virtualEndpoints.erase(vep.GetUniqueName());
    delete &vep;
    virtualEndpointsLock.Unlock();
    router.UnlockNameTable();
}

VirtualEndpoint* AllJoynObj::FindVirtualEndpoint(const qcc::String& uniqueName)
{
    VirtualEndpoint* ret = NULL;
    virtualEndpointsLock.Lock();
    map<qcc::String, VirtualEndpoint*>::iterator it = virtualEndpoints.find(uniqueName);
    if (it != virtualEndpoints.end()) {
        ret = it->second;
    }
    virtualEndpointsLock.Unlock();
    return ret;
}

void AllJoynObj::NameOwnerChanged(const qcc::String& alias, const qcc::String* oldOwner, const qcc::String* newOwner)
{
    QStatus status;
    const String& shortGuidStr = guid.ToShortString();

    /* Validate that there is either a new owner or an old owner */
    const qcc::String* un = oldOwner ? oldOwner : newOwner;
    if (NULL == un) {
        QCC_LogError(ER_BUS_NO_ENDPOINT, ("Invalid NameOwnerChanged without oldOwner or newOwner"));
        return;
    }

    /* Validate format of unique name */
    size_t guidLen = un->find_first_of('.');
    if ((qcc::String::npos == guidLen) || (guidLen < 3)) {
        QCC_LogError(ER_FAIL, ("Invalid unique name \"%s\"", un->c_str()));
    }

    /* Ignore name changes that involve any bus controller endpoint */
    if (0 == ::strcmp(un->c_str() + guidLen, ".1")) {
        return;
    }

    /* Remove unique names from sessionsMap entries */
    if (!newOwner && (alias[0] == ':')) {
        sessionMapLock.Lock();
        map<pair<String, SessionId>, SessionMapEntry>::iterator it = sessionMap.begin();
        while (it != sessionMap.end()) {
            /* Remove member entries */
            vector<String>::iterator mit = it->second.memberNames.begin();
            while (mit != it->second.memberNames.end()) {
                if (*mit == alias) {
                    it->second.memberNames.erase(mit);
                    break;
                }
                ++mit;
            }
            /* Remove session entry */
            if (it->first.first == alias) {
                // TODO CALL LOST SESSION
                sessionMap.erase(it++);
            } else {
                ++it;
            }
        }
        sessionMapLock.Unlock();
    }

    /* Only if local name */
    if (0 == ::strncmp(shortGuidStr.c_str(), un->c_str() + 1, shortGuidStr.size())) {

        /* Send NameChanged to all directly connected controllers */
        router.LockNameTable();
        b2bEndpointsLock.Lock();
        map<qcc::StringMapKey, RemoteEndpoint*>::iterator it = b2bEndpoints.begin();
        while (it != b2bEndpoints.end()) {
            const qcc::String& un = it->second->GetUniqueName();
            Message sigMsg(bus);
            MsgArg args[3];
            args[0].Set("s", alias.c_str());
            args[1].Set("s", oldOwner ? oldOwner->c_str() : "");
            args[2].Set("s", newOwner ? newOwner->c_str() : "");

            status = sigMsg->SignalMsg("sss",
                                       org::alljoyn::Daemon::WellKnownName,
                                       0,
                                       org::alljoyn::Daemon::ObjectPath,
                                       org::alljoyn::Daemon::InterfaceName,
                                       "NameChanged",
                                       args,
                                       ArraySize(args),
                                       0,
                                       0);
            if (ER_OK == status) {
                status = it->second->PushMessage(sigMsg);
            }
            if (ER_OK != status) {
                QCC_LogError(status, ("Failed to send NameChanged to %s", un.c_str()));
            }
            ++it;
        }
        b2bEndpointsLock.Unlock();

        /* If a local well-known name dropped, then remove any nameMap entry */
        if ((NULL == newOwner) && (alias[0] != ':')) {
            multimap<String, NameMapEntry>::const_iterator it = nameMap.lower_bound(alias);
            while ((it != nameMap.end()) && (it->first == alias)) {
                if (it->second.transport & TRANSPORT_LOCAL) {
                    vector<String> names;
                    names.push_back(alias);
                    FoundNames("local:", it->second.guid, TRANSPORT_LOCAL, &names, 0);
                    break;
                }
                ++it;
            }
        }
        router.UnlockNameTable();

        /* If a local unique name dropped, then remove any refs it had in the connnect, advertise and discover maps */
        if ((NULL == newOwner) && (alias[0] == ':')) {
            /* Remove endpoint refs from connect map */
            qcc::String last;
            router.LockNameTable();
            connectMapLock.Lock();
            multimap<qcc::String, qcc::String>::iterator it = connectMap.begin();
            while (it != connectMap.end()) {
                if (it->second == *oldOwner) {
                    bool isFirstSpec = (last != it->first);
                    qcc::String lastOwner;
                    do {
                        last = it->first;
                        connectMap.erase(it++);
                    } while ((connectMap.end() != it) && (last == it->first) && (*oldOwner == it->second));
                    if (isFirstSpec && ((connectMap.end() == it) || (last != it->first))) {
                        QStatus status = bus.Disconnect(last.c_str());
                        if (ER_OK != status) {
                            QCC_LogError(status, ("Failed to disconnect connect spec %s", last.c_str()));
                        }
                    }
                } else {
                    last = it->first;
                    ++it;
                }
            }
            connectMapLock.Unlock();

            /* Remove endpoint refs from advertise map */
            advertiseMapLock.Lock();
            multimap<String, pair<TransportMask, String> >::const_iterator ait = advertiseMap.begin();
            while (ait != advertiseMap.end()) {
                if (ait->second.second == *oldOwner) {
                    String name = ait->first;
                    TransportMask mask = ait->second.first;
                    ++ait;
                    QStatus status = ProcCancelAdvertise(*oldOwner, name, mask);
                    if (ER_OK != status) {
                        QCC_LogError(status, ("Failed to cancel advertise for name \"%s\"", name.c_str()));
                    }
                } else {
                    ++ait;
                }
            }
            advertiseMapLock.Unlock();

            /* Remove endpoint refs from discover map */
            discoverMapLock.Lock();
            it = discoverMap.begin();
            while (it != discoverMap.end()) {
                if (it->second == *oldOwner) {
                    last = it++->first;
                    QCC_DbgPrintf(("Calling ProcCancelFindName from NameOwnerChanged [%s]", Thread::GetThread()->GetName().c_str()));
                    QStatus status = ProcCancelFindName(*oldOwner, last);
                    if (ER_OK != status) {
                        QCC_LogError(status, ("Failed to cancel discover for name \"%s\"", last.c_str()));
                    }
                } else {
                    ++it;
                }
            }
            discoverMapLock.Unlock();
            router.UnlockNameTable();
        }
    }
}

void AllJoynObj::FoundNames(const qcc::String& busAddr,
                            const qcc::String& guid,
                            TransportMask transport,
                            const vector<qcc::String>* names,
                            uint8_t ttl)
{
    QCC_DbgTrace(("AllJoynObj::FoundNames(busAddr = \"%s\", guid = \"%s\", *names = %p, ttl = %d)",
                  busAddr.c_str(), guid.c_str(), names, ttl));

    if (NULL == foundNameSignal) {
        return;
    }
    /* If name is NULL expire all names for the given bus address. */
    if (names == NULL) {
        if (ttl == 0) {
            router.LockNameTable();
            discoverMapLock.Lock();
            multimap<String, NameMapEntry>::iterator it = nameMap.begin();
            while (it != nameMap.end()) {
                NameMapEntry& nme = it->second;
                if ((nme.guid == guid) && (nme.busAddr == busAddr)) {
                    SendLostAdvertisedName(it->first, transport);
                    nameMap.erase(it++);
                } else {
                    it++;
                }
            }
            discoverMapLock.Unlock();
            router.UnlockNameTable();
        }
        return;
    }

    /* Generate a list of name deltas */
    router.LockNameTable();
    discoverMapLock.Lock();
    vector<String>::const_iterator nit = names->begin();
    while (nit != names->end()) {
        multimap<String, NameMapEntry>::iterator it = nameMap.find(*nit);
        bool isNew = true;
        while ((it != nameMap.end()) && (*nit == it->first)) {
            if ((it->second.guid == guid) && (it->second.transport & transport)) {
                isNew = false;
                break;
            }
            ++it;
        }
        if (0 < ttl) {
            if (isNew) {
                /* Add new name to map */
                nameMap.insert(pair<String, NameMapEntry>(*nit, NameMapEntry(busAddr,
                                                                             guid,
                                                                             transport,
                                                                             (ttl == numeric_limits<uint8_t>::max()) ? numeric_limits<uint32_t>::max() : (1000 * ttl))));

                /* Send FoundAdvertisedName to anyone who is discovering *nit */
                if (0 < discoverMap.size()) {
                    multimap<String, String>::const_iterator dit = discoverMap.lower_bound((*nit)[0]);
                    while ((dit != discoverMap.end()) && (dit->first.compare(*nit) <= 0)) {
                        if (nit->compare(0, dit->first.size(), dit->first) == 0) {
                            QStatus status = SendFoundAdvertisedName(dit->second, *nit, transport, dit->first);
                            if (ER_OK != status) {
                                QCC_LogError(status, ("Failed to send FoundAdvertisedName to %s (name=%s)", dit->second.c_str(), nit->c_str()));
                            }
                        }
                        ++dit;
                    }
                }
            } else {
                /*
                 * If the busAddr doesn't match, then this is actually a new but redundant advertsement.
                 * Don't track it. Don't updated the TTL for the existing advertisment with the same name
                 * and don't tell clients about this alternate way to connect to the name
                 * since it will look like a duplicate to the client (that doesn't receive busAddr).
                 */
                if (busAddr == it->second.busAddr) {
                    it->second.timestamp = GetTimestamp();
                }
            }
            nameMapReaper.Alert();
        } else {
            /* 0 == ttl means flush the record */
            if (!isNew) {
                SendLostAdvertisedName(it->first, transport);
                nameMap.erase(it);
            }
        }
        ++nit;
    }
    discoverMapLock.Unlock();
    router.UnlockNameTable();

}

QStatus AllJoynObj::SendFoundAdvertisedName(const String& dest,
                                            const String& name,
                                            TransportMask transport,
                                            const String& namePrefix)
{
    QCC_DbgTrace(("AllJoynObj::SendFoundAdvertisedName(%s, %s, 0x%x, %s)", dest.c_str(), name.c_str(), transport, namePrefix.c_str()));

    MsgArg args[3];
    args[0].Set("s", name.c_str());
    args[1].Set("q", transport);
    args[2].Set("s", namePrefix.c_str());
    return Signal(dest.c_str(), 0, *foundNameSignal, args, ArraySize(args));
}

QStatus AllJoynObj::SendLostAdvertisedName(const String& name, TransportMask transport)
{
    QCC_DbgTrace(("AllJoynObj::SendLostAdvertisdName(%s, 0x%x)", name.c_str(), transport));

    QStatus status = ER_OK;

    /* Send LostAdvertisedName to anyone who is discovering name */
    router.LockNameTable();
    discoverMapLock.Lock();
    if (0 < discoverMap.size()) {
        multimap<String, String>::const_iterator dit = discoverMap.lower_bound(name[0]);
        while ((dit != discoverMap.end()) && (dit->first.compare(name) <= 0)) {
            if (name.compare(0, dit->first.size(), dit->first) == 0) {
                MsgArg args[3];
                args[0].Set("s", name.c_str());
                args[1].Set("q", transport);
                args[2].Set("s", dit->first.c_str());
                QCC_DbgPrintf(("Sending LostAdvertisedName(%s, 0x%x, %s) to %s", name.c_str(), transport, dit->first.c_str(), dit->second.c_str()));
                QStatus tStatus = Signal(dit->second.c_str(), 0, *lostAdvNameSignal, args, ArraySize(args));
                if (ER_OK != tStatus) {
                    status = (ER_OK == status) ? tStatus : status;
                    QCC_LogError(tStatus, ("Failed to send LostAdvertisedName to %s (name=%s)", dit->second.c_str(), name.c_str()));
                }
            }
            ++dit;
        }
    }
    discoverMapLock.Unlock();
    router.UnlockNameTable();
    return status;
}

ThreadReturn STDCALL AllJoynObj::NameMapReaperThread::Run(void* arg)
{
    uint32_t waitTime(Event::WAIT_FOREVER);
    Event evt(waitTime);
    while (!IsStopping()) {
        ajnObj->router.LockNameTable();
        ajnObj->discoverMapLock.Lock();
        set<qcc::String> expiredBuses;
        multimap<String, NameMapEntry>::iterator it = ajnObj->nameMap.begin();
        uint32_t now = GetTimestamp();
        waitTime = Event::WAIT_FOREVER;
        while (it != ajnObj->nameMap.end()) {
            if ((now - it->second.timestamp) >= it->second.ttl) {
                QCC_DbgPrintf(("Expiring discovered name %s for guid %s", it->first.c_str(), it->second.guid.c_str()));
                expiredBuses.insert(it->second.busAddr);
                ajnObj->SendLostAdvertisedName(it->first, it->second.transport);
                ajnObj->nameMap.erase(it++);
            } else {
                if (it->second.ttl != numeric_limits<uint32_t>::max()) {
                    uint32_t nextTime(it->second.ttl - (now - it->second.timestamp));
                    if (nextTime < waitTime) {
                        waitTime = nextTime;
                    }
                }
                ++it;
            }
        }
        ajnObj->discoverMapLock.Unlock();
        ajnObj->router.UnlockNameTable();

        while (expiredBuses.begin() != expiredBuses.end()) {
            expiredBuses.erase(expiredBuses.begin());
        }

        evt.ResetTime(waitTime, 0);
        QStatus status = Event::Wait(evt);
        if (status == ER_ALERTED_THREAD) {
            stopEvent.ResetEvent();
        }
    }
    return 0;
}

void AllJoynObj::BusConnectionLost(const qcc::String& busAddr)
{
    /* Clear the connection map of this busAddress */
    connectMapLock.Lock();
    bool foundName = false;
    multimap<String, String>::iterator it = connectMap.lower_bound(busAddr);
    while ((it != connectMap.end()) && (0 == busAddr.compare(it->first))) {
        foundName = true;
        connectMap.erase(it++);
    }
    connectMapLock.Unlock();

    /* Send a signal to interested local clients */
    if (foundName && busConnLostSignal) {
        MsgArg arg;
        arg.Set("s", busAddr.c_str());
        QStatus status = Signal(NULL, 0, *busConnLostSignal, &arg, 1);
        if (ER_OK != status) {
            QCC_LogError(status, ("Failed to send BusConnectionLost signal"));
        }
    }
}

}
