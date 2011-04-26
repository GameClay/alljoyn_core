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

#define SIG_SET_STATE_IN            SIG_MINION_CNT SIG_BUSADDR SIG_NODE_STATES SIG_FOUND_NODES
#define SIG_SET_STATE_IN_SIZE       (SIG_MINION_CNT_SIZE + SIG_BUSADDR_SIZE + SIG_NODE_STATES_SIZE + SIG_FOUND_NODES_SIZE)
#define SIG_SET_STATE_OUT           SIG_BUSADDR SIG_NODE_STATES SIG_FOUND_NODES
#define SIG_SET_STATE_OUT_SIZE      (SIG_BUSADDR_SIZE + SIG_NODE_STATES_SIZE + SIG_FOUND_NODES_SIZE)
#define SIG_NAME_OP                 SIG_BUSADDR SIG_NAME
#define SIG_NAME_OP_SIZE            (SIG_BUSADDR_SIZE + SIG_NAME_SIZE)
#define SIG_DELEGATE_AD             SIG_UUIDREV SIG_BUSADDR SIG_AD_NAME_MAP SIG_DURATION
#define SIG_DELEGATE_AD_SIZE        (SIG_UUIDREV_SIZE + SIG_BUSADDR_SIZE + SIG_AD_NAME_MAP_SIZE + SIG_DURATION_SIZE)
#define SIG_DELEGATE_FIND           SIG_NAME SIG_FIND_FILTER_LIST SIG_DURATION
#define SIG_DELEGATE_FIND_SIZE      (SIG_NAME_SIZE + SIG_FIND_FILTER_LIST_SIZE + SIG_DURATION_SIZE)
#define SIG_FOUND_NAMES             SIG_AD_NAME_MAP
#define SIG_FOUND_NAMES_SIZE        (SIG_AD_NAME_MAP_SIZE)
#define SIG_FOUND_DEV               SIG_BDADDR SIG_UUIDREV SIG_UUIDREV
#define SIG_FOUND_DEV_SIZE          (SIG_BDADDR_SIZE + SIG_UUIDREV_SIZE + SIG_UUIDREV_SIZE)


const InterfaceDesc btmIfcTable[] = {
    /* Methods */
    { MESSAGE_METHOD_CALL, "SetState", SIG_SET_STATE_IN, SIG_SET_STATE_OUT, "minionCnt,busAddr,psm,nodeStates,foudnNodes,busAddr,psm,nodeStates,foundNodes" },

    /* Signals */
    { MESSAGE_SIGNAL, "FindName",            SIG_NAME_OP,       NULL, "requestorAddr,requestorPSM,findName" },
    { MESSAGE_SIGNAL, "CancelFindName",      SIG_NAME_OP,       NULL, "requestorAddr,requestorPSM,findName" },
    { MESSAGE_SIGNAL, "AdvertiseName",       SIG_NAME_OP,       NULL, "requestorAddr,requestorPSM,adName" },
    { MESSAGE_SIGNAL, "CancelAdvertiseName", SIG_NAME_OP,       NULL, "requestorAddr,requestorPSM,adName" },
    { MESSAGE_SIGNAL, "DelegateAdvertise",   SIG_DELEGATE_AD,   NULL, "uuidRev,bdAddr,psm,adNames,duration" },
    { MESSAGE_SIGNAL, "DelegateFind",        SIG_DELEGATE_FIND, NULL, "resultDest,ignoreBDAddr,duration" },
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
    masterUUIDRev(bt::INVALID_UUIDREV),
    directMinions(0),
    maxConnections(min(StringToU32(Environ::GetAppEnviron()->Find("ALLJOYN_MAX_BT_CONNECTIONS"), 0, DEFAULT_MAX_CONNECTIONS),
                       ABSOLUTE_MAX_CONNECTIONS)),
    listening(false),
    devAvailable(false),
    advertise(*this, bus.GetInternal().GetDispatcher()),
    find(*this, bus.GetInternal().GetDispatcher())
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
    org.alljoyn.Bus.BTController.LostDevice =          ifc->GetMember("LostDevice");

    advertise.delegateSignal = org.alljoyn.Bus.BTController.DelegateAdvertise;
    find.delegateSignal = org.alljoyn.Bus.BTController.DelegateFind;

    static_cast<DaemonRouter&>(bus.GetInternal().GetRouter()).AddBusNameListener(this);

    // Setup the BT node info for ourself.
    self->SetGUID(bus.GetGlobalGUIDString());
    advertise.minion = self;
    find.minion = self;
}


BTController::~BTController()
{
    // Don't need to remove our bus name change listener from the router (name
    // table) since the router is already destroyed at this point in time.

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
    vector<MsgArg> foundNodeArgsStorage;
    MsgArg args[SIG_SET_STATE_IN_SIZE];
    size_t numArgs = ArraySize(args);
    Message reply(bus);
    ProxyBusObject* newMaster = new ProxyBusObject(bus, busName.c_str(), bluetoothObjPath, 0);

    newMaster->AddInterface(*org.alljoyn.Bus.BTController.interface);

    QCC_DbgPrintf(("SendSetState prep args"));
    FillNodeStateMsgArgs(nodeStateArgsStorage);
    FillFoundNodesMsgArgs(foundNodeArgsStorage);

    status = MsgArg::Set(args, numArgs, SIG_SET_STATE_IN,
                         directMinions,
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

    status = newMaster->MethodCall(*org.alljoyn.Bus.BTController.SetState, args, ArraySize(args), reply);
    if (status != ER_OK) {
        delete newMaster;
        QCC_LogError(status, ("Dropping %s due to internal error", busName.c_str()));
        bt.Disconnect(busName);
        goto exit;
    } else {
        size_t numNodeStateArgs;
        MsgArg* nodeStateArgs;
        size_t numFoundNodeArgs;
        MsgArg* foundNodeArgs;
        uint64_t rawBDAddr;
        uint16_t psm;

        status = reply->GetArgs(SIG_SET_STATE_OUT,
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

        if (numNodeStateArgs == 0) {
            // We are now a minion (or a drone if we have more than one direct connection)
            master = newMaster;
            masterNode = BTNodeInfo(addr, master->GetServiceName());
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
                       busName.c_str(), IsMaster() ? "minion" : "master"));

        if (!IsMinion()) {
            UpdateDelegations(advertise, listening);
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
            QCC_DEBUG_ONLY(DumpNodeStateTable());
        }

        if (!IsMaster()) {
            bus.GetInternal().GetDispatcher().RemoveAlarm(stopAd);
        }
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
        BTNodeInfo node(self->GetBusAddress(), self->GetUniqueName(), self->GetGUID());  // make an actual copy of self
        self->AddAdvertiseName(name);  // self gets new name added to list of existing names
        node->AddAdvertiseName(name);  // copy of self only gets the new names (not the existing names)
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
        BTNodeInfo node(self->GetBusAddress(), self->GetUniqueName(), self->GetGUID());  // make an actual copy of self
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
        UUIDRevCacheMap::iterator ci;
        UUIDRevCacheMap::iterator ciEnd;
        BTNodeDB empty;
        BTNodeDB* adInfo = NULL;
        BDAddress connBDAddr;
        uint16_t connPSM;

        if (!lost) {
            adInfo = new BTNodeDB();

            status = bt.GetDeviceInfo(adBdAddr, newUUIDRev, connBDAddr, connPSM, *adInfo);

            if (status == ER_OK) {
                if (adInfo->Size() == 0) {
                    // Process this as a lost device.
                    lost = true;
                    delete adInfo;
                    adInfo = NULL;
                }
            } else {
                delete adInfo;
                adInfo = NULL;
            }
        }

        if (lost) {
            /*
             * There are few reasons that we have received a "lost" advertisement:
             *
             * 1. A device/piconet/scatternet we knew about has gone out of
             * range.  This is a legitemate lost advertisement.  We need to
             * remove the advertisement information from the cache and
             * foundNodeDB, and inform everyone of the lost information.
             *
             * 2. A device/piconet/scatternet joined another
             * piconet/scatternet and is no longer advertising itself.  This
             * is not a real "lost" advertisement.  The code that handles
             * "new" advertisements is expected to clean up the old
             * advertisement information so that processing of the "lost"
             * advertisement becomes a no-op.  Given the timeouts involved,
             * this will always happen after the "new" advertisements are
             * handled.
             *
             * 3. A piconet/scatternet has changed advertising nodes.  This
             * too is not a real "lost" advertisement.  This is just an effect
             * of rotating advertising nodes when the piconet/scatternet is
             * large enough.  Like case 2, the code that handles "new"
             * advertisements is expected to clean up the old advertisement
             * information so that processing this "lost" advertisement
             * becomes a no-op.  Again, the timeouts involved will ensure that
             * the "new" advertisements are handled first.
             */
            cacheLock.Lock();

            ci = uuidRevCache.lower_bound(oldUUIDRev);
            ciEnd = uuidRevCache.upper_bound(oldUUIDRev);

            BTNodeDB* oldAdInfo = NULL;

            while ((ci != ciEnd) && (ci->second.adAddr != adBdAddr)) {
                ++ci;
            }

            /* For case 1, ci should not equal ciEnd.  For case 2 and 3 ci
             * normally will be the same as ciEnd.
             */
            if (ci != ciEnd) {
                oldAdInfo = ci->second.adInfo;
                uuidRevCache.erase(ci);
            }
            cacheLock.Unlock();

            if (oldAdInfo) {
                /* Distribute the lost names before updating the foundNodeDB,
                 * since removing names from nodes found in foundNodeDB will
                 * cause those names to be removed from oldAdInfo if they both
                 * have references to the same instance of BTNodeInfo.
                 */
                DistributeAdvertisedNameChanges(empty, *oldAdInfo);
                foundNodeDB.UpdateDB(NULL, oldAdInfo);

                delete oldAdInfo;
            }
        } else {
            /*
             * There are several reasons that we have a "new" advertisement:
             *
             * 1. A device/piconet/scatternet we have never seen before came
             * into range.  This is the simplest case and we only need to add
             * the newly advertised information to our cache and foundNodeDB,
             * and to send everyone the set of newly discovered information.
             *
             * 2. A device/piconet/scatternet we know about changed it's set
             * of advertised names.  This case is slightly more complex.  We
             * need to find the cache entry and update it with the current
             * information.  We also need to update findNodeDB with the new
             * information and inform everyone of the changes (both additions
             * and removals).
             *
             * 3. 2 devices/piconets/scatternets we know about connected to
             * each other to form their own piconet/scatternet.  This case is
             * the most complex.  We need to find the cache information for
             * all the nodes in the discovered piconet/scatternet, coalesce
             * the cache information, update the foundNodeDB, and send the
             * changes to everyone (both additions and removals).
             *
             * 4. A piconet/scatternet has undergone a disconnection so there
             * are now 2 devices/piconets/scatternets.  This case is similar
             * to 2 except that the connect address for some names/node will
             * change.  It is also likely, that there will be a temporary lost
             * names condition until both advertisements are received.
             *
             * 5. A piconet/scatternet has changed which node is advertising
             * the names.  This not really a "new" advertisement.  We will
             * need to update the cache information so that when the "lost"
             * advertisement comes in for the old advertising device, none of
             * the names actually become lost.
             */

            if (adInfo) {
                BTBusAddress connAddr(connBDAddr, connPSM);
                BTNodeDB::const_iterator nodeit;
                BTNodeDB oldAdInfo;

                /* Need a container to keep track of which cache entries are
                 * to be removed in favor of the new advertisement information
                 * to be cached.  We really only need the iterator into the
                 * cache, but iterators can't be contained in a set<> so use a
                 * map and the pointer to the adInfo cache member since that
                 * is known to be unique to each iterator.
                 */
                map<void*, UUIDRevCacheMap::iterator> cacheEntries;

                BTNodeInfo connNode = foundNodeDB.FindNode(connAddr);

                if (!connNode->IsValid()) {
                    connNode = BTNodeInfo(connAddr);
                    connNode->SetUUIDRev(newUUIDRev);
                }

                /* The value in oldUUIDRev is basically irrelevant even if it
                 * is a valid value since we need to handle devices that may
                 * have become part of larger piconets/scatternets.  The cache
                 * will have them under their own respective UUID revisions
                 * rather than the one passed in.
                 */

                cacheLock.Lock();
                for (nodeit = adInfo->Begin(); nodeit != adInfo->End(); ++nodeit) {
                    BTNodeInfo adNode = *nodeit;
                    BTNodeInfo foundNode = foundNodeDB.FindNode(adNode->GetBusAddress());
                    if (foundNode->IsValid()) {
                        uint32_t uuidRev = foundNode->GetUUIDRev();

                        // Find the right cache entry.
                        ci = uuidRevCache.lower_bound(uuidRev);
                        ciEnd = uuidRevCache.upper_bound(uuidRev);

                        while ((ci != ciEnd) && (ci->second.connAddr != connAddr)) {
                            ++ci;
                        }

                        if (ci != ciEnd) {
                            cacheEntries[ci->second.adInfo] = ci;
                            BTNodeInfo cachedNode = ci->second.adInfo->FindNode(adNode->GetBusAddress());
                            NameSet::const_iterator nameit;
                            for (nameit = adNode->GetAdvertiseNamesBegin();
                                 nameit != adNode->GetAdvertiseNamesEnd();
                                 ++nameit) {
                                cachedNode->RemoveAdvertiseName(*nameit);
                            }
                            if (cachedNode->AdvertiseNamesEmpty()) {
                                ci->second.adInfo->RemoveNode(adNode);
                            }
                        }

                        // Update the node's UUID revision and connect address
                        foundNode->SetUUIDRev(newUUIDRev);
                        foundNode->SetConnectNode(connNode);
                    } else {
                        // Update the node's UUID revision and connect address
                        if (adNode->AdvertiseNamesEmpty()) {
                            adInfo->RemoveNode(adNode);
                        } else {
                            adNode->SetUUIDRev(newUUIDRev);
                            adNode->SetConnectNode(connNode);
                        }
                    }
                }

                /* Any names left in the cache entries that were found will be
                 * considered lost.  We cannot distinguish between a
                 * piconet/scatternet splitting in 2 and a name/device leaving
                 * and no longer discoverable.  This will be our only chance
                 * to notify everyone of the lost name so we need the set of
                 * lost names and devices.  If it was just a simple
                 * disconnection into 2 separate piconets/scatternets, we
                 * should get the second advertisement soon and it will be
                 * handled as a new discovery.
                 */
                map<void*, UUIDRevCacheMap::iterator>::iterator ceit;
                for (ceit = cacheEntries.begin(); ceit != cacheEntries.end(); ++ceit) {
                    ci = ceit->second;
                    if (ci->second.adInfo->Size() > 0) {
                        oldAdInfo.UpdateDB(ci->second.adInfo, NULL);
                    }
                    delete ci->second.adInfo;
                    uuidRevCache.erase(ci);
                }

                uuidRevCache.insert(UUIDRevCacheMapEntry(newUUIDRev, UUIDRevCacheInfo(adBdAddr,
                                                                                      newUUIDRev,
                                                                                      connAddr,
                                                                                      adInfo)));

                cacheLock.Unlock();

                /* Distribute the names before updating the foundNodeDB, since
                 * removing names in oldAdInfo from nodes found in foundNodeDB
                 * will cause those names to be removed from oldAdInfo if they
                 * both have references to the same instance of BTNodeInfo.
                 */
                DistributeAdvertisedNameChanges(*adInfo, oldAdInfo);
                foundNodeDB.UpdateDB(adInfo, &oldAdInfo);
            }
        }
    } else {
        // Must be a drone or slave.
        MsgArg args[SIG_FOUND_DEV_SIZE];
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


const BTBusAddress& BTController::PrepConnect(const BTBusAddress& addr)
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

    BTNodeInfo node;
    if (!IsMinion()) {
        node = nodeDB.FindNode(addr);
        if (!node->IsValid()) {
            node = foundNodeDB.FindNode(addr);
        }
    }

    if (!IsMaster() && !node->IsValid()) {
        node = masterNode;
    }

    return node->GetConnectAddress();
}


void BTController::PostConnect(QStatus status, const RemoteEndpoint* ep)
{
    // Assumes lock acquired in BTController::PrepConnect()

    if (status == ER_OK) {
        assert(ep);
        BTBusAddress addr = static_cast<const BTEndpoint*>(ep)->GetBTBusAddress();
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
                bt.StartFind(find.ignoreAddrs);
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
                bt.StartAdvertise(masterUUIDRev, self->GetBusAddress().addr, self->GetBusAddress().psm, adInfo);
                advertise.active = true;
            }
        }
    }
    lock.Unlock();
}


void BTController::BTDeviceAvailable(bool on)
{
    QCC_DbgPrintf(("BTController::BTDeviceAvailable(<%s>)", on ? "on" : "off"));
    devAvailable = on;
    lock.Lock();
    if (on) {
        BTBusAddress addr;
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
                UpdateDelegations(advertise, listening);
                UpdateDelegations(find);
                QCC_DEBUG_ONLY(DumpNodeStateTable());
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
            listenAddr.addr.SetRaw(0);
            listenAddr.psm = bt::INVALID_PSM;
            self->SetBusAddress(listenAddr);
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
        QCC_DbgPrintf(("Always accept incomming connection as Master."));
        return true;
    } else if (addr == masterNode->GetBusAddress().addr) {
        QCC_DbgPrintf(("Always accept incomming connection from Master."));
        return true;
    } else if (IsDrone()) {
        const BTNodeInfo& node = nodeDB.FindNode(addr);
        QCC_DbgPrintf(("% incomming connection from %s %s.",
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
    if (oldOwner && (alias == *oldOwner)) {
        // An endpoint left the bus.
        QCC_DbgPrintf(("%s has left the bus", alias.c_str()));

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
            masterNode = BTNodeInfo();

            BDAddressSet ignoreAddrs;
            BTNodeDB::const_iterator it;
            nodeDB.Lock();
            for (it = nodeDB.Begin(); it != nodeDB.End(); ++it) {
                ignoreAddrs->insert((*it)->GetBusAddress().addr);
            }
            nodeDB.Unlock();
            find.resultDest.clear();
            find.ignoreAddrs = ignoreAddrs;

            UpdateDelegations(advertise, listening);
            UpdateDelegations(find);
            QCC_DEBUG_ONLY(DumpNodeStateTable());

        } else {
            // Someone else left.  If it was a minion node, remove their find/ad names.
            BTNodeInfo minion = nodeDB.FindNode(alias);

            if (minion->IsValid()) {
                // Remove the minion's state information.
                QCC_DbgPrintf(("One of our minions left us: %s", minion->GetUniqueName().c_str()));

                bool wasAdvertiseMinion = minion == advertise.minion;
                bool wasFindMinion = minion == find.minion;
                bool wasDirect = minion->IsDirectMinion();
                bool wasRotateMinions = RotateMinions();

                // Advertise minion and find minion should never be the same.
                assert(!(wasAdvertiseMinion && wasFindMinion));

                nodeDB.RemoveNode(minion);
                find.ignoreAddrs->erase(minion->GetBusAddress().addr);

                // Indicate the name lists have changed.
                if (!minion->AdvertiseNamesEmpty()) {
                    advertise.count -= minion->AdvertiseNamesSize();
                    advertise.dirty = true;
                }
                if (!minion->FindNamesEmpty()) {
                    find.count -= minion->FindNamesSize();
                    find.dirty = true;
                }

                if (wasDirect) {
                    --directMinions;

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
                        if (directMinions > 0) {
                            if (directMinions == 1) {
                                // We had 2 minions.  The one that was finding
                                // for us left, so now we must advertise for
                                // ourself and tell our remaining minion to
                                // find for us.
                                advertise.active = false;
                                advertise.ClearArgs();
                                Signal(advertise.minion->GetUniqueName().c_str(), 0, *advertise.delegateSignal, advertise.args, advertise.argsSize);
                                advertise.minion = self;
                                QCC_DbgPrintf(("Set advertise minion to ourself"));
                            }

                            NextDirectMinion(find.minion);
                        }
                        // ... else our only minion was finding for us.  We'll
                        // have to find for ourself now.

                        QCC_DbgPrintf(("Selected %s as our find minion.",
                                       (find.minion == self) ? "ourself" :
                                       find.minion->GetUniqueName().c_str()));
                    }

                    if (wasAdvertiseMinion) {
                        assert(directMinions != 0);
                        advertise.minion = self;
                        advertise.active = false;

                        if (directMinions > 1) {
                            // We had more than 2 minions, so at least one is
                            // idle.  Select the next available minion and to
                            // do the advertising for us.
                            NextDirectMinion(advertise.minion);
                        }
                        // ... else we had 2 minions. The one that was
                        // advertising for us left, so now we must advertise
                        // for ourself.

                        QCC_DbgPrintf(("Selected %s as our advertise minion.",
                                       (advertise.minion == self) ? "ourself" :
                                       advertise.minion->GetUniqueName().c_str()));
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
        if (!node->FindNamesEmpty() && (node != self)) {
            QCC_DbgPrintf(("Notify %s of the name changes.", node->GetBusAddress().ToString().c_str()));
            if (oldAdInfo.Size() > 0) {
                status = SendFoundNamesChange(node, oldAdInfo, true);
            }
            if (newAdInfo.Size() > 0) {
                status = SendFoundNamesChange(node, newAdInfo, false);
            }
        }
    }
    lock.Unlock();

    QCC_DbgPrintf(("Notify ourself of the name changes."));
    BTNodeDB::const_iterator node;
    // Tell ourself about the names (This is best done outside the noseStateLock just in case).
    for (node = oldAdInfo.Begin(); node != oldAdInfo.End(); ++node) {
        if (((*node)->AdvertiseNamesSize() > 0) && (*node != self)) {
            vector<String> vectorizedNames;
            vectorizedNames.reserve((*node)->AdvertiseNamesSize());
            vectorizedNames.assign((*node)->GetAdvertiseNamesBegin(), (*node)->GetAdvertiseNamesEnd());
            bt.FoundNamesChange((*node)->GetGUID(), vectorizedNames, (*node)->GetBusAddress().addr, (*node)->GetBusAddress().psm, true);
        }
    }
    for (node = newAdInfo.Begin(); node != newAdInfo.End(); ++node) {
        if (((*node)->AdvertiseNamesSize() > 0) && (*node != self)) {
            vector<String> vectorizedNames;
            vectorizedNames.reserve((*node)->AdvertiseNamesSize());
            vectorizedNames.assign((*node)->GetAdvertiseNamesBegin(), (*node)->GetAdvertiseNamesEnd());
            bt.FoundNamesChange((*node)->GetGUID(), vectorizedNames, (*node)->GetBusAddress().addr, (*node)->GetBusAddress().psm, false);
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

    FillAdvertiseNamesMsgArgs(nodeList, adInfo);

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
            MsgArg args[SIG_NAME_OP_SIZE];
            size_t argsSize = ArraySize(args);
            MsgArg::Set(args, argsSize, SIG_NAME_OP,
                        listenAddr.addr.GetRaw(),
                        listenAddr.psm,
                        name.c_str());
            status = Signal(master->GetServiceName().c_str(), 0, signal, args, argsSize);
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
    char* nameStr;
    uint64_t addrRaw;
    uint16_t psm;

    QStatus status = msg->GetArgs(SIG_NAME_OP, &addrRaw, &psm, &nameStr);

    if (status == ER_OK) {
        BTBusAddress addr(addrRaw, psm);
        BTNodeInfo node = nodeDB.FindNode(addr);

        if (node->IsValid()) {
            QCC_DbgPrintf(("%s %s to the list of %s names for %s.",
                           addName ? "Adding" : "Removing",
                           nameStr,
                           findOp ? "find" : "advertise",
                           node->GetBusAddress().ToString().c_str()));

            // All nodes need to be registered via SetState
            qcc::String name(nameStr);
            lock.Lock();
            if (addName) {
                nameCollection->AddName(name, node);
            } else {
                nameCollection->RemoveName(name, node);
            }

            if (IsMaster()) {
                UpdateDelegations(advertise, listening);
                UpdateDelegations(find);
                QCC_DEBUG_ONLY(DumpNodeStateTable());

                if (findOp) {
                    if (addName && (node->FindNamesSize() == 1)) {
                        // Prime the name cache for our minion
                        nodeDB.Lock();
                        status = SendFoundNamesChange(node, nodeDB, false);
                        nodeDB.Unlock();
                    } else if (!addName && node->FindNamesEmpty()) {
                        // Clear out the name cache for our minion
                        nodeDB.Lock();
                        status = SendFoundNamesChange(node, nodeDB, true);
                        nodeDB.Unlock();
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
        } else {
            QCC_LogError(ER_FAIL, ("Did not find node %s in node DB", addr.ToString().c_str()));
        }
    } else {
        QCC_LogError(status, ("Processing msg args"));
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
    qcc::String sender = msg->GetSender();
    BTEndpoint* ep = static_cast<BTEndpoint*>(bt.LookupEndpoint(sender));

    if ((ep == NULL) ||
        (ep->GetBTBusAddress().IsValid()) ||
        (nodeDB.FindNode(ep->GetRemoteName())->IsValid())) {
        /* We don't acknowledge anyone calling the SetState method call who
         * fits into one of these categories:
         *
         * - Not a Bluetooth endpoint
         * - Not an incomming connection
         * - Has already called SetState
         *
         * Don't send a response as punishment >:)
         */
        if (ep) {
            bt.ReturnEndpoint(ep);
        }
        return;
    }
    bt.ReturnEndpoint(ep);

    uint8_t numConnections;
    QStatus status;
    uint64_t rawBDAddr;
    uint16_t psm;

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
    status = (status != ER_OK) ? status : msg->GetArg(1)->Get(SIG_BDADDR, &rawBDAddr);
    status = (status != ER_OK) ? status : msg->GetArg(2)->Get(SIG_PSM, &psm);
    if (status != ER_OK) {
        MethodReply(msg, "org.alljoyn.Bus.BTController.InternalError", QCC_StatusText(status));
        lock.Unlock();
        bt.Disconnect(sender);
        return;
    }

    MsgArg args[SIG_SET_STATE_OUT_SIZE];
    size_t numArgs = ArraySize(args);
    vector<MsgArg> nodeStateArgsStorage;
    vector<MsgArg> foundNodeArgsStorage;

    if (numConnections > directMinions) {
        // We are now a minion (or a drone if we have more than one direct connection)
        master = new ProxyBusObject(bus, sender.c_str(), bluetoothObjPath, 0);
        BTBusAddress addr(rawBDAddr, psm);
        masterNode = BTNodeInfo(addr, sender);

        QCC_DbgPrintf(("HandleSetState prep args"));
        FillNodeStateMsgArgs(nodeStateArgsStorage);
        FillFoundNodesMsgArgs(foundNodeArgsStorage);

    } else {
        // We are still the master
        size_t numNodeStateArgs;
        MsgArg* nodeStateArgs;
        size_t numFoundNodeArgs;
        MsgArg* foundNodeArgs;

        status = msg->GetArg(3)->Get(SIG_NODE_STATES, &numNodeStateArgs, &nodeStateArgs);
        status = (status != ER_OK) ? status : msg->GetArg(4)->Get(SIG_FOUND_NODES, &numFoundNodeArgs, &foundNodeArgs);
        if (status != ER_OK) {
            MethodReply(msg, "org.alljoyn.Bus.BTController.InternalError", QCC_StatusText(status));
            lock.Unlock();
            bt.Disconnect(sender);
            return;
        }

        BTBusAddress addr(rawBDAddr, psm);
        bool noRotateMinions = !RotateMinions();
        status = ImportState(addr, nodeStateArgs, numNodeStateArgs, foundNodeArgs, numFoundNodeArgs);
        if (status != ER_OK) {
            QCC_LogError(status, ("Dropping %s due to import state error", sender.c_str()));
            lock.Unlock();
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

    status = MsgArg::Set(args, numArgs, SIG_SET_STATE_OUT,
                         self->GetBusAddress().addr.GetRaw(),
                         self->GetBusAddress().psm,
                         nodeStateArgsStorage.size(), &nodeStateArgsStorage.front(),
                         foundNodeArgsStorage.size(), &foundNodeArgsStorage.front());
    if (status != ER_OK) {
        QCC_LogError(status, ("MsgArg::Set(%s)", SIG_SET_STATE_OUT));
        lock.Unlock();
        bt.Disconnect(sender);
        return;
    }

    status = MethodReply(msg, args, numArgs);
    if (status != ER_OK) {
        QCC_LogError(status, ("MethodReply"));
        assert(status == ER_OK);
        lock.Unlock();
        bt.Disconnect(sender);
        return;
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
                bt.StartFind(ignoreAddrs, duration);
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
        QCC_DbgPrintf(("Selected %s as our find minion.", find.minion->GetUniqueName().c_str()));

        Signal(find.minion->GetUniqueName().c_str(), 0, *find.delegateSignal, args, numArgs);
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
        QCC_DbgHLPrintf(("%s tried to delegate an advertisement to us; our master is %s",
                         msg->GetSender(),
                         IsMaster() ? "ourself" : master->GetServiceName().c_str()));
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
        uint32_t duration;

        QStatus status = msg->GetArgs(SIG_DELEGATE_AD, &uuidRev, &bdAddrRaw, &psm, &size, &entries, &duration);

        if (status == ER_OK) {
            status = ExtractAdInfo(entries, size, adInfo);
        }

        if (status == ER_OK) {
            if (adInfo.Size() > 0) {
                BDAddress bdAddr(bdAddrRaw);

                QCC_DbgPrintf(("Starting advertise for %u seconds...", duration));
                bt.StartAdvertise(uuidRev, bdAddr, psm, adInfo, duration);
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
        QCC_DbgPrintf(("Selected %s as our advertise minion.", advertise.minion->GetUniqueName().c_str()));

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
    if (IsMaster() || (strcmp(sourcePath, bluetoothObjPath) != 0)) {
        // We only accept FoundNames signals if we are not the master!
        return;
    }

    BTNodeDB adInfo;
    bool lost = (member == org.alljoyn.Bus.BTController.LostNames);
    MsgArg* entries;
    size_t size;

    QStatus status = msg->GetArgs(SIG_FOUND_NAMES, &size, &entries);

    if (status == ER_OK) {
        status = ExtractAdInfo(entries, size, adInfo);
    }

    if ((status == ER_OK) && (adInfo.Size() > 0)) {
        BTNodeDB::const_iterator nodeit;

        lock.Lock();
        for (nodeit = adInfo.Begin(); nodeit != adInfo.End(); ++nodeit) {
            vector<String> vectorizedNames;
            const BTNodeInfo& node = *nodeit;
            vectorizedNames.reserve(node->AdvertiseNamesSize());
            vectorizedNames.assign(node->GetAdvertiseNamesBegin(), node->GetAdvertiseNamesEnd());
            bt.FoundNamesChange(node->GetGUID(), vectorizedNames, node->GetBusAddress().addr, node->GetBusAddress().psm, lost);
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

    if (!nodeDB.FindNode(msg->GetSender())->IsDirectMinion()) {
        // We only handle FoundDevice or LostDevice signals from our minions.
        QCC_LogError(ER_FAIL, ("Received %s from %s who is NOT a direct minion.",
                               msg->GetMemberName(), msg->GetSender()));
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

    incomingDB.Clear();
    for (i = 0; i < numFoundNodes; ++i) {
        uint64_t connAddrRaw;
        uint16_t connPSM;
        uint32_t uuidRev;
        size_t adMapSize;
        MsgArg* adMap;
        size_t j;

        status = foundNodeArgs[i].Get(SIG_FOUND_NODE_ENTRY, &connAddrRaw, &connPSM, &uuidRev, &adMapSize, &adMap);
        if (status != ER_OK) {
            return status;
        }

        BTBusAddress connNodeAddr(BDAddress(connAddrRaw), connPSM);
        if ((self->GetBusAddress() == connNodeAddr) || nodeDB.FindNode(connNodeAddr)->IsValid()) {
            // Don't add ourself or any node on our piconet/scatternet to foundNodeDB.
            QCC_DbgPrintf(("Skipping nodes with connect address: %s", connNodeAddr.ToString().c_str()));
            continue;
        }

        BTNodeInfo connNode = incomingDB.FindNode(connNodeAddr);
        if (!connNode->IsValid()) {
            connNode = BTNodeInfo(connNodeAddr);
        }

        for (j = 0; j < adMapSize; ++j) {
            char* guidStr;
            uint64_t rawBdAddr;
            uint16_t psm;
            size_t anSize;
            MsgArg* anList;
            size_t k;

            status = adMap[j].Get(SIG_AD_NAME_MAP_ENTRY, &guidStr, &rawBdAddr, &psm, &anSize, &anList);
            if (status != ER_OK) {
                return status;
            }

            BTBusAddress nodeAddr(BDAddress(rawBdAddr), psm);
            BTNodeInfo node = (nodeAddr == connNode->GetBusAddress()) ? connNode : BTNodeInfo(nodeAddr);

            QCC_DbgPrintf(("Processing names for new found device %s (connectable via %s):",
                           nodeAddr.ToString().c_str(),
                           connNodeAddr.ToString().c_str()));

            node->SetGUID(String(guidStr));
            node->SetConnectNode(connNode);
            node->SetUUIDRev(uuidRev);
            for (k = 0; k < anSize; ++k) {
                char* n;
                status = anList[k].Get(SIG_NAME, &n);
                if (status != ER_OK) {
                    return status;
                }
                QCC_DbgPrintf(("    Ad Name: %s", n));
                String name(n);
                node->AddAdvertiseName(name);
            }
            incomingDB.AddNode(node);
            addedDB.AddNode(node);
        }
    }

    foundNodeDB.UpdateDB(&incomingDB, &staleDB);
    DistributeAdvertisedNameChanges(addedDB, staleDB);

    ++directMinions;

    if (find.minion == self) {
        NextDirectMinion(find.minion);
        QCC_DbgPrintf(("Selected %s as our find minion.", find.minion->GetUniqueName().c_str()));
    }

    if ((advertise.minion == self) && (!UseLocalAdvertise())) {
        NextDirectMinion(advertise.minion);
        QCC_DbgPrintf(("Selected %s as our advertise minion.", advertise.minion->GetUniqueName().c_str()));
    }

    return ER_OK;
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


void BTController::FillAdvertiseNamesMsgArgs(vector<MsgArg>& args, const BTNodeDB& adInfo)
{
    args.reserve(adInfo.Size());
    BTNodeDB::const_iterator it;
    for (it = adInfo.Begin(); it != adInfo.End(); ++it) {
        const BTNodeInfo& node = *it;
        QCC_DbgPrintf(("    Advertised node %s:", node->GetBusAddress().ToString().c_str()));
        NameSet::const_iterator nit;

        vector<const char*> nodeAdNames;
        nodeAdNames.reserve(node->AdvertiseNamesSize());
        for (nit = node->GetAdvertiseNamesBegin(); nit != node->GetAdvertiseNamesEnd(); ++nit) {
            QCC_DbgPrintf(("        Ad name: %s", nit->c_str()));
            nodeAdNames.push_back(nit->c_str());
        }

        args.push_back(MsgArg(SIG_AD_NAME_MAP_ENTRY,
                              node->GetGUID().c_str(),
                              node->GetBusAddress().addr.GetRaw(),
                              node->GetBusAddress().psm,
                              nodeAdNames.size(), &nodeAdNames.front()));
        args.back().Stabilize();
    }
}


void BTController::FillNodeStateMsgArgs(vector<MsgArg>& args) const
{
    args.reserve(nodeDB.Size());
    BTNodeDB::const_iterator it;
    nodeDB.Lock();
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


void BTController::FillFoundNodesMsgArgs(vector<MsgArg>& args) const
{
    BTNodeDB::const_iterator it;
    map<BTBusAddress, BTNodeDB> xformMap;
    foundNodeDB.Lock();
    for (it = foundNodeDB.Begin(); it != foundNodeDB.End(); ++it) {
        xformMap[(*it)->GetConnectAddress()].AddNode(*it);
    }
    foundNodeDB.Unlock();

    args.reserve(xformMap.size());
    map<BTBusAddress, BTNodeDB>::const_iterator xmit;
    for (xmit = xformMap.begin(); xmit != xformMap.end(); ++xmit) {
        vector<MsgArg> adNamesArgs;

        BTNodeInfo connNode = xmit->second.FindNode(xmit->first);
        assert(connNode->IsValid());

        FillAdvertiseNamesMsgArgs(adNamesArgs, xmit->second);
        args.push_back(MsgArg(SIG_FOUND_NODE_ENTRY,
                              xmit->first.addr.GetRaw(),
                              xmit->first.psm,
                              connNode->GetUUIDRev(),
                              adNamesArgs.size(), &adNamesArgs.front()));
        args.back().Stabilize();
    }
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
                Signal(advertise.minion->GetUniqueName().c_str(), 0, *advertise.delegateSignal, advertise.args, advertise.argsSize);
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
                       minion->GetUniqueName().c_str(),
                       (minion == bto.find.minion) ? "find" : "advertise"));
        bto.Signal(minion->GetUniqueName().c_str(), 0, *delegateSignal, args, argsSize);
        bto.lock.Unlock();

        // Manually re-arm alarm since automatically recurring alarms cannot be stopped.
        StartAlarm();
    }
}


QStatus BTController::NameArgInfo::SendDelegateSignal()
{
    QCC_DbgPrintf(("Sending %s signal to %s", delegateSignal->name.c_str(),
                   minion->GetUniqueName().c_str()));
    assert(minion != bto.self);

    return bto.Signal(minion->GetUniqueName().c_str(), 0, *delegateSignal, args, argsSize);
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


BTController::AdvertiseNameArgInfo::AdvertiseNameArgInfo(BTController& bto, qcc::Timer& dispatcher) :
    NameArgInfo(bto, SIG_DELEGATE_AD_SIZE, dispatcher)
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
                                    node->GetGUID().c_str(),
                                    node->GetBusAddress().addr.GetRaw(),
                                    node->GetBusAddress().psm,
                                    names.size(), &names.front()));
    }

    MsgArg::Set(args, argsSize, SIG_DELEGATE_AD,
                bto.masterUUIDRev,
                bto.self->GetBusAddress().addr.GetRaw(),
                bto.self->GetBusAddress().psm,
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
                bto.self->GetBusAddress().addr.GetRaw(),
                bto.self->GetBusAddress().psm,
                static_cast<size_t>(0), NULL,
                static_cast<uint32_t>(0));
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
                bt::INVALID_UUIDREV,
                0ULL,
                bt::INVALID_PSM,
                static_cast<size_t>(0), NULL,
                static_cast<uint32_t>(0));
    assert(argsSize == this->argsSize);
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


BTController::FindNameArgInfo::FindNameArgInfo(BTController& bto, qcc::Timer& dispatcher) :
    NameArgInfo(bto, SIG_DELEGATE_FIND_SIZE, dispatcher)
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
    size_t argsSize = this->argsSize;
    const char* rdest = bto.IsMaster() ? bto.bus.GetUniqueName().c_str() : resultDest.c_str();

    bto.nodeDB.Lock();
    ignoreAddrsCache.clear();
    ignoreAddrsCache.reserve(bto.nodeDB.Size());
    BTNodeDB::const_iterator it;
    for (it = bto.nodeDB.Begin(); it != bto.nodeDB.End(); ++it) {
        ignoreAddrsCache.push_back((*it)->GetBusAddress().addr.GetRaw());
    }
    bto.nodeDB.Unlock();

    MsgArg::Set(args, argsSize, SIG_DELEGATE_FIND,
                rdest,
                ignoreAddrsCache.size(), &ignoreAddrsCache.front(),
                bto.RotateMinions() ? DELEGATE_TIME : (uint32_t)0);
    assert(argsSize == this->argsSize);

    dirty = false;
}


void BTController::FindNameArgInfo::ClearArgs()
{
    QCC_DbgTrace(("BTController::FindNameArgInfo::ClearArgs()"));
    size_t argsSize = this->argsSize;

    MsgArg::Set(args, argsSize, SIG_DELEGATE_FIND,
                NULL,
                static_cast<size_t>(0), NULL,
                static_cast<uint32_t>(0));
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
        QCC_DbgPrintf(("    %s %s (%s):",
                       node->GetBusAddress().ToString().c_str(),
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
#endif


}
