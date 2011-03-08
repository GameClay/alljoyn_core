/**
 * @file
 * BTTransport is an implementation of Transport for Bluetooth.
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
#ifndef _ALLJOYNBTTRANSPORT_H
#define _ALLJOYNBTTRANSPORT_H

#ifndef __cplusplus
#error Only include BTTransport.h in C++ code.
#endif

#include <qcc/platform.h>
#include <vector>

#include <qcc/GUID.h>
#include <qcc/Mutex.h>
#include <qcc/Stream.h>
#include <qcc/String.h>
#include <qcc/Thread.h>

#include <alljoyn/QosInfo.h>

#include "BDAddress.h"
#include "BTController.h"
#include "RemoteEndpoint.h"
#include "Transport.h"

#include <Status.h>

namespace ajn {

#define ALLJOYN_BT_VERSION_NUM_ATTR    0x400
#define ALLJOYN_BT_CONN_ADDR_ATTR      0x401
#define ALLJOYN_BT_L2CAP_PSM_ATTR      0x402
#define ALLJOYN_BT_RFCOMM_CH_ATTR      0x403
#define ALLJOYN_BT_ADVERTISEMENTS_ATTR 0x404

#define ALLJOYN_BT_UUID_BASE "-1c25-481f-9dfb-59193d238280"

/**
 * Class for a Bluetooth endpoint
 */
class BTEndpoint;

/**
 * %BTTransport is an implementation of Transport for Bluetooth.
 */
class BTTransport :
    public Transport,
    public RemoteEndpoint::EndpointListener,
    public qcc::Thread,
    public BluetoothDeviceInterface {

    friend class BTController;

  public:
    /**
     * Returns the name of this transport
     */
    static const char* TransportName()  { return "bluetooth"; }

    /**
     * Normalize a bluetooth transport specification.
     *
     * @param inSpec    Input transport connect spec.
     * @param[out] outSpec   Output transport connect spec.
     * @param[out] argMap    Map of connect parameters.
     * @return
     *      - ER_OK if successful.
     *      - ER_BUS_BAD_TRANSPORT_ARGS  is unable to parse the Input transport connect specification
     */
    QStatus NormalizeTransportSpec(const char* inSpec, qcc::String& outSpec, std::map<qcc::String, qcc::String>& argMap);

    /**
     * Create a Bluetooth connection based Transport.
     *
     * @param bus  The bus associated with this transport.
     */
    BTTransport(BusAttachment& bus);

    /**
     * Destructor
     */
    ~BTTransport();

    /**
     * Start the transport and associate it with a router.
     *
     * @return
     *      - ER_OK if successful.
     *      - ER_BUS_TRANSPORT_NOT_STARTED if unsuccessful
     */
    QStatus Start();

    /**
     * Stop the transport.
     * @return
     *      - ER_OK if successful.
     *      - ER_EXTERNAL_THREAD if unable to stop because operation not supported on external thread wrapper
     */
    QStatus Stop(void);

    /**
     * Pend the caller until the transport stops.
     * @return
     *      - ER_OK if successful.
     *      - ER_OS_ERROR if unable to join (Underlying OS has indicated an error)
     */
    QStatus Join(void);

    /**
     * Determine if this transport is running. Running means Start() has been called.
     *
     * @return  Returns true if the transport is running.
     */
    bool IsRunning() { return Thread::IsRunning(); }

    /**
     * Connect to a remote bluetooth devices
     *
     * @param connectSpec key/value arguments used to configure the client-side endpoint.
     *                    The form of this string is @c "<transport>:<key1>=<val1>,<key2>=<val2>..."
     *                    - Valid transport is "bluetooth". All others ignored.
     *                    - Valid keys are:
     *                        - @c addr = Bluetooth device address
     *                        - @c name = Bluetooth Bus name
     *
     * @return
     *      - ER_OK if successful.
     *      - ER_BUS_BAD_TRANSPORT_ARGS if unable to parse the @c connectSpec param
     *      - An error status otherwise
     */
    QStatus Connect(const char* connectSpec, RemoteEndpoint** newep);

    /**
     * Disconnect a bluetooth endpoint
     *
     * @param connectSpec   The connect spec to be disconnected.
     *
     * @return
     *      - ER_OK if successful.
     *      - an error status otherwise.
     */
    QStatus Disconnect(const char* connectSpec);

    /**
     * Start listening for incomming connections on a specified bus address.
     *
     * @param listenSpec  Transport specific key/value arguments that specify the physical interface to listen on.
     *                      The form of this string is @c "<transport>:<key1>=<val1>,<key2>=<val2>..."
     *                      - Valid transport is "bluetooth". All others ignored.
     *                      - Valid keys are:
     *                          - @c addr = Bluetooth device address
     *                          - @c name = Bluetooth Bus name
     *
     * @return ER_OK
     */
    QStatus StartListen(const char* listenSpec);

    /**
     * Stop listening for incomming connections on a specified bus address.
     * This method cancels a StartListen request. Therefore, the listenSpec must match previous call to StartListen()
     *
     * @param listenSpec  Transport specific key/value arguments that specify the physical interface to listen on.
     *                      The form of this string is @c "<transport>:<key1>=<val1>,<key2>=<val2>..."
     *                       - Valid transport is "bluetooth". All others ignored.
     *                       - Valid keys are:
     *                          - @c addr = Bluetooth device address
     *                          - @c name = Bluetooth Bus name
     *
     * @return ER_OK
     *
     * @see BTTransport::StartListen
     */
    QStatus StopListen(const char* listenSpec);

    /**
     * Function for the BT Accessor to inform a change in the
     * power/availablity of the Bluetooth device.
     *
     * @param avail    true if BT device is powered on and available, false otherwise.
     */
    void BTDeviceAvailable(bool avail) { btController->BTDeviceAvailable(avail); }

    /**
     * Callback for BTEndpoint thead exit.
     *
     * @param endpoint   BTEndpoint instance that has exited.
     */
    void EndpointExit(RemoteEndpoint* endpoint);

    /**
     * Register a listener for transport related events.
     *
     * @param listener The listener to register. If this parameter is NULL the current listener is removed
     *
     * @return ER_OK
     */
    void SetListener(TransportListener* listener) { this->listener = listener; }

    /**
     * Start discovering busses to connect to
     *
     * @param namePrefix    First part of a well known name to search for.
     */
    void EnableDiscovery(const char* namePrefix);

    /**
     * Stop discovering busses to connect to
     *
     * @param namePrefix    First part of a well known name to stop searching for.
     */
    void DisableDiscovery(const char* namePrefix);

    /**
     * Start advertising a well-known name with the given quality of service.
     *
     * @param advertiseName   Well-known name to add to list of advertised names.
     * @param advQos          Quality of service for advertisement.
     * @return
     *      - ER_OK if successful.
     *      - an error status otherwise.
     */
    QStatus EnableAdvertisement(const qcc::String& advertiseName, const QosInfo& advQos);

    /**
     * Stop advertising a well-known name with a given quality of service.
     *
     * @param advertiseName   Well-known name to remove from list of advertised names.
     * @param advQos          Quality of service for advertisement (NULL indicates all/any qos).
     * @param nameListEmpty   Indicates whether advertise name list is completely empty (safe to disable OTA advertising).
     */
    void DisableAdvertisement(const qcc::String& advertiseName, const QosInfo* advQos, bool nameListEmpty);

    /**
     * Returns the name of this transport
     * @return The name of this transport.
     */
    const char* GetTransportName()  { return TransportName(); };

    /**
     * Indicates whether this transport may be used for a connection between
     * an application and the daemon on the same machine or not.
     *
     * @return  true indicates this transport may be used for local connections.
     */
    bool LocallyConnectable() const { return false; }

    /**
     * Indicates whether this transport may be used for a connection between
     * an application and the daemon on a different machine or not.
     *
     * @return  true indicates this transport may be used for external connections.
     */
    bool ExternallyConnectable() const { return true; }

    /**
     * Incomplete class for BT stack specific state.
     */
    class BTAccessor;

  protected:
    /**
     * Thread entry point.
     *
     * @param arg  Unused thread entry arg.
     */
    qcc::ThreadReturn STDCALL Run(void* arg);

  private:

    /**
     * Internal connect method to establish a bus connection to a given BD Address.
     *
     * @param bdAddr    BD Address of the device to connect to.
     * @param channel   RFCOMM channel to connect to (if psm is 0).
     * @param psm       L2CAP PSM to connect to.
     *
     * @return  ER_OK if successful.
     */
    QStatus Connect(const BDAddress& bdAddr,
                    uint8_t channel,
                    uint16_t psm,
                    RemoteEndpoint** newep);
    QStatus Connect(const BDAddress& bdAddr,
                    uint8_t channel,
                    uint16_t psm)
    {
        return Connect(bdAddr, channel, psm, NULL);
    }

    /**
     * Internal disconnect method to remove a bus connection from a given BD Address.
     *
     * @param bdAddr    BD Address of the device to connect to.
     *
     * @return  ER_OK if successful.
     */
    QStatus Disconnect(const BDAddress& bdAddr);

    /**
     * Called by BTAccessor to inform transport of an AllJoyn capable device.
     * Processes the found device information or sends it to the BT topology
     * manager master as appropriate.
     *
     * @param adBdAddr  BD Address of the device advertising names.
     * @param uuidRev   UUID revision number of the device that was found.
     *
     * @return  ER_OK if successful.
     */
    QStatus FoundDevice(const BDAddress& bdAddr,
                        uint32_t uuidRev);

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
    virtual void StartFind(uint32_t ignoreUUID, uint32_t duration = 0);

    /**
     * Stop the find operation.
     */
    virtual void StopFind();

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
                                const BluetoothDeviceInterface::AdvertiseInfo& adInfo,
                                uint32_t duration = 0);

    /**
     * Stop the advertise operation.
     */
    virtual void StopAdvertise();

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
                          uint16_t psm);

    /**
     * Tells the Bluetooth transport to start listening for incoming connections.
     *
     * @param addr      [OUT] BD Address of the adapter listening for connections
     * @param channel   [OUT] RFCOMM channel allocated
     * @param psm       [OUT] L2CAP PSM allocated
     */
    virtual QStatus StartListen(BDAddress& addr,
                                uint8_t& channel,
                                uint16_t& psm);

    /**
     * Tells the Bluetooth transport to stop listening for incoming connections.
     */
    virtual void StopListen();

    /**
     * Retrieves the information from the specified device necessary to
     * establish a connection and get the list of advertised names.
     *
     * @param addr      BD address of the device of interest.
     * @param connAddr  [OUT] Address of the Bluetooth device accepting connections.
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
                                  BluetoothDeviceInterface::AdvertiseInfo& adInfo);


    BusAttachment& bus;                            /**< The message bus for this transport */
    BTAccessor* btAccessor;                        /**< Object for accessing the Bluetooth device */
    BTController* btController;                    /**< Bus Object that manages the BT topology */
    std::map<qcc::String, qcc::String> serverArgs; /**< Map of server configuration args */
    std::vector<BTEndpoint*> threadList;           /**< List of active BT endpoints */
    qcc::Mutex threadListLock;                     /**< Mutex that protects threadList */
    TransportListener* listener;
    bool transportIsStopping;                      /**< The transport has recevied a stop request */
    QosInfo btQos;                                 /**< Hardcoded qos for bluetooth links */
    bool btmActive;                                /**< Indicates if the Bluetooth Topology Manager is registered */
};

}

#endif
