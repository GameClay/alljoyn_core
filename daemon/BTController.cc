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

#include <alljoyn/AllJoynStd.h>

#include "BTController.h"
#include "BTEndpoint.h"

#define QCC_MODULE "ALLJOYN_BTC"


using namespace std;
using namespace qcc;

#define MethodHander(_a) static_cast<MessageReceiver::MethodHandler>(_a)
#define SignalHander(_a) static_cast<MessageReceiver::SignalHandler>(_a)
#define ReplyHander(_a) static_cast<MessageReceiver::ReplyHandler>(_a)

static const uint32_t ABSOLUTE_MAX_CONNECTIONS = 7; /* BT can't have more than 7 direct connections */
static const uint32_t DEFAULT_MAX_CONNECTIONS =  6; /* Gotta allow 1 connection for car-kit/headset/headphones */

/*
 * Timeout for detecting lost devices.  The nominal timeout is 30 seconds.
 * Absolute timing isn't critical so an additional 5 seconds is acctually
 * applied to when the alarm triggers.  This will allow lost device
 * expirations that are close to each other to be processed at the same time.
 * It also reduces the number of alarm resets if we get 2 updates within 5
 * seconds from the lower layer.
 */
static const uint32_t LOST_DEVICE_TIMEOUT = 30000;    /* 30 seconds */
static const uint32_t LOST_DEVICE_TIMEOUT_EXT = 5000; /* 5 seconds */

namespace ajn {

struct InterfaceDesc {
    AllJoynMessageType type;
    const char* name;
    const char* inputSig;
    const char* outSig;
    const char* argNames;
};

struct SignalEntry {
    const InterfaceDescription::Member* member;  /**< Pointer to signal's member */
    MessageReceiver::SignalHandler handler;      /**< Signal handler implementation */
};

static const char* bluetoothObjPath = "/org/alljoyn/Bus/BluetoothController";
static const char* bluetoothTopoMgrIfcName = "org.alljoyn.Bus.BluetoothController";

#define SIG_ARRAY           "a"
#define SIG_ARRAY_SIZE      1
#define SIG_BDADDR          "t"
#define SIG_BDADDR_SIZE     1
#define SIG_DURATION        "u"
#define SIG_DURATION_SIZE   1
#define SIG_GUID            "s"
#define SIG_GUID_SIZE       1
#define SIG_MINION_CNT      "y"
#define SIG_MINION_CNT_SIZE 1
#define SIG_NAME            "s"
#define SIG_NAME_SIZE       1
#define SIG_PSM             "q"
#define SIG_PSM_SIZE        1
#define SIG_STATUS          "u"
#define SIG_STATUS_SIZE     1
#define SIG_UUIDREV         "u"
#define SIG_UUIDREV_SIZE    1

#define SIG_NAME_LIST               SIG_ARRAY SIG_NAME
#define SIG_NAME_LIST_SIZE          SIG_ARRAY_SIZE
#define SIG_BUSADDR                 SIG_BDADDR SIG_PSM
#define SIG_BUSADDR_SIZE            (SIG_BDADDR_SIZE + SIG_PSM_SIZE)
#define SIG_FIND_FILTER_LIST        SIG_ARRAY SIG_BDADDR
#define SIG_FIND_FILTER_LIST_SIZE   SIG_ARRAY_SIZE
#define SIG_AD_NAME_MAP_ENTRY       "(" SIG_GUID SIG_BUSADDR SIG_NAME_LIST ")"
#define SIG_AD_NAME_MAP_ENTRY_SIZE  1
#define SIG_AD_NAME_MAP             SIG_ARRAY SIG_AD_NAME_MAP_ENTRY
#define SIG_AD_NAME_MAP_SIZE        SIG_ARRAY_SIZE
#define SIG_AD_NAMES                SIG_NAME_LIST
#define SIG_AD_NAMES_SIZE           SIG_NAME_LIST_SIZE
#define SIG_FIND_NAMES              SIG_NAME_LIST
#define SIG_FIND_NAMES_SIZE         SIG_NAME_LIST_SIZE
#define SIG_NODE_STATE_ENTRY        "(" SIG_GUID SIG_NAME SIG_BUSADDR SIG_AD_NAMES SIG_FIND_NAMES ")"
#define SIG_NODE_STATE_ENTRY_SIZE   1
#define SIG_NODE_STATES             SIG_ARRAY SIG_NODE_STATE_ENTRY
#define SIG_NODE_STATES_SIZE        SIG_ARRAY_SIZE
#define SIG_FOUND_NODE_ENTRY        "(" SIG_BUSADDR SIG_UUIDREV SIG_AD_NAME_MAP ")"
#define SIG_FOUND_NODE_ENTRY_SIZE   1
#define SIG_FOUND_NODES             SIG_ARRAY SIG_FOUND_NODE_ENTRY
#define SIG_FOUND_NODES_SIZE        SIG_ARRAY_SIZE

#define SIG_SET_STATE_IN            SIG_MINION_CNT SIG_UUIDREV SIG_BUSADDR SIG_NODE_STATES SIG_FOUND_NODES
#define SIG_SET_STATE_IN_SIZE       (SIG_MINION_CNT_SIZE + SIG_UUIDREV_SIZE + SIG_BUSADDR_SIZE + SIG_NODE_STATES_SIZE + SIG_FOUND_NODES_SIZE)
#define SIG_SET_STATE_OUT           SIG_UUIDREV SIG_BUSADDR SIG_NODE_STATES SIG_FOUND_NODES
#define SIG_SET_STATE_OUT_SIZE      (SIG_UUIDREV_SIZE + SIG_BUSADDR_SIZE + SIG_NODE_STATES_SIZE + SIG_FOUND_NODES_SIZE)
#define SIG_NAME_OP                 SIG_BUSADDR SIG_NAME
#define SIG_NAME_OP_SIZE            (SIG_BUSADDR_SIZE + SIG_NAME_SIZE)
#define SIG_DELEGATE_AD             SIG_UUIDREV SIG_BUSADDR SIG_AD_NAME_MAP SIG_DURATION
#define SIG_DELEGATE_AD_SIZE        (SIG_UUIDREV_SIZE + SIG_BUSADDR_SIZE + SIG_AD_NAME_MAP_SIZE + SIG_DURATION_SIZE)
#define SIG_DELEGATE_FIND           SIG_NAME SIG_FIND_FILTER_LIST SIG_DURATION
#define SIG_DELEGATE_FIND_SIZE      (SIG_NAME_SIZE + SIG_FIND_FILTER_LIST_SIZE + SIG_DURATION_SIZE)
#define SIG_FOUND_NAMES             SIG_FOUND_NODES
#define SIG_FOUND_NAMES_SIZE        (SIG_FOUND_NODES_SIZE)
#define SIG_FOUND_DEV               SIG_BDADDR SIG_UUIDREV
#define SIG_FOUND_DEV_SIZE          (SIG_BDADDR_SIZE + SIG_UUIDREV_SIZE)


const InterfaceDesc btmIfcTable[] = {
    /* Methods */
    { MESSAGE_METHOD_CALL, "SetState", SIG_SET_STATE_IN, SIG_SET_STATE_OUT, "minionCnt,uuidRev,busAddr,psm,nodeStates,foundNodes,uuidRev,busAddr,psm,nodeStates,foundNodes" },

    /* Signals */
    { MESSAGE_SIGNAL, "FindName",            SIG_NAME_OP,       NULL, "requestorAddr,requestorPSM,findName" },
    { MESSAGE_SIGNAL, "CancelFindName",      SIG_NAME_OP,       NULL, "requestorAddr,requestorPSM,findName" },
    { MESSAGE_SIGNAL, "AdvertiseName",       SIG_NAME_OP,       NULL, "requestorAddr,requestorPSM,adName" },
    { MESSAGE_SIGNAL, "CancelAdvertiseName", SIG_NAME_OP,       NULL, "requestorAddr,requestorPSM,adName" },
    { MESSAGE_SIGNAL, "DelegateAdvertise",   SIG_DELEGATE_AD,   NULL, "uuidRev,bdAddr,psm,adNames,duration" },
    { MESSAGE_SIGNAL, "DelegateFind",        SIG_DELEGATE_FIND, NULL, "resultDest,ignoreBDAddr,duration" },
    { MESSAGE_SIGNAL, "FoundNames",          SIG_FOUND_NAMES,   NULL, "adNamesTable" },
    { MESSAGE_SIGNAL, "LostNames",           SIG_FOUND_NAMES,   NULL, "adNamesTable" },
    { MESSAGE_SIGNAL, "FoundDevice",         SIG_FOUND_DEV,     NULL, "bdAddr,uuidRev" },
};


BTController::BTController(BusAttachment& bus, BluetoothDeviceInterface& bt) :
    BusObject(bus, bluetoothObjPath),
#ifndef NDEBUG
    dbgIface(this),
    discoverTimer(dbgIface.LookupTimingProperty("DiscoverTimes")),
    sdpQueryTimer(dbgIface.LookupTimingProperty("SDPQueryTimes")),
    connectTimer(dbgIface.LookupTimingProperty("ConnectTimes")),
#endif
    bus(bus),
    bt(bt),
    master(NULL),
    masterUUIDRev(bt::INVALID_UUIDREV),
    directMinions(0),
    maxConnections(min(StringToU32(Environ::GetAppEnviron()->Find("ALLJOYN_MAX_BT_CONNECTIONS"), 0, DEFAULT_MAX_CONNECTIONS),
                       ABSOLUTE_MAX_CONNECTIONS)),
    listening(false),
    devAvailable(false),
    advertise(*this),
    find(*this),
    dispatcher("BTC-Dispatcher")
{
    while (masterUUIDRev == bt::INVALID_UUIDREV) {
        masterUUIDRev = qcc::Rand32();
    }

    InterfaceDescription* ifc;
    bus.CreateInterface(bluetoothTopoMgrIfcName, ifc);
    for (size_t i = 0; i < ArraySize(btmIfcTable); ++i) {
        ifc->AddMember(btmIfcTable[i].type,
                       btmIfcTable[i].name,
                       btmIfcTable[i].inputSig,
                       btmIfcTable[i].outSig,
                       btmIfcTable[i].argNames,
                       0);
    }
    ifc->Activate();

    org.alljoyn.Bus.BTController.interface =           ifc;
    org.alljoyn.Bus.BTController.SetState =            ifc->GetMember("SetState");
    org.alljoyn.Bus.BTController.FindName =            ifc->GetMember("FindName");
    org.alljoyn.Bus.BTController.CancelFindName =      ifc->GetMember("CancelFindName");
    org.alljoyn.Bus.BTController.AdvertiseName =       ifc->GetMember("AdvertiseName");
    org.alljoyn.Bus.BTController.CancelAdvertiseName = ifc->GetMember("CancelAdvertiseName");
    org.alljoyn.Bus.BTController.DelegateAdvertise =   ifc->GetMember("DelegateAdvertise");
    org.alljoyn.Bus.BTController.DelegateFind =        ifc->GetMember("DelegateFind");
    org.alljoyn.Bus.BTController.FoundNames =          ifc->GetMember("FoundNames");
    org.alljoyn.Bus.BTController.LostNames =           ifc->GetMember("LostNames");
    org.alljoyn.Bus.BTController.FoundDevice =         ifc->GetMember("FoundDevice");

    advertise.delegateSignal = org.alljoyn.Bus.BTController.DelegateAdvertise;
    find.delegateSignal = org.alljoyn.Bus.BTController.DelegateFind;

    static_cast<DaemonRouter&>(bus.GetInternal().GetRouter()).AddBusNameListener(this);

    // Setup the BT node info for ourself.
    self->SetGUID(bus.GetGlobalGUIDString());
    advertise.minion = self;
    find.minion = self;

    dispatcher.Start();
}


BTController::~BTController()
{
    // Don't need to remove our bus name change listener from the router (name
    // table) since the router is already destroyed at this point in time.

    dispatcher.Stop();
    dispatcher.Join();

    if (advertise.active) {
        bt.StopAdvertise();
    }

    bus.UnregisterBusObject(*this);
    if (master) {
        delete master;
    }
}


void BTController::ObjectRegistered() {
    // Set our unique name now that we know it.
    self->SetUniqueName(bus.GetUniqueName());
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
    };

    const SignalEntry signalEntries[] = {
        { org.alljoyn.Bus.BTController.FindName,            SignalHandler(&BTController::HandleNameSignal) },
        { org.alljoyn.Bus.BTController.CancelFindName,      SignalHandler(&BTController::HandleNameSignal) },
        { org.alljoyn.Bus.BTController.AdvertiseName,       SignalHandler(&BTController::HandleNameSignal) },
        { org.alljoyn.Bus.BTController.CancelAdvertiseName, SignalHandler(&BTController::HandleNameSignal) },
        { org.alljoyn.Bus.BTController.DelegateAdvertise,   SignalHandler(&BTController::HandleDelegateOp) },
        { org.alljoyn.Bus.BTController.DelegateFind,        SignalHandler(&BTController::HandleDelegateOp) },
        { org.alljoyn.Bus.BTController.FoundNames,          SignalHandler(&BTController::HandleFoundNamesChange) },
        { org.alljoyn.Bus.BTController.LostNames,           SignalHandler(&BTController::HandleFoundNamesChange) },
        { org.alljoyn.Bus.BTController.FoundDevice,         SignalHandler(&BTController::HandleFoundDeviceChange) },
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


QStatus BTController::AddAdvertiseName(const qcc::String& name)
{
    QStatus status = DoNameOp(name, *org.alljoyn.Bus.BTController.AdvertiseName, true, advertise);

    lock.Lock();
    bool isMaster = IsMaster();
    bool lDevAvailable = devAvailable;
    BTBusAddress addr = self->GetBusAddress();
    lock.Unlock();

    if (isMaster && (status == ER_OK)) {
        self->AddAdvertiseName(name);  // self gets new name added to list of existing names
        if (lDevAvailable) {
            BTNodeDB newAdInfo;
            BTNodeInfo node(addr, self->GetUniqueName(), self->GetGUID());  // make an actual copy of self
            node->AddAdvertiseName(name);  // copy of self only gets the new names (not the existing names)
            newAdInfo.AddNode(node);
            status = DistributeAdvertisedNameChanges(&newAdInfo, NULL);
        }
    }

    return status;
}


QStatus BTController::RemoveAdvertiseName(const qcc::String& name)
{
    QStatus status = DoNameOp(name, *org.alljoyn.Bus.BTController.CancelAdvertiseName, false, advertise);

    lock.Lock();
    bool isMaster = IsMaster();
    bool lDevAvailable = devAvailable;
    BTBusAddress addr = self->GetBusAddress();
    lock.Unlock();

    if (isMaster && (status == ER_OK)) {
        self->RemoveAdvertiseName(name);
        if (lDevAvailable) {
            BTNodeDB oldAdInfo;
            BTNodeInfo node(addr, self->GetUniqueName(), self->GetGUID());  // make an actual copy of self
            node->AddAdvertiseName(name);  // Yes 'Add' the name being removed (it goes in the old ad info).
            oldAdInfo.AddNode(node);
            status = DistributeAdvertisedNameChanges(NULL, &oldAdInfo);
        }
    }

    return status;
}

void BTController::ProcessDeviceChange(const BDAddress& adBdAddr,
                                       uint32_t uuidRev)
{
    QCC_DbgTrace(("BTController::ProcessDeviceChange(adBdAddr = %s, uuidRev = %08x)",
                  adBdAddr.ToString().c_str(), uuidRev));

    // This gets called when the BTAccessor layer detects either a new
    // advertising device and/or a new uuidRev for associated with that
    // advertising device.

    assert(uuidRev != bt::INVALID_UUIDREV);
    assert(adBdAddr.GetRaw() != static_cast<uint64_t>(0));

    QStatus status;

    lock.Lock();
    if (IsMaster()) {
        BTNodeInfo adNode = foundNodeDB.FindNode(adBdAddr);
        BTNodeDB newAdInfo;
        BTNodeDB oldAdInfo;
        bool distributeChanges = false;

        if (nodeDB.FindNode(adNode->GetBusAddress())->IsValid()) {
            /*
             * Normally the minion will not send us advertisements from nodes
             * that are connected to us since they contain our advertisements.
             * That said however, there is a race condition when establishing
             * a connection with a remote device where our find minion
             * received a device found indication for the device that we are
             * in the process of connecting to.  That minion may even see the
             * new UUIDRev for that newly connected remote device in its EIR
             * packets.  This will of course cause our minion to notify us
             * since we haven't told our minion to ingore that device's BD
             * Address yet (the connection may fail).  The most expedient
             * thing is to just ignore found device notification for devices
             * that we know are connected to us.
             */
            lock.Unlock();
            return;
        }

        if ((adNode->IsValid()) && (adNode->GetUUIDRev() == uuidRev)) {
            // We've see this advertising node before and nothing has changed
            // so just refresh the expiration time of all the nodes.
            foundNodeDB.DumpTable((String("foundNodeDB: Before Refresh Expiration for nodes with connect address: ") + adNode->GetConnectAddress().ToString()).c_str());
            foundNodeDB.RefreshExpiration(adNode->GetConnectAddress(), LOST_DEVICE_TIMEOUT);
            foundNodeDB.DumpTable((String("foundNodeDB: Refresh Expiration for nodes with connect address: ") + adNode->GetConnectAddress().ToString()).c_str());
            ResetExpireNameAlarm();

        } else {
            uint32_t newUUIDRev;
            BTBusAddress connAddr;

            QCC_DEBUG_ONLY(sdpQueryStartTime = sdpQueryTimer.StartTime());
            lock.Unlock();
            status = bt.GetDeviceInfo(adBdAddr, newUUIDRev, connAddr, newAdInfo);
            lock.Lock();
            QCC_DEBUG_ONLY(sdpQueryTimer.RecordTime(adBdAddr, sdpQueryStartTime));

            // Make sure we are still master
            if (IsMaster()) {
                if (status != ER_OK) {
                    lock.Unlock();
                    return;
                }

                foundNodeDB.Lock();
                if (adNode->IsValid()) {
                    foundNodeDB.GetNodesFromConnectAddr(adNode->GetConnectAddress(), oldAdInfo);
                } else {
                    QCC_DEBUG_ONLY(discoverTimer.RecordTime(adBdAddr, discoverStartTime));
                }

                if (!connAddr.IsValid()) {
                    QCC_LogError(ER_FAIL, ("Invalid connect address %s in advertisement",
                                           connAddr.ToString().c_str()));
                    foundNodeDB.Unlock();
                    lock.Unlock();
                    return;
                }

                BTNodeInfo newConnNode = newAdInfo.FindNode(connAddr);
                if (!newConnNode->IsValid()) {
                    QCC_LogError(ER_FAIL, ("No device with connect address %s in advertisement",
                                           connAddr.ToString().c_str()));
                    foundNodeDB.Unlock();
                    lock.Unlock();
                    return;
                }

                // We actually want the nodes in newAdInfo to use the existing
                // node in foundNodeDB if it exists.  This will ensure a
                // consistent foundNodeDB and allow operations like
                // RefreshExpireTime() and GetNodesFromConnectAddr() to work
                // properly.
                BTNodeInfo connNode = foundNodeDB.FindNode(connAddr);
                if (!connNode->IsValid()) {
                    connNode = newConnNode;
                }

                BTNodeDB::const_iterator nodeit;
                Timespec now;
                GetTimeNow(&now);
                uint64_t expireTime = now.GetAbsoluteMillis() + LOST_DEVICE_TIMEOUT;
                for (nodeit = newAdInfo.Begin(); nodeit != newAdInfo.End(); ++nodeit) {
                    BTNodeInfo node = *nodeit;
                    node->SetUUIDRev(newUUIDRev);
                    node->SetConnectNode(connNode);
                    node->SetExpireTime(expireTime);
                }

                foundNodeDB.DumpTable("foundNodeDB - Before update");
                foundNodeDB.UpdateDB(&newAdInfo, &oldAdInfo);
                foundNodeDB.DumpTable("foundNodeDB - Updated set of found devices due to remote device advertisement change");
                foundNodeDB.Unlock();

                distributeChanges = true;

                ResetExpireNameAlarm();
            }
        }

        lock.Unlock();

        if (distributeChanges) {
            DistributeAdvertisedNameChanges(&newAdInfo, &oldAdInfo);
        }
    } else {
        MsgArg args[SIG_FOUND_DEV_SIZE];
        size_t numArgs = ArraySize(args);

        status = MsgArg::Set(args, numArgs, SIG_FOUND_DEV, adBdAddr.GetRaw(), uuidRev);
        if (status != ER_OK) {
            QCC_LogError(status, ("MsgArg::Set(args = <>, numArgs = %u, %s, %s, %08x) failed",
                                  numArgs, SIG_FOUND_DEV, adBdAddr.ToString().c_str(), uuidRev));
            return;
        }

        String resultDest = find.resultDest;
        lock.Unlock();

        status = Signal(resultDest.c_str(), 0, *org.alljoyn.Bus.BTController.FoundDevice, args, numArgs);
    }
}


const BTBusAddress& BTController::PrepConnect(const BTBusAddress& addr)
{
    BTNodeInfo node;
    lock.Lock();
    if (!IsMinion()) {
        node = nodeDB.FindNode(addr);
        if (IsMaster() && !node->IsValid()) {
            node = foundNodeDB.FindNode(addr);
        }
    }

    if (!IsMaster() && !node->IsValid()) {
        node = masterNode;
    }
    lock.Unlock();

    QCC_DEBUG_ONLY(connectStartTimes[addr.addr] = connectTimer.StartTime());

    return node->GetConnectAddress();
}


void BTController::PostConnect(QStatus status, const BTBusAddress& addr, const String& remoteName)
{
    if (status == ER_OK) {
        QCC_DEBUG_ONLY(connectTimer.RecordTime(addr.addr, connectStartTimes[addr.addr]));
        assert(!remoteName.empty());
        if (!master && !nodeDB.FindNode(addr)->IsValid()) {
            /* Only call SendSetState for new outgoing connections.  If we
             * have a master then the connection can't be new.  If we are the
             * master then there should be node device with the same bus
             * address as the endpoint.
             */
            DispatchOperation(new SendSetStateDispatchInfo(addr, remoteName));
        }
    }
}


void BTController::BTDeviceAvailable(bool on)
{
    QCC_DbgTrace(("BTController::BTDeviceAvailable(<%s>)", on ? "on" : "off"));
    DispatchOperation(new BTDevAvailDispatchInfo(on));
}


void BTController::DeferredBTDeviceAvailable(bool on)
{
    QCC_DbgTrace(("BTController::DeferredBTDeviceAvailable(<%s>)", on ? "on" : "off"));
    lock.Lock();
    devAvailable = on;
    if (on) {
        BTBusAddress listenAddr;
        QStatus status = bt.StartListen(listenAddr.addr, listenAddr.psm);
        if (status == ER_OK) {
            assert(listenAddr.IsValid());
            listening = true;
            nodeDB.Lock();
            if (listenAddr != self->GetBusAddress()) {
                if (self->IsValid()) {
                    // Gotta remove it from the DB since the DB has it indexed
                    // on the BusAddress which changed.
                    nodeDB.RemoveNode(self);
                }
                self->SetBusAddress(listenAddr);
                nodeDB.AddNode(self);
            } // else 'self' is already in nodeDB with the correct BusAddress.
            BDAddressSet ignoreAddrs;
            BTNodeDB::const_iterator it;
            for (it = nodeDB.Begin(); it != nodeDB.End(); ++it) {
                ignoreAddrs->insert((*it)->GetBusAddress().addr);
            }
            nodeDB.Unlock();

            find.ignoreAddrs = ignoreAddrs;
            find.dirty = true;

            if (IsMaster()) {
                UpdateDelegations(advertise);
                UpdateDelegations(find);
            }
        }
    } else {
        if (IsMaster()) {
            if (advertise.active) {
                if (UseLocalAdvertise()) {
                    bt.StopAdvertise();
                }
                advertise.active = false;
            }
            if (find.active) {
                if (UseLocalFind()) {
                    bt.StopFind();
                }
                find.active = false;
            }
        }
        if (listening) {
            self->SetBusAddress(BTBusAddress());
            bt.StopListen();
            listening = false;
        }
    }
    lock.Unlock();
}


bool BTController::CheckIncomingAddress(const BDAddress& addr) const
{
    QCC_DbgTrace(("BTController::CheckIncomingAddress(addr = %s)", addr.ToString().c_str()));
    if (IsMaster()) {
        QCC_DbgPrintf(("Always accept incoming connection as Master."));
        return true;
    } else if (addr == masterNode->GetBusAddress().addr) {
        QCC_DbgPrintf(("Always accept incoming connection from Master."));
        return true;
    } else if (IsDrone()) {
        const BTNodeInfo& node = nodeDB.FindNode(addr);
        QCC_DbgPrintf(("% incoming connection from %s %s.",
                       (node->IsValid() && node->IsDirectMinion()) ? "Accepting" : "Not Accepting",
                       node->IsValid() ?
                       (node->IsDirectMinion() ? "direct" : "indirect") : "unknown node:",
                       node->IsValid() ? "minion" : addr.ToString().c_str()));
        return node->IsValid() && node->IsDirectMinion();
    }

    QCC_DbgPrintf(("Always reject incoming connection from %s because we are a %s (our master is %s).",
                   addr.ToString().c_str(),
                   IsMaster() ? "master" : (IsDrone() ? "drone" : "minion"),
                   masterNode->GetBusAddress().addr.ToString().c_str()));
    return false;
}


void BTController::NameOwnerChanged(const qcc::String& alias,
                                    const qcc::String* oldOwner,
                                    const qcc::String* newOwner)
{
    QCC_DbgTrace(("BTController::NameOwnerChanged(alias = %s, oldOwner = %s, newOwner = %s)",
                  alias.c_str(),
                  oldOwner ? oldOwner->c_str() : "<null>",
                  newOwner ? newOwner->c_str() : "<null>"));
    if (oldOwner && (alias == *oldOwner)) {
        DispatchOperation(new NameLostDispatchInfo(alias));
    }
}


void BTController::DeferredNameLostHander(const String& name)
{
    // An endpoint left the bus.
    QCC_DbgPrintf(("%s has left the bus", name.c_str()));
    bool updateDelegations = false;
    String stopAdSignalDest;
    NameArgInfo::NameArgs stopAdArgs(advertise.argsSize);

    lock.Lock();
    if (master && (master->GetServiceName() == name)) {
        // We are a minion or a drone and our master has left us.

        QCC_DbgPrintf(("Our master left us: %s", masterNode->GetBusAddress().ToString().c_str()));
        // We are the master now.
        bool skipStopAd = IsDrone() && (advertise.minion == self);
        bool skipStopFind = IsDrone() && (find.minion == self);

        delete master;
        master = NULL;
        masterNode = BTNodeInfo();

        if (!skipStopAd) {
            advertise.StopOp();
        }
        if (!skipStopFind) {
            find.StopOp();
        }

        // Our master and all of our master's minions excluding ourself and
        // our minions are in foundNodeDB so refreshing the expiration time on
        // the entire foundNodeDB will cause their advertised names to expire
        // as well.  No need to distribute lost names at this time.
        foundNodeDB.RefreshExpiration(LOST_DEVICE_TIMEOUT);

        // We need to prepare for controlling discovery.
        BDAddressSet ignoreAddrs;
        nodeDB.Lock();
        for (BTNodeDB::const_iterator it = nodeDB.Begin(); it != nodeDB.End(); ++it) {
            ignoreAddrs->insert((*it)->GetBusAddress().addr);
        }
        nodeDB.Unlock();
        find.resultDest.clear();
        find.ignoreAddrs = ignoreAddrs;

        if (UseLocalAdvertise()) {
            advertise.minion = self;
        } else {
            NextDirectMinion(advertise.minion);
        }
        QCC_DbgPrintf(("Selected %s as our advertise minion.",
                       (find.minion == self) ? "ourself" :
                       find.minion->GetBusAddress().ToString().c_str()));

        if (UseLocalFind()) {
            find.minion = self;
        } else {
            NextDirectMinion(find.minion);
        }
        QCC_DbgPrintf(("Selected %s as our find minion.",
                       (find.minion == self) ? "ourself" :
                       find.minion->GetBusAddress().ToString().c_str()));

        updateDelegations = true;

    } else {
        // Someone else left.  If it was a minion node, remove their find/ad names.
        BTNodeInfo minion = nodeDB.FindNode(name);

        if (minion->IsValid()) {
            // We are a master or a drone and one of our minions has left.

            QCC_DbgPrintf(("One of our minions left us: %s", minion->GetBusAddress().ToString().c_str()));

            bool wasAdvertiseMinion = minion == advertise.minion;
            bool wasFindMinion = minion == find.minion;
            bool wasDirect = minion->IsDirectMinion();
            bool wasRotateMinions = RotateMinions();

            // Advertise minion and find minion should never be the same.
            assert(!(wasAdvertiseMinion && wasFindMinion));

            find.ignoreAddrs->erase(minion->GetBusAddress().addr);

            // Indicate the name lists have changed.
            advertise.count -= minion->AdvertiseNamesSize();
            advertise.dirty = true;

            find.count -= minion->FindNamesSize();
            find.dirty = find.dirty || !minion->FindNamesEmpty();

            if (wasDirect) {
                if (!RotateMinions() && wasRotateMinions) {
                    advertise.StopAlarm();
                    find.StopAlarm();
                    // Force changing from durational delegations to permanent delegations
                    advertise.dirty = true;
                    find.dirty = true;
                }
                if (wasFindMinion) {
                    find.minion = self;
                    find.active = false;
                    if (directMinions > 1) {
                        if (directMinions == 2) {
                            // We had 2 minions.  The one that was finding
                            // for us left, so now we must advertise for
                            // ourself and tell our remaining minion to
                            // find for us.
                            advertise.active = false;
                            advertise.ClearArgs();
                            stopAdSignalDest = advertise.minion->GetUniqueName();
                            stopAdArgs = advertise.args;
                            advertise.minion = self;
                            QCC_DbgPrintf(("Set advertise minion to ourself"));
                        }

                        NextDirectMinion(find.minion);
                    }
                    // ... else our only minion was finding for us.  We'll
                    // have to find for ourself now.

                    QCC_DbgPrintf(("Selected %s as our find minion.",
                                   (find.minion == self) ? "ourself" :
                                   find.minion->GetBusAddress().ToString().c_str()));
                }

                if (wasAdvertiseMinion) {
                    assert(directMinions > (IsDrone() ? 0 : 1));
                    advertise.minion = self;
                    advertise.active = false;

                    if (directMinions > 2) {
                        // We had more than 2 minions, so at least one is
                        // idle.  Select the next available minion and to
                        // do the advertising for us.
                        NextDirectMinion(advertise.minion);
                    }
                    // ... else we had 2 minions (or 1 if we were a
                    // Drone). The one that was advertising for us left, so
                    // now we must advertise for ourself.

                    QCC_DbgPrintf(("Selected %s as our advertise minion.",
                                   (advertise.minion == self) ? "ourself" :
                                   advertise.minion->GetBusAddress().ToString().c_str()));
                }

                --directMinions;
            }

            nodeDB.RemoveNode(minion);

            updateDelegations = true;

            if (IsMaster() && !minion->AdvertiseNamesEmpty()) {
                // The minion we lost was advertising one or more names.  We need
                // to setup to expire those advertised names.
                Timespec now;
                GetTimeNow(&now);
                uint64_t expireTime = now.GetAbsoluteMillis() + LOST_DEVICE_TIMEOUT;
                minion->SetExpireTime(expireTime);
                foundNodeDB.AddNode(minion);

                ResetExpireNameAlarm();
            }
        }
    }

    if (updateDelegations) {
        UpdateDelegations(advertise);
        UpdateDelegations(find);
        QCC_DbgPrintf(("NodeDB after processing lost node"));
        QCC_DEBUG_ONLY(DumpNodeStateTable());
    }
    lock.Unlock();

    if (!stopAdSignalDest.empty()) {
        Signal(stopAdSignalDest.c_str(), 0, *advertise.delegateSignal, stopAdArgs->args, stopAdArgs->argsSize);
    }
}


QStatus BTController::DeferredSendSetState(const BTBusAddress& addr, const qcc::String& busName)
{
    QCC_DbgTrace(("BTController::DeferredSendSetState(addr = %s, busName = %s)",
                  addr.ToString().c_str(), busName.c_str()));
    assert(!master);

    QStatus status;
    vector<MsgArg> nodeStateArgsStorage;
    vector<MsgArg> foundNodeArgsStorage;
    MsgArg args[SIG_SET_STATE_IN_SIZE];
    size_t numArgs = ArraySize(args);
    Message reply(bus);
    ProxyBusObject* newMaster = new ProxyBusObject(bus, busName.c_str(), bluetoothObjPath, 0);

    lock.Lock();
    if (UseLocalFind() && find.active) {
        /*
         * Gotta shut down the local find operation since the exchange
         * of the SetState method call and response which will result
         * in one side or the other taking control of who performs the
         * find operation.
         */
        QCC_DbgPrintf(("Stopping find..."));
        bt.StopFind();
        find.active = false;
    }
    if (UseLocalAdvertise() && advertise.active) {
        /*
         * Gotta shut down the local advertise operation since the
         * exchange for the SetState method call and response which
         * will result in one side or the other taking control of who
         * performs the advertise operation.
         */
        QCC_DbgPrintf(("Stopping advertise..."));
        bt.StopAdvertise();
        advertise.active = false;
    }

    newMaster->AddInterface(*org.alljoyn.Bus.BTController.interface);

    QCC_DbgPrintf(("SendSetState prep args"));
    FillNodeStateMsgArgs(nodeStateArgsStorage);
    FillFoundNodesMsgArgs(foundNodeArgsStorage, foundNodeDB);

    status = MsgArg::Set(args, numArgs, SIG_SET_STATE_IN,
                         directMinions,
                         masterUUIDRev,
                         self->GetBusAddress().addr.GetRaw(),
                         self->GetBusAddress().psm,
                         nodeStateArgsStorage.size(), &nodeStateArgsStorage.front(),
                         foundNodeArgsStorage.size(), &foundNodeArgsStorage.front());
    if (status != ER_OK) {
        delete newMaster;
        QCC_LogError(status, ("Dropping %s due to internal error", busName.c_str()));
        bt.Disconnect(busName);
        goto exit;
    }

    /*
     * There is a small chance that 2 devices initiating a connection to each
     * other may each send the SetState method call simultaneously.  We
     * release the lock while making the synchronous method call to prevent a
     * possible deadlock in that case.  The SendSetState function must not run
     * in the same thread as that HandleSetState function.
     */
    exchangingState.insert(addr);
    lock.Unlock();
    QCC_DbgPrintf(("Sending SetState method call to %s (%s)", busName.c_str(), addr.ToString().c_str()));
    status = newMaster->MethodCallAsync(*org.alljoyn.Bus.BTController.SetState,
                                        this, ReplyHandler(&BTController::HandleSetStateReply),
                                        args, ArraySize(args),
                                        new SetStateReplyContext(newMaster, addr, busName));

    if (status != ER_OK) {
        delete newMaster;
        QCC_LogError(status, ("Dropping %s due to internal error", busName.c_str()));
        bt.Disconnect(busName);
    }

exit:
    return status;
}


QStatus BTController::DistributeAdvertisedNameChanges(const BTNodeDB* newAdInfo,
                                                      const BTNodeDB* oldAdInfo)
{
    QCC_DbgTrace(("BTController::DistributeAdvertisedNameChanges(newAdInfo = <%lu nodes>, oldAdInfo = <%lu nodes>)",
                  newAdInfo ? newAdInfo->Size() : 0, oldAdInfo ? oldAdInfo->Size() : 0));
    QStatus status = ER_OK;

    /*
     * Lost names in oldAdInfo must be sent out before found names in
     * newAdInfo.  The same advertised names for a given device may appear in
     * both.  This happens when the underlying connect address changes.  This
     * can result in a device that previously failed to connect to become
     * successfully connectable.  AllJoyn client apps will not know this
     * happens unless they get a LostAdvertisedName signal followed by a
     * FoundAdvertisedName signal.
     */

    if (oldAdInfo) oldAdInfo->DumpTable("oldAdInfo - Old ad information");
    if (newAdInfo) newAdInfo->DumpTable("newAdInfo - New ad information");

    // Now inform everyone of the changes in advertised names.
    if (!IsMinion()) {
        vector<BTNodeInfo> destNodesOld;
        vector<BTNodeInfo> destNodesNew;
        nodeDB.Lock();
        destNodesOld.reserve(nodeDB.Size());
        destNodesNew.reserve(nodeDB.Size());
        for (BTNodeDB::const_iterator it = nodeDB.Begin(); it != nodeDB.End(); ++it) {
            const BTNodeInfo& node = *it;
            if (node->IsDirectMinion() && !node->FindNamesEmpty() && (node != self)) {
                QCC_DbgPrintf(("Notify %s of the name changes.", node->GetBusAddress().ToString().c_str()));
                if (oldAdInfo && oldAdInfo->Size() > 0) {
                    destNodesOld.push_back(node);
                }
                if (newAdInfo && newAdInfo->Size() > 0) {
                    destNodesNew.push_back(node);
                }
            }
        }
        nodeDB.Unlock();

        for (vector<BTNodeInfo>::const_iterator it = destNodesOld.begin(); it != destNodesOld.end(); ++it) {
            status = SendFoundNamesChange(*it, *oldAdInfo, true);
        }

        for (vector<BTNodeInfo>::const_iterator it = destNodesNew.begin(); it != destNodesNew.end(); ++it) {
            status = SendFoundNamesChange(*it, *newAdInfo, false);
        }
    }

    BTNodeDB::const_iterator nodeit;
    // Tell ourself about the names (This is best done outside the Lock just in case).
    if (oldAdInfo) {
        for (nodeit = oldAdInfo->Begin(); nodeit != oldAdInfo->End(); ++nodeit) {
            const BTNodeInfo& node = *nodeit;
            if ((node->AdvertiseNamesSize() > 0) && (node != self)) {
                vector<String> vectorizedNames;
                vectorizedNames.reserve(node->AdvertiseNamesSize());
                vectorizedNames.assign(node->GetAdvertiseNamesBegin(), node->GetAdvertiseNamesEnd());
                bt.FoundNamesChange(node->GetGUID(), vectorizedNames, node->GetBusAddress().addr, node->GetBusAddress().psm, true);
            }
        }
    }
    if (newAdInfo) {
        for (nodeit = newAdInfo->Begin(); nodeit != newAdInfo->End(); ++nodeit) {
            const BTNodeInfo& node = *nodeit;
            if ((node->AdvertiseNamesSize() > 0) && (node != self)) {
                vector<String> vectorizedNames;
                vectorizedNames.reserve(node->AdvertiseNamesSize());
                vectorizedNames.assign(node->GetAdvertiseNamesBegin(), node->GetAdvertiseNamesEnd());
                bt.FoundNamesChange(node->GetGUID(), vectorizedNames, node->GetBusAddress().addr, node->GetBusAddress().psm, false);
            }
        }
    }

    return status;
}


QStatus BTController::SendFoundNamesChange(const BTNodeInfo& destNode,
                                           const BTNodeDB& adInfo,
                                           bool lost)
{
    QCC_DbgTrace(("BTController::SendFoundNamesChange(destNode = \"%s\", adInfo = <>, <%s>)",
                  destNode->GetBusAddress().ToString().c_str(),
                  lost ? "lost" : "found/changed"));

    vector<MsgArg> nodeList;

    FillFoundNodesMsgArgs(nodeList, adInfo);

    MsgArg arg(SIG_FOUND_NAMES, nodeList.size(), &nodeList.front());
    QStatus status;
    if (lost) {
        status = Signal(destNode->GetUniqueName().c_str(), 0,
                        *org.alljoyn.Bus.BTController.LostNames,
                        &arg, 1);
    } else {
        status = Signal(destNode->GetUniqueName().c_str(), 0,
                        *org.alljoyn.Bus.BTController.FoundNames,
                        &arg, 1);
    }

    return status;
}


QStatus BTController::DoNameOp(const qcc::String& name,
                               const InterfaceDescription::Member& signal,
                               bool add,
                               NameArgInfo& nameArgInfo)
{
    QCC_DbgTrace(("BTController::DoNameOp(name = %s, signal = %s, add = %s, nameArgInfo = <%s>)",
                  name.c_str(), signal.name.c_str(), add ? "true" : "false",
                  (&nameArgInfo == static_cast<NameArgInfo*>(&find)) ? "find" : "advertise"));
    QStatus status = ER_OK;

    lock.Lock();
    if (add) {
        nameArgInfo.AddName(name, self);
    } else {
        nameArgInfo.RemoveName(name, self);
    }

    nameArgInfo.dirty = true;

    bool devAvail = devAvailable;
    bool isMaster = IsMaster();
    lock.Unlock();

    if (devAvail) {
        if (isMaster) {
            QCC_DbgPrintf(("Handling %s locally (we're the master)", signal.name.c_str()));

#ifndef NDEBUG
            if (add && (&nameArgInfo == static_cast<NameArgInfo*>(&find))) {
                discoverStartTime = discoverTimer.StartTime();
            }
#endif

            DispatchOperation(new UpdateDelegationsDispatchInfo());

        } else {
            QCC_DbgPrintf(("Sending %s to our master: %s", signal.name.c_str(), master->GetServiceName().c_str()));
            MsgArg args[SIG_NAME_OP_SIZE];
            size_t argsSize = ArraySize(args);
            MsgArg::Set(args, argsSize, SIG_NAME_OP,
                        self->GetBusAddress().addr.GetRaw(),
                        self->GetBusAddress().psm,
                        name.c_str());
            status = Signal(master->GetServiceName().c_str(), 0, signal, args, argsSize);
        }
    }

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
    bool findOp = (fn || cfn);
    NameArgInfo* nameCollection = (findOp ?
                                   static_cast<NameArgInfo*>(&find) :
                                   static_cast<NameArgInfo*>(&advertise));
    char* nameStr;
    uint64_t addrRaw;
    uint16_t psm;

    QStatus status = msg->GetArgs(SIG_NAME_OP, &addrRaw, &psm, &nameStr);

    if (status == ER_OK) {
        BTBusAddress addr(addrRaw, psm);
        BTNodeInfo node = nodeDB.FindNode(addr);

        if (node->IsValid()) {
            QCC_DbgPrintf(("%s %s %s the list of %s names for %s.",
                           addName ? "Adding" : "Removing",
                           nameStr,
                           addName ? "to" : "from",
                           findOp ? "find" : "advertise",
                           node->GetBusAddress().ToString().c_str()));

            lock.Lock();

            // All nodes need to be registered via SetState
            qcc::String name(nameStr);
            if (addName) {
                nameCollection->AddName(name, node);
            } else {
                nameCollection->RemoveName(name, node);
            }

            bool isMaster = IsMaster();
            lock.Unlock();

            if (isMaster) {
                DispatchOperation(new UpdateDelegationsDispatchInfo());

                if (findOp) {
                    if (addName && (node->FindNamesSize() == 1)) {
                        // Prime the name cache for our minion
                        status = SendFoundNamesChange(node, nodeDB, false);
                    } else if (!addName && node->FindNamesEmpty()) {
                        // Clear out the name cache for our minion
                        status = SendFoundNamesChange(node, nodeDB, true);
                    }  // else do nothing
                } else {
                    BTNodeDB newAdInfo;
                    BTNodeDB oldAdInfo;
                    BTNodeInfo nodeChange(node->GetBusAddress(), node->GetUniqueName(), node->GetGUID());
                    nodeChange->AddAdvertiseName(name);
                    if (addName) {
                        newAdInfo.AddNode(nodeChange);
                    } else {
                        oldAdInfo.AddNode(nodeChange);
                    }
                    DistributeAdvertisedNameChanges(&newAdInfo, &oldAdInfo);
                }

            } else {
                // We are a drone so pass on the name
                const MsgArg* args;
                size_t numArgs;
                msg->GetArgs(numArgs, args);
                Signal(master->GetServiceName().c_str(), 0, *member, args, numArgs);
            }
        } else {
            QCC_LogError(ER_FAIL, ("Did not find node %s in node DB", addr.ToString().c_str()));
        }
    } else {
        QCC_LogError(status, ("Processing msg args"));
    }
}


void BTController::HandleSetState(const InterfaceDescription::Member* member, Message& msg)
{
    QCC_DbgTrace(("BTController::HandleSetState(member = \"%s\", msg = <>)", member->name.c_str()));
    qcc::String sender = msg->GetSender();
    BTEndpoint* ep = static_cast<BTEndpoint*>(bt.LookupEndpoint(sender));

    if ((ep == NULL) ||
        (ep->GetBTBusAddress().IsValid()) ||
        (nodeDB.FindNode(ep->GetRemoteName())->IsValid())) {
        /* We don't acknowledge anyone calling the SetState method call who
         * fits into one of these categories:
         *
         * - Not a Bluetooth endpoint
         * - Not an incoming connection
         * - Has already called SetState
         *
         * Don't send a response as punishment >:)
         */
        if (ep) {
            bt.ReturnEndpoint(ep);
        }
        return;
    }

    uint32_t remoteProtocolVersion = ep->GetRemoteProtocolVersion();
    bt.ReturnEndpoint(ep);

    uint8_t numConnections;
    QStatus status;
    uint64_t rawBDAddr;
    uint16_t psm;
    uint32_t otherUUIDRev;
    size_t numNodeStateArgs;
    MsgArg* nodeStateArgs;
    size_t numFoundNodeArgs;
    MsgArg* foundNodeArgs;
    bool updateDelegations = false;

    lock.Lock();
    if (!IsMaster()) {
        // We are not the master so we should not get a SetState method call.
        // Don't send a response as punishment >:)
        QCC_LogError(ER_FAIL, ("SetState method call received while not a master"));
        lock.Unlock();
        return;
    }

    status = msg->GetArgs(SIG_SET_STATE_IN,
                          &numConnections,
                          &otherUUIDRev,
                          &rawBDAddr,
                          &psm,
                          &numNodeStateArgs, &nodeStateArgs,
                          &numFoundNodeArgs, &foundNodeArgs);

    if (status != ER_OK) {
        lock.Unlock();
        MethodReply(msg, "org.alljoyn.Bus.BTController.InternalError", QCC_StatusText(status));
        bt.Disconnect(sender);
        return;
    }

    const BTBusAddress addr(rawBDAddr, psm);
    MsgArg args[SIG_SET_STATE_OUT_SIZE];
    size_t numArgs = ArraySize(args);
    vector<MsgArg> nodeStateArgsStorage;
    vector<MsgArg> foundNodeArgsStorage;

    if (addr == self->GetBusAddress()) {
        // We should never get a connection from a device with the same address as our own.
        // Don't send a response as punishment >:)
        QCC_LogError(ER_FAIL, ("SetState method call received with remote bus address the same as ours (%s)",
                               addr.ToString().c_str()));
        lock.Unlock();
        bt.Disconnect(sender);
        return;
    }

    // Check if we're receiving a SetState method call from a device we have
    // just sent a SetState method call to.  If so ignore their state
    // information if their address is greater than ours.
    if ((exchangingState.find(addr) == exchangingState.end()) || (addr < self->GetBusAddress())) {
        if (UseLocalFind() && find.active) {
            QCC_DbgPrintf(("Stopping find..."));
            bt.StopFind();
            find.active = false;
        }

        FillFoundNodesMsgArgs(foundNodeArgsStorage, foundNodeDB);

        if ((remoteProtocolVersion > ALLJOYN_PROTOCOL_VERSION) ||
            ((remoteProtocolVersion == ALLJOYN_PROTOCOL_VERSION) && (numConnections > directMinions))) {
            // We are now a minion (or a drone if we have more than one direct connection)
            master = new ProxyBusObject(bus, sender.c_str(), bluetoothObjPath, 0);
            masterNode = BTNodeInfo(addr, sender);
            masterNode->SetUUIDRev(otherUUIDRev);

            if (dispatcher.HasAlarm(expireAlarm)) {
                dispatcher.RemoveAlarm(expireAlarm);
            }

            FillNodeStateMsgArgs(nodeStateArgsStorage);

            status = ImportState(addr, NULL, 0, foundNodeArgs, numFoundNodeArgs);
            if (status != ER_OK) {
                lock.Unlock();
                MethodReply(msg, "org.alljoyn.Bus.BTController.InternalError", QCC_StatusText(status));
                bt.Disconnect(sender);
                return;
            }

        } else {
            // We are still the master

            // Add information about the already connected nodes to the found
            // node data so that our new minions will have up-to-date
            // advertising information about our existing minions.
            FillFoundNodesMsgArgs(foundNodeArgsStorage, nodeDB);

            bool noRotateMinions = !RotateMinions();
            status = ImportState(addr, nodeStateArgs, numNodeStateArgs, foundNodeArgs, numFoundNodeArgs);
            if (status != ER_OK) {
                lock.Unlock();
                QCC_LogError(status, ("Dropping %s due to import state error", sender.c_str()));
                bt.Disconnect(sender);
                return;
            }

            assert(find.resultDest.empty());
            if (noRotateMinions && RotateMinions()) {
                // Force changing from permanent delegations to durational delegations
                advertise.dirty = true;
                find.dirty = true;
            }
        }

        QCC_DbgPrintf(("We are %s, %s is now our %s",
                       IsMaster() ? "still the master" : (IsDrone() ? "now a drone" : "just a minion"),
                       addr.ToString().c_str(), IsMaster() ? "minion" : "master"));

        if (!IsMinion()) {
            if (IsMaster()) {
                // Can't let the to-be-updated masterUUIDRev have a value that is
                // the same as the UUIDRev value used by our new minion.
                const uint32_t lowerBound = ((otherUUIDRev > (bt::INVALID_UUIDREV + 10)) ?
                                             (otherUUIDRev - 10) :
                                             bt::INVALID_UUIDREV);
                const uint32_t upperBound = ((otherUUIDRev < (numeric_limits<uint32_t>::max() - 10)) ?
                                             (otherUUIDRev + 10) :
                                             numeric_limits<uint32_t>::max());
                while ((masterUUIDRev == bt::INVALID_UUIDREV) &&
                       (masterUUIDRev > lowerBound) &&
                       (masterUUIDRev < upperBound)) {
                    masterUUIDRev = qcc::Rand32();
                }
            }
            updateDelegations = true;
        }

        if (IsMaster()) {
            ResetExpireNameAlarm();
        } else {
            RemoveExpireNameAlarm();
            dispatcher.RemoveAlarm(stopAd);
        }

        status = MsgArg::Set(args, numArgs, SIG_SET_STATE_OUT,
                             masterUUIDRev,
                             self->GetBusAddress().addr.GetRaw(),
                             self->GetBusAddress().psm,
                             nodeStateArgsStorage.size(), &nodeStateArgsStorage.front(),
                             foundNodeArgsStorage.size(), &foundNodeArgsStorage.front());
    } else {
        /*
         * Both sides are sending SetState method calls simultaneously.  We
         * decided to ignore this state information in preference to the state
         * information we will receive from from our SetState method call.  We
         * need to inform the other side that we are ignoring this SetState
         * method call.
         */
        status = MsgArg::Set(args, numArgs, SIG_SET_STATE_OUT,
                             bt::INVALID_UUIDREV,  // Tell the other side to skip this response
                             self->GetBusAddress().addr.GetRaw(),
                             self->GetBusAddress().psm,
                             static_cast<size_t>(0), NULL,
                             static_cast<size_t>(0), NULL);
    }

    lock.Unlock();

    if (status != ER_OK) {
        QCC_LogError(status, ("MsgArg::Set(%s)", SIG_SET_STATE_OUT));
        bt.Disconnect(sender);
        return;
    }

    status = MethodReply(msg, args, numArgs);
    if (status != ER_OK) {
        QCC_LogError(status, ("MethodReply"));
        bt.Disconnect(sender);
        return;
    }

    if (updateDelegations) {
        DispatchOperation(new UpdateDelegationsDispatchInfo());
    }
}


void BTController::HandleSetStateReply(Message& msg, void* context)
{
    QCC_DbgTrace(("BTController::HandleSetStateReply(reply = <>, context = %p)", context));
    SetStateReplyContext* ctx = reinterpret_cast<SetStateReplyContext*>(context);
    DispatchOperation(new ProcessSetStateReplyDispatchInfo(msg, ctx->newMaster, ctx->addr, ctx->busName));
}


void BTController::DeferredProcessSetStateReply(Message& reply,
                                                ProxyBusObject* newMaster,
                                                const BTBusAddress& addr,
                                                const String& busName)
{
    QCC_DbgTrace(("BTController::DeferredProcessSetStateReply(reply = <>, newMaster = %p, addr = %s, busName = \"%s\")",
                  newMaster, addr.ToString().c_str(), busName.c_str()));

    lock.Lock();
    exchangingState.erase(addr);

    if (reply->GetType() == MESSAGE_METHOD_RET) {
        size_t numNodeStateArgs;
        MsgArg* nodeStateArgs;
        size_t numFoundNodeArgs;
        MsgArg* foundNodeArgs;
        uint64_t rawBDAddr;
        uint16_t psm;
        uint32_t otherUUIDRev;
        QStatus status;

        if (nodeDB.FindNode(addr)->IsValid()) {
            QCC_DbgHLPrintf(("Already got node state information."));
            delete newMaster;
            goto exit;
        }

        status = reply->GetArgs(SIG_SET_STATE_OUT,
                                &otherUUIDRev,
                                &rawBDAddr,
                                &psm,
                                &numNodeStateArgs, &nodeStateArgs,
                                &numFoundNodeArgs, &foundNodeArgs);
        if (status != ER_OK) {
            delete newMaster;
            QCC_LogError(status, ("Dropping %s due to error parsing the args (sig: \"%s\")",
                                  busName.c_str(), SIG_SET_STATE_OUT));
            bt.Disconnect(busName);
            goto exit;
        }

        BTBusAddress addr(rawBDAddr, psm);

        if (otherUUIDRev != bt::INVALID_UUIDREV) {
            if (numNodeStateArgs == 0) {
                // We are now a minion (or a drone if we have more than one direct connection)
                master = newMaster;
                masterNode = foundNodeDB.FindNode(addr);
                masterNode->SetUUIDRev(otherUUIDRev);

                if (dispatcher.HasAlarm(expireAlarm)) {
                    dispatcher.RemoveAlarm(expireAlarm);
                }

                status = ImportState(addr, NULL, 0, foundNodeArgs, numFoundNodeArgs);
                if (status != ER_OK) {
                    QCC_LogError(status, ("Dropping %s due to import state error", busName.c_str()));
                    bt.Disconnect(busName);
                    goto exit;
                }

            } else {
                // We are the still the master
                bool noRotateMinions = !RotateMinions();
                delete newMaster;

                status = ImportState(addr, nodeStateArgs, numNodeStateArgs, foundNodeArgs, numFoundNodeArgs);
                if (status != ER_OK) {
                    QCC_LogError(status, ("Dropping %s due to import state error", busName.c_str()));
                    bt.Disconnect(busName);
                    goto exit;
                }

                assert(find.resultDest.empty());
                if (noRotateMinions && RotateMinions()) {
                    // Force changing from permanent delegations to durational delegations
                    advertise.dirty = true;
                    find.dirty = true;
                }
            }

            QCC_DbgPrintf(("We are %s, %s is now our %s",
                           IsMaster() ? "still the master" : (IsDrone() ? "now a drone" : "just a minion"),
                           addr.ToString().c_str(), IsMaster() ? "minion" : "master"));

            if (!IsMinion()) {
                if (IsMaster()) {
                    // Can't let the to-be-updated masterUUIDRev have a value that is
                    // the same as the UUIDRev value used by our new minion.
                    const uint32_t lowerBound = ((otherUUIDRev > (bt::INVALID_UUIDREV + 10)) ?
                                                 (otherUUIDRev - 10) :
                                                 bt::INVALID_UUIDREV);
                    const uint32_t upperBound = ((otherUUIDRev < (numeric_limits<uint32_t>::max() - 10)) ?
                                                 (otherUUIDRev + 10) :
                                                 numeric_limits<uint32_t>::max());
                    while ((masterUUIDRev == bt::INVALID_UUIDREV) &&
                           (masterUUIDRev > lowerBound) &&
                           (masterUUIDRev < upperBound)) {
                        masterUUIDRev = qcc::Rand32();
                    }
                }

                UpdateDelegations(advertise);
                UpdateDelegations(find);
                if (IsDrone()) {
                    if (advertise.minion != self) {
                        advertise.minion = self;
                        QCC_DbgPrintf(("Set advertise minion to ourself"));
                    }
                    if (find.minion != self) {
                        find.minion = self;
                        QCC_DbgPrintf(("Set find minion to ourself"));
                    }
                }
            }

            if (IsMaster()) {
                ResetExpireNameAlarm();
            } else {
                RemoveExpireNameAlarm();
                dispatcher.RemoveAlarm(stopAd);
            }
        }
    } else {
        delete newMaster;
        qcc::String errMsg;
        const char* errName = reply->GetErrorName(&errMsg);
        QCC_LogError(ER_FAIL, ("Dropping %s due to internal error: %s - %s", busName.c_str(), errName, errMsg.c_str()));
        bt.Disconnect(busName);
    }

exit:
    lock.Unlock();
}


void BTController::HandleDelegateOp(const InterfaceDescription::Member* member,
                                    const char* sourcePath,
                                    Message& msg)
{
    QCC_DbgTrace(("BTController::HandleDelegateOp(member = \"%s\", sourcePath = %s, msg = <>)",
                  member->name.c_str(), sourcePath));
    bool findOp = member == org.alljoyn.Bus.BTController.DelegateFind;
    if (IsMaster() || (strcmp(sourcePath, bluetoothObjPath) != 0) ||
        (master->GetServiceName() != msg->GetSender())) {
        // We only accept delegation commands from our master!
        QCC_DbgHLPrintf(("%s tried to delegate %s to us; our master is %s",
                         msg->GetSender(),
                         findOp ? "find" : "advertise",
                         IsMaster() ? "ourself" : master->GetServiceName().c_str()));
        return;
    }

    DispatchOperation(new HandleDelegateOpDispatchInfo(msg, findOp));
}


void BTController::DeferredHandleDelegateFind(Message& msg)
{
    QCC_DbgTrace(("BTController::HandleDelegateFind(msg = <>)"));

    lock.Lock();
    if (IsMinion()) {
        char* resultDest;
        uint64_t* ignoreAddrsArg;
        size_t numIgnoreAddrs;
        uint32_t duration;

        QStatus status = msg->GetArgs(SIG_DELEGATE_FIND, &resultDest, &numIgnoreAddrs, &ignoreAddrsArg, &duration);

        if (status == ER_OK) {
            if (resultDest && (resultDest[0] != '\0')) {
                size_t i;
                BDAddressSet ignoreAddrs;
                find.resultDest = resultDest;
                for (i = 0; i < numIgnoreAddrs; ++i) {
                    ignoreAddrs->insert(ignoreAddrsArg[i]);
                }

                QCC_DbgPrintf(("Starting find for %u seconds...", duration));
                status = bt.StartFind(ignoreAddrs, duration);
                find.active = (status == ER_OK);
            } else {
                QCC_DbgPrintf(("Stopping find..."));
                bt.StopFind();
                find.active = false;
            }
        }
    } else {
        const MsgArg* args;
        size_t numArgs;

        msg->GetArgs(numArgs, args);

        // Pick a minion to do the work for us.
        NextDirectMinion(find.minion);
        QCC_DbgPrintf(("Selected %s as our find minion.", find.minion->GetBusAddress().ToString().c_str()));

        Signal(find.minion->GetUniqueName().c_str(), 0, *find.delegateSignal, args, numArgs);
    }
    lock.Unlock();
}


void BTController::DeferredHandleDelegateAdvertise(Message& msg)
{
    QCC_DbgTrace(("BTController::DeferredHandleDelegateAdvertise(msg = <>)"));

    lock.Lock();
    if (IsMinion()) {
        uint32_t uuidRev;
        uint64_t bdAddrRaw;
        uint16_t psm;
        BTNodeDB adInfo;
        MsgArg* entries;
        size_t size;
        uint32_t duration;

        QStatus status = msg->GetArgs(SIG_DELEGATE_AD, &uuidRev, &bdAddrRaw, &psm, &size, &entries, &duration);

        if (status == ER_OK) {
            status = ExtractAdInfo(entries, size, adInfo);
        }

        if (status == ER_OK) {
            if (adInfo.Size() > 0) {
                BDAddress bdAddr(bdAddrRaw);

                QCC_DbgPrintf(("Starting advertise for %u seconds...", duration));
                status = bt.StartAdvertise(uuidRev, bdAddr, psm, adInfo, duration);
                advertise.active = (status == ER_OK);
            } else {
                QCC_DbgPrintf(("Stopping advertise..."));
                bt.StopAdvertise();
                advertise.active = false;
            }
        }
    } else {
        const MsgArg* args;
        size_t numArgs;

        msg->GetArgs(numArgs, args);

        // Pick a minion to do the work for us.
        NextDirectMinion(advertise.minion);
        QCC_DbgPrintf(("Selected %s as our advertise minion.", advertise.minion->GetBusAddress().ToString().c_str()));

        Signal(advertise.minion->GetUniqueName().c_str(), 0, *advertise.delegateSignal, args, numArgs);
    }
    lock.Unlock();
}


void BTController::HandleFoundNamesChange(const InterfaceDescription::Member* member,
                                          const char* sourcePath,
                                          Message& msg)
{
    QCC_DbgTrace(("BTController::HandleFoundNamesChange(member = %s, sourcePath = \"%s\", msg = <>)",
                  member->name.c_str(), sourcePath));

    if (IsMaster() || (strcmp(sourcePath, bluetoothObjPath) != 0) ||
        (master->GetServiceName() != msg->GetSender())) {
        // We only accept FoundNames signals from our direct master!
        return;
    }

    BTNodeDB adInfo;
    bool lost = (member == org.alljoyn.Bus.BTController.LostNames);
    MsgArg* entries;
    size_t size;

    QStatus status = msg->GetArgs(SIG_FOUND_NAMES, &size, &entries);

    if (status == ER_OK) {
        status = ExtractNodeInfo(entries, size, adInfo);
    }

    if ((status == ER_OK) && (adInfo.Size() > 0)) {

        // Figure out which name changes belong to which DB (nodeDB or foundNodeDB).
        BTNodeDB minionDB;
        BTNodeDB externalDB;
        nodeDB.Diff(adInfo, &externalDB, NULL);
        externalDB.Diff(adInfo, &minionDB, NULL);

        const BTNodeDB* newAdInfo = lost ? NULL : &adInfo;
        const BTNodeDB* oldAdInfo = lost ? &adInfo : NULL;
        const BTNodeDB* newMinionDB = lost ? NULL : &minionDB;
        const BTNodeDB* oldMinionDB = lost ? &minionDB : NULL;
        const BTNodeDB* newExternalDB = lost ? NULL : &externalDB;
        const BTNodeDB* oldExternalDB = lost ? &externalDB : NULL;

        nodeDB.UpdateDB(newMinionDB, oldMinionDB);
        foundNodeDB.UpdateDB(newExternalDB, oldExternalDB);
        foundNodeDB.DumpTable("foundNodeDB - Updated set of found devices");

        DistributeAdvertisedNameChanges(newAdInfo, oldAdInfo);
    }
}


void BTController::HandleFoundDeviceChange(const InterfaceDescription::Member* member,
                                           const char* sourcePath,
                                           Message& msg)
{
    QCC_DbgTrace(("BTController::HandleFoundDeviceChange(member = %s, sourcePath = \"%s\", msg = <>)",
                  member->name.c_str(), sourcePath));

    if (!nodeDB.FindNode(msg->GetSender())->IsDirectMinion()) {
        // We only handle FoundDevice signals from our minions.
        QCC_LogError(ER_FAIL, ("Received %s from %s who is NOT a direct minion.",
                               msg->GetMemberName(), msg->GetSender()));
        return;
    }

    uint32_t uuidRev;
    uint64_t adBdAddrRaw;

    QStatus status = msg->GetArgs(SIG_FOUND_DEV, &adBdAddrRaw, &uuidRev);

    if (status == ER_OK) {
        BDAddress adBdAddr(adBdAddrRaw);
        ProcessDeviceChange(adBdAddr, uuidRev);
    }
}


QStatus BTController::ImportState(const BTBusAddress& addr,
                                  MsgArg* nodeStateArgs,
                                  size_t numNodeStates,
                                  MsgArg* foundNodeArgs,
                                  size_t numFoundNodes)
{
    QCC_DbgTrace(("BTController::ImportState(addr = (%s), nodeStateArgs = <>, numNodeStates = %u, foundNodeArgs = <>, numFoundNodes = %u)",
                  addr.ToString().c_str(), numNodeStates, numFoundNodes));

    /*
     * Here we need to bring in the state information from one or more nodes
     * that have just connected to the bus.  Typically, only one node will be
     * connecting, but it is possible for a piconet or even a scatternet of
     * nodes to connect.  Since we are processing the ImportState() we are by
     * definition the master and importing the state information of new
     * minions.
     *
     * In most cases, we actually already have all the information in the
     * foundNodeDB gathered via advertisements.  However, it is possible that
     * the information cached in foundNodeDB is stale and we will be getting
     * the latest and greatest information via the SetState method call.  We
     * need to determine if and what those changes are then tell our existing
     * connected minions.
     */

    QStatus status;
    size_t i;

    BTNodeDB incomingDB;
    BTNodeDB addedDB;
    BTNodeDB removedDB;
    BTNodeDB staleDB;
    BTNodeDB newFoundDB;
    BTNodeInfo connectingNode(addr);
    connectingNode->SetDirectMinion(true);

    for (i = 0; i < numNodeStates; ++i) {
        char* bn;
        char* guidStr;
        uint64_t rawBdAddr;
        uint16_t psm;
        size_t anSize, fnSize;
        MsgArg* anList;
        MsgArg* fnList;
        size_t j;
        BTNodeInfo node;

        status = nodeStateArgs[i].Get(SIG_NODE_STATE_ENTRY,
                                      &guidStr,
                                      &bn,
                                      &rawBdAddr, &psm,
                                      &anSize, &anList,
                                      &fnSize, &fnList);
        if (status != ER_OK) {
            return status;
        }

        String busName(bn);
        BTBusAddress nodeAddr(BDAddress(rawBdAddr), psm);
        String guid(guidStr);

        QCC_DbgPrintf(("Processing names for new minion %s:", nodeAddr.ToString().c_str()));

        if (nodeAddr == connectingNode->GetBusAddress()) {
            node = connectingNode;  // need to modify the existing instance since other nodes already refer to it.
            node->SetGUID(guid);
            node->SetUniqueName(busName);
        } else {
            node = BTNodeInfo(nodeAddr, busName, guid);
            node->SetConnectNode(connectingNode);
        }

        /*
         * NOTE: expiration time is explicitly NOT set for nodes that we are
         * connected to.  Their advertisements will go away when the node
         * disconnects.
         */

        advertise.dirty = advertise.dirty || (anSize > 0);
        find.dirty = find.dirty || (fnSize > 0);

        for (j = 0; j < anSize; ++j) {
            char* n;
            status = anList[j].Get(SIG_NAME, &n);
            if (status != ER_OK) {
                return status;
            }
            QCC_DbgPrintf(("    Ad Name: %s", n));
            qcc::String name(n);
            advertise.AddName(name, node);
        }

        for (j = 0; j < fnSize; ++j) {
            char* n;
            status = fnList[j].Get(SIG_NAME, &n);
            if (status != ER_OK) {
                return status;
            }
            QCC_DbgPrintf(("    Find Name: %s", n));
            qcc::String name(n);
            find.AddName(name, node);
        }

        incomingDB.AddNode(node);
        nodeDB.AddNode(node);
    }

    // At this point nodeDB now has all the nodes that have connected to us
    // (if we are the master).

    lock.Lock();  // Must be acquired before the foundNodeDB lock.
    foundNodeDB.Lock();
    // Figure out set of devices/names that are part of the incoming
    // device/piconet to be removed from the set of found nodes.
    foundNodeDB.Diff(incomingDB, &addedDB, &removedDB);

    // addedDB contains the devices being added to our network that was unknown in foundNodeDB.
    // removedDB contains the devices not being added our network that are known to us via foundNodeDB.

    // What we need to find out is which names in foundNodeDB were reachable
    // from the connected address of the node that just connected to us but
    // are not part of the incoming names.  Normally, this should be an empty
    // set but it is not guaranteed to be so.

    BTNodeDB::const_iterator nodeit;
    for (nodeit = removedDB.Begin(); nodeit != removedDB.End(); ++nodeit) {
        // If the connect address for the node from removedDB is in
        // incomingDB, then that node is a stale advertisement.  (This should
        // be rare.)
        if (incomingDB.FindNode((*nodeit)->GetConnectAddress())->IsValid()) {
            staleDB.AddNode(*nodeit);
        }
    }

    // Now we have staleDB which contains those nodes in foundNodeDB that need
    // to be removed (or at least some of the names for nodes in staleDB need
    // to be removed from their respective nodes in foundNodeDB).

    status = ExtractNodeInfo(foundNodeArgs, numFoundNodes, newFoundDB);

    // Now we have newFoundDB which contains advertisement information that
    // the newly connected node knows about.  It may contain information about
    // nodes that we don't know about so we need to incorporate that
    // information into foundNodeDB as well as addedDB for distribution to our
    // minions.  First we'll trim down newFoundDB with what we already know.

    newFoundDB.UpdateDB(NULL, &nodeDB);
    newFoundDB.UpdateDB(NULL, &foundNodeDB);

    addedDB.UpdateDB(&newFoundDB, NULL);

    foundNodeDB.UpdateDB(&newFoundDB, &staleDB);
    foundNodeDB.DumpTable("foundNodeDB - Updated set of found devices from imported state information from new connection");

    if (IsMaster()) {
        ResetExpireNameAlarm();
    } else {
        RemoveExpireNameAlarm();
    }
    foundNodeDB.Unlock();
    lock.Unlock();

    DistributeAdvertisedNameChanges(&addedDB, &staleDB);

    if (IsMaster()) {
        ++directMinions;

        if (find.minion == self) {
            NextDirectMinion(find.minion);
            QCC_DbgPrintf(("Selected %s as our find minion.", find.minion->GetBusAddress().ToString().c_str()));
        }

        if ((advertise.minion == self) && (!UseLocalAdvertise())) {
            NextDirectMinion(advertise.minion);
            QCC_DbgPrintf(("Selected %s as our advertise minion.", advertise.minion->GetBusAddress().ToString().c_str()));
        }
    }

    return ER_OK;
}


void BTController::UpdateDelegations(NameArgInfo& nameInfo)
{
    const bool advertiseOp = (&nameInfo == &advertise);

    QCC_DbgTrace(("BTController::UpdateDelegations(nameInfo = <%s>)",
                  advertiseOp ? "advertise" : "find"));

    const bool allowConn = (!advertiseOp | listening) && IsMaster() && (directMinions < maxConnections);
    const bool changed = nameInfo.Changed();
    const bool empty = nameInfo.Empty();
    const bool active = nameInfo.active;

    const bool start = !active && !empty && allowConn && devAvailable;
    const bool stop = active && (empty || !allowConn);
    const bool restart = active && changed && !empty && allowConn;

    QCC_DbgPrintf(("%s %s operation because device is %s, conn is %s, name list %s%s, and op is %s.",
                   start ? "Starting" : (restart ? "Updating" : (stop ? "Stopping" : "Skipping")),
                   advertiseOp ? "advertise" : "find",
                   devAvailable ? "available" : "not available",
                   allowConn ? "allowed" : "not allowed",
                   changed ? "changed" : "didn't change",
                   empty ? " to empty" : "",
                   active ? "active" : "not active"));

    assert(!(!active && stop));     // assert that we are not "stopping" an operation that is already stopped.
    assert(!(active && start));     // assert that we are not "starting" an operation that is already running.
    assert(!(!active && restart));  // assert that we are not "restarting" an operation that is stopped.
    assert(!(start && stop));
    assert(!(start && restart));
    assert(!(restart && stop));

    if (advertiseOp && changed) {
        ++masterUUIDRev;
        if (masterUUIDRev == bt::INVALID_UUIDREV) {
            ++masterUUIDRev;
        }
        find.dirty = true; // Force updating the ignore UUID
    }

    if (start) {
        // Set the advertise/find arguments.
        nameInfo.StartOp();

    } else if (restart) {
        // Update the advertise/find arguments.
        nameInfo.RestartOp();

    } else if (stop) {
        // Clear out the advertise/find arguments.
        nameInfo.StopOp();
    }
}


QStatus BTController::ExtractAdInfo(const MsgArg* entries, size_t size, BTNodeDB& adInfo)
{
    QCC_DbgTrace(("BTController::ExtractAdInfo()"));

    QStatus status = ER_OK;

    if (entries && (size > 0)) {
        for (size_t i = 0; i < size; ++i) {
            char* guid;
            uint64_t rawAddr;
            uint16_t psm;
            MsgArg* names;
            size_t numNames;

            status = entries[i].Get(SIG_AD_NAME_MAP_ENTRY, &guid, &rawAddr, &psm, &numNames, &names);

            if (status == ER_OK) {
                String guidStr(guid);
                String empty;
                BTBusAddress addr(rawAddr, psm);
                BTNodeInfo node(addr, empty, guidStr);

                QCC_DbgPrintf(("Extracting %u advertise names for %s:",
                               numNames, addr.ToString().c_str()));
                for (size_t j = 0; j < numNames; ++j) {
                    char* name;
                    status = names[j].Get(SIG_NAME, &name);
                    if (status == ER_OK) {
                        QCC_DbgPrintf(("    %s", name));
                        node->AddAdvertiseName(String(name));
                    }
                }
                adInfo.AddNode(node);
            }
        }
    }
    return status;
}


QStatus BTController::ExtractNodeInfo(const MsgArg* entries, size_t size, BTNodeDB& db)
{
    QCC_DbgTrace(("BTController::ExtractNodeInfo()"));

    QStatus status = ER_OK;
    Timespec now;
    GetTimeNow(&now);
    uint64_t expireTime = now.GetAbsoluteMillis() + LOST_DEVICE_TIMEOUT;

    QCC_DbgPrintf(("Extracting node information from %lu connect nodes:", size));

    for (size_t i = 0; i < size; ++i) {
        uint64_t connAddrRaw;
        uint16_t connPSM;
        uint32_t uuidRev;
        size_t adMapSize;
        MsgArg* adMap;
        size_t j;

        status = entries[i].Get(SIG_FOUND_NODE_ENTRY, &connAddrRaw, &connPSM, &uuidRev, &adMapSize, &adMap);
        if (status != ER_OK) {
            QCC_LogError(status, ("Failed MsgArg::Get(\"%s\", ...)", SIG_FOUND_NODE_ENTRY));
            return status;
        }

        BTBusAddress connNodeAddr(BDAddress(connAddrRaw), connPSM);
        if ((self->GetBusAddress() == connNodeAddr) || nodeDB.FindNode(connNodeAddr)->IsValid()) {
            // Don't add ourself or any node on our piconet/scatternet to foundNodeDB.
            QCC_DbgPrintf(("    Skipping nodes with connect address: %s", connNodeAddr.ToString().c_str()));
            continue;
        }

        assert(!db.FindNode(connNodeAddr)->IsValid());
        BTNodeInfo connNode = BTNodeInfo(connNodeAddr);

        for (j = 0; j < adMapSize; ++j) {
            char* guidStr;
            uint64_t rawBdAddr;
            uint16_t psm;
            size_t anSize;
            MsgArg* anList;
            size_t k;

            status = adMap[j].Get(SIG_AD_NAME_MAP_ENTRY, &guidStr, &rawBdAddr, &psm, &anSize, &anList);
            if (status != ER_OK) {
                QCC_LogError(status, ("Failed MsgArg::Get(\"%s\", ...)", SIG_AD_NAME_MAP_ENTRY));
                return status;
            }

            BTBusAddress nodeAddr(BDAddress(rawBdAddr), psm);
            BTNodeInfo node = (nodeAddr == connNode->GetBusAddress()) ? connNode : BTNodeInfo(nodeAddr);

            QCC_DbgPrintf(("    Processing advertised names for device %lu-%lu %s (connectable via %s):",
                           i, j,
                           nodeAddr.ToString().c_str(),
                           connNodeAddr.ToString().c_str()));

            // If the node is in our subnet, then we are the connect node for it from our and our minion's perspectives.
            node->SetConnectNode(nodeDB.FindNode(nodeAddr)->IsValid() ? self : connNode);
            node->SetGUID(String(guidStr));
            node->SetUUIDRev(uuidRev);
            node->SetExpireTime(expireTime);
            for (k = 0; k < anSize; ++k) {
                char* n;
                status = anList[k].Get(SIG_NAME, &n);
                if (status != ER_OK) {
                    QCC_LogError(status, ("Failed MsgArg::Get(\"%s\", ...)", SIG_NAME));
                    return status;
                }
                QCC_DbgPrintf(("        Name: %s", n));
                String name(n);
                node->AddAdvertiseName(name);
            }
            db.AddNode(node);
        }
    }
    return status;
}


void BTController::FillNodeStateMsgArgs(vector<MsgArg>& args) const
{
    BTNodeDB::const_iterator it;

    nodeDB.Lock();
    args.reserve(nodeDB.Size());
    for (it = nodeDB.Begin(); it != nodeDB.End(); ++it) {
        const BTNodeInfo& node = *it;
        QCC_DbgPrintf(("    Node State node %s:", node->GetBusAddress().ToString().c_str()));
        NameSet::const_iterator nit;

        vector<const char*> nodeAdNames;
        nodeAdNames.reserve(node->AdvertiseNamesSize());
        for (nit = node->GetAdvertiseNamesBegin(); nit != node->GetAdvertiseNamesEnd(); ++nit) {
            QCC_DbgPrintf(("        Ad name: %s", nit->c_str()));
            nodeAdNames.push_back(nit->c_str());
        }

        vector<const char*> nodeFindNames;
        nodeFindNames.reserve(node->FindNamesSize());
        for (nit = node->GetFindNamesBegin(); nit != node->GetFindNamesEnd(); ++nit) {
            QCC_DbgPrintf(("        Find name: %s", nit->c_str()));
            nodeFindNames.push_back(nit->c_str());
        }

        args.push_back(MsgArg(SIG_NODE_STATE_ENTRY,
                              node->GetGUID().c_str(),
                              node->GetUniqueName().c_str(),
                              node->GetBusAddress().addr.GetRaw(),
                              node->GetBusAddress().psm,
                              nodeAdNames.size(), &nodeAdNames.front(),
                              nodeFindNames.size(), &nodeFindNames.front()));

        args.back().Stabilize();
    }
    nodeDB.Unlock();
}


void BTController::FillFoundNodesMsgArgs(vector<MsgArg>& args, const BTNodeDB& adInfo)
{
    BTNodeDB::const_iterator it;
    map<BTBusAddress, BTNodeDB> xformMap;
    adInfo.Lock();
    for (it = adInfo.Begin(); it != adInfo.End(); ++it) {
        xformMap[(&adInfo == &nodeDB) ? self->GetBusAddress() : (*it)->GetConnectAddress()].AddNode(*it);
    }
    adInfo.Unlock();

    args.reserve(args.size() + xformMap.size());
    map<BTBusAddress, BTNodeDB>::const_iterator xmit;
    for (xmit = xformMap.begin(); xmit != xformMap.end(); ++xmit) {
        vector<MsgArg> adNamesArgs;

        const BTNodeDB& db = xmit->second;
        BTNodeInfo connNode = db.FindNode(xmit->first);

        if (!connNode->IsValid()) {
            connNode = foundNodeDB.FindNode(xmit->first);

            if (!connNode->IsValid()) {
                // Should never happen, since it is an internal bug (hence assert
                // check below), but gracefully handle it in case it does in
                // release mode.
                QCC_LogError(ER_NONE, ("Failed to find address %s in DB that should contain it!", xmit->first.ToString().c_str()));
                db.DumpTable("db: Corrupt DB?");
                assert(connNode->IsValid());
                continue;
            }
        }

        adNamesArgs.reserve(adInfo.Size());
        for (it = db.Begin(); it != db.End(); ++it) {
            const BTNodeInfo& node = *it;
            NameSet::const_iterator nit;

            vector<const char*> nodeAdNames;
            nodeAdNames.reserve(node->AdvertiseNamesSize());
            for (nit = node->GetAdvertiseNamesBegin(); nit != node->GetAdvertiseNamesEnd(); ++nit) {
                nodeAdNames.push_back(nit->c_str());
            }

            adNamesArgs.push_back(MsgArg(SIG_AD_NAME_MAP_ENTRY,
                                         node->GetGUID().c_str(),
                                         node->GetBusAddress().addr.GetRaw(),
                                         node->GetBusAddress().psm,
                                         nodeAdNames.size(), &nodeAdNames.front()));
            adNamesArgs.back().Stabilize();
        }

        BTBusAddress connAddr = nodeDB.FindNode(xmit->first)->IsValid() ? self->GetBusAddress() : xmit->first;

        args.push_back(MsgArg(SIG_FOUND_NODE_ENTRY,
                              connAddr.addr.GetRaw(),
                              connAddr.psm,
                              connNode->GetUUIDRev(),
                              adNamesArgs.size(), &adNamesArgs.front()));
        args.back().Stabilize();
    }
}


void BTController::ResetExpireNameAlarm()
{
    RemoveExpireNameAlarm();
    uint64_t dispatchTime = foundNodeDB.NextNodeExpiration();
    if (dispatchTime < (numeric_limits<uint64_t>::max() - LOST_DEVICE_TIMEOUT_EXT)) {
        expireAlarm = DispatchOperation(new ExpireCachedNodesDispatchInfo(), dispatchTime + LOST_DEVICE_TIMEOUT_EXT);
    }
}


void BTController::AlarmTriggered(const Alarm& alarm, QStatus reason)
{
    QCC_DbgTrace(("BTController::AlarmTriggered(alarm = <>, reasons = %s)", QCC_StatusText(reason)));
    DispatchInfo* op = static_cast<DispatchInfo*>(alarm.GetContext());
    assert(op);

    if (reason == ER_OK) {
        QCC_DbgPrintf(("Handling deferred operation:"));
        switch (op->operation) {
        case DispatchInfo::STOP_ADVERTISEMENTS: {
            QCC_DbgPrintf(("    Stopping discoverability now"));
            lock.Lock();
            assert(IsMaster());
            bool empty = advertise.Empty();
            bool useLocalAdvertise = UseLocalAdvertise();
            String signalDest = advertise.minion->GetUniqueName();
            NameArgInfo::NameArgs args = advertise.args;
            lock.Unlock();
            if (empty) {
                if (useLocalAdvertise) {
                    QCC_DbgPrintf(("Stopping advertise..."));
                    bt.StopAdvertise();
                } else {
                    // Tell the minion to stop advertising (presumably the empty list).
                    Signal(signalDest.c_str(), 0, *advertise.delegateSignal, args->args, args->argsSize);
                }
            }
            break;
        }

        case DispatchInfo::UPDATE_DELEGATIONS:
            QCC_DbgPrintf(("    Updating delegations"));
            lock.Lock();
            UpdateDelegations(advertise);
            UpdateDelegations(find);
            QCC_DbgPrintf(("NodeDB after updating delegations"));
            QCC_DEBUG_ONLY(DumpNodeStateTable());
            lock.Unlock();
            break;

        case DispatchInfo::EXPIRE_CACHED_NODES:
            QCC_DbgPrintf(("    Expire cached nodes"));
            if (IsMaster()) {
                BTNodeDB expiredDB;
                foundNodeDB.PopExpiredNodes(expiredDB);

                expiredDB.DumpTable("expiredDB - Expiring cached advertisements");
                foundNodeDB.DumpTable("foundNodeDB - Remaining cached advertisements after expiration");

                DistributeAdvertisedNameChanges(NULL, &expiredDB);
                uint64_t dispatchTime = foundNodeDB.NextNodeExpiration();
                if (dispatchTime < (numeric_limits<uint64_t>::max() - LOST_DEVICE_TIMEOUT_EXT)) {
                    expireAlarm = DispatchOperation(new ExpireCachedNodesDispatchInfo(), dispatchTime + LOST_DEVICE_TIMEOUT_EXT);
                }
            }
            break;

        case DispatchInfo::NAME_LOST:
            QCC_DbgPrintf(("    Process local bus name lost"));
            DeferredNameLostHander(static_cast<NameLostDispatchInfo*>(op)->name);
            break;

        case DispatchInfo::BT_DEVICE_AVAILABLE:
            QCC_DbgPrintf(("    BT device available"));
            DeferredBTDeviceAvailable(static_cast<BTDevAvailDispatchInfo*>(op)->on);
            break;

        case DispatchInfo::SEND_SET_STATE: {
            QCC_DbgPrintf(("    Send set state"));
            SendSetStateDispatchInfo* di = static_cast<SendSetStateDispatchInfo*>(op);
            DeferredSendSetState(di->addr, di->busName);
            break;
        }

        case DispatchInfo::PROCESS_SET_STATE_REPLY: {
            QCC_DbgPrintf(("    Process set state reply"));
            ProcessSetStateReplyDispatchInfo* di = static_cast<ProcessSetStateReplyDispatchInfo*>(op);
            DeferredProcessSetStateReply(di->msg, di->newMaster, di->addr, di->busName);
            break;
        }

        case DispatchInfo::HANDLE_DELEGATE_FIND:
            QCC_DbgPrintf(("    Handle delegate find"));
            DeferredHandleDelegateFind(static_cast<HandleDelegateOpDispatchInfo*>(op)->msg);
            break;

        case DispatchInfo::HANDLE_DELEGATE_ADVERTISE:
            QCC_DbgPrintf(("    Handle delegate advertise"));
            DeferredHandleDelegateAdvertise(static_cast<HandleDelegateOpDispatchInfo*>(op)->msg);
            break;
        }
    }

    delete op;
}


void BTController::NameArgInfo::AlarmTriggered(const Alarm& alarm, QStatus reason)
{
    QCC_DbgTrace(("BTController::NameArgInfo::AlarmTriggered(alarm = <%s>, reason = %s)",
                  alarm == bto.find.alarm ? "find" : "advertise", QCC_StatusText(reason)));

    if (reason == ER_OK) {
        bto.lock.Lock();
        bto.NextDirectMinion(minion);

        QCC_DbgPrintf(("Selected %s as our %s minion.",
                       minion->GetBusAddress().ToString().c_str(),
                       (minion == bto.find.minion) ? "find" : "advertise"));

        // Manually re-arm alarm since automatically recurring alarms cannot be stopped.
        StartAlarm();

        SendDelegateSignal();
        bto.lock.Unlock();
    }
}


QStatus BTController::NameArgInfo::SendDelegateSignal()
{
    QCC_DbgPrintf(("Sending %s signal to %s", delegateSignal->name.c_str(),
                   minion->GetBusAddress().ToString().c_str()));
    assert(minion != bto.self);

    QStatus status = bto.Signal(minion->GetUniqueName().c_str(), 0, *delegateSignal, args->args, args->argsSize);

    return status;
}


void BTController::NameArgInfo::StartOp(bool restart)
{
    QStatus status;

    SetArgs();

    if (UseLocal()) {
        status = StartLocal();
    } else {
        status = SendDelegateSignal();
        if (bto.RotateMinions()) {
            assert(minion->IsValid());
            assert(minion != bto.self);
            if (restart) {
                StopAlarm();
            }
            StartAlarm();
        }
    }

    if (status != ER_OK) {
        QCC_LogError(status, ("StartOp(restart = %d) failed", restart));
    }

    active = (status == ER_OK);
}


void BTController::NameArgInfo::StopOp()
{
    QStatus status;

    ClearArgs();
    active = false;

    if (UseLocal()) {
        status = StopLocal();
    } else {
        status = SendDelegateSignal();
        if (bto.RotateMinions()) {
            StopAlarm();
        }
    }

    if (status != ER_OK) {
        QCC_LogError(status, ("StopOp() failed"));
    }

    active = !(status == ER_OK);
}


BTController::AdvertiseNameArgInfo::AdvertiseNameArgInfo(BTController& bto) :
    NameArgInfo(bto, SIG_DELEGATE_AD_SIZE)
{
}

void BTController::AdvertiseNameArgInfo::AddName(const qcc::String& name, BTNodeInfo& node)
{
    node->AddAdvertiseName(name);
    ++count;
    dirty = true;
}


void BTController::AdvertiseNameArgInfo::RemoveName(const qcc::String& name, BTNodeInfo& node)
{
    NameSet::iterator nit = node->FindAdvertiseName(name);
    if (nit != node->GetAdvertiseNamesEnd()) {
        node->RemoveAdvertiseName(nit);
        --count;
        dirty = true;
    }
}


void BTController::AdvertiseNameArgInfo::SetArgs()
{
    QCC_DbgTrace(("BTController::AdvertiseNameArgInfo::SetArgs()"));
    NameArgs newArgs(argsSize);
    size_t localArgsSize = argsSize;

    bto.nodeDB.Lock();
    adInfoArgs.clear();
    adInfoArgs.reserve(bto.nodeDB.Size());

    BTNodeDB::const_iterator nodeit;
    NameSet::const_iterator nameit;
    vector<const char*> names;
    for (nodeit = bto.nodeDB.Begin(); nodeit != bto.nodeDB.End(); ++nodeit) {
        const BTNodeInfo& node = *nodeit;
        names.clear();
        names.reserve(node->AdvertiseNamesSize());
        for (nameit = node->GetAdvertiseNamesBegin(); nameit != node->GetAdvertiseNamesEnd(); ++nameit) {
            names.push_back(nameit->c_str());
        }
        adInfoArgs.push_back(MsgArg(SIG_AD_NAME_MAP_ENTRY,
                                    node->GetGUID().c_str(),
                                    node->GetBusAddress().addr.GetRaw(),
                                    node->GetBusAddress().psm,
                                    names.size(), &names.front()));
    }

    bto.nodeDB.Unlock();

    MsgArg::Set(newArgs->args, localArgsSize, SIG_DELEGATE_AD,
                bto.masterUUIDRev,
                bto.self->GetBusAddress().addr.GetRaw(),
                bto.self->GetBusAddress().psm,
                adInfoArgs.size(), &adInfoArgs.front(),
                bto.RotateMinions() ? DELEGATE_TIME : (uint32_t)0);
    assert(localArgsSize == argsSize);

    bto.lock.Lock();
    args = newArgs;
    bto.lock.Unlock();

    dirty = false;
}


void BTController::AdvertiseNameArgInfo::ClearArgs()
{
    QCC_DbgTrace(("BTController::AdvertiseNameArgInfo::ClearArgs()"));
    NameArgs newArgs(argsSize);
    size_t localArgsSize = argsSize;

    /* Advertise an empty list for a while */
    MsgArg::Set(newArgs->args, localArgsSize, SIG_DELEGATE_AD,
                bto.masterUUIDRev,
                bto.self->GetBusAddress().addr.GetRaw(),
                bto.self->GetBusAddress().psm,
                static_cast<size_t>(0), NULL,
                static_cast<uint32_t>(0));
    assert(localArgsSize == argsSize);

    bto.lock.Lock();
    args = newArgs;
    bto.lock.Unlock();
}


void BTController::AdvertiseNameArgInfo::StartOp(bool restart)
{
    if (!restart) {
        bto.dispatcher.RemoveAlarm(bto.stopAd);
    }

    NameArgInfo::StartOp(restart);
}


void BTController::AdvertiseNameArgInfo::StopOp()
{
    NameArgInfo::StopOp();

    assert(!bto.dispatcher.HasAlarm(bto.stopAd));

    QCC_DbgPrintf(("Stopping discoverability in %d seconds.", DELEGATE_TIME));

    bto.stopAd = bto.DispatchOperation(new StopAdDispatchInfo(), DELEGATE_TIME * 1000);

    // Clear out the advertise arguments (for real this time).
    NameArgs newArgs(argsSize);
    size_t localArgsSize = argsSize;

    MsgArg::Set(newArgs->args, localArgsSize, SIG_DELEGATE_AD,
                bt::INVALID_UUIDREV,
                0ULL,
                bt::INVALID_PSM,
                static_cast<size_t>(0), NULL,
                static_cast<uint32_t>(0));
    assert(localArgsSize == argsSize);

    bto.lock.Lock();
    args = newArgs;
    bto.lock.Unlock();
}


QStatus BTController::AdvertiseNameArgInfo::StartLocal()
{
    QStatus status;
    BTNodeDB adInfo;

    status = ExtractAdInfo(&adInfoArgs.front(), adInfoArgs.size(), adInfo);
    if (status == ER_OK) {
        status = bto.bt.StartAdvertise(bto.masterUUIDRev, bto.self->GetBusAddress().addr, bto.self->GetBusAddress().psm, adInfo);
    }
    return status;
}


QStatus BTController::AdvertiseNameArgInfo::StopLocal()
{
    // Should still advertise our node even though there are no names to
    // advertise so that other nodes can clean up their cache's appropriately.
    return bto.bt.StartAdvertise(bto.masterUUIDRev,
                                 bto.self->GetBusAddress().addr, bto.self->GetBusAddress().psm,
                                 bto.nodeDB);
}


BTController::FindNameArgInfo::FindNameArgInfo(BTController& bto) :
    NameArgInfo(bto, SIG_DELEGATE_FIND_SIZE)
{
}


void BTController::FindNameArgInfo::AddName(const qcc::String& name, BTNodeInfo& node)
{
    node->AddFindName(name);
    ++count;
    dirty = true;
}


void BTController::FindNameArgInfo::RemoveName(const qcc::String& name, BTNodeInfo& node)
{
    NameSet::iterator nit = node->FindFindName(name);
    if (nit != node->GetFindNamesEnd()) {
        node->RemoveFindName(nit);
        --count;
        dirty = true;
    }
}


void BTController::FindNameArgInfo::SetArgs()
{
    QCC_DbgTrace(("BTController::FindNameArgInfo::SetArgs()"));
    NameArgs newArgs(argsSize);
    size_t localArgsSize = argsSize;
    const char* rdest = bto.IsMaster() ? bto.bus.GetUniqueName().c_str() : resultDest.c_str();

    bto.nodeDB.Lock();
    ignoreAddrsCache.clear();
    ignoreAddrsCache.reserve(bto.nodeDB.Size());
    BTNodeDB::const_iterator it;
    for (it = bto.nodeDB.Begin(); it != bto.nodeDB.End(); ++it) {
        ignoreAddrsCache.push_back((*it)->GetBusAddress().addr.GetRaw());
    }
    bto.nodeDB.Unlock();

    MsgArg::Set(newArgs->args, localArgsSize, SIG_DELEGATE_FIND,
                rdest,
                ignoreAddrsCache.size(), &ignoreAddrsCache.front(),
                bto.RotateMinions() ? DELEGATE_TIME : (uint32_t)0);
    assert(localArgsSize == argsSize);

    bto.lock.Lock();
    args = newArgs;
    bto.lock.Unlock();

    dirty = false;
}


void BTController::FindNameArgInfo::ClearArgs()
{
    QCC_DbgTrace(("BTController::FindNameArgInfo::ClearArgs()"));
    NameArgs newArgs(argsSize);
    size_t localArgsSize = argsSize;

    MsgArg::Set(newArgs->args, localArgsSize, SIG_DELEGATE_FIND,
                NULL,
                static_cast<size_t>(0), NULL,
                static_cast<uint32_t>(0));
    assert(localArgsSize == argsSize);

    bto.lock.Lock();
    args = newArgs;
    bto.lock.Unlock();
}


#ifndef NDEBUG
void BTController::DumpNodeStateTable() const
{
    BTNodeDB::const_iterator nodeit;
    QCC_DbgPrintf(("Node State Table (local = %s):", bus.GetUniqueName().c_str()));
    for (nodeit = nodeDB.Begin(); nodeit != nodeDB.End(); ++nodeit) {
        const BTNodeInfo& node = *nodeit;
        NameSet::const_iterator nameit;
        QCC_DbgPrintf(("    %s (conn: %s) %s (%s):",
                       node->GetBusAddress().ToString().c_str(),
                       node->GetConnectAddress().ToString().c_str(),
                       node->GetUniqueName().c_str(),
                       (node == self) ? "local" :
                       ((node == find.minion) ? "find minion" :
                        ((node == advertise.minion) ? "advertise minion" :
                         (node->IsDirectMinion() ? "direct minon" : "indirect minion")))));
        QCC_DbgPrintf(("         Advertise names:"));
        for (nameit = node->GetAdvertiseNamesBegin(); nameit != node->GetAdvertiseNamesEnd(); ++nameit) {
            QCC_DbgPrintf(("            %s", nameit->c_str()));
        }
        QCC_DbgPrintf(("         Find names:"));
        for (nameit = node->GetFindNamesBegin(); nameit != node->GetFindNamesEnd(); ++nameit) {
            QCC_DbgPrintf(("            %s", nameit->c_str()));
        }
    }
}


void BTController::FlushCachedNames()
{
    if (IsMaster()) {
        DistributeAdvertisedNameChanges(NULL, &foundNodeDB);
        foundNodeDB.Clear();
    } else {
        const InterfaceDescription* ifc;
        ifc = master->GetInterface("org.alljoyn.Bus.Debug.BT");
        if (!ifc) {
            ifc = bus.GetInterface("org.alljoyn.Bus.Debug.BT");
            if (!ifc) {
                InterfaceDescription* newIfc;
                bus.CreateInterface("org.alljoyn.Bus.Debug.BT", newIfc);
                newIfc->AddMethod("FlushDiscoverTimes", NULL, NULL, NULL, 0);
                newIfc->AddMethod("FlushSDPQueryTimes", NULL, NULL, NULL, 0);
                newIfc->AddMethod("FlushConnectTimes", NULL, NULL, NULL, 0);
                newIfc->AddMethod("FlushCachedNames", NULL, NULL, NULL, 0);
                newIfc->AddProperty("DiscoverTimes", "a(su)", PROP_ACCESS_READ);
                newIfc->AddProperty("SDPQueryTimes", "a(su)", PROP_ACCESS_READ);
                newIfc->AddProperty("ConnectTimes", "a(su)", PROP_ACCESS_READ);
                newIfc->Activate();
                ifc = newIfc;
            }
            master->AddInterface(*ifc);
        }

        if (ifc) {
            master->MethodCall("org.alljoyn.Bus.Debug.BT", "FlushCachedNames", NULL, 0);
        }
    }
}
#endif


}
