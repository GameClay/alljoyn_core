/**
 * @file
 * BusObject responsible for controlling/handling Bluetooth delegations.
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
#ifndef _ALLJOYN_BLUETOOTHOBJ_H
#define _ALLJOYN_BLUETOOTHOBJ_H

#include <qcc/platform.h>

#include <list>
#include <map>
#include <set>
#include <vector>

#include <qcc/String.h>
#include <qcc/StringMapKey.h>
#include <qcc/Timer.h>

#include <alljoyn/BusObject.h>
#include <alljoyn/Message.h>
#include <alljoyn/MessageReceiver.h>

#include "BDAddress.h"
#include "Bus.h"
#include "NameTable.h"


namespace ajn {

#define INVALID_UUIDREV 0

class BTController;

class BluetoothDeviceInterface {
    friend class BTController;

  public:
    typedef std::vector<qcc::String> AdvertiseNames;
    typedef std::vector<std::pair<qcc::String, AdvertiseNames> > AdvertiseInfo;

    virtual ~BluetoothDeviceInterface() { }

  private:


    /**
     * Start the find operation for AllJoyn capable devices.  A duration may
     * be specified that will result in the find operation to automatically
     * stop after the specified number of seconds.  Exclude any results from
     * any device that includes the specified UUID in its EIR.  If an AllJoyn
     * capable device is found with a UUID that does not match the ignore UUID
     * (and was not previously seen from that device), call the
     * BTController::ProcessFoundDevice() method with the appropriate
     * information.
     *
     * @param ignoreUUID    EIR UUID revision to ignore
     * @param duration      Find duration in seconds (0 = forever)
     */
    virtual void StartFind(uint32_t ignoreUUID, uint32_t duration = 0) = 0;

    /**
     * Stop the find operation.
     */
    virtual void StopFind() = 0;

    /**
     * Start the advertise operation for the given list of names.  A duration
     * may be specified that will result in the advertise operation to
     * automatically stop after the specified number of seconds.  This
     * includes setting the SDP record to contain the information specified in
     * the parameters.
     *
     * @param uuidRev   AllJoyn Bluetooth service UUID revision
     * @param bdAddr    BD address of the connectable node
     * @param channel   The RFCOMM channel number for the AllJoyn service
     * @param psm       The L2CAP PSM number for the AllJoyn service
     * @param adInfo    The complete list of names to advertise and their associated GUIDs
     * @param duration      Find duration in seconds (0 = forever)
     */
    virtual void StartAdvertise(uint32_t uuidRev,
                                const BDAddress& bdAddr,
                                uint8_t channel,
                                uint16_t psm,
                                const AdvertiseInfo& adInfo,
                                uint32_t duration = 0) = 0;


    /**
     * Stop the advertise operation.
     */
    virtual void StopAdvertise() = 0;

    /**
     * This provides the Bluetooth transport with the information needed to
     * call AllJoynObj::FoundNames and to generate the connect spec.
     *
     * @param bdAddr    BD address of the connectable node
     * @param guid      Bus GUID of the discovered bus
     * @param names     The advertised names
     * @param channel   RFCOMM channel accepting connections
     * @param psm       L2CAP PSM accepting connections
     */
    virtual void FoundBus(const BDAddress& bdAddr,
                          const qcc::String& guid,
                          const std::vector<qcc::String>& names,
                          uint8_t channel,
                          uint16_t psm) = 0;

    /**
     * Tells the Bluetooth transport to start listening for incoming connections.
     *
     * @param addr      [OUT] BD Address of the adapter listening for connections
     * @param channel   [OUT] RFCOMM channel allocated
     * @param psm       [OUT] L2CAP PSM allocated
     */
    virtual QStatus StartListen(BDAddress& addr,
                                uint8_t& channel,
                                uint16_t& psm) = 0;

    /**
     * Tells the Bluetooth transport to stop listening for incoming connections.
     */
    virtual void StopListen() = 0;

    /**
     * Retrieves the information from the specified device necessary to
     * establish a connection and get the list of advertised names.
     *
     * @param addr      BD address of the device of interest.
     * @param connAddr  [OUT] BD address of the connectable device.
     * @param uuidRev   [OUT] UUID revision number.
     * @param channel   [OUT] RFCOMM channel that is accepting AllJoyn connections.
     * @param psm       [OUT] L2CAP PSM that is accepting AllJoyn connections.
     * @param adInfo    [OUT] Advertisement information.
     */
    virtual QStatus GetDeviceInfo(const BDAddress& addr,
                                  BDAddress& connAddr,
                                  uint32_t& uuidRev,
                                  uint8_t& channel,
                                  uint16_t& psm,
                                  AdvertiseInfo& adInfo) = 0;

    virtual QStatus Connect(const BDAddress& bdAddr,
                            uint8_t channel,
                            uint16_t psm) = 0;

    virtual QStatus Disconnect(const BDAddress& bdAddr) = 0;

    /**
     * Tells the Bluetooth transport to first initiate a secondary connection
     * to the bus via the new device, then disconnect from the old device if
     * the connection to the new device was successful.
     *
     * @param oldDev    BD Address of the old device to disconnect from
     * @param newDev    BD Address of the new device to connect to
     * @param channel   RFCOMM channel of the new device to connect to
     * @param psm       L2CAP PSM of the new device to connect to
     *
     * @return  ER_OK if the connection was successfully moved.
     */
    virtual QStatus MoveConnection(const BDAddress& oldDev,
                                   const BDAddress& newDev,
                                   uint8_t channel,
                                   uint16_t psm) = 0;
};


/**
 * BusObject responsible for Bluetooth topology management.  This class is
 * used by the Bluetooth transport for the purposes of maintaining a sane
 * topology.
 */
class BTController : public BusObject, public NameListener, public qcc::AlarmListener {
  public:

    struct NodeState {
        std::set<qcc::String> advertiseNames;
        std::set<qcc::String> findNames;
        qcc::String guid;
        bool direct;
        NodeState() : direct(false) { }
    };

    typedef std::map<qcc::StringMapKey, NodeState> NodeStateMap;  // key: unique bus name

    /**
     * Constructor
     *
     * @param bus    Bus to associate with org.freedesktop.DBus message handler.
     * @param bt     Bluetooth transport to interact with.
     */
    BTController(BusAttachment& bus, BluetoothDeviceInterface& bt);

    /**
     * Destructor
     */
    ~BTController();

    /**
     * Called by the message bus when the object has been successfully registered. The object can
     * perform any initialization such as adding match rules at this time.
     */
    void ObjectRegistered();

    /**
     * Initialize and register this DBusObj instance.
     *
     * @return ER_OK if successful.
     */
    QStatus Init();

    /**
     * Send the SetState method call to the Master node we are connecting to.
     *
     * @param busName           Unique name of the endpoint with the
     *                          BTController object on the device just
     *                          connected to
     *
     * @return ER_OK if successful.
     */
    QStatus SendSetState(const qcc::String& busName);

    /**
     * Send the AdvertiseName signal to the node we believe is the Master node
     * (may actually be a drone node).
     *
     * @param name          Name to add to the list of advertise names.
     *
     * @return ER_OK if successful.
     */
    QStatus AddAdvertiseName(const qcc::String& name)
    {
        return DoNameOp(name, *org.alljoyn.Bus.BTController.AdvertiseName, true, advertise);
    }

    /**
     * Send the CancelAdvertiseName signal to the node we believe is the
     * Master node (may actually be a drone node).
     *
     * @param name          Name to remove from the list of advertise names.
     *
     * @return ER_OK if successful.
     */
    QStatus RemoveAdvertiseName(const qcc::String& name)
    {
        return DoNameOp(name, *org.alljoyn.Bus.BTController.CancelAdvertiseName, false, advertise);
    }

    /**
     * Send the FindName signal to the node we believe is the Master node (may
     * actually be a drone node).
     *
     * @param name          Name to add to the list of find names.
     *
     * @return ER_OK if successful.
     */
    QStatus AddFindName(const qcc::String& name)
    {
        return DoNameOp(name, *org.alljoyn.Bus.BTController.FindName, true, find);
    }

    /**
     * Send the CancelFindName signal to the node we believe is the Master
     * node (may actually be a drone node).
     *
     * @param findName      Name to remove from the list of find names.
     *
     * @return ER_OK if successful.
     */
    QStatus RemoveFindName(const qcc::String& name)
    {
        return DoNameOp(name, *org.alljoyn.Bus.BTController.CancelFindName, false, find);
    }

    /**
     * Process the found device information or pass it up the chain to the
     * master node if we are not the master.
     *
     * @param bdAddr   BD Address from the SDP record.
     * @param uuidRev  UUID revsision of the found bus.
     *
     * @return ER_OK if successful.
     */
    QStatus ProcessFoundDevice(const BDAddress& adBdAddr,
                               uint32_t uuidRev);

    bool OKToConnect() { return IsMaster() && (directMinions < maxConnections); }

    /**
     * Perform preparations for an outgoing connection.  For now, this just
     * turns off discovery and discoverability when there are no other
     * Bluetooth AllJoyn connections.
     */
    void PrepConnect();

    /**
     * Perform operations necessary based on the result of connect operation.
     * For now, this just restores the local discovery and discoverability
     * when the connect operation failed and there are no other Bluetooth
     * AllJoyn connections.
     */
    void PostConnect(QStatus status);

    /**
     * Send a method call to have our master connect to the remote device for
     * us.
     *
     * @param bdAddr   BD Address of device to connect to
     * @param channel  RFCOMM channel number
     * @param psm      L2CAP PSM number
     * @param delegate [OUT] Unique name of the node the proxy connect was sent to
     *
     * @return ER_OK if successful.
     */
    QStatus ProxyConnect(const BDAddress& bdAddr,
                         uint8_t channel,
                         uint16_t psm,
                         qcc::String* delegate);

    /**
     * Send a method call to have our master disconnect fromthe remote device
     * for us.
     *
     * @param bdAddr   BD Address of device to disconnect from
     *
     * @return ER_OK if successful.
     */
    QStatus ProxyDisconnect(const BDAddress& bdAddr);


    void NameOwnerChanged(const qcc::String& alias,
                          const qcc::String* oldOwner,
                          const qcc::String* newOwner);

  private:

    struct NameArgInfo {
        BTController& bto;
        NodeStateMap::const_iterator minion;
        MsgArg* args;
        const size_t argsSize;
        const InterfaceDescription::Member* delegateSignal;
        qcc::Alarm alarm;
        bool active;
        NameArgInfo(BTController& bto, size_t size, const InterfaceDescription::Member* signal) :
            bto(bto), argsSize(size), delegateSignal(signal), active(false)
        {
            args = new MsgArg[size];
        }
        virtual ~NameArgInfo() { delete[] args; }
        virtual void SetArgs() = 0;
        virtual void ClearArgs() = 0;
        virtual void AddName(const qcc::String& name, NodeStateMap::iterator it) = 0;
        virtual void RemoveName(const qcc::String& name, NodeStateMap::iterator it) = 0;
        virtual size_t Count() const = 0;
        void AddName(const qcc::String& name)
        {
            AddName(name, bto.nodeStates.find(bto.bus.GetUniqueName()));
        }
        void RemoveName(const qcc::String& name)
        {
            RemoveName(name, bto.nodeStates.find(bto.bus.GetUniqueName()));
        }
    };

    struct AdvertiseNameArgInfo : public NameArgInfo {
        uint32_t uuidRev;
        BDAddress bdAddr;
        uint8_t channel;
        uint16_t psm;
        std::vector<MsgArg> adInfoArgs;
        size_t count;
        AdvertiseNameArgInfo(BTController& bto) :
            NameArgInfo(bto, 5, bto.org.alljoyn.Bus.BTController.DelegateAdvertise), count(0)
        { }
        void AddName(const qcc::String& name, NodeStateMap::iterator it);
        void RemoveName(const qcc::String& name, NodeStateMap::iterator it);
        size_t Count() const { return count; }
        void SetArgs();
        void ClearArgs();
    };

    struct FindNameArgInfo : public NameArgInfo {
        qcc::String resultDest;
        uint32_t ignoreUUID;
        BDAddress ignoreAddr;
        std::set<qcc::String> names;
        std::vector<const char*> nameArgs;
        FindNameArgInfo(BTController& bto) :
            NameArgInfo(bto, 4, bto.org.alljoyn.Bus.BTController.DelegateFind)
        { }
        void AddName(const qcc::String& name, NodeStateMap::iterator it);
        void RemoveName(const qcc::String& name, NodeStateMap::iterator it);
        size_t Count() const { return names.size(); }
        void SetArgs();
        void ClearArgs();
    };


    struct UUIDRevCacheInfo {
        BDAddress connAddr;
        uint32_t uuidRev;
        uint32_t lastUpdate;
        uint8_t channel;
        uint16_t psm;
        BluetoothDeviceInterface::AdvertiseInfo adInfo;
        std::list<uint32_t>::iterator position;
        UUIDRevCacheInfo() : uuidRev(INVALID_UUIDREV) { }
    };

    typedef std::map<uint32_t, UUIDRevCacheInfo> UUIDRevCacheMap;

    /**
     * Send the FoundBus signal to the node interested in one or more of the
     * names on that bus.
     *
     * @param bdAddr   BD Address from the SDP record.
     * @param channel  RFCOMM channel number from in the SDP record.
     * @param psm      L2CAP PSM number from in the SDP record.
     * @param names    List of advertised names.
     *
     * @return ER_OK if successful.
     */
    QStatus SendFoundBus(const BDAddress& bdAddr,
                         uint8_t channel,
                         uint16_t psm,
                         const BluetoothDeviceInterface::AdvertiseInfo& adInfo,
                         const char* dest = NULL);


    /**
     * Send the FoundDevice signal to the Master node (may actually be a drone
     * node).
     *
     * @param bdAddr   BD Address from the SDP record.
     * @param uuidRev  UUID revsision of the found bus.
     *
     * @return ER_OK if successful.
     */
    QStatus SendFoundDevice(const BDAddress& bdAddr,
                            uint32_t uuidRev);

    /**
     * Send the one of the following specified signals to the node we believe
     * is the Master node (may actually be a drone node): FindName,
     * CancelFindName, AdvertiseName, CancelAdvertiseName.
     *
     * @param name          Name to remove from the list of advertise names.
     * @param signal        Reference to the signal to be sent.
     * @param add           Flag indicating if adding or removing a name.
     * @param nameArgInfo   Advertise of Find name arg info structure to inform of name change.
     *
     * @return ER_OK if successful.
     */
    QStatus DoNameOp(const qcc::String& findName,
                     const InterfaceDescription::Member& signal,
                     bool add,
                     NameArgInfo& nameArgInfo);

    /**
     * Handle one of the following incoming signals: FindName, CancelFindName,
     * AdvertiseName, CancelAdvertiseName.
     *
     * @param member        Member.
     * @param sourcePath    Object path of signal sender.
     * @param msg           The incoming message.
     */
    void HandleNameSignal(const InterfaceDescription::Member* member,
                          const char* sourcePath,
                          Message& msg);

    /**
     * Handle the incoming SetState method call.
     *
     * @param member    Member.
     * @param msg       The incoming message - "y(ssasas)":
     *                    - Number of direct minions
     *                    - struct:
     *                        - The node's unique bus name
     *                        - The node's GUID
     *                        - List of names to advertise
     *                        - List of names to find
     */
    void HandleSetState(const InterfaceDescription::Member* member,
                        Message& msg);

    /**
     * Handle the incoming DelegateFind signal.
     *
     * @param member        Member.
     * @param sourcePath    Object path of signal sender.
     * @param msg           The incoming message - "sas":
     *                        - Master node's bus name
     *                        - List of names to find
     *                        - Bluetooth UUID revision to ignore
     */
    void HandleDelegateFind(const InterfaceDescription::Member* member,
                            const char* sourcePath,
                            Message& msg);

    /**
     * Handle the incoming DelegateAdvertise signal.
     *
     * @param member        Member.
     * @param sourcePath    Object path of signal sender.
     * @param msg           The incoming message - "ssyqas":
     *                        - Bluetooth UUID
     *                        - BD Address
     *                        - RFCOMM channel number
     *                        - L2CAP PSM
     *                        - List of names to advertise
     */
    void HandleDelegateAdvertise(const InterfaceDescription::Member* member,
                                 const char* sourcePath,
                                 Message& msg);

    /**
     * Handle the incoming FoundBus signal.
     *
     * @param member        Member.
     * @param sourcePath    Object path of signal sender.
     * @param msg           The incoming message - "syqas":
     *                        - BD Address
     *                        - RFCOMM channel number
     *                        - L2CAP PSM
     *                        - List of advertised names
     */
    void HandleFoundBus(const InterfaceDescription::Member* member,
                        const char* sourcePath,
                        Message& msg);

    /**
     * Handle the incoming FoundDevice signal.
     *
     * @param member        Member.
     * @param sourcePath    Object path of signal sender.
     * @param msg           The incoming message - "su":
     *                        - BD Address
     *                        - UUID Revision number
     */
    void HandleFoundDevice(const InterfaceDescription::Member* member,
                           const char* sourcePath,
                           Message& msg);

    /**
     * Handle the incoming ProxyConnect method call.
     *
     * @param member    Member.
     * @param msg       The incoming message - "syq":
     *                    - BD Address of device to connect to
     *                    - RFCOMM channel number
     *                    - L2CAP PSM number
     */
    void HandleProxyConnect(const InterfaceDescription::Member* member,
                            Message& msg);

    /**
     * Handle the incoming ProxyConnect method call.
     *
     * @param member    Member.
     * @param msg       The incoming message - "s":
     *                    - BD Address of device to disconnect from
     */
    void HandleProxyDisconnect(const InterfaceDescription::Member* member,
                               Message& msg);

    /**
     * Update the internal state information for other nodes based on incoming
     * message args.
     *
     * @param num       Number of entries in the message arg list.
     * @param entries   List of message args containing the remote node's state
     *                  information.
     * @param newNode   Unique bus name of the newly connected minion (drone?) node.
     */
    void ImportState(size_t num, MsgArg* entries, const qcc::String& newNode);

    /**
     * Updates the find/advertise name information on the minion assigned to
     * perform the specified name discovery operation.  It will update the
     * delegation for find or advertise based on current activity, whether we
     * are the Master or not, if the name list changed, and if we can
     * participate in more connections.
     *
     * @param nameInfo  Reference to the advertise or find name info struct
     * @param allow     false = force stop; true = allow normal update
     */
    void UpdateDelegations(NameArgInfo& nameInfo, bool allow = true);

    /**
     * Extract advertisement information from a message arg into the internal
     * representation.
     *
     * @param adInfo    Advertisement information to be encoded
     * @param arg       Message arg to be filled with the signature "a{sas}":
     *                    - Array of dict entries:
     *                      - Key: Bus GUID associated with advertise names
     *                      - Value: Array of bus names advertised by device with associated Bus GUID
     */
    static void EncodeAdInfo(const BluetoothDeviceInterface::AdvertiseInfo& adInfo, MsgArg& arg);

    /**
     * Extract advertisement information from a message arg into the internal
     * representation.
     *
     * @param arg       Message arg with the signature "a{sas}":
     *                    - Array of dict entries:
     *                      - Key: Bus GUID associated with advertise names
     *                      - Value: Array of bus names advertised by device with associated Bus GUID
     * @param adInfo    Advertisement information to be filled
     */
    static void ExtractAdInfo(const MsgArg& arg, BluetoothDeviceInterface::AdvertiseInfo& adInfo);


    void AlarmTriggered(const qcc::Alarm& alarm, QStatus reason);

    bool IsMaster() const { return !master; }
    bool IsDrone() const { return (master && (NumMinions() > 0)); }
    bool IsMinion() const { return (master && (NumMinions() == 0)); }

    size_t NumMinions() const
    {
        size_t size;
        nodeStateLock.Lock();
        size = nodeStates.size();
        nodeStateLock.Unlock();
        return size - 1;
    }

    void NextDirectMinion(NodeStateMap::const_iterator& minion)
    {
        assert(!nodeStates.empty());
        do {
            ++minion;
            if (minion == nodeStates.end()) {
                minion = nodeStates.begin();
            }
        } while (!minion->second.direct ||
                 (master && (master->GetServiceName() == minion->first.c_str())));
    }

    bool UseLocalFind() { return directMinions == 0; }
    bool UseLocalAdvertise() { return directMinions <= 1; }

    BusAttachment& bus;
    BluetoothDeviceInterface& bt;

    ProxyBusObject* master;        // Bus Object we believe is our master

    uint8_t maxConnects;           // Maximum number of direct connections
    uint32_t uuidRev;              // Revision number for AllJoyn Bluetooth UUID
    uint8_t directMinions;         // Number of directly connected minions
    const uint8_t maxConnections;
    bool listening;


    NodeStateMap nodeStates;
    mutable qcc::Mutex nodeStateLock;

    AdvertiseNameArgInfo advertise;
    FindNameArgInfo find;

    qcc::Timer delegationTimer;

    UUIDRevCacheMap uuidRevCache;
    std::list<uint32_t> uuidRevCacheAging;

    struct {
        struct {
            struct {
                struct {
                    const InterfaceDescription* interface;
                    // Methods
                    const InterfaceDescription::Member* SetState;
                    const InterfaceDescription::Member* ProxyConnect;
                    const InterfaceDescription::Member* ProxyDisconnect;
                    // Signals
                    const InterfaceDescription::Member* FindName;
                    const InterfaceDescription::Member* CancelFindName;
                    const InterfaceDescription::Member* AdvertiseName;
                    const InterfaceDescription::Member* CancelAdvertiseName;
                    const InterfaceDescription::Member* DelegateAdvertise;
                    const InterfaceDescription::Member* DelegateFind;
                    const InterfaceDescription::Member* FoundBus;
                    const InterfaceDescription::Member* FoundDevice;
                } BTController;
            } Bus;
        } alljoyn;
    } org;

};

}

#endif
