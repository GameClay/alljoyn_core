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
#include "BTEndpoint.h"

#define QCC_MODULE "ALLJOYN_BTC"


using namespace std;
using namespace qcc;

#define MethodHander(_a) static_cast<MessageReceiver::MethodHandler>(_a)
#define SignalHander(_a) static_cast<MessageReceiver::SignalHandler>(_a)

static const uint32_t ABSOLUTE_MAX_CONNECTIONS = 7; /* BT can't have more than 7 direct connections */
static const uint32_t DEFAULT_MAX_CONNECTIONS =  6; /* Gotta allow 1 connection for car-kit/headset/headphones */


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

#define SIG_ARRAY      "a"

#define SIG_BDADDR     "t"
#define SIG_DURATION   "u"
#define SIG_GUID       "s"
#define SIG_MINION_CNT "y"
#define SIG_NAME       "s"
#define SIG_PSM        "q"
#define SIG_STATUS     "u"
#define SIG_UUIDREV    "u"

#define SIG_NAME_LIST         SIG_ARRAY SIG_NAME
#define SIG_BUSADDR           SIG_BDADDR SIG_PSM
#define SIG_AD_NAME_MAP_ENTRY "(" SIG_GUID SIG_BUSADDR SIG_NAME_LIST ")"
#define SIG_AD_NAME_MAP       SIG_ARRAY SIG_AD_NAME_MAP_ENTRY
#define SIG_AD_NAMES          SIG_NAME_LIST
#define SIG_FIND_NAMES        SIG_NAME_LIST
#define SIG_NODE_STATE_ENTRY  "(" SIG_GUID SIG_NAME SIG_BUSADDR SIG_AD_NAMES SIG_FIND_NAMES ")"
#define SIG_NODE_STATES       SIG_ARRAY SIG_NODE_STATE_ENTRY

#define SIG_SET_STATE_IN      SIG_MINION_CNT SIG_BUSADDR SIG_NODE_STATES
#define SIG_SET_STATE_OUT     SIG_BUSADDR SIG_NODE_STATES
#define SIG_PROXY_CONN_IN     SIG_BDADDR SIG_PSM
#define SIG_PROXY_CONN_OUT    SIG_STATUS
#define SIG_PROXY_DISC_IN     SIG_BDADDR
#define SIG_PROXY_DISC_OUT    SIG_STATUS
#define SIG_NAME_OP           SIG_BUSADDR SIG_NAME
#define SIG_DELEGATE_AD       SIG_UUIDREV SIG_BDADDR SIG_PSM SIG_AD_NAME_MAP SIG_DURATION
#define SIG_DELEGATE_FIND     SIG_NAME SIG_UUIDREV SIG_BDADDR SIG_DURATION
#define SIG_FOUND_NAMES       SIG_AD_NAME_MAP
#define SIG_FOUND_DEV         SIG_BDADDR SIG_UUIDREV SIG_UUIDREV


const InterfaceDesc btmIfcTable[] = {
    /* Methods */
    { MESSAGE_METHOD_CALL, "SetState",        SIG_SET_STATE_IN,  SIG_SET_STATE_OUT,  "minionCnt,busAddr,routedAddrs,nodeStates,busAddr,routedAddrs,nodeStates" },

    /* Signals */
    { MESSAGE_SIGNAL, "FindName",            SIG_NAME_OP,       NULL, "requestorAddr,requestorPSM,findName" },
    { MESSAGE_SIGNAL, "CancelFindName",      SIG_NAME_OP,       NULL, "requestorAddr,requestorPSM,findName" },
    { MESSAGE_SIGNAL, "AdvertiseName",       SIG_NAME_OP,       NULL, "requestorAddr,requestorPSM,adName" },
    { MESSAGE_SIGNAL, "CancelAdvertiseName", SIG_NAME_OP,       NULL, "requestorAddr,requestorPSM,adName" },
    { MESSAGE_SIGNAL, "DelegateAdvertise",   SIG_DELEGATE_AD,   NULL, "uuidRev,bdAddr,psm,adNames" },
    { MESSAGE_SIGNAL, "DelegateFind",        SIG_DELEGATE_FIND, NULL, "resultDest,ignoreUUIDRev,bdAddr,duration" },
    { MESSAGE_SIGNAL, "FoundNames",          SIG_FOUND_NAMES,   NULL, "adNamesTable" },
    { MESSAGE_SIGNAL, "LostNames",           SIG_FOUND_NAMES,   NULL, "adNamesTable" },
    { MESSAGE_SIGNAL, "FoundDevice",         SIG_FOUND_DEV,     NULL, "bdAddr,newUUIDRev,oldUUIDRev" },
    { MESSAGE_SIGNAL, "LostDevice",          SIG_FOUND_DEV,     NULL, "bdAddr,newUUIDRev,oldUUIDRev" }
};


BTController::BTController(BusAttachment& bus, BluetoothDeviceInterface& bt) :
    BusObject(bus, bluetoothObjPath),
    bus(bus),
    bt(bt),
    master(NULL),
    masterUUIDRev(INVALID_UUIDREV),
    directMinions(0),
    maxConnections(min(StringToU32(Environ::GetAppEnviron()->Find("ALLJOYN_MAX_BT_CONNECTIONS"), 0, DEFAULT_MAX_CONNECTIONS),
                       ABSOLUTE_MAX_CONNECTIONS)),
    listening(false),
    advertise(*this, bus.GetInternal().GetDispatcher()),
    find(*this, bus.GetInternal().GetDispatcher())
{
    while (masterUUIDRev == INVALID_UUIDREV) {
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
    org.alljoyn.Bus.BTController.LostDevice =          ifc->GetMember("LostDevice");

    advertise.delegateSignal = org.alljoyn.Bus.BTController.DelegateAdvertise;
    find.delegateSignal = org.alljoyn.Bus.BTController.DelegateFind;

    static_cast<DaemonRouter&>(bus.GetInternal().GetRouter()).AddBusNameListener(this);

    // Setup the BT node info for our self.
    self->guid = bus.GetGlobalGUIDString();
    self->directMinion = false;
    nodeDB.AddNode(self);
}


BTController::~BTController()
{
    // Don't need to remove our bus name change listener from the router (name
    // table) since the router is already destroyed at this point in time.

    bus.DeregisterBusObject(*this);
    if (master) {
        delete master;
    }
}


void BTController::ObjectRegistered() {
    // Set our unique name now that we know it.
    self->uniqueName = bus.GetUniqueName();
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
        { org.alljoyn.Bus.BTController.DelegateAdvertise,   SignalHandler(&BTController::HandleDelegateAdvertise) },
        { org.alljoyn.Bus.BTController.DelegateFind,        SignalHandler(&BTController::HandleDelegateFind) },
        { org.alljoyn.Bus.BTController.FoundNames,          SignalHandler(&BTController::HandleFoundNamesChange) },
        { org.alljoyn.Bus.BTController.LostNames,           SignalHandler(&BTController::HandleFoundNamesChange) },
        { org.alljoyn.Bus.BTController.FoundDevice,         SignalHandler(&BTController::HandleFoundDeviceChange) },
        { org.alljoyn.Bus.BTController.LostDevice,          SignalHandler(&BTController::HandleFoundDeviceChange) }
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

    QStatus status;
    vector<MsgArg> nodeStateArgsStorage;
    MsgArg* nodeStateArgs;
    MsgArg args[4];
    size_t numArgs = ArraySize(args);
    Message reply(bus);
    ProxyBusObject* newMaster = new ProxyBusObject(bus, busName.c_str(), bluetoothObjPath, 0);

    newMaster->AddInterface(*org.alljoyn.Bus.BTController.interface);

    QCC_DbgPrintf(("SendSetState prep args"));
    nodeDB.FillNodeStateMsgArgs(nodeStateArgsStorage);

    nodeStateArgs = &nodeStateArgsStorage.front();
    status = MsgArg::Set(args, numArgs, SIG_SET_STATE_IN,
                         directMinions,
                         self->nodeAddr.addr.GetRaw(),
                         self->nodeAddr.psm,
                         nodeStateArgsStorage.size(), nodeStateArgs);
    if (status != ER_OK) {
        goto exit;
    }

    status = newMaster->MethodCall(*org.alljoyn.Bus.BTController.SetState, args, ArraySize(args), reply);

    if (status == ER_OK) {
        size_t numNodeStateArgs;
        uint64_t rawBDAddr;
        uint16_t psm;

        status = reply->GetArgs(SIG_SET_STATE_OUT,
                                &rawBDAddr,
                                &psm,
                                &numNodeStateArgs, &nodeStateArgs);
        if (status != ER_OK) {
            goto exit;
        }

        BTBusAddress addr(rawBDAddr, psm);

        lock.Lock();
        if (numNodeStateArgs == 0) {
            // We are now a minion (or a drone if we have more than one direct connection)
            master = newMaster;
            masterNode->nodeAddr = addr;
            masterNode->uniqueName = master->GetServiceName();
        } else {
            // We are the still the master
            ImportState(nodeStateArgs, numNodeStateArgs, addr);

            delete newMaster;

            assert(find.resultDest.empty());
            find.dirty = true;
        }

        QCC_DbgPrintf(("We are %s, %s is now our %s",
                       IsMaster() ? "still the master" : (IsDrone() ? "now a drone" : "just a minion"),
                       busName.c_str(), IsMaster() ? "minion" : "master"));

        if (!IsMinion()) {
            UpdateDelegations(advertise, listening);
            UpdateDelegations(find);
            QCC_DEBUG_ONLY(DumpNodeStateTable());
        }

        if (!IsMaster()) {
            bus.GetInternal().GetDispatcher().RemoveAlarm(stopAd);
        }
        lock.Unlock();
    }

exit:

    return status;
}


QStatus BTController::AddAdvertiseName(const qcc::String& name)
{
    QStatus status = DoNameOp(name, *org.alljoyn.Bus.BTController.AdvertiseName, true, advertise);

    if (IsMaster() && (status == ER_OK)) {
        BTNodeDB newAdInfo;
        BTNodeDB oldAdInfo;
        BTNodeInfo node(self->guid, self->uniqueName, self->nodeAddr);
        self->AddAdvertiseName(name);
        node->AddAdvertiseName(name);
        newAdInfo.AddNode(node);
        status = DistributeAdvertisedNameChanges(newAdInfo, oldAdInfo);
    }

    return status;
}


QStatus BTController::RemoveAdvertiseName(const qcc::String& name)
{
    QStatus status = DoNameOp(name, *org.alljoyn.Bus.BTController.CancelAdvertiseName, false, advertise);

    if (IsMaster() && (status == ER_OK)) {
        BTNodeDB newAdInfo;
        BTNodeDB oldAdInfo;
        BTNodeInfo node(self->guid, self->uniqueName, self->nodeAddr);
        self->RemoveAdvertiseName(name);
        node->AddAdvertiseName(name);  // Yes 'Add' the name being removed (it goes in the old ad info).
        oldAdInfo.AddNode(node);
        status = DistributeAdvertisedNameChanges(newAdInfo, oldAdInfo);
    }

    return status;
}


void BTController::ProcessDeviceChange(const BDAddress& adBdAddr,
                                       uint32_t newUUIDRev,
                                       uint32_t oldUUIDRev,
                                       bool lost)
{
    QCC_DbgTrace(("BTController::ProcessDeviceChange(adBdAddr = %s, newUUIDRev = %08x, oldUUIDRev = %08x, <%s>)",
                  adBdAddr.ToString().c_str(), newUUIDRev, oldUUIDRev, lost ? "lost" : "found/changed"));
    QStatus status = ER_OK;


    if (IsMaster()) {
        BTNodeDB empty;
        if (lost) {
            cacheLock.Lock();
            UUIDRevCacheMap::iterator ci = uuidRevCache.lower_bound(oldUUIDRev);
            UUIDRevCacheMap::const_iterator ciEnd = uuidRevCache.upper_bound(oldUUIDRev);

            BTNodeDB* oldAdInfo = NULL;

            while ((ci != ciEnd) && (ci->second.adAddr != adBdAddr)) {
                ++ci;
            }

            if (ci != ciEnd) {
                oldAdInfo = ci->second.adInfo;
                uuidRevCache.erase(ci);
            }
            cacheLock.Unlock();

            if (oldAdInfo) {
                DistributeAdvertisedNameChanges(empty, *oldAdInfo);
                delete oldAdInfo;
            }
        } else {
            BTNodeDB* adInfo = new BTNodeDB();
            BTNodeDB* newAdInfo;
            BTNodeDB* oldAdInfo;
            BDAddress connBDAddr;
            uint16_t connPSM;

            status = bt.GetDeviceInfo(adBdAddr, newUUIDRev, connBDAddr, connPSM, *adInfo);

            if (status == ER_OK) {
                BTBusAddress connAddr(connBDAddr, connPSM);

                cacheLock.Lock();
                UUIDRevCacheMap::iterator ci = uuidRevCache.lower_bound(oldUUIDRev);
                UUIDRevCacheMap::const_iterator ciEnd = uuidRevCache.upper_bound(oldUUIDRev);

                while ((ci != ciEnd) && (ci->second.connAddr != connAddr)) {
                    ++ci;
                }

                if (ci == ciEnd) {
                    newAdInfo = adInfo;
                    oldAdInfo = &empty;
                } else {
                    newAdInfo = new BTNodeDB();
                    oldAdInfo = new BTNodeDB();
                    ci->second.adInfo->Diff(*adInfo, newAdInfo, oldAdInfo);
                    delete ci->second.adInfo;
                    uuidRevCache.erase(ci);
                }
                uuidRevCache.insert(pair<uint32_t, UUIDRevCacheInfo>(newUUIDRev, UUIDRevCacheInfo(adBdAddr, newUUIDRev, connAddr, adInfo)));
                cacheLock.Unlock();

                foundNodeDB.UpdateDB(newAdInfo, oldAdInfo);
                DistributeAdvertisedNameChanges(*newAdInfo, *oldAdInfo);

                // newAdInfo is either temporarily on the heap or the same as adInfo
                if (newAdInfo != adInfo) {
                    delete newAdInfo;
                }
                // oldAdInfo is either temporarily on the heap or pointing to empty
                if (oldAdInfo != &empty) {
                    delete oldAdInfo;
                }
            }
        }
    } else {
        // Must be a drone or slave.
        MsgArg args[3];
        size_t numArgs = ArraySize(args);

        status = MsgArg::Set(args, numArgs, SIG_FOUND_DEV, adBdAddr.GetRaw(), newUUIDRev, oldUUIDRev);
        if (status != ER_OK) {
            QCC_LogError(status, ("MsgArg::Set(args = <>, numArgs = %u, %s, %s, %08x, %08x) failed",
                                  numArgs, SIG_FOUND_DEV, adBdAddr.ToString().c_str(), newUUIDRev, oldUUIDRev));
            return;
        }

        lock.Lock();
        if (lost) {
            Signal(find.resultDest.c_str(), 0, *org.alljoyn.Bus.BTController.LostDevice, args, numArgs);
        } else {
            Signal(find.resultDest.c_str(), 0, *org.alljoyn.Bus.BTController.FoundDevice, args, numArgs);
        }
        lock.Unlock();
    }
}


void BTController::PrepConnect()
{
    lock.Lock();
    if (UseLocalFind()) {
        /*
         * Gotta shut down the local find operation since a successful
         * connection will cause the exchange of the SetState method call and
         * response which will result in one side or the other taking control
         * of who performs the find operation.
         */
        if (!find.Empty()) {
            QCC_DbgPrintf(("Stopping find..."));
            assert(find.active);
            bt.StopFind();
            find.active = false;
        }
    }
    if (UseLocalAdvertise() && advertise.active) {
        /*
         * Gotta shut down the local advertise operation since a successful
         * connection will cause the exchange for the SetState method call and
         * response which will result in one side or the other taking control
         * of who performs the advertise operation.
         */
        if (!advertise.Empty()) {
            QCC_DbgPrintf(("Stopping advertise..."));
            bt.StopAdvertise();
            advertise.active = false;
        }
    }

    // Unlocks in BTController::PostConnect()
}


void BTController::PostConnect(QStatus status, const RemoteEndpoint* ep)
{
    // Assumes lock acquired in BTController::PrepConnect()

    if (status == ER_OK) {
        assert(ep);
        BTBusAddress addr = reinterpret_cast<const BTEndpoint*>(ep)->GetBTBusAddress();
        if (!master && !nodeDB.FindNode(addr)->IsValid()) {
            /* Only call SendSetState for new outgoing connections.  If we
             * have a master then the connection can't be new.  If we are the
             * master then there should be node device with the same bus
             * address as the endpoint.
             */
            SendSetState(ep->GetRemoteName());
        }
    } else {
        if (UseLocalFind()) {
            /*
             * Gotta restart the find operation since the connect failed and
             * we need to do the find for ourself.
             */
            if (!find.Empty()) {
                QCC_DbgPrintf(("Starting find..."));
                assert(!find.active);
                bt.StartFind(masterUUIDRev);
                find.active = true;
            }
        }
        if (UseLocalAdvertise()) {
            /*
             * Gotta restart the advertise operation since the connect failed and
             * we need to do the advertise for ourself.
             */
            if (!advertise.Empty()) {
                BTNodeDB adInfo;
                status = ExtractAdInfo(&advertise.adInfoArgs.front(), advertise.adInfoArgs.size(), adInfo);
                QCC_DbgPrintf(("Starting advertise..."));
                assert(!advertise.active);
                bt.StartAdvertise(masterUUIDRev, self->nodeAddr.addr, self->nodeAddr.psm, adInfo);
                advertise.active = true;
            }
        }
    }
    lock.Unlock();
}


void BTController::BTDeviceAvailable(bool on)
{
    QCC_DbgPrintf(("BTController::BTDevicePower(<%s>)", on ? "on" : "off"));
    if (IsMaster()) {
        lock.Lock();
        if (on) {
            QStatus status = bt.StartListen(self->nodeAddr.addr, self->nodeAddr.psm);
            listening = (status == ER_OK);
            if (listening) {
                find.ignoreAddr = self->nodeAddr.addr;
                find.ignoreUUID = masterUUIDRev;
                find.dirty = true;
            }

            UpdateDelegations(advertise, listening);
            UpdateDelegations(find);
            QCC_DEBUG_ONLY(DumpNodeStateTable());
        } else {
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
            if (listening) {
                bt.StopListen();
                listening = false;
            }
        }
        lock.Unlock();
    }
    devAvailable = on;
}


bool BTController::CheckIncomingAddress(const BDAddress& addr) const
{
    if (IsMaster()) {
        return true;
    } else if (addr == masterNode->nodeAddr.addr) {
        return true;
    } else if (IsDrone()) {
        const BTNodeInfo& node = nodeDB.FindNode(addr);
        return node->IsValid() && node->directMinion;
    }
    return false;
}


void BTController::NameOwnerChanged(const qcc::String& alias,
                                    const qcc::String* oldOwner,
                                    const qcc::String* newOwner)
{
    if (oldOwner && (alias == *oldOwner)) {
        // An endpoint left the bus.

        lock.Lock();
        if (master && (master->GetServiceName() == alias)) {
            QCC_DbgPrintf(("Our master left us: %s", master->GetServiceName().c_str()));
            // We are the master now.
            if (IsMinion()) {
                if (find.active) {
                    QCC_DbgPrintf(("Stopping find..."));
                    bt.StopFind();
                    find.active = false;
                }
                if (advertise.active) {
                    QCC_DbgPrintf(("Stopping advertise..."));
                    bt.StopAdvertise();
                    advertise.active = false;
                }
            }

            delete master;
            master = NULL;

            find.resultDest.clear();
            find.ignoreAddr = self->nodeAddr.addr;
            find.ignoreUUID = masterUUIDRev;

            UpdateDelegations(advertise, listening);
            UpdateDelegations(find);
            QCC_DEBUG_ONLY(DumpNodeStateTable());

        } else {
            // Someone else left.  If it was a minion node, remove their find/ad names.
            BTNodeInfo minion = nodeDB.FindNode(alias);

            if (minion->uniqueName == alias) {
                // Remove the minion's state information.
                nodeDB.RemoveNode(minion);
                QCC_DbgPrintf(("One of our minions left us: %s", minion->uniqueName.c_str()));

                bool wasAdvertiseMinion = minion == advertise.minion;
                bool wasFindMinion = minion == find.minion;
                bool wasDirect = minion->directMinion;

                // Advertise minion and find minion should never be the same.
                assert(!(wasAdvertiseMinion && wasFindMinion));

                // Indicate name list has changed.
                advertise.dirty = !minion->AdvertiseNamesEmpty();
                find.dirty = !minion->FindNamesEmpty();

                // Remove find names for the lost minion.
                for (NameSet::const_iterator it = minion->GetFindNamesBegin(); it != minion->GetFindNamesEnd(); ++it) {
                    find.names.erase(*it);
                }

                if (wasDirect) {
                    --directMinions;

                    if (wasFindMinion) {
                        find.active = false;
                        if (directMinions == 0) {
                            // Our only minion was finding for us.  We'll have to
                            // find for ourself now.
                            find.minion = self;
                            QCC_DbgPrintf(("Selected ourself as find minion."));
                        } else {
                            if (directMinions == 1) {
                                // We had 2 minions.  The one that was finding
                                // for us left, so now we must advertise for
                                // ourself and tell our remaining minion to
                                // find for us.
                                advertise.active = false;
                                advertise.ClearArgs();
                                Signal(advertise.minion->uniqueName.c_str(), 0, *advertise.delegateSignal, advertise.args, advertise.argsSize);
                                advertise.minion = self;
                            }

                            NextDirectMinion(find.minion);
                            QCC_DbgPrintf(("Selected %s as our find minion.", find.minion->uniqueName.c_str()));
                        }
                    }

                    if (wasAdvertiseMinion) {
                        assert(directMinions != 0);
                        advertise.active = false;

                        if (directMinions == 1) {
                            // We had 2 minions. The one that was advertising
                            // for us left, so now we must advertise for
                            // ourself.
                            advertise.active = false;
                            advertise.minion = self;
                            QCC_DbgPrintf(("Selected ourself as advertise minion."));
                        } else {
                            // We had more than 2 minions, so at least one is
                            // idle.  Select the next available minion and to
                            // do the advertising for us.
                            NextDirectMinion(advertise.minion);
                            QCC_DbgPrintf(("Selected %s as our advertise minion.", advertise.minion->uniqueName.c_str()));
                        }
                    }
                }

                UpdateDelegations(advertise, listening);
                UpdateDelegations(find);
                QCC_DEBUG_ONLY(DumpNodeStateTable());
            }
        }
        lock.Unlock();
    }
}


QStatus BTController::DistributeAdvertisedNameChanges(const BTNodeDB& newAdInfo,
                                                      const BTNodeDB& oldAdInfo)
{
    QCC_DbgTrace(("BTController::DistributeAdvertisedNameChanges(newAdInfo = <%u nodes>, oldAdInfo = <%u nodes>",
                  newAdInfo.Size(), oldAdInfo.Size()));
    QStatus status = ER_OK;

    // Now inform everyone of the changes in advertised names.
    lock.Lock();
    for (BTNodeDB::const_iterator it = nodeDB.Begin(); it != nodeDB.End(); ++it) {
        const BTNodeInfo& node = *it;
        if (!node->FindNamesEmpty() && node->IsMinionOf(self)) {
            QCC_DbgPrintf(("Notify %s-%04x of the names.", node->nodeAddr.addr.ToString().c_str(), node->nodeAddr.psm));
            if (oldAdInfo.Size() > 0) {
                status = SendFoundNamesChange(node->uniqueName, oldAdInfo, true);
            }
            if (newAdInfo.Size() > 0) {
                status = SendFoundNamesChange(node->uniqueName, newAdInfo, false);
            }
        }
    }
    lock.Unlock();

    if (!self->FindNamesEmpty()) {
        QCC_DbgPrintf(("Notify ourself of the names."));
        BTNodeDB::const_iterator node;
        // Tell ourself about the names (This is best done outside the noseStateLock just in case).
        for (node = oldAdInfo.Begin(); node != oldAdInfo.End(); ++node) {
            if (((*node)->AdvertiseNamesSize() > 0) && (*node != self)) {
                vector<String> vectorizedNames;
                vectorizedNames.reserve((*node)->AdvertiseNamesSize());
                vectorizedNames.assign((*node)->GetAdvertiseNamesBegin(), (*node)->GetAdvertiseNamesEnd());
                bt.FoundNamesChange((*node)->guid, vectorizedNames, (*node)->nodeAddr.addr, (*node)->nodeAddr.psm, true);
            }
        }
        for (node = newAdInfo.Begin(); node != newAdInfo.End(); ++node) {
            if (((*node)->AdvertiseNamesSize() > 0) && (*node != self)) {
                vector<String> vectorizedNames;
                vectorizedNames.reserve((*node)->AdvertiseNamesSize());
                vectorizedNames.assign((*node)->GetAdvertiseNamesBegin(), (*node)->GetAdvertiseNamesEnd());
                bt.FoundNamesChange((*node)->guid, vectorizedNames, (*node)->nodeAddr.addr, (*node)->nodeAddr.psm, false);
            }
        }
    }

    return status;
}


QStatus BTController::SendFoundNamesChange(const String& dest,
                                           const BTNodeDB& adInfo,
                                           bool lost)
{
    QCC_DbgTrace(("BTController::SendFoundNamesChange(dest = \"%s\", adInfo = <>, <%s>)",
                  dest.c_str(), lost ? "lost" : "found/changed"));

    MsgArg args[4];
    size_t argsSize = ArraySize(args);
    vector<MsgArg> nodeList;

    BTNodeDB::const_iterator it;

    nodeList.reserve(adInfo.Size());

    for (it = adInfo.Begin(); it != adInfo.End(); ++it) {
        const BTNodeInfo& node = *it;
        if (node->uniqueName != dest) {
            vector<const char*> nameList;
            NameSet::const_iterator name;
            nameList.reserve(node->AdvertiseNamesSize());
            QCC_DbgPrintf(("Encoding %u advertise names for %s:", node->AdvertiseNamesSize(), node->guid.c_str()));
            for (name = node->GetAdvertiseNamesBegin(); name != node->GetAdvertiseNamesEnd(); ++name) {
                QCC_DbgPrintf(("    %s", name->c_str()));
                nameList.push_back(name->c_str());
            }
            nodeList.push_back(MsgArg(SIG_AD_NAME_MAP_ENTRY,
                                      node->guid.c_str(),
                                      node->nodeAddr.addr.GetRaw(),
                                      node->nodeAddr.psm,
                                      nameList.size(), &nameList.front()));
        }
    }

    QStatus status = MsgArg::Set(args, argsSize, SIG_FOUND_NAMES, nodeList.size(), &nodeList.front());
    if (status == ER_OK) {
        if (lost) {
            status = Signal(dest.c_str(), 0, *org.alljoyn.Bus.BTController.LostNames, args, ArraySize(args));
        } else {
            status = Signal(dest.c_str(), 0, *org.alljoyn.Bus.BTController.FoundNames, args, ArraySize(args));
        }
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

    lock.Lock();
    if (add) {
        nameArgInfo.AddName(name, self);
    } else {
        nameArgInfo.RemoveName(name, self);
    }

    nameArgInfo.dirty = true;

    if (devAvailable) {
        if (IsMaster()) {
            QCC_DbgPrintf(("Handling %s locally (we're the master)", signal.name.c_str()));

            UpdateDelegations(advertise, listening);
            UpdateDelegations(find);
            QCC_DEBUG_ONLY(DumpNodeStateTable());

        } else {
            QCC_DbgPrintf(("Sending %s to our master: %s", signal.name.c_str(), master->GetServiceName().c_str()));
            MsgArg args[2];
            args[0].Set(SIG_NAME, bus.GetUniqueName().c_str());
            args[1].Set(SIG_NAME, name.c_str());
            status = Signal(master->GetServiceName().c_str(), 0, signal, args, ArraySize(args));
        }
    }
    lock.Unlock();

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
    char* name;
    uint64_t addrRaw;
    uint16_t psm;

    QStatus status = msg->GetArgs(SIG_NAME_OP, &addrRaw, &psm, &name);

    if (status == ER_OK) {
        BTNodeInfo node = nodeDB.FindNode(addrRaw, psm);

        if (node->IsValid()) {
            QCC_DbgPrintf(("%s %s to the list of %s names for %s.",
                           addName ? "Adding" : "Removing",
                           name,
                           findOp ? "find" : "advertise",
                           node->uniqueName.c_str()));



            // All nodes need to be registered via SetState
            qcc::String n(name);
            lock.Lock();
            if (addName) {
                nameCollection->AddName(n, node);
            } else {
                nameCollection->RemoveName(n, node);
            }

            if (IsMaster()) {
                UpdateDelegations(advertise, listening);
                UpdateDelegations(find);
                QCC_DEBUG_ONLY(DumpNodeStateTable());

                if (!findOp) {
                    BTNodeDB newAdInfo;
                    BTNodeDB oldAdInfo;
                    BTNodeInfo nodeChange(node->guid, node->uniqueName, node->nodeAddr);
                    if (addName) {
                        newAdInfo.AddNode(nodeChange);
                    } else {
                        oldAdInfo.AddNode(nodeChange);
                    }
                    DistributeAdvertisedNameChanges(newAdInfo, oldAdInfo);
                }
            } else {
                // We are a drone so pass on the name
                const MsgArg* args;
                size_t numArgs;
                msg->GetArgs(numArgs, args);
                Signal(master->GetServiceName().c_str(), 0, *member, args, numArgs);
            }
            lock.Unlock();
        }
    }
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
    qcc::String sender = msg->GetSender();
    QStatus status;

    lock.Lock();
    if (UseLocalFind() && find.active) {
        QCC_DbgPrintf(("Stopping find..."));
        bt.StopFind();
        find.active = false;
    }

    /*
     * Only get the number of direct connections first.  If the other guy has
     * more direct connections then there is no point in extracting all of his
     * names from the second argument.
     */
    status = msg->GetArg(0)->Get(SIG_MINION_CNT, &numConnections);
    if (status != ER_OK) {
        MethodReply(msg, "org.alljoyn.Bus.BTController.InternalError", QCC_StatusText(status));
        return;
    }

    MsgArg* nodeStateArgs;
    MsgArg args[3];
    size_t numArgs = ArraySize(args);

    if (numConnections > directMinions) {
        // We are now a minion (or a drone if we have more than one direct connection)
        master = new ProxyBusObject(bus, sender.c_str(), bluetoothObjPath, 0);

        vector<MsgArg> nodeStateArgsStorage;

        nodeDB.FillNodeStateMsgArgs(nodeStateArgsStorage);
        nodeStateArgs = &nodeStateArgsStorage.front();

        status = MsgArg::Set(args, numArgs, SIG_SET_STATE_OUT,
                             self->nodeAddr.addr.GetRaw(),
                             self->nodeAddr.psm,
                             nodeStateArgsStorage.size(), nodeStateArgs);

    } else {
        // We are still the master
        size_t numNodeStateArgs;
        uint64_t rawBDAddr;
        uint16_t psm;

        status = msg->GetArg(1)->Get(SIG_BDADDR, &rawBDAddr);
        status = msg->GetArg(2)->Get(SIG_PSM, &psm);
        status = msg->GetArg(3)->Get(SIG_NODE_STATES, &numNodeStateArgs, &nodeStateArgs);
        if (status != ER_OK) {
            MethodReply(msg, "org.alljoyn.Bus.BTController.InternalError", QCC_StatusText(status));
            return;
        }

        BTBusAddress addr(rawBDAddr, psm);
        ImportState(nodeStateArgs, numNodeStateArgs, addr);

        assert(find.resultDest.empty());
        find.dirty = true;

        status = MsgArg::Set(args, numArgs, SIG_SET_STATE_OUT,
                             self->nodeAddr.addr.GetRaw(),
                             self->nodeAddr.psm,
                             0, NULL);
    }

    status = MethodReply(msg, args, numArgs);
    if (status != ER_OK) {
        QCC_LogError(status, ("MethodReply"));
    }

    QCC_DbgPrintf(("We are %s, %s is now our %s",
                   IsMaster() ? "still the master" : (IsDrone() ? "now a drone" : "just a minion"),
                   sender.c_str(), IsMaster() ? "minion" : "master"));

    if (!IsMinion()) {
        UpdateDelegations(advertise, listening);
        UpdateDelegations(find);
        QCC_DEBUG_ONLY(DumpNodeStateTable());
    }

    if (!IsMaster()) {
        bus.GetInternal().GetDispatcher().RemoveAlarm(stopAd);
    }
    lock.Unlock();
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

    lock.Lock();
    if (IsMinion()) {
        char* resultDest;
        uint64_t bdAddrRaw;
        uint32_t ignoreUUID;
        uint32_t duration;

        QStatus status = msg->GetArgs(SIG_DELEGATE_FIND, &resultDest, &ignoreUUID, &bdAddrRaw, &duration);

        if (status == ER_OK) {
            QCC_DbgPrintf(("Delegated Find: result dest: \"%s\"  ignore UUIDRev: %08x  ignore BDAddr: %012llx",
                           resultDest, ignoreUUID, bdAddrRaw));

            if (resultDest && (resultDest[0] != '\0')) {
                find.resultDest = resultDest;
                find.ignoreAddr.SetRaw(bdAddrRaw);
                find.ignoreUUID = ignoreUUID;
                find.dirty = true;

                QCC_DbgPrintf(("Starting find for %u seconds...", duration));
                bt.StartFind(ignoreUUID, duration);
                find.active = true;
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
        QCC_DbgPrintf(("Selected %s as our find minion.", find.minion->uniqueName.c_str()));

        Signal(find.minion->uniqueName.c_str(), 0, *find.delegateSignal, args, numArgs);
    }
    lock.Unlock();
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

    lock.Lock();
    if (IsMinion()) {
        uint32_t uuidRev;
        uint64_t bdAddrRaw;
        uint16_t psm;
        BTNodeDB adInfo;
        MsgArg* entries;
        size_t size;

        QStatus status = msg->GetArgs(SIG_DELEGATE_AD, &uuidRev, &bdAddrRaw, &psm, &size, &entries);

        if (status == ER_OK) {
            status = ExtractAdInfo(entries, size, adInfo);
        }

        if (status == ER_OK) {
            if (adInfo.Size() > 0) {
                BDAddress bdAddr(bdAddrRaw);

                QCC_DbgPrintf(("Starting advertise..."));
                bt.StartAdvertise(uuidRev, bdAddr, psm, adInfo, DELEGATE_TIME);
                advertise.active = true;
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
        QCC_DbgPrintf(("Selected %s as our advertise minion.", advertise.minion->uniqueName.c_str()));

        Signal(advertise.minion->uniqueName.c_str(), 0, *advertise.delegateSignal, args, numArgs);
    }
    lock.Unlock();
}


void BTController::HandleFoundNamesChange(const InterfaceDescription::Member* member,
                                          const char* sourcePath,
                                          Message& msg)
{
    QCC_DbgTrace(("BTController::HandleFoundNamesChange(member = %s, sourcePath = \"%s\", msg = <>)",
                  member->name.c_str(), sourcePath));
    if (IsMaster() || (strcmp(sourcePath, bluetoothObjPath) != 0)) {
        // We only accept FoundNames signals if we are not the master!
        return;
    }

    BTNodeDB adInfo;
    uint64_t addrRaw;
    uint16_t psm;
    bool lost = (member == org.alljoyn.Bus.BTController.LostNames);
    MsgArg* entries;
    size_t size;

    QStatus status = msg->GetArgs(SIG_FOUND_NAMES, &size, &entries, &addrRaw, &psm);

    if (status == ER_OK) {
        status = ExtractAdInfo(entries, size, adInfo);
    }

    if ((status == ER_OK) && (adInfo.Size() > 0)) {
        BDAddress bdAddr(addrRaw);

        BTNodeDB::const_iterator node;

        lock.Lock();
        for (node = adInfo.Begin(); node != adInfo.End(); ++node) {
            vector<String> vectorizedNames;
            vectorizedNames.reserve((*node)->AdvertiseNamesSize());
            vectorizedNames.assign((*node)->GetAdvertiseNamesBegin(), (*node)->GetAdvertiseNamesEnd());
            bt.FoundNamesChange((*node)->uniqueName, vectorizedNames, bdAddr, psm, lost);
        }
        lock.Unlock();
    }
}


void BTController::HandleFoundDeviceChange(const InterfaceDescription::Member* member,
                                           const char* sourcePath,
                                           Message& msg)
{
    QCC_DbgTrace(("BTController::HandleFoundDeviceChange(member = %s, sourcePath = \"%s\", msg = <>)",
                  member->name.c_str(), sourcePath));

    if (nodeDB.FindNode(msg->GetSender())->directMinion) {
        // We only handle FoundDevice or LostDevice signals from our minions.
        return;
    }

    uint32_t newUUIDRev;
    uint32_t oldUUIDRev;
    uint64_t adBdAddrRaw;
    bool lost = (member == org.alljoyn.Bus.BTController.LostDevice);

    QStatus status = msg->GetArgs(SIG_FOUND_DEV, &adBdAddrRaw, &newUUIDRev, &oldUUIDRev);

    if (status == ER_OK) {
        BDAddress adBdAddr(adBdAddrRaw);

        ProcessDeviceChange(adBdAddr, newUUIDRev, oldUUIDRev, lost);
    }
}


void BTController::ImportState(MsgArg* nodeStateEntries,
                               size_t numNodeStates,
                               const BTBusAddress& addr)
{
    QCC_DbgTrace(("BTController::ImportState(nodeStateEntries = <>, numNodeStates = %u, addr = (%s, %04x) )",
                  numNodeStates, addr.addr.ToString().c_str(), addr.psm));

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
     * the latest and greatest information via ImportState().  We need to
     * determine if and what those changes are then tell our existing
     * connected minions.
     */

    size_t i;

    BTNodeDB incomingDB;
    BTNodeDB added;
    BTNodeDB removed;
    BTNodeInfo node;
    BTNodeInfo connectingNode;
    connectingNode->nodeAddr = addr;
    connectingNode->directMinion = true;
    connectingNode->SetConnectNode(self);

    for (i = 0; i < numNodeStates; ++i) {
        char* bn;
        char* guidStr;
        uint64_t rawBdAddr;
        uint16_t psm;
        size_t anSize, fnSize;
        MsgArg* anList;
        MsgArg* fnList;
        size_t j;
        nodeStateEntries[i].Get(SIG_NODE_STATE_ENTRY, &guidStr, &bn, &rawBdAddr, &psm, &anSize, &anList, &fnSize, &fnList);
        QCC_DbgPrintf(("Processing names for %s:", bn));

        String busName(bn);
        BTBusAddress nodeAddr(BDAddress(rawBdAddr), psm);
        String guid(guidStr);
        if (nodeAddr == connectingNode->nodeAddr) {
            node = connectingNode;
            node->guid = guid;
            node->uniqueName = busName;
        } else {
            node = BTNodeInfo(guid, busName, nodeAddr);
            node->SetConnectNode(connectingNode);
        }

        advertise.dirty = advertise.dirty || (anSize > 0);
        find.dirty = find.dirty || (fnSize > 0);

        for (j = 0; j < anSize; ++j) {
            char* n;
            anList[j].Get(SIG_NAME, &n);
            QCC_DbgPrintf(("    Ad Name: %s", n));
            qcc::String name(n);
            advertise.AddName(name, node);
        }

        for (j = 0; j < fnSize; ++j) {
            char* n;
            fnList[j].Get(SIG_NAME, &n);
            QCC_DbgPrintf(("    Find Name: %s", n));
            qcc::String name(n);
            find.AddName(name, node);
        }

        incomingDB.AddNode(node);
        nodeDB.AddNode(node);
    }

    foundNodeDB.Diff(incomingDB, &added, &removed);
    foundNodeDB.UpdateDB(&added, &removed);

    DistributeAdvertisedNameChanges(added, removed);

    ++directMinions;

    if (find.minion == self) {
        NextDirectMinion(find.minion);
        QCC_DbgPrintf(("Selected %s as our find minion.", find.minion->uniqueName.c_str()));
    }

    if ((advertise.minion == self) && (!UseLocalAdvertise())) {
        NextDirectMinion(advertise.minion);
        QCC_DbgPrintf(("Selected %s as our advertise minion.", advertise.minion->uniqueName.c_str()));
    }
}


void BTController::UpdateDelegations(NameArgInfo& nameInfo, bool allow)
{
    const bool advertiseOp = (&nameInfo == &advertise);

    QCC_DbgTrace(("BTController::UpdateDelegations(nameInfo = <%s>, allow = %s)",
                  advertiseOp ? "advertise" : "find", allow ? "true" : "false"));

    const bool allowConn = allow && IsMaster() && (directMinions < maxConnections);
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
        if (masterUUIDRev == INVALID_UUIDREV) {
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
                BTBusAddress addr(rawAddr, psm);
                BTNodeInfo node(guidStr, guidStr, addr);

                QCC_DbgPrintf(("Extracting %u advertise names for %s:", numNames, guid));
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


void BTController::AlarmTriggered(const Alarm& alarm, QStatus reason)
{
    QCC_DbgTrace(("BTController::AlarmTriggered(alarm = <>, reasons = %s)", QCC_StatusText(reason)));
    assert(IsMaster());

    if (reason == ER_OK) {
        lock.Lock();
        if (advertise.Empty()) {
            if (UseLocalAdvertise()) {
                QCC_DbgPrintf(("Stopping advertise..."));
                bt.StopAdvertise();
            } else {
                // Tell the minion to stop advertising (presumably the empty list).
                Signal(advertise.minion->uniqueName.c_str(), 0, *advertise.delegateSignal, advertise.args, advertise.argsSize);
            }
        }
        lock.Unlock();
    }
}


void BTController::NameArgInfo::AlarmTriggered(const Alarm& alarm, QStatus reason)
{
    QCC_DbgTrace(("BTController::NameArgInfo::AlarmTriggered(alarm = <%s>, reason = %s)",
                  alarm == bto.find.alarm ? "find" : "advertise", QCC_StatusText(reason)));

    if (reason == ER_OK) {
        bto.lock.Lock();
        bto.NextDirectMinion(minion);
        QCC_DbgPrintf(("Selected %s as our %s minion.",
                       minion->uniqueName.c_str(),
                       (minion == bto.find.minion) ? "find" : "advertise"));
        bto.Signal(minion->uniqueName.c_str(), 0, *delegateSignal, args, argsSize);
        bto.lock.Unlock();

        // Manually re-arm alarm since automatically recurring alarms cannot be stopped.
        StartAlarm();
    }
}


QStatus BTController::NameArgInfo::SendDelegateSignal()
{
    QCC_DbgPrintf(("Sending %s signal to %s", delegateSignal->name.c_str(),
                   minion->uniqueName.c_str()));
    assert(bto.bus.GetUniqueName() != minion->uniqueName);
    return bto.Signal(minion->uniqueName.c_str(), 0, *delegateSignal, args, argsSize);
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
            if (restart) {
                StopAlarm();
            }
            StartAlarm();
        }
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

    active = !(status == ER_OK);
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
    size_t argsSize = this->argsSize;

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
                                    node->guid.c_str(),
                                    node->nodeAddr.addr.GetRaw(),
                                    node->nodeAddr.psm,
                                    names.size(), &names.front()));
    }

    MsgArg::Set(args, argsSize, SIG_DELEGATE_AD,
                bto.masterUUIDRev,
                bto.self->nodeAddr.addr.GetRaw(),
                bto.self->nodeAddr.psm,
                adInfoArgs.size(), &adInfoArgs.front(),
                bto.RotateMinions() ? DELEGATE_TIME : (uint32_t)0);
    assert(argsSize == this->argsSize);

    dirty = false;
}


void BTController::AdvertiseNameArgInfo::ClearArgs()
{
    QCC_DbgTrace(("BTController::AdvertiseNameArgInfo::ClearArgs()"));
    size_t argsSize = this->argsSize;

    /* Advertise an empty list for a while */
    MsgArg::Set(args, argsSize, SIG_DELEGATE_AD,
                bto.masterUUIDRev,
                bto.self->nodeAddr.addr.GetRaw(),
                bto.self->nodeAddr.psm,
                (size_t)0, NULL,
                (uint32_t)0);
    assert(argsSize == this->argsSize);
}


void BTController::AdvertiseNameArgInfo::StartOp(bool restart)
{
    if (!restart) {
        bto.bus.GetInternal().GetDispatcher().RemoveAlarm(bto.stopAd);
    }

    NameArgInfo::StartOp(restart);
}


void BTController::AdvertiseNameArgInfo::StopOp()
{
    NameArgInfo::StopOp();

    bto.stopAd = Alarm(DELEGATE_TIME * 1000, &bto);
    bto.bus.GetInternal().GetDispatcher().AddAlarm(bto.stopAd);

    // Clear out the advertise arguments (for real this time).
    size_t argsSize = this->argsSize;

    MsgArg::Set(args, argsSize, SIG_DELEGATE_AD,
                INVALID_UUIDREV,
                0ULL,
                BTBusAddress::INVALID_PSM,
                (size_t)0, NULL,
                (uint32_t)0);
    assert(argsSize == this->argsSize);
}


QStatus BTController::AdvertiseNameArgInfo::StartLocal()
{
    QStatus status;
    BTNodeDB adInfo;

    status = ExtractAdInfo(&adInfoArgs.front(), adInfoArgs.size(), adInfo);
    if (status == ER_OK) {
        status = bto.bt.StartAdvertise(bto.masterUUIDRev, bto.self->nodeAddr.addr, bto.self->nodeAddr.psm, adInfo);
    }
    return status;
}


QStatus BTController::AdvertiseNameArgInfo::StopLocal()
{
    // Advertise empty list for a while so others detect lost name quickly.
    BTNodeDB adInfo;
    return bto.bt.StartAdvertise(bto.masterUUIDRev, bto.self->nodeAddr.addr, bto.self->nodeAddr.psm, adInfo);
}


void BTController::FindNameArgInfo::AddName(const qcc::String& name, BTNodeInfo& node)
{
    node->AddFindName(name);
    names.insert(name);
    dirty = true;
}


void BTController::FindNameArgInfo::RemoveName(const qcc::String& name, BTNodeInfo& node)
{
    node->RemoveFindName(name);
    names.erase(name);
    dirty = true;
}


void BTController::FindNameArgInfo::SetArgs()
{
    QCC_DbgTrace(("BTController::FindNameArgInfo::SetArgs()"));
    size_t argsSize = this->argsSize;
    const char* rdest = bto.IsMaster() ? bto.bus.GetUniqueName().c_str() : resultDest.c_str();

    MsgArg::Set(args, argsSize, SIG_DELEGATE_FIND,
                rdest,
                ignoreUUID,
                ignoreAddr.GetRaw(),
                bto.RotateMinions() ? DELEGATE_TIME : (uint32_t)0);
    assert(argsSize == this->argsSize);

    dirty = false;
}


void BTController::FindNameArgInfo::ClearArgs()
{
    QCC_DbgTrace(("BTController::FindNameArgInfo::ClearArgs()"));
    size_t argsSize = this->argsSize;

    MsgArg::Set(args, argsSize, SIG_DELEGATE_FIND,
                "",
                INVALID_UUIDREV,
                0ULL,
                (uint32_t)0);
    assert(argsSize == this->argsSize);
}


#ifndef NDEBUG
void BTController::DumpNodeStateTable() const
{
    BTNodeDB::const_iterator nodeit;
    QCC_DbgPrintf(("Node State Table (local = %s):", bus.GetUniqueName().c_str()));
    for (nodeit = nodeDB.Begin(); nodeit != nodeDB.End(); ++nodeit) {
        const BTNodeInfo& node = *nodeit;
        NameSet::const_iterator nameit;
        QCC_DbgPrintf(("    %s-%04x %s (%s):",
                       node->nodeAddr.addr.ToString().c_str(),
                       node->nodeAddr.psm,
                       node->uniqueName.c_str(),
                       (node == self) ? "local" :
                       ((node == find.minion) ? "find minion" :
                        ((node == advertise.minion) ? "advertise minion" :
                         (node->directMinion ? "direct minon" :
                          (node->IsMinionOf(self) ? "indirect minion" : "<orphan>"))))));
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
#endif


}
