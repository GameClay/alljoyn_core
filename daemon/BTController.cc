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
#include <qcc/GUID.h>
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
 * Timeout for detecting lost devices.  The nominal timeout is 60 seconds.
 * Absolute timing isn't critical so an additional 5 seconds is acctually
 * applied to when the alarm triggers.  This will allow lost device
 * expirations that are close to each other to be processed at the same time.
 * It also reduces the number of alarm resets if we get 2 updates within 5
 * seconds from the lower layer.
 */
static const uint32_t LOST_DEVICE_TIMEOUT = 60000;    /* 60 seconds */
static const uint32_t LOST_DEVICE_TIMEOUT_EXT = 5000; /* 5 seconds */

static const uint32_t BLACKLIST_TIME = (60 * 60 * 1000); /* 1 hour */

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

static const SessionOpts BTSESSION_OPTS(SessionOpts::TRAFFIC_MESSAGES,
                                        false,
                                        SessionOpts::PROXIMITY_ANY,
                                        TRANSPORT_BLUETOOTH);

#define SIG_ARRAY              "a"
#define SIG_ARRAY_SIZE         1
#define SIG_BDADDR             "t"
#define SIG_BDADDR_SIZE        1
#define SIG_DURATION           "u"
#define SIG_DURATION_SIZE      1
#define SIG_EIR_CAPABLE        "b"
#define SIG_EIR_CAPABLE_SIZE   1
#define SIG_GUID               "s"
#define SIG_GUID_SIZE          1
#define SIG_MINION_CNT         "y"
#define SIG_MINION_CNT_SIZE    1
#define SIG_NAME               "s"
#define SIG_NAME_SIZE          1
#define SIG_PSM                "q"
#define SIG_PSM_SIZE           1
#define SIG_SLAVE_FACTOR       "y"
#define SIG_SLAVE_FACTOR_SIZE  1
#define SIG_STATUS             "u"
#define SIG_STATUS_SIZE        1
#define SIG_UUIDREV            "u"
#define SIG_UUIDREV_SIZE       1

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
#define SIG_NODE_STATE_ENTRY        "(" SIG_GUID SIG_NAME SIG_BUSADDR SIG_AD_NAMES SIG_FIND_NAMES SIG_EIR_CAPABLE ")"
#define SIG_NODE_STATE_ENTRY_SIZE   1
#define SIG_NODE_STATES             SIG_ARRAY SIG_NODE_STATE_ENTRY
#define SIG_NODE_STATES_SIZE        SIG_ARRAY_SIZE
#define SIG_FOUND_NODE_ENTRY        "(" SIG_BUSADDR SIG_UUIDREV SIG_AD_NAME_MAP ")"
#define SIG_FOUND_NODE_ENTRY_SIZE   1
#define SIG_FOUND_NODES             SIG_ARRAY SIG_FOUND_NODE_ENTRY
#define SIG_FOUND_NODES_SIZE        SIG_ARRAY_SIZE

#define SIG_SET_STATE_IN            SIG_MINION_CNT SIG_SLAVE_FACTOR SIG_EIR_CAPABLE SIG_UUIDREV SIG_BUSADDR SIG_NODE_STATES SIG_FOUND_NODES
#define SIG_SET_STATE_IN_SIZE       (SIG_MINION_CNT_SIZE + SIG_SLAVE_FACTOR_SIZE + SIG_EIR_CAPABLE_SIZE + SIG_UUIDREV_SIZE + SIG_BUSADDR_SIZE + SIG_NODE_STATES_SIZE + SIG_FOUND_NODES_SIZE)
#define SIG_SET_STATE_OUT           SIG_EIR_CAPABLE SIG_UUIDREV SIG_BUSADDR SIG_NODE_STATES SIG_FOUND_NODES
#define SIG_SET_STATE_OUT_SIZE      (SIG_EIR_CAPABLE_SIZE + SIG_UUIDREV_SIZE + SIG_BUSADDR_SIZE + SIG_NODE_STATES_SIZE + SIG_FOUND_NODES_SIZE)
#define SIG_NAME_OP                 SIG_BUSADDR SIG_NAME
#define SIG_NAME_OP_SIZE            (SIG_BUSADDR_SIZE + SIG_NAME_SIZE)
#define SIG_DELEGATE_AD             SIG_UUIDREV SIG_BUSADDR SIG_AD_NAME_MAP SIG_DURATION
#define SIG_DELEGATE_AD_SIZE        (SIG_UUIDREV_SIZE + SIG_BUSADDR_SIZE + SIG_AD_NAME_MAP_SIZE + SIG_DURATION_SIZE)
#define SIG_DELEGATE_AD_DURATION_PARAM (SIG_UUIDREV_SIZE + SIG_BUSADDR_SIZE + SIG_AD_NAME_MAP_SIZE)
#define SIG_DELEGATE_FIND           SIG_FIND_FILTER_LIST SIG_DURATION
#define SIG_DELEGATE_FIND_SIZE      (SIG_FIND_FILTER_LIST_SIZE + SIG_DURATION_SIZE)
#define SIG_FOUND_NAMES             SIG_FOUND_NODES
#define SIG_FOUND_NAMES_SIZE        (SIG_FOUND_NODES_SIZE)
#define SIG_FOUND_DEV               SIG_BDADDR SIG_UUIDREV SIG_EIR_CAPABLE
#define SIG_FOUND_DEV_SIZE          (SIG_BDADDR_SIZE + SIG_UUIDREV_SIZE + SIG_EIR_CAPABLE_SIZE)
#define SIG_CONN_ADDR_CHANGED       SIG_BUSADDR SIG_BUSADDR
#define SIG_CONN_ADDR_CHANGED_SIZE  (SIG_BUSADDR_SIZE + SIG_BUSADDR_SIZE)

const InterfaceDesc btmIfcTable[] = {
    /* Methods */
    { MESSAGE_METHOD_CALL, "SetState", SIG_SET_STATE_IN, SIG_SET_STATE_OUT, "minionCnt,slaveFactor,eirCapable,uuidRev,busAddr,psm,nodeStates,foundNodes,eirCapable,uuidRev,busAddr,psm,nodeStates,foundNodes" },

    /* Signals */
    { MESSAGE_SIGNAL, "FindName",            SIG_NAME_OP,           NULL, "requestorAddr,requestorPSM,findName" },
    { MESSAGE_SIGNAL, "CancelFindName",      SIG_NAME_OP,           NULL, "requestorAddr,requestorPSM,findName" },
    { MESSAGE_SIGNAL, "AdvertiseName",       SIG_NAME_OP,           NULL, "requestorAddr,requestorPSM,adName" },
    { MESSAGE_SIGNAL, "CancelAdvertiseName", SIG_NAME_OP,           NULL, "requestorAddr,requestorPSM,adName" },
    { MESSAGE_SIGNAL, "DelegateAdvertise",   SIG_DELEGATE_AD,       NULL, "uuidRev,bdAddr,psm,adNames,duration" },
    { MESSAGE_SIGNAL, "DelegateFind",        SIG_DELEGATE_FIND,     NULL, "ignoreBDAddr,duration" },
    { MESSAGE_SIGNAL, "FoundNames",          SIG_FOUND_NAMES,       NULL, "adNamesTable" },
    { MESSAGE_SIGNAL, "LostNames",           SIG_FOUND_NAMES,       NULL, "adNamesTable" },
    { MESSAGE_SIGNAL, "FoundDevice",         SIG_FOUND_DEV,         NULL, "bdAddr,uuidRev,eirCapable" },
    { MESSAGE_SIGNAL, "ConnectAddrChanged",  SIG_CONN_ADDR_CHANGED, NULL, "oldBDAddr,oldPSM,newBDAddr,newPSM" },
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
    eirMinions(0),
    maxConnections(min(StringToU32(Environ::GetAppEnviron()->Find("ALLJOYN_MAX_BT_CONNECTIONS"), 0, DEFAULT_MAX_CONNECTIONS),
                       ABSOLUTE_MAX_CONNECTIONS)),
    listening(false),
    devAvailable(false),
    advertise(*this),
    find(*this),
    dispatcher("BTC-Dispatcher"),
    incompleteConnections(0)
{
    while (masterUUIDRev == bt::INVALID_UUIDREV) {
        masterUUIDRev = qcc::Rand32();
    }

    const InterfaceDescription* ifc = NULL;
    InterfaceDescription* newIfc;
    QStatus status = bus.CreateInterface(bluetoothTopoMgrIfcName, newIfc);
    if (status == ER_OK) {
        for (size_t i = 0; i < ArraySize(btmIfcTable); ++i) {
            newIfc->AddMember(btmIfcTable[i].type,
                              btmIfcTable[i].name,
                              btmIfcTable[i].inputSig,
                              btmIfcTable[i].outSig,
                              btmIfcTable[i].argNames,
                              0);
        }
        newIfc->Activate();
        ifc = newIfc;
    } else if (status == ER_BUS_IFACE_ALREADY_EXISTS) {
        ifc = bus.GetInterface(bluetoothTopoMgrIfcName);
    }

    if (ifc) {
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
        org.alljoyn.Bus.BTController.ConnectAddrChanged =  ifc->GetMember("ConnectAddrChanged");

        advertise.delegateSignal = org.alljoyn.Bus.BTController.DelegateAdvertise;
        find.delegateSignal = org.alljoyn.Bus.BTController.DelegateFind;

        static_cast<DaemonRouter&>(bus.GetInternal().GetRouter()).AddBusNameListener(this);
    }

    // Setup the BT node info for ourself.
    self->SetGUID(bus.GetGlobalGUIDString());
    self->SetRelationship(_BTNodeInfo::SELF);
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

    if (advertise.active && (advertise.minion == self)) {
        QCC_DbgPrintf(("Stopping local advertise..."));
        advertise.StopLocal();
    }

    if (find.active && (find.minion == self)) {
        QCC_DbgPrintf(("Stopping local find..."));
        find.StopLocal();
    }

    bus.UnregisterBusObject(*this);
    if (master) {
        delete master;
    }
}


void BTController::ObjectRegistered() {
    // Set our unique name now that we know it.
    self->SetUniqueName(bus.GetUniqueName());
    self->SetEIRCapable(bt.IsEIRCapable());
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
        { org.alljoyn.Bus.BTController.ConnectAddrChanged,  SignalHandler(&BTController::HandleConnectAddrChanged) },
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
        if (lDevAvailable) {
            BTNodeDB newAdInfo;
            BTNodeInfo node(addr, self->GetUniqueName(), self->GetGUID());  // make an actual copy of self
            node->AddAdvertiseName(name);  // copy of self only gets the new names (not the existing names)
            newAdInfo.AddNode(node);
            DistributeAdvertisedNameChanges(&newAdInfo, NULL);
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
        if (lDevAvailable) {
            BTNodeDB oldAdInfo;
            BTNodeInfo node(addr, self->GetUniqueName(), self->GetGUID());  // make an actual copy of self
            node->AddAdvertiseName(name);  // Yes 'Add' the name being removed (it goes in the old ad info).
            oldAdInfo.AddNode(node);
            DistributeAdvertisedNameChanges(NULL, &oldAdInfo);
        }
    }

    return status;
}


QStatus BTController::RemoveFindName(const qcc::String& name)
{
    QStatus status = DoNameOp(name, *org.alljoyn.Bus.BTController.CancelFindName, false, find);

    if (self->FindNamesEmpty() && !IsMaster()) {
        // We're not looking for any names so our master will stop sending us
        // updates and assume that our set of found names is empty if we do
        // start finding names again so we need tell AlljoynObj that the BT
        // names we know about are expired.  Set an expiration timer for the
        // names we currently know about.
        foundNodeDB.RefreshExpiration(LOST_DEVICE_TIMEOUT);
        ResetExpireNameAlarm();
    }
    return status;
}


void BTController::ProcessDeviceChange(const BDAddress& adBdAddr,
                                       uint32_t uuidRev,
                                       bool eirCapable)
{
    QCC_DbgTrace(("BTController::ProcessDeviceChange(adBdAddr = %s, uuidRev = %08x)",
                  adBdAddr.ToString().c_str(), uuidRev));

    // This gets called when the BTAccessor layer detects either a new
    // advertising device and/or a new uuidRev for associated with that
    // advertising device.

    assert(!eirCapable || uuidRev != bt::INVALID_UUIDREV);
    assert(adBdAddr.GetRaw() != static_cast<uint64_t>(0));

    QStatus status;

    lock.Lock();
    if (IsMaster()) {
        if (nodeDB.FindNode(adBdAddr)->IsValid()) {
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

        BTNodeInfo adNode = foundNodeDB.FindNode(adBdAddr);
        BTNodeDB newAdInfo;
        BTNodeDB oldAdInfo;
        BTNodeDB added;
        BTNodeDB removed;
        bool distributeChanges = false;

        bool knownAdNode = adNode->IsValid();
        bool getInfo = (!bt.IsEIRCapable() ||
                        !knownAdNode ||
                        (!adNode->IsEIRCapable() && (!eirCapable || (adNode->GetUUIDRev() != uuidRev))) ||
                        (adNode->IsEIRCapable() && (eirCapable && (adNode->GetUUIDRev() != uuidRev))));
        bool refreshExpiration = (bt.IsEIRCapable() &&
                                  knownAdNode &&
                                  eirCapable &&
                                  (adNode->GetUUIDRev() == uuidRev));

        if (refreshExpiration) {

            if (!adNode->IsEIRCapable()) {
                adNode->SetEIRCapable(eirCapable);
            }

            // We've see this advertising node before and nothing has changed
            // so just refresh the expiration time of all the nodes.
            foundNodeDB.RefreshExpiration(adNode->GetConnectNode(), LOST_DEVICE_TIMEOUT);
            foundNodeDB.DumpTable((String("foundNodeDB: Refresh Expiration for nodes with connect address: ") + adNode->GetConnectNode()->GetBusAddress().ToString()).c_str());
            ResetExpireNameAlarm();

        } else if (getInfo) {
            uint32_t newUUIDRev;
            BTBusAddress connAddr;

            if (!knownAdNode && !eirCapable && (blacklist->find(adBdAddr) != blacklist->end())) {
                return; // blacklisted - ignore it
            }

            QCC_DbgPrintf(("Getting device info from %s (adNode: %s in foundNodeDB, adNode %s EIR capable, received %s EIR capable, adNode UUIDRev: %08x, received UUIDRev: %08x)",
                           adBdAddr.ToString().c_str(),
                           knownAdNode ? "is" : "is not",
                           adNode->IsEIRCapable() ? "is" : "is not",
                           eirCapable ? "is" : "is not",
                           adNode->GetUUIDRev(),
                           uuidRev));

            QCC_DEBUG_ONLY(sdpQueryStartTime = sdpQueryTimer.StartTime());
            lock.Unlock();
            status = bt.GetDeviceInfo(adBdAddr, newUUIDRev, connAddr, newAdInfo);
            lock.Lock();
            QCC_DEBUG_ONLY(sdpQueryTimer.RecordTime(adBdAddr, sdpQueryStartTime));

            // Make sure we are still master
            if (IsMaster()) {
                if ((status != ER_OK) || !connAddr.IsValid()) {
                    if (!eirCapable) {
                        uint32_t blacklistTime = BLACKLIST_TIME + (Rand32() % BLACKLIST_TIME);
                        QCC_DbgPrintf(("Blacklisting %s for %d.%03ds",
                                       adBdAddr.ToString().c_str(),
                                       blacklistTime / 1000, blacklistTime % 1000));
                        blacklist->insert(adBdAddr);
                        DispatchOperation(new ExpireBlacklistedDevDispatchInfo(adBdAddr), blacklistTime);

                        // Gotta add the new blacklist entry to ignore addresses set.
                        find.dirty = true;
                        DispatchOperation(new UpdateDelegationsDispatchInfo());
                    }
                    lock.Unlock();
                    return;
                }

                if (nodeDB.FindNode(connAddr)->IsValid()) {
                    // Already connected.
                    lock.Unlock();
                    return;
                }

                bool autoConnect = !bt.IsEIRCapable() || (!eirCapable && !(knownAdNode && adNode->IsEIRCapable()));

                if (newAdInfo.FindNode(self->GetBusAddress())->IsValid()) {
                    QCC_DbgPrintf(("Device %s is advertising a set of nodes that include our own BD Address, ignoring it for now.", adBdAddr.ToString().c_str()));
                    // Clear out the newAdInfo DB then re-add minimal
                    // information about the advertising node so that we'll
                    // ignore it until its UUID revision changes.
                    BTNodeInfo n(newAdInfo.FindNode(adBdAddr)->GetBusAddress());
                    n->SetEIRCapable(eirCapable || adNode->IsEIRCapable());
                    newAdInfo.Clear();
                    newAdInfo.AddNode(n);
                    autoConnect = false;  // We do not want to connect to this device since its probably in a bad state.
                }

                BTNodeInfo newConnNode = newAdInfo.FindNode(connAddr);
                if (!newConnNode->IsValid()) {
                    QCC_LogError(ER_FAIL, ("No device with connect address %s in advertisement",
                                           connAddr.ToString().c_str()));
                    lock.Unlock();
                    return;
                }

                foundNodeDB.Lock();

                if (knownAdNode) {
                    foundNodeDB.GetNodesFromConnectNode(adNode->GetConnectNode(), oldAdInfo);
                } else {
                    QCC_DEBUG_ONLY(discoverTimer.RecordTime(adBdAddr, discoverStartTime));
                    adNode = newAdInfo.FindNode(adBdAddr);
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
                for (nodeit = newAdInfo.Begin(); nodeit != newAdInfo.End(); ++nodeit) {
                    BTNodeInfo node = *nodeit;
                    node->SetConnectNode(connNode);
                    if (node->GetBusAddress().addr == adBdAddr) {
                        node->SetEIRCapable(eirCapable);
                    }
                }

                oldAdInfo.Diff(newAdInfo, &added, &removed);

                foundNodeDB.DumpTable("foundNodeDB - Before update");
                foundNodeDB.UpdateDB(&added, &removed);
                connNode->SetUUIDRev(newUUIDRev);
                foundNodeDB.RefreshExpiration(connNode, LOST_DEVICE_TIMEOUT);
                foundNodeDB.DumpTable("foundNodeDB - Updated set of found devices due to remote device advertisement change");

                foundNodeDB.Unlock();

                // Only autoconnect if the advertising device is not EIR
                // capable.  Sometimes, however, the BTAccessor layer may
                // indicate that an EIR capable device is not EIR capable, so
                // check our cache as well.
                if (autoConnect) {
                    // Make sure we didn't become connected to the found
                    // device while doing the SDP query.
                    if (!nodeDB.FindNode(adBdAddr)->IsValid()) {
                        if (newConnNode->IsValid()) {
                            vector<String> vectorizedNames;
                            // Build up the bus name of the remote daemon based on informatin we do have.
                            String name = String(":") + newConnNode->GetGUID().ToShortString() + ".1";
                            vectorizedNames.push_back(name);
                            bt.FoundNamesChange(newConnNode->GetGUID().ToString(),
                                                vectorizedNames,
                                                newConnNode->GetBusAddress().addr,
                                                newConnNode->GetBusAddress().psm, false);
                            // Now the session manager knows the unique name we are interested in joining.
                            newConnNode->SetUniqueName(name);
                            joinSessionNodeDB.AddNode(newConnNode);
                            QCC_DbgPrintf(("Joining BT topology manager session for %s", connAddr.ToString().c_str()));
                            status = bus.JoinSessionAsync(name.c_str(),
                                                          ALLJOYN_BTCONTROLLER_SESSION_PORT,
                                                          NULL,
                                                          BTSESSION_OPTS,
                                                          this,
                                                          new BTNodeInfo(newConnNode));
                        }
                    }
                }

                distributeChanges = true;
                ResetExpireNameAlarm();
            }
        }

        lock.Unlock();

        if (distributeChanges) {
            DistributeAdvertisedNameChanges(&added, &removed);
        }
    } else {
        MsgArg args[SIG_FOUND_DEV_SIZE];
        size_t numArgs = ArraySize(args);

        status = MsgArg::Set(args, numArgs, SIG_FOUND_DEV, adBdAddr.GetRaw(), uuidRev, eirCapable);
        if (status != ER_OK) {
            QCC_LogError(status, ("MsgArg::Set(args = <>, numArgs = %u, %s, %s, %08x, <%s>) failed",
                                  numArgs, SIG_FOUND_DEV, adBdAddr.ToString().c_str(), uuidRev, eirCapable ? "true" : "false"));
            return;
        }

        lock.Unlock();

        status = Signal(masterNode->GetUniqueName().c_str(), masterNode->GetSessionID(), *org.alljoyn.Bus.BTController.FoundDevice, args, numArgs);
    }
}


BTNodeInfo BTController::PrepConnect(const BTBusAddress& addr)
{
    BTNodeInfo node;

    bool repeat;
    bool newDevice;

    do {
        repeat = false;
        newDevice = false;

        lock.Lock();
        if (!IsMinion()) {
            node = nodeDB.FindNode(addr);
            if (IsMaster() && !node->IsValid() && (directMinions < maxConnections)) {
                node = foundNodeDB.FindNode(addr);
                newDevice = node->IsValid() && !joinSessionNodeDB.FindNode(addr)->IsValid();
            }
        }

        if (!IsMaster() && !node->IsValid()) {
            node = masterNode;
        }
        lock.Unlock();

        if (newDevice && IncrementAndFetch(&incompleteConnections) > 1) {
            QStatus status = Event::Wait(connectCompleted);
            connectCompleted.ResetEvent();
            node = BTNodeInfo();
            if (status != ER_OK) {
                return node;  // Fail the connection (probably shutting down anyway).
            } else {
                repeat = true;
            }

            if (DecrementAndFetch(&incompleteConnections) > 0) {
                if (!IsMaster()) {
                    connectCompleted.SetEvent();
                }
            }
        }
    } while (repeat);


    QCC_DEBUG_ONLY(connectStartTimes[node->GetBusAddress().addr] = connectTimer.StartTime());

    QCC_DbgPrintf(("Connect address %s for %s is %s",
                   node->GetConnectNode()->GetBusAddress().ToString().c_str(),
                   addr.ToString().c_str(),
                   (foundNodeDB.FindNode(addr) == node) ? "in foundNodeDB" :
                   ((nodeDB.FindNode(addr) == node) ? "in nodeDB" :
                    ((node == masterNode) ? "masterNode" : "<unknown>"))));

    return node->GetConnectNode();
}


void BTController::PostConnect(QStatus status, BTNodeInfo& node, const String& remoteName)
{
    if (status == ER_OK) {
        QCC_DEBUG_ONLY(connectTimer.RecordTime(node->GetBusAddress().addr, connectStartTimes[node->GetBusAddress().addr]));
        assert(!remoteName.empty());
        /* Only call JoinSessionAsync for new outgoing connections where we
         * didn't already start the join session process.
         */
        if (IsMaster() &&
            !nodeDB.FindNode(node->GetBusAddress())->IsValid() &&
            !joinSessionNodeDB.FindNode(node->GetBusAddress())->IsValid()) {

            if (node->GetUniqueName().empty()) {
                node->SetUniqueName(remoteName);
            }
            assert(node->GetUniqueName() == remoteName);
            joinSessionNodeDB.AddNode(node);
            QCC_DbgPrintf(("Joining BT topology manager session for %s", node->GetBusAddress().ToString().c_str()));
            status = bus.JoinSessionAsync(remoteName.c_str(),
                                          ALLJOYN_BTCONTROLLER_SESSION_PORT,
                                          NULL,
                                          BTSESSION_OPTS,
                                          this,
                                          new BTNodeInfo(node));
            if (status != ER_OK) {
                bt.Disconnect(remoteName);
            }
        }
    }
}


void BTController::LostLastConnection(const BDAddress& addr)
{
    QCC_DbgTrace(("BTController::LostLastConnection(addr = %s)",
                  addr.ToString().c_str()));

    BTNodeInfo node;

    if (addr == masterNode->GetBusAddress().addr) {
        node = masterNode;
    } else {
        BTNodeDB::const_iterator it;
        BTNodeDB::const_iterator end;
        nodeDB.FindNodes(addr, it, end);
        for (; it != end; ++it) {
            if ((*it)->GetConnectionCount() == 1) {
                node = *it;
                break;
            }
        }
    }

    if ((node->IsValid()) && (node->IsEIRCapable())) {
        SessionId sessionID = node->GetSessionID();
        nodeDB.NodeSessionLost(sessionID);
        bus.LeaveSession(sessionID);
    }
}


void BTController::BTDeviceAvailable(bool on)
{
    QCC_DbgTrace(("BTController::BTDeviceAvailable(<%s>)", on ? "on" : "off"));
    DispatchOperation(new BTDevAvailDispatchInfo(on));
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
    } else if (!oldOwner && newOwner && (alias == org::alljoyn::Daemon::WellKnownName)) {
        /*
         * Need to bind the session port here instead of in the
         * ObjectRegistered() function since there is a race condition because
         * there is a race condition between which object will get registered
         * first: AllJoynObj or BTController.  Since AllJoynObj must be
         * registered before we can bind the session port we wait for
         * AllJoynObj to acquire its well known name.
         */
        SessionPort port = ALLJOYN_BTCONTROLLER_SESSION_PORT;
        QStatus status = bus.BindSessionPort(port, BTSESSION_OPTS, *this);
        if (status != ER_OK) {
            QCC_LogError(status, ("BindSessionPort(port = %04x, opts = <%x, %x, %x>, listener = %p)",
                                  port,
                                  BTSESSION_OPTS.traffic, BTSESSION_OPTS.proximity, BTSESSION_OPTS.transports,
                                  this));
        }
    }
}


bool BTController::AcceptSessionJoiner(SessionPort sessionPort,
                                       const char* joiner,
                                       const SessionOpts& opts)
{
    bool accept = (sessionPort == ALLJOYN_BTCONTROLLER_SESSION_PORT) && BTSESSION_OPTS.IsCompatible(opts);
    String uniqueName(joiner);
    BTNodeInfo node = nodeDB.FindNode(uniqueName);

    if (accept) {
        RemoteEndpoint* ep = static_cast<RemoteEndpoint*>(bt.LookupEndpoint(uniqueName));

        /* We only accept sessions from joiners who meet the following criteria:
         * - The endpoint is a Bluetooth endpoint (endpoint lookup succeeds).
         * - It is an incoming connection (BTBusAddress of endpoint is invalid).
         * - Is not already connected to us (sessionID is 0).
         */
        accept = (ep &&
                  ep->IsIncomingConnection() &&
                  (!node->IsValid() || (node->GetSessionID() == 0)));

        if (ep) {
            bt.ReturnEndpoint(ep);
        }
    }

    if (accept) {
        /* If we happen to be joining the joiner at the same time then we need
         * to figure out which session will be rejected.  The deciding factor
         * will be who's unique name is "less".  (The unique names should
         * never be equal, but we'll reject those just in case.)
         */
        if (joinSessionNodeDB.FindNode(uniqueName)->IsValid() && !(uniqueName < bus.GetUniqueName())) {
            accept = false;
        }
    }

    QCC_DbgPrintf(("%s session join from %s",
                   accept ? "Accepting" : "Rejecting",
                   node->IsValid() ? node->GetBusAddress().ToString().c_str() : uniqueName.c_str()));

    return accept;
}


void BTController::SessionJoined(SessionPort sessionPort, SessionId id, const char* joiner)
{
    String uniqueName(joiner);
    BTNodeInfo node = nodeDB.FindNode(uniqueName);

    if (node->IsValid()) {
        QCC_DbgPrintf(("Session joined by %s", node->GetBusAddress().ToString().c_str()));
        nodeDB.UpdateNodeSessionID(id, node);
    }
}


void BTController::SessionLost(SessionId id)
{
    QCC_DbgPrintf(("BTController::SessionLost(id = %x)", id));
    nodeDB.NodeSessionLost(id);
}


void BTController::JoinSessionCB(QStatus status, SessionId sessionID, SessionOpts opts, void* context)
{
    QCC_DbgTrace(("BTController::JoinSessionCB(status = %s, sessionID = %x, opts = <>, context = %p",
                  QCC_StatusText(status), sessionID, context));
    assert(context);
    BTNodeInfo* node = static_cast<BTNodeInfo*>(context);
    if (status == ER_OK) {
        assert((*node != masterNode) && !nodeDB.FindNode((*node)->GetBusAddress())->IsValid());

        uint16_t connCnt = (*node)->GetConnectionCount();

        if ((*node)->IsEIRCapable() && (connCnt == 1)) {
            bus.LeaveSession(sessionID);
        } else {
            (*node)->SetSessionID(sessionID);
            DispatchOperation(new SendSetStateDispatchInfo((*node)));
        }
    }
    delete node;
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
            status = Signal(masterNode->GetUniqueName().c_str(), masterNode->GetSessionID(), signal, args, argsSize);
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
                        SendFoundNamesChange(node, nodeDB, false);
                        if (foundNodeDB.Size() > 0) {
                            SendFoundNamesChange(node, foundNodeDB, false);
                        }
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
                Signal(masterNode->GetUniqueName().c_str(), masterNode->GetSessionID(), *member, args, numArgs);
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
    RemoteEndpoint* ep = bt.LookupEndpoint(sender);

    if ((ep == NULL) ||
        !ep->IsIncomingConnection() ||
        nodeDB.FindNode(ep->GetRemoteName())->IsValid()) {
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

    QStatus status;

    uint8_t remoteDirectMinions;
    uint8_t remoteSlaveFactor;
    bool remoteEIRCapable;
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
                          &remoteDirectMinions,
                          &remoteSlaveFactor,
                          &remoteEIRCapable,
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


    FillFoundNodesMsgArgs(foundNodeArgsStorage, foundNodeDB);

    bool wantMaster = ((ALLJOYN_PROTOCOL_VERSION > remoteProtocolVersion) ||

                       ((ALLJOYN_PROTOCOL_VERSION == remoteProtocolVersion) &&
                        ((!bt.IsEIRCapable() && remoteEIRCapable) ||

                         ((bt.IsEIRCapable() == remoteEIRCapable) &&
                          (directMinions >= remoteDirectMinions)))));

    bool isMaster;
    status = bt.IsMaster(addr.addr, isMaster);
    if (status != ER_OK) {
        isMaster = false; // couldn't tell, so guess
    }

    if (wantMaster != isMaster) {
        bt.RequestBTRole(addr.addr, wantMaster ? bt::MASTER : bt::SLAVE);

        // Now see if ForceMaster() worked...
        status = bt.IsMaster(addr.addr, isMaster);
        if (status != ER_OK) {
            isMaster = false; // couldn't tell, so guess
        }
    }

    uint8_t slaveFactor = ComputeSlaveFactor();

    QCC_DbgPrintf(("Who becomes Master? proto ver: %u, %u   EIR support: %d, %d   minion cnt: %u, %u   slave factor: %u, %u   bt role: %s  wantMaster: %s",
                   ALLJOYN_PROTOCOL_VERSION, remoteProtocolVersion,
                   bt.IsEIRCapable(), remoteEIRCapable,
                   directMinions, remoteDirectMinions,
                   slaveFactor, remoteSlaveFactor,
                   isMaster ? "master" : "slave",
                   wantMaster ? "true" : "false"));

    if ((slaveFactor > remoteSlaveFactor) ||
        ((slaveFactor == remoteSlaveFactor) && !isMaster)) {
        // We are now a minion (or a drone if we have more than one direct connection)
        master = new ProxyBusObject(bus, sender.c_str(), bluetoothObjPath, 0);
        masterNode = BTNodeInfo(addr, sender);
        masterNode->SetUUIDRev(otherUUIDRev);
        masterNode->SetSessionID(msg->GetSessionId());
        masterNode->SetRelationship(_BTNodeInfo::MASTER);
        masterNode->SetEIRCapable(remoteEIRCapable);

        if (advertise.active) {
            advertise.StopOp(true);
            advertise.minion = self;
        }
        if (find.active) {
            find.StopOp(true);
            find.minion = self;
        }

        if (dispatcher.HasAlarm(expireAlarm)) {
            dispatcher.RemoveAlarm(expireAlarm);
        }

        FillNodeStateMsgArgs(nodeStateArgsStorage);

        status = ImportState(masterNode, NULL, 0, foundNodeArgs, numFoundNodeArgs);
        if (status != ER_OK) {
            lock.Unlock();
            MethodReply(msg, "org.alljoyn.Bus.BTController.InternalError", QCC_StatusText(status));
            bt.Disconnect(sender);
            return;
        }

        foundNodeDB.RemoveExpiration();

    } else {
        // We are still the master

        // Add information about the already connected nodes to the found
        // node data so that our new minions will have up-to-date
        // advertising information about our existing minions.
        FillFoundNodesMsgArgs(foundNodeArgsStorage, nodeDB);

        bool noRotateMinions = !RotateMinions();
        BTNodeInfo connectingNode(addr, sender);
        connectingNode->SetUUIDRev(otherUUIDRev);
        connectingNode->SetSessionID(msg->GetSessionId());
        connectingNode->SetRelationship(_BTNodeInfo::DIRECT_MINION);

        status = ImportState(connectingNode, nodeStateArgs, numNodeStateArgs, foundNodeArgs, numFoundNodeArgs);
        if (status != ER_OK) {
            lock.Unlock();
            QCC_LogError(status, ("Dropping %s due to import state error", sender.c_str()));
            bt.Disconnect(sender);
            return;
        }

        if ((find.minion == self) && !UseLocalFind()) {
            // Force updating the find delegation
            if (find.active) {
                QCC_DbgPrintf(("Stopping local find..."));
                find.StopLocal();
            }
            find.dirty = true;
        }

        if ((advertise.minion == self) && !UseLocalAdvertise()) {
            // Force updating the advertise delegation
            if (advertise.active) {
                QCC_DbgPrintf(("Stopping local advertise..."));
                advertise.StopLocal();
            }
            advertise.dirty = true;
        }

        if (noRotateMinions && RotateMinions()) {
            // Force changing from permanent delegations to durational delegations
            advertise.dirty = true;
            find.dirty = true;
        }
        updateDelegations = true;
    }

    QCC_DbgPrintf(("We are %s, %s is now our %s",
                   IsMaster() ? "still the master" : (IsDrone() ? "now a drone" : "just a minion"),
                   addr.ToString().c_str(), IsMaster() ? "minion" : "master"));

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
        advertise.dirty = true;
    }

    if (IsMaster()) {
        ResetExpireNameAlarm();
    } else {
        RemoveExpireNameAlarm();
        //dispatcher.RemoveAlarm(stopAd);
    }

    status = MsgArg::Set(args, numArgs, SIG_SET_STATE_OUT,
                         bt.IsEIRCapable(),
                         masterUUIDRev,
                         self->GetBusAddress().addr.GetRaw(),
                         self->GetBusAddress().psm,
                         nodeStateArgsStorage.size(), &nodeStateArgsStorage.front(),
                         foundNodeArgsStorage.size(), &foundNodeArgsStorage.front());
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
    DispatchOperation(new ProcessSetStateReplyDispatchInfo(msg, ctx->newMaster, ctx->node));
    delete ctx;
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
        nodeDB.NodeDiff(adInfo, &externalDB, NULL);
        externalDB.NodeDiff(adInfo, &minionDB, NULL);

        const BTNodeDB* newAdInfo = lost ? NULL : &adInfo;
        const BTNodeDB* oldAdInfo = lost ? &adInfo : NULL;
        const BTNodeDB* newMinionDB = lost ? NULL : &minionDB;
        const BTNodeDB* oldMinionDB = lost ? &minionDB : NULL;
        const BTNodeDB* newExternalDB = lost ? NULL : &externalDB;
        const BTNodeDB* oldExternalDB = lost ? &externalDB : NULL;

        nodeDB.UpdateDB(newMinionDB, oldMinionDB, false);
        foundNodeDB.UpdateDB(newExternalDB, oldExternalDB, false);
        foundNodeDB.DumpTable("foundNodeDB - Updated set of found devices");
        assert(!devAvailable || (nodeDB.Size() > 0));

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
        QCC_LogError(ER_FAIL, ("Received %s from %s who is NOT a direct minion",
                               msg->GetMemberName(), msg->GetSender()));
        return;
    }

    uint32_t uuidRev;
    uint64_t adBdAddrRaw;
    bool eirCapable;

    QStatus status = msg->GetArgs(SIG_FOUND_DEV, &adBdAddrRaw, &uuidRev, &eirCapable);

    if (status == ER_OK) {
        BDAddress adBdAddr(adBdAddrRaw);
        ProcessDeviceChange(adBdAddr, uuidRev, eirCapable);
    }
}


void BTController::HandleConnectAddrChanged(const InterfaceDescription::Member* member,
                                            const char* sourcePath,
                                            Message& msg)
{
    QCC_DbgTrace(("BTController::HandleConnectAddrChanged(member = %s, sourcePath = \"%s\", msg = <>)",
                  member->name.c_str(), sourcePath));

    if ((!IsMinion() && !nodeDB.FindNode(msg->GetSender())->IsDirectMinion()) ||
        (!IsMaster() && (master->GetServiceName() == msg->GetSender()))) {
        // We only handle FoundDevice signals from our direct minions or from our master.
        QCC_LogError(ER_FAIL, ("Received %s from %s who is NOT a direct minion NOR our master.",
                               msg->GetMemberName(), msg->GetSender()));
        return;
    }

    uint64_t oldRawAddr;
    uint16_t oldPSM;
    uint64_t newRawAddr;
    uint16_t newPSM;

    QStatus status = msg->GetArgs(SIG_CONN_ADDR_CHANGED, &oldRawAddr, &oldPSM, &newRawAddr, &newPSM);
    if (status == ER_OK) {
        BTBusAddress oldAddr(oldRawAddr, oldPSM);
        BTBusAddress newAddr(newRawAddr, newPSM);
        if (!IsMinion()) {
            nodeDB.Lock();
            BTNodeInfo changedNode = nodeDB.FindNode(oldAddr);
            if (changedNode->IsValid()) {
                nodeDB.RemoveNode(changedNode);
                changedNode->SetBusAddress(newAddr);
                nodeDB.AddNode(changedNode);
            }
            nodeDB.Unlock();
        }
        if (!IsMaster()) {
            lock.Lock();
            if (masterNode->GetBusAddress() == oldAddr) {
                masterNode->SetBusAddress(newAddr);
            }
            lock.Unlock();
        }
    }
}


void BTController::DeferredBTDeviceAvailable(bool on)
{
    QCC_DbgTrace(("BTController::DeferredBTDeviceAvailable(<%s>)", on ? "on" : "off"));
    lock.Lock();
    if (on && !devAvailable) {
        BTBusAddress listenAddr;
        devAvailable = true;
        QStatus status = bt.StartListen(listenAddr.addr, listenAddr.psm);
        if (status == ER_OK) {
            assert(listenAddr.IsValid());
            listening = true;

            if (self->GetBusAddress() != listenAddr) {
                SetSelfAddress(listenAddr);
            }

            find.dirty = true;  // Update ignore addrs

            if (IsMaster()) {
                UpdateDelegations(advertise);
                UpdateDelegations(find);
            }
        } else {
            QCC_LogError(status, ("Failed to start listening for incoming connections"));
        }
    } else if (!on && devAvailable) {
        if (listening) {
            bt.StopListen();
            BTBusAddress nullAddr;
            listening = false;
        }
        if (advertise.active) {
            if (advertise.minion == self) {
                QCC_DbgPrintf(("Stopping local advertise..."));
                advertise.StopLocal();
            }
            advertise.active = false;
            advertise.StopAlarm();
        }
        if (find.active) {
            if (find.minion == self) {
                QCC_DbgPrintf(("Stopping local find..."));
                find.StopLocal();
            }
            find.active = false;
            find.StopAlarm();
        }

        foundNodeDB.RefreshExpiration(LOST_DEVICE_TIMEOUT);
        ResetExpireNameAlarm();

        blacklist->clear();

        devAvailable = false;
    }

    lock.Unlock();
}


QStatus BTController::DeferredSendSetState(const BTNodeInfo& node)
{
    QCC_DbgTrace(("BTController::DeferredSendSetState(node = %s)", node->GetBusAddress().ToString().c_str()));
    assert(!master);

    QStatus status;
    vector<MsgArg> nodeStateArgsStorage;
    vector<MsgArg> foundNodeArgsStorage;
    MsgArg args[SIG_SET_STATE_IN_SIZE];
    size_t numArgs = ArraySize(args);
    Message reply(bus);
    ProxyBusObject* newMaster = new ProxyBusObject(bus, node->GetUniqueName().c_str(), bluetoothObjPath, node->GetSessionID());

    lock.Lock();
    if ((find.minion == self) && find.active) {
        /*
         * Gotta shut down the local find operation since the exchange
         * of the SetState method call and response which will result
         * in one side or the other taking control of who performs the
         * find operation.
         */
        QCC_DbgPrintf(("Stopping local find..."));
        find.StopLocal();
    }
    if ((advertise.minion == self) && advertise.active) {
        /*
         * Gotta shut down the local advertise operation since the
         * exchange for the SetState method call and response which
         * will result in one side or the other taking control of who
         * performs the advertise operation.
         */
        QCC_DbgPrintf(("Stopping local advertise..."));
        advertise.StopLocal();
    }

    newMaster->AddInterface(*org.alljoyn.Bus.BTController.interface);

    uint8_t slaveFactor = ComputeSlaveFactor();

    QCC_DbgPrintf(("SendSetState prep args"));
    FillNodeStateMsgArgs(nodeStateArgsStorage);
    FillFoundNodesMsgArgs(foundNodeArgsStorage, foundNodeDB);

    status = MsgArg::Set(args, numArgs, SIG_SET_STATE_IN,
                         directMinions,
                         slaveFactor,
                         bt.IsEIRCapable(),
                         masterUUIDRev,
                         self->GetBusAddress().addr.GetRaw(),
                         self->GetBusAddress().psm,
                         nodeStateArgsStorage.size(), &nodeStateArgsStorage.front(),
                         foundNodeArgsStorage.size(), &foundNodeArgsStorage.front());
    if (status != ER_OK) {
        delete newMaster;
        QCC_LogError(status, ("Dropping %s due to internal error", node->GetBusAddress().ToString().c_str()));
        bt.Disconnect(node->GetUniqueName());
        goto exit;
    }

    /*
     * There is a small chance that 2 devices initiating a connection to each
     * other may each send the SetState method call simultaneously.  We
     * release the lock while making the synchronous method call to prevent a
     * possible deadlock in that case.  The SendSetState function must not run
     * in the same thread as that HandleSetState function.
     */
    lock.Unlock();
    QCC_DbgPrintf(("Sending SetState method call to %s (%s)",
                   node->GetUniqueName().c_str(), node->GetBusAddress().ToString().c_str()));
    status = newMaster->MethodCallAsync(*org.alljoyn.Bus.BTController.SetState,
                                        this, ReplyHandler(&BTController::HandleSetStateReply),
                                        args, ArraySize(args),
                                        new SetStateReplyContext(newMaster, node));

    if (status != ER_OK) {
        delete newMaster;
        QCC_LogError(status, ("Dropping %s due to internal error", node->GetBusAddress().ToString().c_str()));
        bt.Disconnect(node->GetUniqueName());
    }

exit:
    return status;
}


void BTController::DeferredProcessSetStateReply(Message& reply,
                                                ProxyBusObject* newMaster,
                                                BTNodeInfo& node)
{
    QCC_DbgTrace(("BTController::DeferredProcessSetStateReply(reply = <>, newMaster = %p, node = %s)",
                  newMaster, node->GetBusAddress().ToString().c_str()));

    lock.Lock();

    if (reply->GetType() == MESSAGE_METHOD_RET) {
        size_t numNodeStateArgs;
        MsgArg* nodeStateArgs;
        size_t numFoundNodeArgs;
        MsgArg* foundNodeArgs;
        uint64_t rawBDAddr;
        uint16_t psm;
        uint32_t otherUUIDRev;
        bool remoteEIRCapable;
        QStatus status;

        if (nodeDB.FindNode(node->GetBusAddress())->IsValid()) {
            QCC_DbgHLPrintf(("Already got node state information."));
            delete newMaster;
            goto exit;
        }

        status = reply->GetArgs(SIG_SET_STATE_OUT,
                                &remoteEIRCapable,
                                &otherUUIDRev,
                                &rawBDAddr,
                                &psm,
                                &numNodeStateArgs, &nodeStateArgs,
                                &numFoundNodeArgs, &foundNodeArgs);
        if ((status != ER_OK) || ((node->GetBusAddress().addr.GetRaw() != rawBDAddr) &&
                                  (node->GetBusAddress().psm != psm))) {
            delete newMaster;
            QCC_LogError(status, ("Dropping %s due to error parsing the args (sig: \"%s\")",
                                  node->GetBusAddress().ToString().c_str(), SIG_SET_STATE_OUT));
            bt.Disconnect(node->GetUniqueName());
            goto exit;
        }

        if (otherUUIDRev != bt::INVALID_UUIDREV) {
            if (bt.IsEIRCapable() && !node->IsEIRCapable() && remoteEIRCapable && (node->GetConnectionCount() == 1)) {
                node->SetEIRCapable(true);
                SessionId sessionID = node->GetSessionID();
                node->SetSessionID(0);
                bus.LeaveSession(sessionID);
                goto exit;
            }

            if (numNodeStateArgs == 0) {
                // We are now a minion (or a drone if we have more than one direct connection)
                master = newMaster;
                assert(foundNodeDB.FindNode(node->GetBusAddress())->IsValid());
                assert(&(*foundNodeDB.FindNode(node->GetBusAddress())) == &(*node));
                masterNode = node;
                masterNode->SetUUIDRev(otherUUIDRev);
                masterNode->SetRelationship(_BTNodeInfo::MASTER);
                masterNode->SetEIRCapable(remoteEIRCapable);

                if (dispatcher.HasAlarm(expireAlarm)) {
                    dispatcher.RemoveAlarm(expireAlarm);
                }

                status = ImportState(masterNode, NULL, 0, foundNodeArgs, numFoundNodeArgs);
                if (status != ER_OK) {
                    QCC_LogError(status, ("Dropping %s due to import state error", node->GetBusAddress().ToString().c_str()));
                    bt.Disconnect(node->GetUniqueName());
                    goto exit;
                }

            } else {
                // We are the still the master
                bool noRotateMinions = !RotateMinions();
                delete newMaster;
                node->SetRelationship(_BTNodeInfo::DIRECT_MINION);

                status = ImportState(node, nodeStateArgs, numNodeStateArgs, foundNodeArgs, numFoundNodeArgs);
                if (status != ER_OK) {
                    QCC_LogError(status, ("Dropping %s due to import state error", node->GetBusAddress().ToString().c_str()));
                    bt.Disconnect(node->GetUniqueName());
                    goto exit;
                }

                if (noRotateMinions && RotateMinions()) {
                    // Force changing from permanent delegations to durational delegations
                    advertise.dirty = true;
                    find.dirty = true;
                }
            }

            QCC_DbgPrintf(("We are %s, %s is now our %s",
                           IsMaster() ? "still the master" : (IsDrone() ? "now a drone" : "just a minion"),
                           node->GetBusAddress().ToString().c_str(), IsMaster() ? "minion" : "master"));

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

                UpdateDelegations(advertise);
                UpdateDelegations(find);

                ResetExpireNameAlarm();
            } else {
                RemoveExpireNameAlarm();
                //dispatcher.RemoveAlarm(stopAd);
            }
        }
    } else {
        delete newMaster;
        qcc::String errMsg;
        const char* errName = reply->GetErrorName(&errMsg);
        QCC_LogError(ER_FAIL, ("Dropping %s due to internal error: %s - %s", node->GetBusAddress().ToString().c_str(), errName, errMsg.c_str()));
        bt.Disconnect(node->GetUniqueName());
    }

exit:
    joinSessionNodeDB.RemoveNode(node);
    lock.Unlock();

    if (DecrementAndFetch(&incompleteConnections) > 0) {
        connectCompleted.SetEvent();
    }
}


void BTController::DeferredHandleDelegateFind(Message& msg)
{
    QCC_DbgTrace(("BTController::HandleDelegateFind(msg = <>)"));

    lock.Lock();

    PickNextDelegate(find);

    if (find.minion == self) {
        uint64_t* ignoreAddrsArg;
        size_t numIgnoreAddrs;
        uint32_t duration;

        QStatus status = msg->GetArgs(SIG_DELEGATE_FIND, &numIgnoreAddrs, &ignoreAddrsArg, &duration);

        if (status == ER_OK) {
            if (numIgnoreAddrs > 0) {
                size_t i;
                BDAddressSet ignoreAddrs(*blacklist); // initialize ignore addresses with the blacklist
                for (i = 0; i < numIgnoreAddrs; ++i) {
                    ignoreAddrs->insert(ignoreAddrsArg[i]);
                }

                QCC_DbgPrintf(("Starting find for %u seconds...", duration));
                status = bt.StartFind(ignoreAddrs, duration);
                find.active = (status == ER_OK);
            } else {
                QCC_DbgPrintf(("Stopping local find..."));
                find.StopLocal();
            }
        }
    } else {
        const MsgArg* args;
        size_t numArgs;

        BTNodeInfo delegate = find.minion->GetConnectNode();

        msg->GetArgs(numArgs, args);

        // Pick a minion to do the work for us.
        assert(nodeDB.FindNode(find.minion->GetBusAddress())->IsValid());
        QCC_DbgPrintf(("Selected %s as our find minion.", find.minion->GetBusAddress().ToString().c_str()));

        Signal(delegate->GetUniqueName().c_str(), delegate->GetSessionID(), *find.delegateSignal, args, numArgs);
    }
    lock.Unlock();
}


void BTController::DeferredHandleDelegateAdvertise(Message& msg)
{
    QCC_DbgTrace(("BTController::DeferredHandleDelegateAdvertise(msg = <>)"));

    lock.Lock();

    PickNextDelegate(advertise);

    if (advertise.minion == self) {
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
                QCC_DbgPrintf(("Stopping local advertise..."));
                advertise.StopLocal();
            }
        }
    } else {
        const MsgArg* args;
        size_t numArgs;

        BTNodeInfo delegate = advertise.minion->GetConnectNode();

        msg->GetArgs(numArgs, args);

        // Pick a minion to do the work for us.
        assert(nodeDB.FindNode(advertise.minion->GetBusAddress())->IsValid());
        QCC_DbgPrintf(("Selected %s as our advertise minion.", advertise.minion->GetBusAddress().ToString().c_str()));

        Signal(delegate->GetUniqueName().c_str(), delegate->GetSessionID(), *advertise.delegateSignal, args, numArgs);
    }
    lock.Unlock();
}


void BTController::DeferredNameLostHander(const String& name)
{
    // An endpoint left the bus.
    QCC_DbgPrintf(("%s has left the bus", name.c_str()));
    bool updateDelegations = false;

    lock.Lock();
    if (master && (master->GetServiceName() == name)) {
        // We are a minion or a drone and our master has left us.

        QCC_DbgPrintf(("Our master left us: %s", masterNode->GetBusAddress().ToString().c_str()));
        // We are the master now.

        if (advertise.minion == self) {
            QCC_DbgPrintf(("Stopping local advertise..."));
            advertise.StopLocal();
        } else {
            MsgArg args[SIG_DELEGATE_AD_SIZE];
            size_t argsSize = ArraySize(args);

            /* Advertise an empty list for a while */
            MsgArg::Set(args, argsSize, SIG_DELEGATE_AD,
                        bt::INVALID_UUIDREV,
                        0ULL,
                        bt::INVALID_PSM,
                        static_cast<size_t>(0), NULL,
                        static_cast<uint32_t>(0));
            assert(argsSize == ArraySize(args));

            BTNodeInfo delegate = advertise.minion->GetConnectNode();

            Signal(delegate->GetUniqueName().c_str(), delegate->GetSessionID(), *advertise.delegateSignal, args, argsSize);
            advertise.active = false;
        }

        if (find.minion == self) {
            QCC_DbgPrintf(("Stopping local find..."));
            find.StopLocal();
        } else {
            MsgArg args[SIG_DELEGATE_FIND_SIZE];
            size_t argsSize = ArraySize(args);

            /* Advertise an empty list for a while */
            MsgArg::Set(args, argsSize, SIG_DELEGATE_FIND,
                        static_cast<size_t>(0), NULL,
                        static_cast<uint32_t>(0));
            assert(argsSize == ArraySize(args));

            BTNodeInfo delegate = find.minion->GetConnectNode();

            Signal(delegate->GetUniqueName().c_str(), delegate->GetSessionID(), *find.delegateSignal, args, argsSize);
            find.active = false;
        }

        delete master;
        master = NULL;
        masterNode = BTNodeInfo();

        // Our master and all of our master's minions excluding ourself and
        // our minions are in foundNodeDB so refreshing the expiration time on
        // the entire foundNodeDB will cause their advertised names to expire
        // as well.  No need to distribute lost names at this time.
        foundNodeDB.RefreshExpiration(LOST_DEVICE_TIMEOUT);
        ResetExpireNameAlarm();

        // We need to prepare for controlling discovery.
        find.dirty = true;  // Update ignore addrs

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

            find.dirty = true;  // Update ignore addrs

            // Indicate the name lists have changed.
            advertise.count -= minion->AdvertiseNamesSize();
            advertise.dirty = true;

            find.count -= minion->FindNamesSize();
            find.dirty = true;

            nodeDB.RemoveNode(minion);
            assert(!devAvailable || (nodeDB.Size() > 0));

            if (minion->IsEIRCapable()) {
                --eirMinions;
            }

            if (!RotateMinions() && wasRotateMinions) {
                advertise.StopAlarm();
                find.StopAlarm();
            }

            if (wasFindMinion) {
                find.minion = self;
                find.active = false;
                find.StopAlarm();
            }

            if (wasAdvertiseMinion) {
                advertise.minion = self;
                advertise.active = false;
                advertise.StopAlarm();
            }

            if (wasDirect) {
                --directMinions;
            }

            if (IsMaster()) {
                updateDelegations = true;

                if (!minion->AdvertiseNamesEmpty()) {
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
    }

    if (updateDelegations) {
        UpdateDelegations(advertise);
        UpdateDelegations(find);
        QCC_DbgPrintf(("NodeDB after processing lost node"));
        QCC_DEBUG_ONLY(DumpNodeStateTable());
    }
    lock.Unlock();
}


void BTController::DistributeAdvertisedNameChanges(const BTNodeDB* newAdInfo,
                                                   const BTNodeDB* oldAdInfo)
{
    QCC_DbgTrace(("BTController::DistributeAdvertisedNameChanges(newAdInfo = <%lu nodes>, oldAdInfo = <%lu nodes>)",
                  newAdInfo ? newAdInfo->Size() : 0, oldAdInfo ? oldAdInfo->Size() : 0));

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
    if (!IsMinion() && devAvailable) {
        set<BTNodeInfo> destNodesOld;
        set<BTNodeInfo> destNodesNew;
        nodeDB.Lock();
        for (BTNodeDB::const_iterator it = nodeDB.Begin(); it != nodeDB.End(); ++it) {
            const BTNodeInfo& node = *it;
            if (node->IsDirectMinion()) {
                assert(node != self);  // We can't be a direct minion of ourself.
                QCC_DbgPrintf(("Notify %s of the name changes.", node->GetBusAddress().ToString().c_str()));
                if (oldAdInfo && oldAdInfo->Size() > 0) {
                    destNodesOld.insert(node);
                }
                if (newAdInfo && newAdInfo->Size() > 0) {
                    destNodesNew.insert(node);
                }
            }
        }
        nodeDB.Unlock();

        for (set<BTNodeInfo>::const_iterator it = destNodesOld.begin(); it != destNodesOld.end(); ++it) {
            SendFoundNamesChange(*it, *oldAdInfo, true);
        }

        for (set<BTNodeInfo>::const_iterator it = destNodesNew.begin(); it != destNodesNew.end(); ++it) {
            SendFoundNamesChange(*it, *newAdInfo, false);
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
                bt.FoundNamesChange(node->GetGUID().ToString(), vectorizedNames, node->GetBusAddress().addr, node->GetBusAddress().psm, true);
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
                bt.FoundNamesChange(node->GetGUID().ToString(), vectorizedNames, node->GetBusAddress().addr, node->GetBusAddress().psm, false);
            }
        }
    }
}


void BTController::SendFoundNamesChange(const BTNodeInfo& destNode,
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
        status = Signal(destNode->GetUniqueName().c_str(), destNode->GetSessionID(),
                        *org.alljoyn.Bus.BTController.LostNames,
                        &arg, 1);
    } else {
        status = Signal(destNode->GetUniqueName().c_str(), destNode->GetSessionID(),
                        *org.alljoyn.Bus.BTController.FoundNames,
                        &arg, 1);
    }

    if (status != ER_OK) {
        QCC_LogError(status, ("Failed to send org.alljoyn.Bus.BTController.%s signal to %s",
                              lost ? "LostNames" : "FoundNames",
                              destNode->GetBusAddress().ToString().c_str()));
    }
}


QStatus BTController::ImportState(BTNodeInfo& connectingNode,
                                  MsgArg* nodeStateArgs,
                                  size_t numNodeStates,
                                  MsgArg* foundNodeArgs,
                                  size_t numFoundNodes)
{
    QCC_DbgTrace(("BTController::ImportState(addr = (%s), nodeStateArgs = <>, numNodeStates = %u, foundNodeArgs = <>, numFoundNodes = %u)",
                  connectingNode->GetBusAddress().ToString().c_str(), numNodeStates, numFoundNodes));

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
        bool eirCapable;

        status = nodeStateArgs[i].Get(SIG_NODE_STATE_ENTRY,
                                      &guidStr,
                                      &bn,
                                      &rawBdAddr, &psm,
                                      &anSize, &anList,
                                      &fnSize, &fnList,
                                      &eirCapable);
        if (status != ER_OK) {
            return status;
        }

        String busName(bn);
        BTBusAddress nodeAddr(BDAddress(rawBdAddr), psm);
        GUID guid(guidStr);

        QCC_DbgPrintf(("Processing names for new minion %s (GUID: %s  uniqueName: %s):",
                       nodeAddr.ToString().c_str(),
                       guid.ToString().c_str(),
                       busName.c_str()));

        if (nodeAddr == connectingNode->GetBusAddress()) {
            node = connectingNode;  // need to modify the existing instance since other nodes already refer to it.
            node->SetGUID(guid);
            node->SetUniqueName(busName);
        } else {
            node = BTNodeInfo(nodeAddr, busName, guid);
            node->SetConnectNode(connectingNode);
            node->SetRelationship(_BTNodeInfo::INDIRECT_MINION);

        }
        node->SetEIRCapable(eirCapable);
        if (eirCapable) {
            ++eirMinions;
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
        if (incomingDB.FindNode((*nodeit)->GetConnectNode()->GetBusAddress())->IsValid()) {
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
    foundNodeDB.UpdateDB(NULL, &incomingDB);
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

    QCC_DbgPrintf(("%s %s operation because device is %s, conn is %s, %s %s%s, and op is %s.",
                   start ? "Starting" : (restart ? "Updating" : (stop ? "Stopping" : "Skipping")),
                   advertiseOp ? "advertise" : "find",
                   devAvailable ? "available" : "not available",
                   allowConn ? "allowed" : "not allowed",
                   advertiseOp ? "name list" : "ignore addrs",
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
    }

    if (start) {
        // Set the advertise/find arguments.
        nameInfo.StartOp();

    } else if (restart) {
        // Update the advertise/find arguments.
        nameInfo.RestartOp();

    } else if (stop) {
        // Clear out the advertise/find arguments.
        nameInfo.StopOp(false);
    }
}


QStatus BTController::ExtractAdInfo(const MsgArg* entries, size_t size, BTNodeDB& adInfo)
{
    QCC_DbgTrace(("BTController::ExtractAdInfo()"));

    QStatus status = ER_OK;

    if (entries && (size > 0)) {
        for (size_t i = 0; i < size; ++i) {
            char* guidRaw;
            uint64_t rawAddr;
            uint16_t psm;
            MsgArg* names;
            size_t numNames;

            status = entries[i].Get(SIG_AD_NAME_MAP_ENTRY, &guidRaw, &rawAddr, &psm, &numNames, &names);

            if (status == ER_OK) {
                String guidStr(guidRaw);
                GUID guid(guidStr);
                String empty;
                BTBusAddress addr(rawAddr, psm);
                BTNodeInfo node(addr, empty, guid);

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
            char* guidRaw;
            uint64_t rawBdAddr;
            uint16_t psm;
            size_t anSize;
            MsgArg* anList;
            size_t k;

            status = adMap[j].Get(SIG_AD_NAME_MAP_ENTRY, &guidRaw, &rawBdAddr, &psm, &anSize, &anList);
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

            // If the node is in our subnet, then use the real connect address.
            BTNodeInfo n = nodeDB.FindNode(nodeAddr);
            node->SetConnectNode(n->IsValid() ? n->GetConnectNode() : connNode);

            String guidStr(guidRaw);
            GUID guid(guidStr);
            node->SetGUID(guid);
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
                              node->GetGUID().ToString().c_str(),
                              node->GetUniqueName().c_str(),
                              node->GetBusAddress().addr.GetRaw(),
                              node->GetBusAddress().psm,
                              nodeAdNames.size(), &nodeAdNames.front(),
                              nodeFindNames.size(), &nodeFindNames.front(),
                              node->IsEIRCapable()));

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
        xformMap[(&adInfo == &nodeDB) ? self->GetBusAddress() : (*it)->GetConnectNode()->GetBusAddress()].AddNode(*it);
    }
    adInfo.Unlock();

    args.reserve(args.size() + xformMap.size());
    map<BTBusAddress, BTNodeDB>::const_iterator xmit;
    for (xmit = xformMap.begin(); xmit != xformMap.end(); ++xmit) {
        vector<MsgArg> adNamesArgs;

        const BTNodeDB& db = xmit->second;
        BTNodeInfo connNode = xmit->second.FindNode(xmit->first);

        if (!connNode->IsValid()) {
            connNode = foundNodeDB.FindNode(xmit->first);
        }

        if (!connNode->IsValid()) {
            connNode = nodeDB.FindNode(xmit->first);
        }

        if (!connNode->IsValid()) {
            // Should never happen, since it is an internal bug (hence assert
            // check below), but gracefully handle it in case it does in
            // release mode.
            QCC_LogError(ER_NONE, ("Failed to find address %s in DB that should contain it!", xmit->first.ToString().c_str()));
            db.DumpTable("db: Corrupt DB?");
            assert(connNode->IsValid());
            continue;
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
                                         node->GetGUID().ToString().c_str(),
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


uint8_t BTController::ComputeSlaveFactor() const
{
    BTNodeDB::const_iterator nit;
    uint8_t cnt = 0;

    nodeDB.Lock();
    for (nit = nodeDB.Begin(); nit != nodeDB.End(); ++nit) {
        const BTNodeInfo& minion = *nit;
        if (minion->IsDirectMinion()) {
            bool master = false;
            QStatus status = bt.IsMaster(minion->GetBusAddress().addr, master);
            if ((status == ER_OK) && !master) {
                ++cnt;
            } else if (status != ER_OK) {
                // failures count against us
                ++cnt;
            }
        }
    }
    nodeDB.Unlock();

    return cnt;
}


void BTController::SetSelfAddress(const BTBusAddress& newAddr)
{
    BTNodeDB::const_iterator nit;
    vector<BTNodeInfo> dests;
    vector<BTNodeInfo>::const_iterator dit;

    MsgArg args[SIG_CONN_ADDR_CHANGED_SIZE];
    size_t argsSize = ArraySize(args);

    lock.Lock();
    MsgArg::Set(args, argsSize, SIG_CONN_ADDR_CHANGED,
                self->GetBusAddress().addr.GetRaw(),
                self->GetBusAddress().psm,
                newAddr.addr.GetRaw(),
                newAddr.psm);

    dests.reserve(directMinions + (!IsMaster() ? 1 : 0));

    nodeDB.Lock();
    nodeDB.RemoveNode(self);
    self->SetBusAddress(newAddr);
    nodeDB.AddNode(self);
    for (nit = nodeDB.Begin(); nit != nodeDB.End(); ++nit) {
        const BTNodeInfo& minion = *nit;
        if (minion->IsDirectMinion()) {
            dests.push_back(minion);
        }
    }
    nodeDB.Unlock();

    if (!IsMaster()) {
        dests.push_back(master->GetServiceName());
    }

    lock.Unlock();

    for (dit = dests.begin(); dit != dests.end(); ++dit) {
        Signal((*dit)->GetUniqueName().c_str(), (*dit)->GetSessionID(), *org.alljoyn.Bus.BTController.ConnectAddrChanged, args, argsSize);
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
        case DispatchInfo::UPDATE_DELEGATIONS:
            QCC_DbgPrintf(("    Updating delegations"));
            lock.Lock();
            UpdateDelegations(advertise);
            UpdateDelegations(find);
            QCC_DbgPrintf(("NodeDB after updating delegations"));
            QCC_DEBUG_ONLY(DumpNodeStateTable());
            lock.Unlock();
            break;

        case DispatchInfo::EXPIRE_CACHED_NODES: {
            QCC_DbgPrintf(("    Expire cached nodes"));
            BTNodeDB expiredDB;
            foundNodeDB.PopExpiredNodes(expiredDB);

            expiredDB.DumpTable("expiredDB - Expiring cached advertisements");
            foundNodeDB.DumpTable("foundNodeDB - Remaining cached advertisements after expiration");

            DistributeAdvertisedNameChanges(NULL, &expiredDB);
            uint64_t dispatchTime = foundNodeDB.NextNodeExpiration();
            if (dispatchTime < (numeric_limits<uint64_t>::max() - LOST_DEVICE_TIMEOUT_EXT)) {
                expireAlarm = DispatchOperation(new ExpireCachedNodesDispatchInfo(), dispatchTime + LOST_DEVICE_TIMEOUT_EXT);
            }
            break;
        }

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
            DeferredSendSetState(static_cast<SendSetStateDispatchInfo*>(op)->node);
            break;
        }

        case DispatchInfo::PROCESS_SET_STATE_REPLY: {
            QCC_DbgPrintf(("    Process set state reply"));
            ProcessSetStateReplyDispatchInfo* di = static_cast<ProcessSetStateReplyDispatchInfo*>(op);
            DeferredProcessSetStateReply(di->msg, di->newMaster, di->node);
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

        case DispatchInfo::EXPIRE_BLACKLISTED_DEVICE:
            QCC_DbgPrintf(("    Expiring blacklisted device"));
            lock.Lock();
            blacklist->erase(static_cast<ExpireBlacklistedDevDispatchInfo*>(op)->addr);
            find.dirty = true;
            UpdateDelegations(find);
            lock.Unlock();
            break;
        }
    }

    delete op;
}


void BTController::PickNextDelegate(NameArgInfo& nameOp)
{
    if (nameOp.UseLocal()) {
        nameOp.minion = self;

    } else {
        BTNodeInfo skip;
        if (NumEIRMinions() > 1) {
            skip = (&nameOp == static_cast<NameArgInfo*>(&find)) ? advertise.minion : find.minion;
        }
        nameOp.minion = nodeDB.FindDelegateMinion(nameOp.minion, skip, (NumEIRMinions() > 0));
    }

    QCC_DbgPrintf(("Selected %s as %s delegate.  (UseLocal(): %s  EIR: %s  Num EIR Minions: %u  Num Minions: %u)",
                   (nameOp.minion == self) ? "ourself" : nameOp.minion->GetBusAddress().ToString().c_str(),
                   (&nameOp == static_cast<NameArgInfo*>(&find)) ? "find" : "advertise",
                   nameOp.UseLocal() ? "true" : "false",
                   bt.IsEIRCapable() ? "true" : "false",
                   NumEIRMinions(),
                   NumMinions()));
}


void BTController::NameArgInfo::AlarmTriggered(const Alarm& alarm, QStatus reason)
{
    QCC_DbgTrace(("BTController::NameArgInfo::AlarmTriggered(alarm = <%s>, reason = %s)",
                  alarm == bto.find.alarm ? "find" : "advertise", QCC_StatusText(reason)));

    if (reason == ER_OK) {
        bto.lock.Lock();
        if (bto.RotateMinions() && !Empty()) {
            // Manually re-arm alarm since automatically recurring alarms cannot be stopped.
            StartAlarm();

            bto.PickNextDelegate(*this);
            SendDelegateSignal();
        } else if (Empty() && (alarm == bto.advertise.alarm)) {
            ClearArgs();
            SendDelegateSignal();
        }
        bto.lock.Unlock();
    }
}


QStatus BTController::NameArgInfo::SendDelegateSignal()
{
    QCC_DbgPrintf(("Sending %s signal to %s (via session %x)", delegateSignal->name.c_str(),
                   minion->GetBusAddress().ToString().c_str(), minion->GetSessionID()));
    assert(minion != bto.self);

    NameArgs largs = args;
    bto.lock.Unlock();  // SendDelegateSignal gets called with bto.lock held.
    QStatus status = bto.Signal(minion->GetUniqueName().c_str(), minion->GetSessionID(), *delegateSignal, largs->args, largs->argsSize);
    bto.lock.Lock();

    return status;
}


void BTController::NameArgInfo::StartOp()
{
    QStatus status;
    size_t retry = ((bto.NumEIRMinions() > 0) ? bto.NumEIRMinions() :
                    (bto.directMinions > 0) ? bto.directMinions : 1);

    SetArgs();

    do {
        bto.PickNextDelegate(*this);

        if (minion == bto.self) {
            status = StartLocal();
        } else {
            status = SendDelegateSignal();
            if (bto.RotateMinions()) {
                assert(minion->IsValid());
                assert(minion != bto.self);
                if (status == ER_OK) {
                    StartAlarm();
                }
            }
        }
    } while ((status == ER_BUS_NO_ROUTE) && (--retry));

    if (status != ER_OK) {
        QCC_LogError(status, ("StartOp() failed"));
    }

    active = (status == ER_OK);
}


void BTController::NameArgInfo::StopOp(bool immediate)
{
    QStatus status;

    if ((this != &bto.advertise) || immediate) {
        ClearArgs();
    } else {
        SetArgs();  // Update advertise to inlcude all devices with no advertised names
    }

    if (this == &bto.advertise) {
        // Set the duration to the delegate time if this is not an immediate stop operation command.
        args->args[SIG_DELEGATE_AD_DURATION_PARAM].Set(SIG_DURATION, immediate ? static_cast<uint32_t>(0) : static_cast<uint32_t>(DELEGATE_TIME));
    }

    active = false;

    if (minion == bto.self) {
        status = StopLocal(immediate);
    } else {
        status = SendDelegateSignal();
        StopAlarm();
        active = !(status == ER_OK);
    }

    if ((this == &bto.advertise) && !immediate) {
        ClearArgs();
    }

    if (status != ER_OK) {
        QCC_LogError(status, ("StopOp() failed"));
    }

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
                                    node->GetGUID().ToString().c_str(),
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


QStatus BTController::AdvertiseNameArgInfo::StopLocal(bool immediate)
{
    QStatus status;
    StopAlarm();
    if (immediate) {
        status = bto.bt.StopAdvertise();
    } else {
        status = bto.bt.StartAdvertise(bto.masterUUIDRev,
                                       bto.self->GetBusAddress().addr, bto.self->GetBusAddress().psm,
                                       bto.nodeDB, BTController::DELEGATE_TIME);
    }
    active = !(status == ER_OK);
    return status;
}


BTController::FindNameArgInfo::FindNameArgInfo(BTController& bto) :
    NameArgInfo(bto, SIG_DELEGATE_FIND_SIZE)
{
}


void BTController::FindNameArgInfo::AddName(const qcc::String& name, BTNodeInfo& node)
{
    node->AddFindName(name);
    ++count;
}


void BTController::FindNameArgInfo::RemoveName(const qcc::String& name, BTNodeInfo& node)
{
    NameSet::iterator nit = node->FindFindName(name);
    if (nit != node->GetFindNamesEnd()) {
        node->RemoveFindName(nit);
        --count;
    }
}


void BTController::FindNameArgInfo::SetArgs()
{
    QCC_DbgTrace(("BTController::FindNameArgInfo::SetArgs()"));
    NameArgs newArgs(argsSize);
    size_t localArgsSize = argsSize;

    bto.lock.Lock();
    bto.nodeDB.Lock();
    ignoreAddrsCache.clear();
    ignoreAddrsCache.reserve(bto.nodeDB.Size() + bto.blacklist->size());
    BTNodeDB::const_iterator it;
    for (it = bto.nodeDB.Begin(); it != bto.nodeDB.End(); ++it) {
        ignoreAddrsCache.push_back((*it)->GetBusAddress().addr.GetRaw());
    }
    bto.nodeDB.Unlock();

    set<BDAddress>::const_iterator bit;
    for (bit = bto.blacklist->begin(); bit != bto.blacklist->end(); ++bit) {
        ignoreAddrsCache.push_back(bit->GetRaw());
    }

    MsgArg::Set(newArgs->args, localArgsSize, SIG_DELEGATE_FIND,
                ignoreAddrsCache.size(), &ignoreAddrsCache.front(),
                bto.RotateMinions() ? DELEGATE_TIME : (uint32_t)0);
    assert(localArgsSize == argsSize);

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
                static_cast<size_t>(0), NULL,
                static_cast<uint32_t>(0));
    assert(localArgsSize == argsSize);

    bto.lock.Lock();
    args = newArgs;
    bto.lock.Unlock();
}


QStatus BTController::FindNameArgInfo::StartLocal()
{
    bto.nodeDB.Lock();
    BDAddressSet ignoreAddrs(*bto.blacklist); // initialize ignore addresses with the blacklist
    BTNodeDB::const_iterator it;
    for (it = bto.nodeDB.Begin(); it != bto.nodeDB.End(); ++it) {
        ignoreAddrs->insert((*it)->GetBusAddress().addr);
    }
    bto.nodeDB.Unlock();

    QCC_DbgPrintf(("Starting local find..."));
    return bto.bt.StartFind(ignoreAddrs);
}


QStatus BTController::FindNameArgInfo::StopLocal(bool immediate)
{
    StopAlarm();
    QStatus status = bto.bt.StopFind();
    active = !(status == ER_OK);
    return status;
}


#ifndef NDEBUG
void BTController::DumpNodeStateTable() const
{
    BTNodeDB::const_iterator nodeit;
    QCC_DbgPrintf(("Node State Table (local = %s):", bus.GetUniqueName().c_str()));
    for (nodeit = nodeDB.Begin(); nodeit != nodeDB.End(); ++nodeit) {
        const BTNodeInfo& node = *nodeit;
        NameSet::const_iterator nameit;
        QCC_DbgPrintf(("    %s (conn: %s) %s (%s%s%s%s):",
                       node->GetBusAddress().ToString().c_str(),
                       node->GetConnectNode()->GetBusAddress().ToString().c_str(),
                       node->GetUniqueName().c_str(),
                       (node == self) ? "local" : (node->IsDirectMinion() ? "direct minion" : "indirect minion"),
                       ((node == find.minion) || (node == advertise.minion)) ? " -" : "",
                       (node == find.minion) ? " find" : "",
                       (node == advertise.minion) ? " advertise" : ""));
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
