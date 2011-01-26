/**
 * @file
 *
 * BusObject responsible for controlling/handling Bluetooth delegations and
 * implements the org.alljoyn.Bus.BTController interface.
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

#include <set>
#include <vector>

#include <qcc/Environ.h>
#include <qcc/String.h>
#include <qcc/StringUtil.h>

#include "BTController.h"

#define QCC_MODULE "ALLJOYN_BT"


using namespace std;
using namespace qcc;

#define MethodHander(_a) static_cast<MessageReceiver::MethodHandler>(_a)
#define SignalHander(_a) static_cast<MessageReceiver::SignalHandler>(_a)

#define DELEGATE_TIME 30
#define MAX_CACHE_SIZE 100

static const uint32_t ABSOLUTE_MAX_CONNECTIONS = 7; /* BT can't have more than 7 direct connections */
static const uint32_t DEFAULT_MAX_CONNECTIONS =  6; /* Gotta allow 1 connection for car-kit/headset/headphones */

namespace ajn {

struct InterfaceDesc {
    AllJoynMessageType type;
    const char* name;
    const char* inputSig;
    const char* outSig;
    const char* argNames;
    uint8_t annotation;
};

struct SignalEntry {
    const InterfaceDescription::Member* member;  /**< Pointer to signal's member */
    MessageReceiver::SignalHandler handler;      /**< Signal handler implementation */
};

static const char* bluetoothObjPath = "/org/alljoyn/Bus/BluetoothTopologyManager";
static const char* bluetoothTopoMgrIfcName = "org.alljoyn.Bus.BluetoothTopologyManager";

const InterfaceDesc btmIfcTable[] = {
    { MESSAGE_METHOD_CALL, "SetState",            "ya(sasas)", "a(sasas)", "directMinions,nameMaps,nameMaps",    0 },
    { MESSAGE_METHOD_CALL, "ProxyConnect",        "syq",       "u",        "bdAddr,channel,psm,status",          0 },
    { MESSAGE_METHOD_CALL, "ProxyDisconnect",     "s",         "u",        "bdAddr,status",                      0 },
    { MESSAGE_SIGNAL,      "FindName",            "ss",        NULL,       "requestor,findName",                 0 },
    { MESSAGE_SIGNAL,      "CancelFindName",      "ss",        NULL,       "requestor,findName",                 0 },
    { MESSAGE_SIGNAL,      "AdvertiseName",       "ss",        NULL,       "requestor,adName",                   0 },
    { MESSAGE_SIGNAL,      "CancelAdvertiseName", "ss",        NULL,       "requestor,adName",                   0 },
    { MESSAGE_SIGNAL,      "DelegateAdvertise",   "usyqas",    NULL,       "uuidRev,bdAddr,channel,psm,adNames", 0 },
    { MESSAGE_SIGNAL,      "DelegateFind",        "sus",       NULL,       "resultDest,ignoreUUIDRev,bdAddr",    0 },
    { MESSAGE_SIGNAL,      "FoundBus",            "ssyq",      NULL,       "guid,bdAddr,channel,psm,adNames",    0 },
    { MESSAGE_SIGNAL,      "FoundDevice",         "su",        NULL,       "bdAddr,uuidRev",                     0 }
};


BTController::BTController(BusAttachment& bus, BluetoothDeviceInterface& bt) :
    BusObject(bus, bluetoothObjPath),
    bus(bus),
    bt(bt),
    master(NULL),
    uuidRev(INVALID_UUIDREV),
    directMinions(0),
    maxConnections(min(StringToU32(Environ::GetAppEnviron()->Find("ALLJOYN_MAX_BT_CONNECTIONS"), 0, DEFAULT_MAX_CONNECTIONS),
                       ABSOLUTE_MAX_CONNECTIONS)),
    listening(false),
    advertise(*this),
    find(*this)
{
    while (uuidRev == INVALID_UUIDREV) {
        uuidRev = qcc::Rand32();
    }

    advertise.alarm = Alarm(DELEGATE_TIME * 1000, this, DELEGATE_TIME * 1000, NULL);
    advertise.minion = nodeStates.end();

    find.alarm = Alarm(DELEGATE_TIME * 1000, this, DELEGATE_TIME * 1000, NULL);
    find.minion = nodeStates.end();
    find.resultDest = bus.GetUniqueName();

    if (!org.alljoyn.Bus.BTController.interface) {
        InterfaceDescription* ifc;
        bus.CreateInterface(bluetoothTopoMgrIfcName, ifc);
        for (size_t i = 0; i < ArraySize(btmIfcTable); ++i) {
            ifc->AddMember(btmIfcTable[i].type,
                           btmIfcTable[i].name,
                           btmIfcTable[i].inputSig,
                           btmIfcTable[i].outSig,
                           btmIfcTable[i].argNames,
                           btmIfcTable[i].annotation);
        }
        ifc->Activate();

        org.alljoyn.Bus.BTController.interface =           ifc;
        org.alljoyn.Bus.BTController.SetState =            ifc->GetMember("SetState");
        org.alljoyn.Bus.BTController.ProxyConnect =        ifc->GetMember("ProxyConnect");
        org.alljoyn.Bus.BTController.ProxyDisconnect =     ifc->GetMember("ProxyDisconnect");
        org.alljoyn.Bus.BTController.FindName =            ifc->GetMember("FindName");
        org.alljoyn.Bus.BTController.CancelFindName =      ifc->GetMember("CancelFindName");
        org.alljoyn.Bus.BTController.AdvertiseName =       ifc->GetMember("AdvertiseName");
        org.alljoyn.Bus.BTController.CancelAdvertiseName = ifc->GetMember("CancelAdvertiseName");
        org.alljoyn.Bus.BTController.DelegateAdvertise =   ifc->GetMember("DelegateAdvertise");
        org.alljoyn.Bus.BTController.DelegateFind =        ifc->GetMember("DelegateFind");
        org.alljoyn.Bus.BTController.FoundBus =            ifc->GetMember("FoundBus");
        org.alljoyn.Bus.BTController.FoundDevice =         ifc->GetMember("FoundDevice");
    }
}


BTController::~BTController()
{
    bus.DeregisterBusObject(*this);
    if (master) {
        delete master;
    }
}


void BTController::ObjectRegistered() {
    /* Create an entry in the node state table for ourself and set the GUID
     * accordingly.  This will allow add and remove advertise and find name
     * functions to work properly.
     */
    nodeStates[bus.GetUniqueName()].guid = bus.GetGlobalGUIDString();
}


QStatus BTController::Init()
{
    if (org.alljoyn.Bus.BTController.interface == NULL) {
        QCC_LogError(ER_FAIL, ("Bluetooth topology manager interface not setup"));
        return ER_FAIL;
    }

    AddInterface(*org.alljoyn.Bus.BTController.interface);

    const MethodEntry methodEntries[] = {
        { org.alljoyn.Bus.BTController.SetState,        MethodHandler(&BTController::HandleSetState) },
        { org.alljoyn.Bus.BTController.ProxyConnect,    MethodHandler(&BTController::HandleProxyConnect) },
        { org.alljoyn.Bus.BTController.ProxyDisconnect, MethodHandler(&BTController::HandleProxyDisconnect) }
    };

    const SignalEntry signalEntries[] = {
        { org.alljoyn.Bus.BTController.FindName,            SignalHandler(&BTController::HandleNameSignal) },
        { org.alljoyn.Bus.BTController.CancelFindName,      SignalHandler(&BTController::HandleNameSignal) },
        { org.alljoyn.Bus.BTController.AdvertiseName,       SignalHandler(&BTController::HandleNameSignal) },
        { org.alljoyn.Bus.BTController.CancelAdvertiseName, SignalHandler(&BTController::HandleNameSignal) },
        { org.alljoyn.Bus.BTController.DelegateAdvertise,   SignalHandler(&BTController::HandleDelegateAdvertise) },
        { org.alljoyn.Bus.BTController.DelegateFind,        SignalHandler(&BTController::HandleDelegateFind) },
        { org.alljoyn.Bus.BTController.FoundBus,            SignalHandler(&BTController::HandleFoundBus) },
        { org.alljoyn.Bus.BTController.FoundDevice,         SignalHandler(&BTController::HandleFoundDevice) }
    };

    QStatus status = AddMethodHandlers(methodEntries, ArraySize(methodEntries));

    for (size_t i = 0; (status == ER_OK) && (i < ArraySize(signalEntries)); ++i) {
        status = bus.RegisterSignalHandler(this, signalEntries[i].handler, signalEntries[i].member, bluetoothObjPath);
    }

    if (status == ER_OK) {
        status = bus.RegisterBusObject(*this);
    }

    return status;
}


QStatus BTController::SendSetState(const qcc::String& busName)
{
    QCC_DbgTrace(("BTController::SendSetState(busName = %s)", busName.c_str()));
    assert(!master);
    nodeStateLock.Lock();

    NodeStateMap::const_iterator it;
    vector<MsgArg> array;
    MsgArg args[2];
    Message reply(bus);
    ProxyBusObject* newMaster = new ProxyBusObject(bus, busName.c_str(), bluetoothObjPath);

    args[0].Set("y", directMinions);

    array.reserve(nodeStates.size());

    for (it = nodeStates.begin(); it != nodeStates.end(); ++it) {
        vector<const char*> nodeAdNames;
        vector<const char*> nodeFindNames;
        set<qcc::String>::const_iterator nit;
        nodeAdNames.reserve(it->second.advertiseNames.size());
        nodeFindNames.reserve(it->second.findNames.size());
        for (nit = it->second.advertiseNames.begin(); nit != it->second.advertiseNames.end(); ++nit) {
            nodeAdNames.push_back(nit->c_str());
        }
        for (nit = it->second.findNames.begin(); nit != it->second.findNames.end(); ++nit) {
            nodeFindNames.push_back(nit->c_str());
        }
        array.push_back(MsgArg("(ssasas)", it->first.c_str(),
                               it->second.guid.c_str(),
                               nodeAdNames.size(), &nodeAdNames.front(),
                               nodeFindNames.size(), &nodeFindNames.front()));
    }
    args[1].Set("a(sasas)", array.size(), &array.front());

    QStatus status = newMaster->MethodCall(*org.alljoyn.Bus.BTController.SetState, args, ArraySize(args), reply);

    if (status == ER_OK) {
        if (reply->GetType() == MESSAGE_ERROR) {
            status = ER_FAIL;
            nodeStateLock.Unlock();
        } else {
            MsgArg* array;
            size_t num;
            reply->GetArg(0)->Get("a(ssasas)", &num, &array);
            if (num == 0) {
                // We are now a minion (or a drone if we have more than one direct connection)
                master = newMaster;
                nodeStateLock.Unlock();
            } else {
                // We are the still the master
                ImportState(num, array, busName);
                delete newMaster;
                nodeStateLock.Unlock();

                ++uuidRev;
                if (uuidRev == INVALID_UUIDREV) {
                    ++uuidRev;
                }
                advertise.uuidRev = uuidRev;

                assert(find.resultDest == bus.GetUniqueName());
                find.ignoreUUID = uuidRev;

            }

            if (!IsMinion()) {
                UpdateDelegations(advertise, listening);
                UpdateDelegations(find);
            }

            if (!IsMaster()) {
                bt.StopListen();
                listening = false;
            }

            if (IsDrone()) {
                // TODO - Move minion connections to new master
            }
        }
    }
    return status;
}


QStatus BTController::ProcessFoundDevice(const BDAddress& adBdAddr, uint32_t uuidRev)
{
    QCC_DbgTrace(("BTController::ProcessFoundDevice(adBdAddr = %s, uuidRev = %08x)", adBdAddr.ToString().c_str(), uuidRev));
    QStatus status = ER_OK;

    if (IsMaster()) {
        UUIDRevCacheInfo& ci = uuidRevCache[uuidRev];

        if (ci.uuidRev == INVALID_UUIDREV) {
            nodeStateLock.Lock();
            if (UseLocalFind()) {
                bt.StopFind();
                find.active = false;
            }

            status = bt.GetDeviceInfo(adBdAddr, ci.connAddr, ci.uuidRev, ci.channel, ci.psm, ci.adInfo);

            if (UseLocalFind()) {
                bt.StartFind(find.ignoreUUID);
                find.active = true;
            }
            nodeStateLock.Unlock();


            if (status != ER_OK) {
                uuidRevCache.erase(uuidRev);
                goto exit;
            }
        } else {
            // Pop the existing entry from the aging list so that it can be
            // added to the end (implments LRU cache policy).
            uuidRevCacheAging.erase(ci.position);
        }

        while (uuidRevCacheAging.size() > MAX_CACHE_SIZE) {
            // The cache has grown too large.  Take the opportunity to clear
            // out old information to keep resource utilization under control
            uuidRevCache.erase(uuidRevCacheAging.front());
            uuidRevCacheAging.pop_front();
        }

        uuidRevCacheAging.push_back(ci.uuidRev);
        ci.position = uuidRevCacheAging.end();
        if (ci.position != uuidRevCacheAging.begin()) {
            ++ci.position;
        }

        if (ci.uuidRev != uuidRev) {
            // Somehow the UUID revision was updated between the time the EIR
            // was sent and we performed an SDP query.  Thus we need to move
            // the cache information to the UUID rev from the SDP record since
            // we will be getting that the next time ProcessFoundDevice is
            // called for this remote bus.
            uuidRevCache[ci.uuidRev] = ci;
            uuidRevCache.erase(uuidRev);
        }

        for (NodeStateMap::iterator it = nodeStates.begin(); it != nodeStates.end(); ++it) {
            if (it->first == bus.GetUniqueName()) {
                BluetoothDeviceInterface::AdvertiseInfo::const_iterator aiit;
                // Tell ourself about the names
                for (aiit = ci.adInfo.begin(); aiit != ci.adInfo.end(); ++aiit) {
                    bt.FoundBus(ci.connAddr, aiit->first, aiit->second, ci.channel, ci.psm);
                }
            } else {
                // Send Found Bus information to the node(s) that is
                // interested in it.
                status = SendFoundBus(ci.connAddr, ci.channel, ci.psm, ci.adInfo, it->first.c_str());
            }
        }
    } else {
        // Must be a drone or slave.
        MsgArg args[2];
        size_t numArgs = sizeof(args);

        MsgArg::Set(args, numArgs, "su", adBdAddr.ToString().c_str(), uuidRev);

        status = Signal(find.resultDest.c_str(), *org.alljoyn.Bus.BTController.FoundDevice, args, numArgs);
    }

exit:
    return status;
}


void BTController::PrepConnect()
{
    nodeStateLock.Lock();
    if (UseLocalFind()) {
        /*
         * Gotta shut down the local find operation since it severely
         * interferes with our ability to establish a connection.  Also, after
         * a successful connection, the exchange of the SetState method call
         * and response will result in one side or the other taking control of
         * who performs the find operation.
         */
        bt.StopFind();
        find.active = false;
    }
    if (UseLocalAdvertise()) {
        /*
         * Gotta shut down the local advertise operation since a successful
         * connection will result in the exchange for the SetState method call
         * and response afterwhich the node responsibility for advertising
         * will be determined.
         */
        bt.StopAdvertise();
        advertise.active = false;
    }
    nodeStateLock.Unlock();
}


void BTController::PostConnect(QStatus status)
{
    if (status != ER_OK) {
        nodeStateLock.Lock();
        if (UseLocalFind()) {
            /*
             * Gotta restart the find operation since the connect failed and
             * we need to do the find for ourself.
             */
            bt.StartFind(find.ignoreUUID);
            find.active = true;
        }
        if (UseLocalAdvertise()) {
            /*
             * Gotta restart the advertise operation since the connect failed and
             * we need to do the advertise for ourself.
             */
            BluetoothDeviceInterface::AdvertiseInfo adInfo;
            MsgArg arg("a{sas}", advertise.adInfoArgs.size(), &advertise.adInfoArgs.front());
            ExtractAdInfo(arg, adInfo);
            advertise.uuidRev = uuidRev;
            bt.StartAdvertise(advertise.uuidRev, advertise.bdAddr, advertise.channel, advertise.psm, adInfo);
            advertise.active = true;
        }
        nodeStateLock.Unlock();
    }
}


QStatus BTController::ProxyConnect(const BDAddress& bdAddr,
                                   uint8_t channel,
                                   uint16_t psm,
                                   qcc::String* delegate)
{
    QCC_DbgTrace(("BTController::ProxyConnect(bdAddr = %s, channel = %u, psm %u)", bdAddr.ToString().c_str(), channel, psm));
    QStatus status = (directMinions < maxConnections) ? ER_OK : ER_BT_MAX_CONNECTIONS_USED;

    if (IsMaster()) {
        // The master cannot send a proxy connect.  (Who would do it?)
        status = ER_FAIL;
    } else if (status == ER_OK) {
        MsgArg args[3];
        size_t argsSize = ArraySize(args);
        Message reply(bus);

        MsgArg::Set(args, argsSize, "syq", bdAddr.ToString().c_str(), channel, psm);

        status = master->MethodCall(*org.alljoyn.Bus.BTController.ProxyConnect, args, argsSize, reply);
        if (status == ER_OK) {
            uint32_t s;
            reply->GetArg(0)->Get("u", &s);
            status = static_cast<QStatus>(s);
            if (delegate && (status == ER_OK)) {
                *delegate = master->GetServiceName();
            }
        }
    }

    return status;
}


QStatus BTController::ProxyDisconnect(const BDAddress& bdAddr)
{
    QCC_DbgTrace(("BTController::ProxyDisconnect(bdAddr = %s)", bdAddr.ToString().c_str()));
    QStatus status;

    if (IsMaster()) {
        // The master cannot send a proxy disconnect.  (Who would do it?)
        status = ER_FAIL;
    } else {
        MsgArg args[3];
        size_t argsSize = ArraySize(args);
        Message reply(bus);

        MsgArg::Set(args, argsSize, "s", bdAddr.ToString().c_str());

        status = master->MethodCall(*org.alljoyn.Bus.BTController.ProxyDisconnect, args, argsSize, reply);
        if (status == ER_OK) {
            uint32_t s;
            reply->GetArg(0)->Get("u", &s);
            status = static_cast<QStatus>(s);
        }
    }

    return status;
}


void BTController::NameOwnerChanged(const qcc::String& alias,
                                    const qcc::String* oldOwner,
                                    const qcc::String* newOwner)
{
    if (oldOwner && (alias == *oldOwner)) {
        if (master && master->GetServiceName() == alias) {
            // We are the master now.
            if (IsMinion()) {
                if (!find.names.empty()) {
                    bt.StopFind();
                }
                if (advertise.Count() > 0) {
                    bt.StopAdvertise();
                }
            }
            delete master;
            master = NULL;

            bt.StartListen(advertise.bdAddr, advertise.channel, advertise.psm);
            find.resultDest = bus.GetUniqueName();
        } else {
            nodeStateLock.Lock();
            NodeStateMap::iterator minion = nodeStates.find(alias);

            if (minion != nodeStates.end()) {
                if (find.minion == minion) {
                    NextDirectMinion(find.minion);
                }
                if (advertise.minion == minion) {
                    NextDirectMinion(advertise.minion);
                }
                if (minion->second.direct) {
                    --directMinions;
                }
                nodeStates.erase(minion);
            }
            nodeStateLock.Unlock();
        }

        if (IsMaster()) {
            UpdateDelegations(advertise, listening);
            UpdateDelegations(find);
        }
    }
}


QStatus BTController::SendFoundBus(const BDAddress& bdAddr,
                                   uint8_t channel,
                                   uint16_t psm,
                                   const BluetoothDeviceInterface::AdvertiseInfo& adInfo,
                                   const char* dest)
{
    QCC_DbgTrace(("BTController::SendFoundBus(bdAddr = %s, channel = %u, psm = %u, adInfo = <>, dest = \"%s\")",
                  bdAddr.ToString().c_str(), channel, psm, dest));
    if (!dest) {
        if (find.resultDest.empty()) {
            return ER_FAIL;
        }
        dest = find.resultDest.c_str();
    }

    MsgArg args[5];

    args[1].Set("s", bdAddr.ToString().c_str());
    args[2].Set("y", channel);
    args[3].Set("q", psm);

    EncodeAdInfo(adInfo, args[4]);

    return Signal(dest, *org.alljoyn.Bus.BTController.FoundBus, args, ArraySize(args));
}


QStatus BTController::SendFoundDevice(const BDAddress& bdAddr,
                                      uint32_t uuidRev)
{
    QCC_DbgTrace(("BTController::SendFoundDevice(bdAddr = %s, uuidRev = %08x)", bdAddr.ToString().c_str(), uuidRev));
    if (find.resultDest.empty()) {
        return ER_FAIL;
    }

    QStatus status = ER_OK;

    if (bdAddr != find.ignoreAddr) {
        const char* dest = find.resultDest.c_str();
        MsgArg args[2];
        size_t argsSize = ArraySize(args);

        MsgArg::Set(args, argsSize, "su", bdAddr.ToString().c_str(), uuidRev);
        assert(argsSize == ArraySize(args));

        status = Signal(dest, *org.alljoyn.Bus.BTController.FoundDevice, args, ArraySize(args));
    }

    return status;
}


QStatus BTController::DoNameOp(const qcc::String& name,
                               const InterfaceDescription::Member& signal,
                               bool add,
                               NameArgInfo& nameArgInfo)
{
    QCC_DbgTrace(("BTController::DoNameOp(name = %s, signal = %s, add = %s, nameArgInfo = <>)",
                  name.c_str(), signal.name.c_str(), add ? "true" : "false"));
    QStatus status = ER_OK;

    nodeStateLock.Lock();
    if (add) {
        nameArgInfo.AddName(name);
    } else {
        nameArgInfo.RemoveName(name);
    }

    if (IsMaster()) {
        if ((advertise.Count() > 0) && !listening && (directMinions < maxConnections)) {
            status = bt.StartListen(advertise.bdAddr, advertise.channel, advertise.psm);
            listening = (status == ER_OK);
        }

        UpdateDelegations(advertise, listening);
        UpdateDelegations(find);

        if ((advertise.Count() == 0) && listening) {
            bt.StopListen();
            listening = false;
        }
    } else {
        MsgArg args[2];
        qcc::String busName = bus.GetUniqueName();
        args[0].Set("s", busName.c_str());
        args[1].Set("s", name.c_str());
        status = Signal(master->GetServiceName().c_str(), signal, args, ArraySize(args));
    }
    nodeStateLock.Unlock();

    return status;
}


void BTController::HandleNameSignal(const InterfaceDescription::Member* member,
                                    const char* sourcePath,
                                    Message& msg)
{
    QCC_DbgTrace(("BTController::HandleNameSignal(member = %s, sourcePath = \"%s\", msg = <>)",
                  member->name.c_str(), sourcePath));
    if (IsMinion()) {
        // Minions should not be getting these signals.
        return;
    }

    bool fn = (*member == *org.alljoyn.Bus.BTController.FindName);
    bool cfn = (*member == *org.alljoyn.Bus.BTController.CancelFindName);
    bool an = (*member == *org.alljoyn.Bus.BTController.AdvertiseName);

    bool addName = (fn || an);
    NameArgInfo* nameCollection = (fn || cfn) ? static_cast<NameArgInfo*>(&find) : static_cast<NameArgInfo*>(&advertise);
    char* busName;
    char* name;

    msg->GetArgs("ss", &busName, &name);

    nodeStateLock.Lock();
    NodeStateMap::iterator it = nodeStates.find(busName);

    if (it != nodeStates.end()) {
        // All nodes need to be registered via SetState
        qcc::String n(name);
        if (addName) {
            nameCollection->AddName(n, it);
        } else {
            nameCollection->RemoveName(n, it);
        }

        if (IsMaster()) {
            UpdateDelegations(this->advertise, listening);
            UpdateDelegations(this->find);
        } else {
            // We are a drone so pass on the name
            const MsgArg* args;
            size_t numArgs;
            msg->GetArgs(numArgs, args);
            Signal(master->GetServiceName().c_str(), *member, args, numArgs);
        }
    }

    nodeStateLock.Unlock();
}


void BTController::HandleSetState(const InterfaceDescription::Member* member, Message& msg)
{
    QCC_DbgTrace(("BTController::HandleSetState(member = %s, msg = <>)", member->name.c_str()));
    if (!IsMaster()) {
        // We are not the master so we should not get a SetState method call.
        // Don't send a response as punishment >:)
        return;
    }

    uint8_t numConnections;
    nodeStateLock.Lock();
    msg->GetArg(0)->Get("y", &numConnections);
    if (numConnections > directMinions) {
        // We are now a minion (or a drone if we have more than one direct connection)
        master = new ProxyBusObject(bus, msg->GetSender(), bluetoothObjPath);
        MsgArg* array = new MsgArg[nodeStates.size()];
        NodeStateMap::const_iterator it;
        size_t cnt;

        for (it = nodeStates.begin(), cnt = 0; it != nodeStates.end(); ++it, ++cnt) {
            vector<qcc::String> nodeAdNames;
            nodeAdNames.reserve(it->second.advertiseNames.size());
            nodeAdNames.assign(it->second.advertiseNames.begin(),
                               it->second.advertiseNames.end());
            vector<qcc::String> nodeFindNames;
            nodeFindNames.reserve(it->second.findNames.size());
            nodeFindNames.assign(it->second.findNames.begin(), it->second.findNames.end());
            array[cnt].Set("(ssasas)", it->first.c_str(),
                           it->second.guid.c_str(),
                           nodeAdNames.size(), &nodeAdNames.front(),
                           nodeFindNames.size(), &nodeFindNames.front());
            assert(cnt < nodeStates.size());
        }

        MsgArg arg("a(ssasas)", cnt, array);
        QStatus status = MethodReply(msg, &arg, 1);
        if (status != ER_OK) {
            QCC_LogError(status, ("MethodReply"));
        }

    } else {
        // We are still the master
        size_t size;
        MsgArg* entries;

        msg->GetArg(1)->Get("a(ssasas)", &size, &entries);
        ImportState(size, entries, msg->GetSender());

        ++uuidRev;
        if (uuidRev == INVALID_UUIDREV) {
            ++uuidRev;
        }
        advertise.uuidRev = uuidRev;

        assert(find.resultDest == bus.GetUniqueName());
        find.ignoreUUID = uuidRev;

        MsgArg arg("a(ssasas)", 0, NULL);
        QStatus status = MethodReply(msg, &arg, 1);
        if (status != ER_OK) {
            QCC_LogError(status, ("MethodReply"));
        }
    }

    if (!IsMinion()) {
        UpdateDelegations(advertise, listening);
        UpdateDelegations(find);
    }
    nodeStateLock.Unlock();

    if (!IsMaster()) {
        bt.StopListen();
        listening = false;
    }
}


void BTController::HandleDelegateFind(const InterfaceDescription::Member* member,
                                      const char* sourcePath,
                                      Message& msg)
{
    QCC_DbgTrace(("BTController::HandleDelegateFind(member = %s, sourcePath = \"%s\", msg = <>)",
                  member->name.c_str(), sourcePath));
    if (IsMaster() || (strcmp(sourcePath, bluetoothObjPath) != 0) ||
        (master->GetServiceName() != msg->GetSender())) {
        // We only accept delegation commands from our master!
        return;
    }

    if (IsMinion()) {
        char* resultDest;
        char* bdAddrStr;
        uint32_t ignoreUUID;

        msg->GetArg(0)->Get("s", &resultDest);
        msg->GetArg(1)->Get("u", &ignoreUUID);
        msg->GetArg(2)->Get("s", &bdAddrStr);

        if (resultDest && (resultDest[0] != '\0')) {
            find.resultDest = resultDest;
            find.ignoreAddr.FromString(bdAddrStr);
            find.ignoreUUID = ignoreUUID;

            bt.StartFind(ignoreUUID, DELEGATE_TIME);
        } else {
            bt.StopFind();
        }
    } else {
        const MsgArg* args;
        size_t numArgs;

        msg->GetArgs(numArgs, args);

        // Pick a minion to do the work for us.
        NextDirectMinion(find.minion);

        Signal(find.minion->first.c_str(), *org.alljoyn.Bus.BTController.DelegateFind, args, numArgs);
    }
}


void BTController::HandleDelegateAdvertise(const InterfaceDescription::Member* member,
                                           const char* sourcePath,
                                           Message& msg)
{
    QCC_DbgTrace(("BTController::HandleDelegateAdvertise(member = %s, sourcePath = \"%s\", msg = <>)",
                  member->name.c_str(), sourcePath));
    if (IsMaster() || (strcmp(sourcePath, bluetoothObjPath) != 0) ||
        (master->GetServiceName() != msg->GetSender())) {
        // We only accept delegation commands from our master!
        return;
    }

    if (IsMinion()) {
        uint32_t uuidRev;
        char* bdAddrStr;
        uint8_t channel;
        uint16_t psm;

        msg->GetArg(0)->Get("u", &uuidRev);
        msg->GetArg(1)->Get("s", &bdAddrStr);
        msg->GetArg(2)->Get("y", &channel);
        msg->GetArg(3)->Get("q", &psm);

        BluetoothDeviceInterface::AdvertiseInfo adInfo;
        ExtractAdInfo(*msg->GetArg(4), adInfo);

        if (!adInfo.empty()) {
            BDAddress bdAddr(bdAddrStr);

            bt.StartAdvertise(uuidRev, bdAddr, channel, psm, adInfo, DELEGATE_TIME);
        } else {
            bt.StopAdvertise();
        }
    } else {
        const MsgArg* args;
        size_t numArgs;

        msg->GetArgs(numArgs, args);

        // Pick a minion to do the work for us.
        NextDirectMinion(advertise.minion);

        Signal(advertise.minion->first.c_str(), *org.alljoyn.Bus.BTController.DelegateAdvertise, args, numArgs);
    }
}


void BTController::HandleFoundBus(const InterfaceDescription::Member* member,
                                  const char* sourcePath,
                                  Message& msg)
{
    QCC_DbgTrace(("BTController::HandleFoundBus(member = %s, sourcePath = \"%s\", msg = <>)",
                  member->name.c_str(), sourcePath));
    if (IsMaster() || (strcmp(sourcePath, bluetoothObjPath) != 0)) {
        // We only accept FoundBus signals if we are not the master!
        return;
    }

    char* bdAddrStr;
    uint8_t channel;
    uint16_t psm;

    msg->GetArg(1)->Get("s", &bdAddrStr);
    msg->GetArg(2)->Get("y", &channel);
    msg->GetArg(3)->Get("q", &psm);

    BluetoothDeviceInterface::AdvertiseInfo adInfo;
    ExtractAdInfo(*msg->GetArg(4), adInfo);

    if (!adInfo.empty()) {
        BDAddress bdAddr(bdAddrStr);

        BluetoothDeviceInterface::AdvertiseInfo::const_iterator aiit;

        for (aiit = adInfo.begin(); aiit != adInfo.end(); ++aiit) {
            bt.FoundBus(bdAddr, aiit->first, aiit->second, channel, psm);
        }
    }
}


void BTController::HandleFoundDevice(const InterfaceDescription::Member* member,
                                     const char* sourcePath,
                                     Message& msg)
{
    QCC_DbgTrace(("BTController::HandleFoundDevice(member = %s, sourcePath = \"%s\", msg = <>)",
                  member->name.c_str(), sourcePath));
    NodeStateMap::const_iterator it = nodeStates.find(msg->GetSender());
    if (it == nodeStates.end()) {
        // We only handle FoundDevice signals from our minions.
        return;
    }

    uint32_t uuidRev;
    char* adBdAddrStr;

    msg->GetArg(0)->Get("su", &adBdAddrStr, &uuidRev);
    BDAddress adBdAddr(adBdAddrStr);

    ProcessFoundDevice(adBdAddr, uuidRev);
}


void BTController::HandleProxyConnect(const InterfaceDescription::Member* member,
                                      Message& msg)
{
    QCC_DbgTrace(("BTController::HandleProxyConnect(member = %s, msg = <>)", member->name.c_str()));
    char* addrStr;
    uint8_t channel;
    uint16_t psm;

    msg->GetArg(0)->Get("s", &addrStr);
    msg->GetArg(1)->Get("s", &channel);
    msg->GetArg(2)->Get("s", &psm);

    BDAddress bdAddr(addrStr);

    QStatus status;

    if (OKToConnect()) {
        status = bt.Connect(bdAddr, channel, psm);
    } else {
        status = ProxyConnect(bdAddr, channel, psm, NULL);
    }

    MsgArg reply("u", static_cast<uint32_t>(status));

    MethodReply(msg, &reply, 1);
}


void BTController::HandleProxyDisconnect(const InterfaceDescription::Member* member,
                                         Message& msg)
{
    QCC_DbgTrace(("BTController::HandleProxyDisconnect(member = %s, msg = <>)", member->name.c_str()));
    char* addrStr;

    msg->GetArg(0)->Get("s", &addrStr);

    BDAddress bdAddr(addrStr);

    QStatus status;

    status = bt.Disconnect(bdAddr);
    if (status != ER_OK) {
        status = ProxyDisconnect(bdAddr);
    }

    MsgArg reply("u", static_cast<uint32_t>(status));

    MethodReply(msg, &reply, 1);
}


void BTController::ImportState(size_t num, MsgArg* entries, const qcc::String& newNode)
{
    QCC_DbgTrace(("BTController::ImportState(num = %u, entries = <>, newNode = %s)", num, newNode.c_str()));
    size_t i;

    for (i = 0; i < num; ++i) {
        char* bn;
        char* guid;
        size_t anSize, fnSize;
        MsgArg* anList;
        MsgArg* fnList;
        size_t j;
        entries[i].Get("(ssasas)", &bn, &guid, &anSize, &anList, &fnSize, &fnList);
        qcc::String busName(bn);

        nodeStates[busName].guid = guid;

        NodeStateMap::iterator it = nodeStates.find(busName);

        for (j = 0; j < anSize; ++j) {
            char* n;
            anList[j].Get("s", &n);
            qcc::String name(n);
            advertise.AddName(name, it);
        }

        for (j = 0; j < fnSize; ++j) {
            char* n;
            fnList[j].Get("s", &n);
            qcc::String name(n);
            find.AddName(name, it);
        }
    }

    nodeStates[newNode].direct = true;
    ++directMinions;

    if (find.minion == nodeStates.end()) {
        find.minion = nodeStates.begin();
    }

    if (advertise.minion == nodeStates.end() || advertise.minion == find.minion) {
        if (advertise.minion == nodeStates.end()) {
            advertise.minion = nodeStates.begin();
        }
        for (uint8_t i = directMinions / 2; i > 0; --i) {
            NextDirectMinion(advertise.minion);
        }
    }
}


void BTController::UpdateDelegations(NameArgInfo& nameInfo, bool allow)
{
    bool sendSignal = false;
    bool allowConn = allow && IsMaster() && (directMinions < maxConnections);
    bool nameListChange = nameInfo.Count() != nameInfo.Count();
    bool empty = nameInfo.Count() == 0;
    bool active = nameInfo.active;
    bool deferredClearArgs = false;
    bool advertiseOp = (&nameInfo == &advertise);

    QCC_DbgTrace(("BTController::UpdateDelegations(nameInfo = <%s>, allow = %s)",
                  advertiseOp ? "advertise" : "find", allow ? "true" : "false"));

    if (active) {
        if (empty || !allowConn) {
            if (empty && nameListChange && advertiseOp) {
                // Advertise empty list for a while so others detect lost name quickly.
                nameInfo.SetArgs();
                deferredClearArgs = true;
            } else {
                // Gotta stop finding so clear the args.
                nameInfo.ClearArgs();
            }
        }
        if (nameListChange || !empty || !allowConn) {
            // Either stopping or updating so remove the old alarm.
            delegationTimer.RemoveAlarm(nameInfo.alarm);
            nameInfo.active = false;  // In case we are stopping.
            sendSignal = true;
        }
    }

    if (allowConn && !empty && (nameListChange || !active)) {
        // Either starting or updating so start a new alarm.
        nameInfo.SetArgs();
        delegationTimer.AddAlarm(nameInfo.alarm);
        nameInfo.active = true;
        sendSignal = true;
    }

    if (sendSignal) {
        if (advertiseOp && UseLocalAdvertise()) {
            if (nameInfo.active || deferredClearArgs) {
                BluetoothDeviceInterface::AdvertiseInfo adInfo;
                MsgArg arg("a{sas}", advertise.adInfoArgs.size(), &advertise.adInfoArgs.front());
                ExtractAdInfo(arg, adInfo);
                advertise.uuidRev = uuidRev;
                bt.StartAdvertise(advertise.uuidRev, advertise.bdAddr, advertise.channel, advertise.psm, adInfo);
            } else {
                bt.StopAdvertise();
            }
        } else if (UseLocalFind()) {
            if (nameInfo.active) {
                bt.StartFind(find.ignoreUUID);
            } else {
                bt.StopFind();
            }
        } else {
            Signal(nameInfo.minion->first.c_str(), *nameInfo.delegateSignal,
                   nameInfo.args, nameInfo.argsSize);
        }
    }

    if (deferredClearArgs) {
        nameInfo.ClearArgs();
    }
}


void BTController::EncodeAdInfo(const BluetoothDeviceInterface::AdvertiseInfo& adInfo, MsgArg& arg)
{
    QCC_DbgTrace(("BTController::EncodeAdInfo()"));
    MsgArg* nodeList = new MsgArg[adInfo.size()];

    for (size_t i = 0; i < adInfo.size(); ++i) {
        const qcc::String& guid = adInfo[i].first;
        const BluetoothDeviceInterface::AdvertiseNames& names = adInfo[i].second;

        MsgArg* nameList = new MsgArg[names.size()];

        for (size_t j = 0; i < names.size(); ++i) {
            nameList[j].Set("s", names[j].c_str());
        }

        nodeList[i].Set("{sas}", guid.c_str(), names.size(), nameList);
    }

    arg.Set("a{sas}", adInfo.size(), nodeList);
}


void BTController::ExtractAdInfo(const MsgArg& arg, BluetoothDeviceInterface::AdvertiseInfo& adInfo)
{
    QCC_DbgTrace(("BTController::ExtractAdInfo()"));
    MsgArg* entries;
    size_t size;
    QStatus status;

    status = arg.Get("a{sas}", &size, &entries);

    if ((status == ER_OK) && (size > 0)) {
        adInfo.clear();
        adInfo.resize(size);

        for (size_t i = 0; i < size; ++i) {
            char* node;
            MsgArg* names;
            size_t numNames;

            status = entries[i].Get("{sas}", &node, &numNames, &names);

            if (status == ER_OK) {
                BluetoothDeviceInterface::AdvertiseNames& nameList = adInfo[i].second;
                adInfo[i].first = node;

                nameList.clear();
                nameList.reserve(numNames);

                for (size_t j = 0; j < numNames; ++j) {
                    char* name;
                    status = names[j].Get("s", &name);
                    if (status == ER_OK) {
                        nameList.push_back(name);
                    }
                }
            }
        }
    }
}


void BTController::AlarmTriggered(const Alarm& alarm, QStatus reason)
{
    QCC_DbgTrace(("BTController::AlarmTriggered(alarm = <%s>)", alarm == find.alarm ? "find" : "advertise"));
    assert(IsMaster());

    if (reason == ER_OK) {
        nodeStateLock.Lock();
        if (alarm == find.alarm) {
            NextDirectMinion(find.minion);
            Signal(find.minion->first.c_str(), *org.alljoyn.Bus.BTController.DelegateFind, find.args, ArraySize(find.args));
        } else { // (alarm == advertise.alarm)
            NextDirectMinion(advertise.minion);
            Signal(advertise.minion->first.c_str(), *org.alljoyn.Bus.BTController.DelegateAdvertise, advertise.args, ArraySize(advertise.args));
        }
        nodeStateLock.Unlock();
    }
}


void BTController::AdvertiseNameArgInfo::AddName(const qcc::String& name, NodeStateMap::iterator it)
{
    assert(it != bto.nodeStates.end());
    it->second.advertiseNames.insert(name);
    ++count;
}


void BTController::AdvertiseNameArgInfo::RemoveName(const qcc::String& name, NodeStateMap::iterator it)
{
    assert(it != bto.nodeStates.end());
    it->second.advertiseNames.erase(name);
    --count;
}


void BTController::FindNameArgInfo::AddName(const qcc::String& name, NodeStateMap::iterator it)
{
    assert(it != bto.nodeStates.end());
    it->second.findNames.insert(name);
    names.insert(name);
}


void BTController::FindNameArgInfo::RemoveName(const qcc::String& name, NodeStateMap::iterator it)
{
    assert(it != bto.nodeStates.end());
    it->second.findNames.erase(name);
    names.erase(name);
}


void BTController::AdvertiseNameArgInfo::SetArgs()
{
    size_t argsSize = this->argsSize;

    MsgArg::Set(args, argsSize, "usyq", uuidRev, bdAddr.ToString().c_str(), channel, psm);
    assert(argsSize == (this->argsSize - 1));

    adInfoArgs.clear();
    adInfoArgs.reserve(bto.nodeStates.size());

    NodeStateMap::const_iterator nodeit;
    set<qcc::String>::const_iterator nameit;
    vector<const char*> names;
    size_t i;
    for (i = 0, nodeit = bto.nodeStates.begin(); nodeit != bto.nodeStates.end(); ++nodeit, ++i) {
        assert(i < adInfoArgs.capacity());
        names.reserve(nodeit->second.advertiseNames.size());
        for (nameit = nodeit->second.advertiseNames.begin(); nameit != nodeit->second.advertiseNames.end(); ++nameit) {
            names.push_back(nameit->c_str());
        }
        adInfoArgs.push_back(MsgArg("{sas}", nodeit->first.c_str(), names.size(), &names.front()));
    }

    args[argsSize].Set("a{sas}", adInfoArgs.size(), &adInfoArgs.front());
}


void BTController::AdvertiseNameArgInfo::ClearArgs()
{
    size_t argsSize = this->argsSize;

    MsgArg::Set(args, argsSize, "usyq", 0, "", 0, 0);
    assert(argsSize == (this->argsSize - 1));

    args[argsSize].Set("a{sas}", 0, NULL);
}


void BTController::FindNameArgInfo::SetArgs()
{
    size_t argsSize = this->argsSize;

    MsgArg::Set(args, argsSize, "sus", resultDest.c_str(), ignoreUUID, ignoreAddr.ToString().c_str());
    assert(argsSize == (this->argsSize - 1));

    nameArgs.clear();
    nameArgs.reserve(names.size());

    set<qcc::String>::const_iterator it;
    for (it = names.begin(); it != names.end(); ++it) {
        nameArgs.push_back(it->c_str());
    }

    args[argsSize].Set("as", nameArgs.size(), &nameArgs.front());
}


void BTController::FindNameArgInfo::ClearArgs()
{
    size_t argsSize = this->argsSize;

    MsgArg::Set(args, argsSize, "sus", "", 0, "");
    assert(argsSize == (this->argsSize - 1));

    args[argsSize].Set("as", 0, NULL);
}

}
