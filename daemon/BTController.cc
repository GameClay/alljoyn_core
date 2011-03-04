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

#define SIG_BDADDR     "s"
#define SIG_CHANNEL    "y"
#define SIG_DURATION   "u"
#define SIG_GUID       "s"
#define SIG_MINION_CNT "y"
#define SIG_NAME       "s"
#define SIG_PSM        "q"
#define SIG_STATUS     "u"
#define SIG_UUIDREV    "u"

#define SIG_NAME_LIST         SIG_ARRAY SIG_NAME
#define SIG_AD_NAME_MAP_ENTRY "{" SIG_GUID SIG_NAME_LIST "}"
#define SIG_AD_NAME_MAP       SIG_ARRAY SIG_AD_NAME_MAP_ENTRY
#define SIG_AD_NAMES          SIG_NAME_LIST
#define SIG_FIND_NAMES        SIG_NAME_LIST
#define SIG_NODE_STATE_ENTRY  "(" SIG_NAME SIG_GUID SIG_AD_NAMES SIG_FIND_NAMES ")"
#define SIG_NODE_STATES       SIG_ARRAY SIG_NODE_STATE_ENTRY

#define SIG_SET_STATE_IN      SIG_MINION_CNT SIG_NODE_STATES
#define SIG_SET_STATE_OUT     SIG_NODE_STATES
#define SIG_PROXY_CONN_IN     SIG_BDADDR SIG_CHANNEL SIG_PSM
#define SIG_PROXY_CONN_OUT    SIG_STATUS
#define SIG_PROXY_DISC_IN     SIG_BDADDR
#define SIG_PROXY_DISC_OUT    SIG_STATUS
#define SIG_NAME_OP           SIG_NAME SIG_NAME
#define SIG_DELEGATE_AD       SIG_UUIDREV SIG_BDADDR SIG_CHANNEL SIG_PSM SIG_AD_NAME_MAP SIG_DURATION
#define SIG_DELEGATE_FIND     SIG_NAME SIG_UUIDREV SIG_BDADDR SIG_DURATION
#define SIG_FOUND_NAMES       SIG_AD_NAME_MAP SIG_BDADDR SIG_CHANNEL SIG_PSM
#define SIG_FOUND_DEV         SIG_BDADDR SIG_UUIDREV SIG_UUIDREV


const InterfaceDesc btmIfcTable[] = {
    /* Methods */
    { MESSAGE_METHOD_CALL, "SetState",        SIG_SET_STATE_IN,  SIG_SET_STATE_OUT,  "minionCnt,findnames,adNames" },
    { MESSAGE_METHOD_CALL, "ProxyConnect",    SIG_PROXY_CONN_IN, SIG_PROXY_CONN_OUT, "bdAddr,channel,psm,status" },
    { MESSAGE_METHOD_CALL, "ProxyDisconnect", SIG_PROXY_DISC_IN, SIG_PROXY_DISC_OUT, "bdAddr,status" },

    /* Signals */
    { MESSAGE_SIGNAL, "FindName",            SIG_NAME_OP,       NULL, "requestor,findName" },
    { MESSAGE_SIGNAL, "CancelFindName",      SIG_NAME_OP,       NULL, "requestor,findName" },
    { MESSAGE_SIGNAL, "AdvertiseName",       SIG_NAME_OP,       NULL, "requestor,adName" },
    { MESSAGE_SIGNAL, "CancelAdvertiseName", SIG_NAME_OP,       NULL, "requestor,adName" },
    { MESSAGE_SIGNAL, "DelegateAdvertise",   SIG_DELEGATE_AD,   NULL, "uuidRev,bdAddr,channel,psm,adNames" },
    { MESSAGE_SIGNAL, "DelegateFind",        SIG_DELEGATE_FIND, NULL, "resultDest,ignoreUUIDRev,bdAddr,duration" },
    { MESSAGE_SIGNAL, "FoundNames",          SIG_FOUND_NAMES,   NULL, "adNamesTable,bdAddr,channel,psm" },
    { MESSAGE_SIGNAL, "LostNames",           SIG_FOUND_NAMES,   NULL, "adNamesTable,bdAddr,channel,psm" },
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
    org.alljoyn.Bus.BTController.ProxyConnect =        ifc->GetMember("ProxyConnect");
    org.alljoyn.Bus.BTController.ProxyDisconnect =     ifc->GetMember("ProxyDisconnect");
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
    /* Create an entry in the node state table for ourself and set the GUID
     * accordingly.  This will allow add and remove advertise and find name
     * functions to work properly.
     */
    nodeStateLock.Lock();
    nodeStates[bus.GetUniqueName()].guid = bus.GetGlobalGUIDString();
    nodeStateLock.Unlock();
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

    nodeStateLock.Lock();

    QStatus status;
    vector<MsgArg> array;
    MsgArg args[2];
    size_t numArgs = ArraySize(args);
    Message reply(bus);
    ProxyBusObject* newMaster = new ProxyBusObject(bus, busName.c_str(), bluetoothObjPath, 0);

    newMaster->AddInterface(*org.alljoyn.Bus.BTController.interface);

    QCC_DbgPrintf(("SendSetState prep args"));
    status = EncodeStateInfo(array);
    if (status != ER_OK) {
        goto exit;
    }

    status = MsgArg::Set(args, numArgs, SIG_SET_STATE_IN, directMinions, array.size(), &array.front());
    if (status != ER_OK) {
        goto exit;
    }

    status = newMaster->MethodCall(*org.alljoyn.Bus.BTController.SetState, args, ArraySize(args), reply);

    if (status == ER_OK) {
        MsgArg* array;
        size_t num;

        status = reply->GetArgs(SIG_SET_STATE_OUT, &num, &array);
        if (status != ER_OK) {
            goto exit;
        }

        if (num == 0) {
            // We are now a minion (or a drone if we have more than one direct connection)
            master = newMaster;
        } else {
            // We are the still the master
            ImportState(num, array, busName);
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
            if (listening) {
                bt.StopListen();
                listening = false;
            }
        }

        if (IsDrone()) {
            // TODO - Move minion connections to new master
        }
    }

exit:
    nodeStateLock.Unlock();

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
        QCC_DbgPrintf(("Stopping find..."));
        assert(find.active);
        bt.StopFind();
        find.active = false;
    }
    if (UseLocalAdvertise() && advertise.active) {
        /*
         * Gotta shut down the local advertise operation since a successful
         * connection will result in the exchange for the SetState method call
         * and response afterwhich the node responsibility for advertising
         * will be determined.
         */
        QCC_DbgPrintf(("Stopping advertise..."));
        bt.StopAdvertise();
        advertise.active = false;
    }
    nodeStateLock.Unlock();
}


void BTController::PostConnect(QStatus status, const RemoteEndpoint* ep)
{
    if (status == ER_OK) {
        assert(ep);
        SendSetState(ep->GetRemoteName());
    } else {
        nodeStateLock.Lock();
        if (UseLocalFind()) {
            /*
             * Gotta restart the find operation since the connect failed and
             * we need to do the find for ourself.
             */
            QCC_DbgPrintf(("Starting find..."));
            assert(!find.active);
            bt.StartFind(masterUUIDRev);
            find.active = true;
        }
        if (UseLocalAdvertise()) {
            /*
             * Gotta restart the advertise operation since the connect failed and
             * we need to do the advertise for ourself.
             */
            BluetoothDeviceInterface::AdvertiseInfo adInfo;
            status = ExtractAdInfo(&advertise.adInfoArgs.front(), advertise.adInfoArgs.size(), adInfo);
            QCC_DbgPrintf(("Starting advertise..."));
            assert(!advertise.active);
            bt.StartAdvertise(masterUUIDRev, advertise.bdAddr, advertise.channel, advertise.psm, adInfo);
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
    QCC_DbgTrace(("BTController::ProxyConnect(bdAddr = %s, channel = %u, psm %u)",
                  bdAddr.ToString().c_str(), channel, psm));
    QStatus status = (directMinions < maxConnections) ? ER_OK : ER_BT_MAX_CONNECTIONS_USED;

    if (IsMaster()) {
        // The master cannot send a proxy connect.  (Who would do it?)
        status = ER_FAIL;
    } else if (status == ER_OK) {
        MsgArg args[3];
        size_t argsSize = ArraySize(args);
        Message reply(bus);
        uint32_t s;

        status = MsgArg::Set(args, argsSize, SIG_PROXY_CONN_IN, bdAddr.ToString().c_str(), channel, psm);
        if (status != ER_OK) {
            goto exit;
        }

        status = master->MethodCall(*org.alljoyn.Bus.BTController.ProxyConnect, args, argsSize, reply);
        if (status != ER_OK) {
            goto exit;
        }

        status = reply->GetArgs(SIG_STATUS, &s);
        if (status != ER_OK) {
            goto exit;
        }

        status = static_cast<QStatus>(s);
        if (delegate && (status == ER_OK)) {
            *delegate = master->GetServiceName();
        }
    }

exit:
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
        uint32_t s;

        status = MsgArg::Set(args, argsSize, SIG_PROXY_DISC_IN, bdAddr.ToString().c_str());
        if (status != ER_OK) {
            goto exit;
        }

        status = master->MethodCall(*org.alljoyn.Bus.BTController.ProxyDisconnect, args, argsSize, reply);
        if (status != ER_OK) {
            goto exit;
        }

        status = reply->GetArgs(SIG_STATUS, &s);
        if (status != ER_OK) {
            goto exit;
        }

        status = static_cast<QStatus>(s);
    }

exit:
    return status;
}


void BTController::BTDeviceAvailable(bool on)
{
    QCC_DbgPrintf(("BTController::BTDevicePower(<%s>)", on ? "on" : "off"));
    if (IsMaster()) {
        nodeStateLock.Lock();
        if (on) {
            if (!advertise.Empty()) {
                QStatus status = bt.StartListen(advertise.bdAddr, advertise.channel, advertise.psm);
                listening = (status == ER_OK);
                if (listening) {
                    find.ignoreAddr = advertise.bdAddr;
                    find.dirty = true;
                }
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
        nodeStateLock.Unlock();
    }
    devAvailable = on;
}


void BTController::NameOwnerChanged(const qcc::String& alias,
                                    const qcc::String* oldOwner,
                                    const qcc::String* newOwner)
{
    if (oldOwner && (alias == *oldOwner)) {
        // An endpoint left the bus.

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

            if (!advertise.Empty()) {
                QStatus status = bt.StartListen(advertise.bdAddr, advertise.channel, advertise.psm);
                listening = (status == ER_OK);
                if (listening) {
                    find.ignoreAddr = advertise.bdAddr;
                    find.dirty = true;
                }
            }
            find.resultDest.clear();

            nodeStateLock.Lock();
            UpdateDelegations(advertise, listening);
            UpdateDelegations(find);
            QCC_DEBUG_ONLY(DumpNodeStateTable());
            nodeStateLock.Unlock();

        } else {
            // Someone else left.  If it was a minion node, remove their find/ad names.
            nodeStateLock.Lock();
            NodeStateMap::iterator minion = nodeStates.find(alias);

            if (minion != nodeStates.end()) {
                QCC_DbgPrintf(("One of our minions left us: %s", minion->first.c_str()));

                bool wasAdvertiseMinion = minion == advertise.minion;
                bool wasFindMinion = minion == find.minion;
                bool wasDirect = minion->second.direct;

                // Advertise minion and find minion should never be the same.
                assert(!(wasAdvertiseMinion && wasFindMinion));

                // Indicate name list has changed.
                advertise.dirty = !minion->second.advertiseNames.empty();
                find.dirty = !minion->second.findNames.empty();

                // Remove find names for the lost minion.
                while (minion->second.findNames.begin() != minion->second.findNames.end()) {
                    find.names.erase(*minion->second.findNames.begin());
                    minion->second.findNames.erase(minion->second.findNames.begin());
                }

                // Remove the minion's state information.
                nodeStates.erase(minion);

                if (wasDirect) {
                    --directMinions;

                    if (wasFindMinion) {
                        find.active = false;
                        if (directMinions == 0) {
                            // Our only minion was finding for us.  We'll have to
                            // find for ourself now.
                            find.minion = nodeStates.end();
                            QCC_DbgPrintf(("Selected ourself as find minion.", find.minion->first.c_str()));
                        } else if (directMinions == 1) {
                            // We had 2 minions.  The one that was finding for
                            // us left, so now we must advertise and tell our
                            // remaining minion to find.
                            advertise.active = false;
                            advertise.ClearArgs();
                            Signal(advertise.minion->first.c_str(), 0, *advertise.delegateSignal, advertise.args, advertise.argsSize);
                            advertise.minion = nodeStates.end();
                            find.minion = nodeStates.begin();
                            while (!find.minion->second.direct || find.minion->first == bus.GetUniqueName()) {
                                ++find.minion;
                                assert(find.minion != nodeStates.end());
                            }
                            QCC_DbgPrintf(("Selected %s as our find minion.", find.minion->first.c_str()));
                        } else {
                            // We had more than 2 minions, so at least one is
                            // idle.  Select the next available minion to do
                            // the finding for us.
                            NextDirectMinion(find.minion);
                            QCC_DbgPrintf(("Selected %s as our find minion.", find.minion->first.c_str()));
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
                            advertise.minion = nodeStates.end();
                            QCC_DbgPrintf(("Selected ourself as advertise minion.", advertise.minion->first.c_str()));
                        } else {
                            // We had more than 2 minions, so at least one is
                            // idle.  Select the next available minion and to
                            // do the advertising for us.
                            NextDirectMinion(advertise.minion);
                            QCC_DbgPrintf(("Selected %s as our advertise minion.", advertise.minion->first.c_str()));
                        }
                    }
                }

                UpdateDelegations(advertise, listening);
                UpdateDelegations(find);
                QCC_DEBUG_ONLY(DumpNodeStateTable());
            }
            nodeStateLock.Unlock();
        }
    }
}


QStatus BTController::SendFoundNamesChange(const char* dest,
                                           const BluetoothDeviceInterface::AdvertiseInfo& adInfo,
                                           const BDAddress& bdAddr,
                                           uint8_t channel,
                                           uint16_t psm,
                                           bool lost)
{
    QCC_DbgTrace(("BTController::SendFoundNamesChange(dest = \"%s\", adInfo = <>, bdAddr = %s, channel = %u, psm = %u, <%s>)",
                  dest, bdAddr.ToString().c_str(), channel, psm, lost ? "lost" : "found/changed"));

    MsgArg args[4];
    size_t argsSize = ArraySize(args);
    vector<MsgArg> nodeList;

    EncodeAdInfo(adInfo, nodeList);

    QStatus status = MsgArg::Set(args, argsSize, SIG_FOUND_NAMES,
                         nodeList.size(), &nodeList.front(),
                         bdAddr.ToString().c_str(),
                         channel,
                         psm);
    if (status == ER_OK) {
        if (lost) {
            status = Signal(dest, 0, *org.alljoyn.Bus.BTController.LostNames, args, ArraySize(args));
        } else {
            status = Signal(dest, 0, *org.alljoyn.Bus.BTController.FoundNames, args, ArraySize(args));
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

    nodeStateLock.Lock();
    if (add) {
        nameArgInfo.AddName(name);
    } else {
        nameArgInfo.RemoveName(name);
    }

    nameArgInfo.dirty = true;

    if (IsMaster()) {
        QCC_DbgPrintf(("Handling %s locally (we're the master)", signal.name.c_str()));
        if (!advertise.Empty() && !listening && (directMinions < maxConnections)) {
            status = bt.StartListen(advertise.bdAddr, advertise.channel, advertise.psm);
            listening = (status == ER_OK);
            if (listening) {
                find.ignoreAddr = advertise.bdAddr;
            }
        }

        UpdateDelegations(advertise, listening);
        UpdateDelegations(find);
        QCC_DEBUG_ONLY(DumpNodeStateTable());

        if (advertise.Empty() && listening) {
            bt.StopListen();
            listening = false;
        }
    } else {
        QCC_DbgPrintf(("Sending %s to our master: %s", signal.name.c_str(), master->GetServiceName().c_str()));
        MsgArg args[2];
        qcc::String busName = bus.GetUniqueName();
        args[0].Set(SIG_NAME, busName.c_str());
        args[1].Set(SIG_NAME, name.c_str());
        status = Signal(master->GetServiceName().c_str(), 0, signal, args, ArraySize(args));
    }
    nodeStateLock.Unlock();

    return status;
}


void BTController::ProcessDeviceChange(const BDAddress& adBdAddr,
                                       uint32_t newUUIDRev,
                                       uint32_t oldUUIDRev,
                                       bool lost)
{
    QCC_DbgTrace(("BTController::ProcessDeviceChange(adBdAddr = %s, newUUIDRev = %08x, oldUUIDRev = %08x, <%s>)",
                  adBdAddr.ToString().c_str(), newUUIDRev, oldUUIDRev, lost ? "lost": "found/changed"));
    QStatus status = ER_OK;

    if (IsMaster()) {
        BDAddress connAddr;
        uint8_t channel;
        uint16_t psm;
        BluetoothDeviceInterface::AdvertiseInfo adInfo;
        BluetoothDeviceInterface::AdvertiseInfo oldAdInfo;
        bool notifyOurself = false;

        if (!lost) {
            // Need to get new or updated information...
            nodeStateLock.Lock();
            if (UseLocalFind()) {
                // TODO - This should probably move into BTAccessor
                QCC_DbgPrintf(("Stopping find..."));
                assert(find.active);
                bt.StopFind();
                find.active = false;
            }

            status = bt.GetDeviceInfo(adBdAddr, connAddr, newUUIDRev, channel, psm, adInfo);

            if (UseLocalFind()) {
                // TODO - This should probably move into BTAccessor
                QCC_DbgPrintf(("Starting find..."));
                bt.StartFind(masterUUIDRev);
                find.active = true;
            }
            nodeStateLock.Unlock();
            if (status != ER_OK) {
                QCC_LogError(status, ("Failed to get device information"));
                return;
            }
        }

        cacheLock.Lock();
        UUIDRevCacheMap::iterator ci = uuidRevCache.lower_bound(oldUUIDRev);
        UUIDRevCacheMap::const_iterator ciEnd = uuidRevCache.upper_bound(oldUUIDRev);

        while ((ci != ciEnd) &&
               ((lost && (ci->second.adAddr != adBdAddr)) ||
                (!lost && (ci->second.connAddr != connAddr)))) {
            ++ci;
        }

        if (ci != ciEnd) {
            oldAdInfo = ci->second.adInfo;
            if (lost) {
                connAddr = ci->second.connAddr;
                channel = ci->second.channel;
                psm = ci->second.psm;
            }
            uuidRevCache.erase(ci);
        }

        if (!lost) {
            ci = uuidRevCache.insert(pair<uint32_t, UUIDRevCacheInfo>(newUUIDRev, UUIDRevCacheInfo()));
            ci->second.adAddr = adBdAddr;
            ci->second.uuidRev = newUUIDRev;
            ci->second.connAddr = connAddr;
            ci->second.channel = channel;
            ci->second.psm = psm;
            ci->second.adInfo = adInfo;
        }
        cacheLock.Unlock();


        // Need to find which previously advertised nodes/names are now gone.
        BluetoothDeviceInterface::_AdvertiseInfo::const_iterator node;
        for (node = adInfo->begin(); node != adInfo->end(); ++node) {
            BluetoothDeviceInterface::_AdvertiseInfo::iterator oldNode = oldAdInfo->find(node->first);
            if (oldNode != oldAdInfo->end()) {
                BluetoothDeviceInterface::AdvertiseNames::const_iterator name;
                for (name = node->second.begin(); !node->second.empty() && (name != node->second.end()); ++name) {
                    oldNode->second.erase(*name);
                }
            }
        }

        // Now inform everyone of the changes in advertised names.
        nodeStateLock.Lock();
        for (NodeStateMap::iterator it = nodeStates.begin(); it != nodeStates.end(); ++it) {
            if (!it->second.findNames.empty()) {
                if (it->first == bus.GetUniqueName()) {
                    notifyOurself = true;
                } else {
                    if (!oldAdInfo->empty()) {
                        status = SendFoundNamesChange(it->first.c_str(), oldAdInfo, connAddr, channel, psm, true);
                    }
                    if (!adInfo->empty()) {
                        status = SendFoundNamesChange(it->first.c_str(), adInfo, connAddr, channel, psm, false);
                    }
                }
            }
        }
        nodeStateLock.Unlock();

        if (notifyOurself) {
            // Tell ourself about the names (This is best done outside the noseStateLock just in case).
            for (node = oldAdInfo->begin(); node != oldAdInfo->end(); ++node) {
                if (!node->second.empty()) {
                    vector<String> vectorizedNames;
                    vectorizedNames.reserve(node->second.size());
                    vectorizedNames.assign(node->second.begin(), node->second.end());
                    bt.FoundNamesChange(node->first, vectorizedNames, connAddr, channel, psm, true);
                }
            }
            for (node = adInfo->begin(); node != adInfo->end(); ++node) {
                if (!node->second.empty()) {
                    vector<String> vectorizedNames;
                    vectorizedNames.reserve(node->second.size());
                    vectorizedNames.assign(node->second.begin(), node->second.end());
                    bt.FoundNamesChange(node->first, vectorizedNames, connAddr, channel, psm, false);
                }
            }
        }

    } else {
        // Must be a drone or slave.
        MsgArg args[2];
        size_t numArgs = ArraySize(args);

        status = MsgArg::Set(args, numArgs, SIG_FOUND_DEV, adBdAddr.ToString().c_str(), newUUIDRev, oldUUIDRev);
        if (status != ER_OK) {
            return;
        }

        if (lost) {
            Signal(find.resultDest.c_str(), 0, *org.alljoyn.Bus.BTController.LostDevice, args, numArgs);
        } else {
            Signal(find.resultDest.c_str(), 0, *org.alljoyn.Bus.BTController.FoundDevice, args, numArgs);
        }
    }
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
    NameArgInfo* nameCollection = ((fn || cfn) ?
                                   static_cast<NameArgInfo*>(&find) :
                                   static_cast<NameArgInfo*>(&advertise));
    char* busName;
    char* name;

    QStatus status = msg->GetArgs(SIG_NAME_OP, &busName, &name);

    if (status == ER_OK) {
        QCC_DbgPrintf(("%s %s to the list of %s names for %s.",
                       addName ? "Adding" : "Removing",
                       name,
                       (fn || cfn) ? "find" : "advertise",
                       busName));

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
                QCC_DEBUG_ONLY(DumpNodeStateTable());
            } else {
                // We are a drone so pass on the name
                const MsgArg* args;
                size_t numArgs;
                msg->GetArgs(numArgs, args);
                Signal(master->GetServiceName().c_str(), 0, *member, args, numArgs);
            }
        }

        nodeStateLock.Unlock();
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
    nodeStateLock.Lock();
    qcc::String sender = msg->GetSender();
    QStatus status;

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

    if (numConnections > directMinions) {
        // We are now a minion (or a drone if we have more than one direct connection)
        master = new ProxyBusObject(bus, sender.c_str(), bluetoothObjPath, 0);

        vector<MsgArg> array;

        status = EncodeStateInfo(array);
        if (status == ER_OK) {
            MsgArg arg(SIG_NODE_STATES, array.size(), &array.front());

            status = MethodReply(msg, &arg, 1);
            if (status != ER_OK) {
                QCC_LogError(status, ("MethodReply"));
            }
        } else {
            MethodReply(msg, "org.alljoyn.Bus.BTController.InternalError", QCC_StatusText(status));
        }

    } else {
        // We are still the master
        size_t size;
        MsgArg* entries;

        status = msg->GetArg(1)->Get(SIG_NODE_STATES, &size, &entries);
        if (status != ER_OK) {
            MethodReply(msg, "org.alljoyn.Bus.BTController.InternalError", QCC_StatusText(status));
            return;
        }

        ImportState(size, entries, sender);

        assert(find.resultDest.empty());
        find.dirty = true;

        MsgArg arg(SIG_NODE_STATES, 0, NULL);
        status = MethodReply(msg, &arg, 1);
        if (status != ER_OK) {
            QCC_LogError(status, ("MethodReply"));
        }
    }

    QCC_DbgPrintf(("We are %s, %s is now our %s",
                   IsMaster() ? "still the master" : (IsDrone() ? "now a drone" : "just a minion"),
                   sender.c_str(), IsMaster() ? "minion" : "master"));

    if (!IsMinion()) {
        UpdateDelegations(advertise, listening);
        UpdateDelegations(find);
        QCC_DEBUG_ONLY(DumpNodeStateTable());
    }
    nodeStateLock.Unlock();

    if (!IsMaster()) {
        bus.GetInternal().GetDispatcher().RemoveAlarm(stopAd);
        if (listening) {
            bt.StopListen();
            listening = false;
        }
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
        uint32_t duration;

        QStatus status = msg->GetArgs(SIG_DELEGATE_FIND, &resultDest, &ignoreUUID, &bdAddrStr, &duration);

        if (status == ER_OK) {
            QCC_DbgPrintf(("Delegated Find: result dest: \"%s\"  ignore UUIDRev: %08x  ignore BDAddr: %s",
                           resultDest, ignoreUUID, bdAddrStr));

            if (resultDest && (resultDest[0] != '\0')) {
                find.resultDest = resultDest;
                find.ignoreAddr.FromString(bdAddrStr);
                //find.ignoreUUID = ignoreUUID;
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
        QCC_DbgPrintf(("Selected %s as our find minion.", find.minion->first.c_str()));

        Signal(find.minion->first.c_str(), 0, *find.delegateSignal, args, numArgs);
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
        BluetoothDeviceInterface::AdvertiseInfo adInfo;
        MsgArg* entries;
        size_t size;

        QStatus status = msg->GetArgs(SIG_DELEGATE_AD, &uuidRev, &bdAddrStr, &channel, &psm, &size, &entries);

        if (status == ER_OK) {
            status = ExtractAdInfo(entries, size, adInfo);
        }

        if (status == ER_OK) {
            if (!adInfo->empty()) {
                BDAddress bdAddr(bdAddrStr);

                QCC_DbgPrintf(("Starting advertise..."));
                bt.StartAdvertise(uuidRev, bdAddr, channel, psm, adInfo, DELEGATE_TIME);
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
        QCC_DbgPrintf(("Selected %s as our advertise minion.", advertise.minion->first.c_str()));

        Signal(advertise.minion->first.c_str(), 0, *advertise.delegateSignal, args, numArgs);
    }
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

    BluetoothDeviceInterface::AdvertiseInfo adInfo;
    char* addrStr;
    uint8_t channel;
    uint16_t psm;
    bool lost = (member == org.alljoyn.Bus.BTController.LostNames);
    MsgArg* entries;
    size_t size;

    QStatus status = msg->GetArgs(SIG_FOUND_NAMES, &size, &entries, &addrStr, &channel, &psm);

    if (status == ER_OK) {
        status = ExtractAdInfo(entries, size, adInfo);
    }

    if ((status == ER_OK) && (!adInfo->empty())) {
        BDAddress bdAddr(addrStr);

        BluetoothDeviceInterface::_AdvertiseInfo::const_iterator node;

        for (node = adInfo->begin(); node != adInfo->end(); ++node) {
            vector<String> vectorizedNames;
            vectorizedNames.reserve(node->second.size());
            vectorizedNames.assign(node->second.begin(), node->second.end());
            bt.FoundNamesChange(node->first, vectorizedNames, bdAddr, channel, psm, lost);
        }
    }
}


void BTController::HandleFoundDeviceChange(const InterfaceDescription::Member* member,
                                           const char* sourcePath,
                                           Message& msg)
{
    QCC_DbgTrace(("BTController::HandleFoundDeviceChange(member = %s, sourcePath = \"%s\", msg = <>)",
                  member->name.c_str(), sourcePath));

    nodeStateLock.Lock();
    bool ourMinion = (nodeStates.find(msg->GetSender()) != nodeStates.end());
    nodeStateLock.Unlock();
    if (!ourMinion) {
        // We only handle FoundDevice or LostDevice signals from our minions.
        return;
    }

    uint32_t newUUIDRev;
    uint32_t oldUUIDRev;
    char* adBdAddrStr;
    bool lost = (member == org.alljoyn.Bus.BTController.LostDevice);

    QStatus status = msg->GetArgs(SIG_FOUND_DEV, &adBdAddrStr, &newUUIDRev, &oldUUIDRev);

    if (status == ER_OK) {
        BDAddress adBdAddr(adBdAddrStr);

        ProcessDeviceChange(adBdAddr, newUUIDRev, oldUUIDRev, lost);
    }
}


void BTController::HandleProxyConnect(const InterfaceDescription::Member* member,
                                      Message& msg)
{
    QCC_DbgTrace(("BTController::HandleProxyConnect(member = %s, msg = <>)", member->name.c_str()));
    char* addrStr;
    uint8_t channel;
    uint16_t psm;

    QStatus status = msg->GetArgs(SIG_PROXY_CONN_IN, &addrStr, &channel, &psm);

    if (status == ER_OK) {
        BDAddress bdAddr(addrStr);

        if (OKToConnect()) {
            status = bt.Connect(bdAddr, channel, psm);
        } else {
            status = ProxyConnect(bdAddr, channel, psm, NULL);
        }
    }
    MsgArg reply(SIG_PROXY_CONN_OUT, static_cast<uint32_t>(status));

    MethodReply(msg, &reply, 1);
}


void BTController::HandleProxyDisconnect(const InterfaceDescription::Member* member,
                                         Message& msg)
{
    QCC_DbgTrace(("BTController::HandleProxyDisconnect(member = %s, msg = <>)", member->name.c_str()));
    char* addrStr;

    QStatus status = msg->GetArgs(SIG_PROXY_DISC_IN, &addrStr);

    if (status == ER_OK) {
        BDAddress bdAddr(addrStr);
        status = bt.Disconnect(bdAddr);
        if (status != ER_OK) {
            status = ProxyDisconnect(bdAddr);
        }
    }

    MsgArg reply(SIG_PROXY_DISC_OUT, static_cast<uint32_t>(status));
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
        entries[i].Get(SIG_NODE_STATE_ENTRY, &bn, &guid, &anSize, &anList, &fnSize, &fnList);
        qcc::String busName(bn);

        QCC_DbgPrintf(("Processing names for %s:", bn));

        nodeStates[busName].guid = guid;
        advertise.dirty = advertise.dirty || (anSize > 0);
        find.dirty = find.dirty || (fnSize > 0);

        NodeStateMap::iterator it = nodeStates.find(busName);

        for (j = 0; j < anSize; ++j) {
            char* n;
            anList[j].Get(SIG_NAME, &n);
            QCC_DbgPrintf(("    Ad Name: %s", n));
            qcc::String name(n);
            advertise.AddName(name, it);
        }

        for (j = 0; j < fnSize; ++j) {
            char* n;
            fnList[j].Get(SIG_NAME, &n);
            QCC_DbgPrintf(("    Find Name: %s", n));
            qcc::String name(n);
            find.AddName(name, it);
        }
    }

    bool adEnd = advertise.minion == nodeStates.end();
    bool findEnd = find.minion == nodeStates.end();
    qcc::String adNode;
    qcc::String findNode;
    if (!adEnd) {
        adNode = advertise.minion->first;
    }
    if (!findEnd) {
        findNode = find.minion->first;
    }


    nodeStates[newNode].direct = true;
    ++directMinions;

    if (findEnd) {
        find.minion = nodeStates.begin();
        while (!find.minion->second.direct || find.minion->first == bus.GetUniqueName()) {
            ++find.minion;
            assert(find.minion != nodeStates.end());
        }
        QCC_DbgPrintf(("Selected %s as our find minion.", find.minion->first.c_str()));
    } else {
        find.minion = nodeStates.find(findNode);
    }



    if (adEnd) {
        if (UseLocalAdvertise()) {
            advertise.minion = nodeStates.end();
        } else {
            nodeStates.begin();
            NextDirectMinion(advertise.minion);   // Make sure we haven't chose ourself.
        }
    } else {
        advertise.minion = nodeStates.find(adNode);
    }

    if (advertise.minion == find.minion) {
        for (uint8_t i = directMinions / 2; i > 0; --i) {
            NextDirectMinion(advertise.minion);
        }

        QCC_DbgPrintf(("Selected %s as our advertise minion.", advertise.minion->first.c_str()));

        // advertise.minion must never be the same as find.minion.
        assert(advertise.minion != find.minion);
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

    const bool sendSignal = stop || start || restart;
    const bool deferredClearArgs = stop && advertiseOp;

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

    if (changed) {
        ++masterUUIDRev;
        if (masterUUIDRev == INVALID_UUIDREV) {
            ++masterUUIDRev;
        }
    }

    if (start) {
        // Set the advertise/find arguments.
        nameInfo.SetArgs();
        nameInfo.active = true;

    } else if (restart) {
        // Update the advertise/find arguments.
        nameInfo.SetArgs();
        assert(nameInfo.active);

    } else if (stop) {
        if (deferredClearArgs) {
            // Advertise an empty name list for a while so that other devices
            // can detect a "name lost".
            static_cast<AdvertiseNameArgInfo&>(nameInfo).SetEmptyArgs();
        } else {
            // Clear out the find arguments.
            nameInfo.ClearArgs();
        }
        nameInfo.active = false;
    }

    if (sendSignal) {
        if (advertiseOp && UseLocalAdvertise()) {
            if (nameInfo.active || deferredClearArgs) {
                BluetoothDeviceInterface::AdvertiseInfo adInfo;
                QStatus status = ER_OK;
                if (!deferredClearArgs) {
                    status = ExtractAdInfo(&advertise.adInfoArgs.front(), advertise.adInfoArgs.size(), adInfo);
                }
                if (status == ER_OK) {
                    QCC_DbgPrintf(("%starting advertise...", restart ? "Res" : "S"));
                    bt.StartAdvertise(masterUUIDRev, advertise.bdAddr, advertise.channel, advertise.psm, adInfo);
                }
            } else {
                QCC_DbgPrintf(("Stopping advertise..."));
                bt.StopAdvertise();
            }
        } else if (UseLocalFind()) {
            if (nameInfo.active) {
                QCC_DbgPrintf(("%starting find...", restart ? "Res" : "S"));
                bt.StartFind(masterUUIDRev);
            } else {
                QCC_DbgPrintf(("Stopping find..."));
                bt.StopFind();
            }
        } else {
            QCC_DbgPrintf(("Sending %s signal to %s", nameInfo.delegateSignal->name.c_str(),
                           nameInfo.minion->first.c_str()));
            assert(bus.GetUniqueName() != nameInfo.minion->first.c_str());
            Signal(nameInfo.minion->first.c_str(), 0, *nameInfo.delegateSignal,
                   nameInfo.args, nameInfo.argsSize);
            if (RotateMinions()) {
                if (stop || restart) {
                    nameInfo.StopAlarm();
                }

                if (start || restart) {
                    nameInfo.StartAlarm();
                }
            }
        }
    }

    if (deferredClearArgs) {
        // Advertise empty list for a while so others detect lost name quickly.
        stopAd = Alarm(DELEGATE_TIME * 1000, this);
        bus.GetInternal().GetDispatcher().AddAlarm(stopAd);

        // Clear out the advertise arguments.
        nameInfo.ClearArgs();
    }
}


QStatus BTController::EncodeStateInfo(vector<MsgArg>& array)
{
    QStatus status = ER_OK;
    array.resize(nodeStates.size());
    NodeStateMap::const_iterator it;
    size_t cnt;
    for (cnt = 0, it = nodeStates.begin(); (status == ER_OK) && (it != nodeStates.end()); ++it, ++cnt) {
        QCC_DbgPrintf(("    Node %s:", it->first.c_str()));
        vector<const char*> nodeAdNames;
        vector<const char*> nodeFindNames;
        set<qcc::String>::const_iterator nit;
        nodeAdNames.reserve(it->second.advertiseNames.size());
        nodeFindNames.reserve(it->second.findNames.size());
        for (nit = it->second.advertiseNames.begin(); nit != it->second.advertiseNames.end(); ++nit) {
            QCC_DbgPrintf(("        Ad name: %s", nit->c_str()));
            nodeAdNames.push_back(nit->c_str());
        }
        for (nit = it->second.findNames.begin(); nit != it->second.findNames.end(); ++nit) {
            QCC_DbgPrintf(("        Find name: %s", nit->c_str()));
            nodeFindNames.push_back(nit->c_str());
        }
        status = array[cnt].Set(SIG_NODE_STATE_ENTRY, it->first.c_str(),
                                it->second.guid.c_str(),
                                nodeAdNames.size(), &nodeAdNames.front(),
                                nodeFindNames.size(), &nodeFindNames.front());
    }

    return status;
}


void BTController::EncodeAdInfo(const BluetoothDeviceInterface::AdvertiseInfo& adInfo,
                                vector<MsgArg>& nodeList)
{
    BluetoothDeviceInterface::_AdvertiseInfo::const_iterator it;

    nodeList.reserve(adInfo->size());

    for (it = adInfo->begin(); it != adInfo->end(); ++it) {
        const qcc::String& guid = it->first;
        const BluetoothDeviceInterface::AdvertiseNames& names = it->second;

        vector<const char*> nameList;
        BluetoothDeviceInterface::AdvertiseNames::const_iterator name;
        nameList.reserve(names.size());
        QCC_DbgPrintf(("Encoding %u advertise names for %s:", names.size(), guid.c_str()));

        for (name = names.begin(); name != names.end(); ++name) {
            QCC_DbgPrintf(("    %s", name->c_str()));
            nameList.push_back(name->c_str());
        }
        nodeList.push_back(MsgArg(SIG_AD_NAME_MAP_ENTRY, guid.c_str(), nameList.size(), &nameList.front()));
    }
}


QStatus BTController::ExtractAdInfo(const MsgArg* entries, size_t size,
                                    BluetoothDeviceInterface::AdvertiseInfo& adInfo)
{
    QCC_DbgTrace(("BTController::ExtractAdInfo()"));

    QStatus status = ER_OK;

    if (entries && (size > 0)) {
        adInfo->clear();

        for (size_t i = 0; i < size; ++i) {
            char* guid;
            MsgArg* names;
            size_t numNames;

            status = entries[i].Get(SIG_AD_NAME_MAP_ENTRY, &guid, &numNames, &names);

            if (status == ER_OK) {
                BluetoothDeviceInterface::AdvertiseNames& nameList = (*adInfo)[guid];

                nameList.clear();

                QCC_DbgPrintf(("Extracting %u advertise names for %s:", numNames, guid));
                for (size_t j = 0; j < numNames; ++j) {
                    char* name;
                    status = names[j].Get(SIG_NAME, &name);
                    if (status == ER_OK) {
                        QCC_DbgPrintf(("    %s", name));
                        nameList.insert(name);
                    }
                }
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
        nodeStateLock.Lock();
        if (advertise.Empty()) {
            if (UseLocalAdvertise()) {
                QCC_DbgPrintf(("Stopping advertise..."));
                bt.StopAdvertise();
            } else {
                // Tell the minion to stop advertising (presumably the empty list).
                Signal(advertise.minion->first.c_str(), 0, *advertise.delegateSignal, advertise.args, advertise.argsSize);
            }
        }
        nodeStateLock.Unlock();
    }
}


void BTController::NameArgInfo::AlarmTriggered(const Alarm& alarm, QStatus reason)
{
    QCC_DbgTrace(("BTController::NameArgInfo::AlarmTriggered(alarm = <%s>, reason = %s)",
                  alarm == bto.find.alarm ? "find" : "advertise", QCC_StatusText(reason)));

    if (reason == ER_OK) {
        bto.nodeStateLock.Lock();
        bto.NextDirectMinion(minion);
        QCC_DbgPrintf(("Selected %s as our %s minion.",
                       minion->first.c_str(),
                       (minion == bto.find.minion) ? "find" : "advertise"));
        bto.Signal(minion->first.c_str(), 0, *delegateSignal, args, argsSize);
        bto.nodeStateLock.Unlock();

        // Manually re-arm alarm since automatically recurring alarms cannot be stopped.
        StartAlarm();
    }
}


void BTController::AdvertiseNameArgInfo::AddName(const qcc::String& name, NodeStateMap::iterator it)
{
    assert(it != bto.nodeStates.end());
    it->second.advertiseNames.insert(name);
    ++count;
    dirty = true;
}


void BTController::AdvertiseNameArgInfo::RemoveName(const qcc::String& name, NodeStateMap::iterator it)
{
    assert(it != bto.nodeStates.end());
    it->second.advertiseNames.erase(name);
    --count;
    dirty = true;
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
    QCC_DbgTrace(("BTController::AdvertiseNameArgInfo::SetArgs()"));
    size_t argsSize = this->argsSize;

    adInfoArgs.clear();
    adInfoArgs.reserve(bto.nodeStates.size());

    NodeStateMap::const_iterator nodeit;
    set<qcc::String>::const_iterator nameit;
    vector<const char*> names;
    size_t i;
    for (i = 0, nodeit = bto.nodeStates.begin(); nodeit != bto.nodeStates.end(); ++nodeit, ++i) {
        assert(i < adInfoArgs.capacity());
        names.clear();
        names.reserve(nodeit->second.advertiseNames.size());
        for (nameit = nodeit->second.advertiseNames.begin(); nameit != nodeit->second.advertiseNames.end(); ++nameit) {
            names.push_back(nameit->c_str());
        }
        adInfoArgs.push_back(MsgArg(SIG_AD_NAME_MAP_ENTRY, nodeit->second.guid.c_str(), names.size(), &names.front()));
    }

    MsgArg::Set(args, argsSize, SIG_DELEGATE_AD,
                bto.masterUUIDRev,
                bdAddr.ToString().c_str(),
                channel,
                psm,
                adInfoArgs.size(), &adInfoArgs.front(),
                bto.RotateMinions() ? DELEGATE_TIME : (uint32_t)0);
    assert(argsSize == this->argsSize);

    dirty = false;
}


void BTController::AdvertiseNameArgInfo::ClearArgs()
{
    QCC_DbgTrace(("BTController::AdvertiseNameArgInfo::ClearArgs()"));
    size_t argsSize = this->argsSize;

    MsgArg::Set(args, argsSize, SIG_DELEGATE_AD,
                INVALID_UUIDREV,
                "",
                INVALID_CHANNEL,
                INVALID_PSM,
                (size_t)0, NULL,
                (uint32_t)0);
    assert(argsSize == this->argsSize);
}


void BTController::AdvertiseNameArgInfo::SetEmptyArgs()
{
    QCC_DbgTrace(("BTController::AdvertiseNameArgInfo::SetEmptyArgs()"));
    size_t argsSize = this->argsSize;

    MsgArg::Set(args, argsSize, SIG_DELEGATE_AD,
                bto.masterUUIDRev,
                bdAddr.ToString().c_str(),
                channel,
                psm,
                (size_t)0, NULL,
                (uint32_t)0);
    assert(argsSize == this->argsSize);
}


void BTController::FindNameArgInfo::SetArgs()
{
    QCC_DbgTrace(("BTController::FindNameArgInfo::SetArgs()"));
    size_t argsSize = this->argsSize;
    const char* rdest = bto.IsMaster() ? bto.bus.GetUniqueName().c_str() : resultDest.c_str();

    MsgArg::Set(args, argsSize, SIG_DELEGATE_FIND,
                rdest,
                bto.masterUUIDRev,
                ignoreAddr.ToString().c_str(),
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
                "",
                (uint32_t)0);
    assert(argsSize == this->argsSize);
}



#ifndef NDEBUG
void BTController::DumpNodeStateTable() const
{
    NodeStateMap::const_iterator node;
    QCC_DbgPrintf(("Node State Table (local = %s):", bus.GetUniqueName().c_str()));
    for (node = nodeStates.begin(); node != nodeStates.end(); ++node) {
        std::set<qcc::String>::const_iterator name;
        QCC_DbgPrintf(("    %s (%s):",
                       node->first.c_str(),
                       (node->first == bus.GetUniqueName()) ? "local" :
                       ((node == find.minion) ? "find minion" :
                        ((node == advertise.minion) ? "advertise minion" :
                         (node->second.direct ? "direct minon" : "indirect minion")))));
        QCC_DbgPrintf(("         Advertise names:"));
        for (name = node->second.advertiseNames.begin(); name != node->second.advertiseNames.end(); ++name) {
            QCC_DbgPrintf(("            %s", name->c_str()));
        }
        QCC_DbgPrintf(("         Find names:"));
        for (name = node->second.findNames.begin(); name != node->second.findNames.end(); ++name) {
            QCC_DbgPrintf(("            %s", name->c_str()));
        }
    }
}
#endif


}
