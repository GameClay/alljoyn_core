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

#include <qcc/String.h>
#include <qcc/GUID.h>
#include <qcc/Mutex.h>
#include <qcc/Thread.h>
#include <qcc/Stream.h>

#include "Transport.h"
#include "RemoteEndpoint.h"

#include <Status.h>

namespace ajn {

/**
 * Class for a Bluetooth endpoint
 */
class BTEndpoint;

/**
 * %BTTransport is an implementation of Transport for Bluetooth.
 */
class BTTransport : public Transport, public RemoteEndpoint::EndpointListener, public qcc::Thread {
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
     * Start advertising AllJoyn capability.
     *
     * @param  advertiseName   Name to be advertised.
     */
    void EnableAdvertisement(const qcc::String& advertiseName);

    /**
     * Stop advertising AllJoyn capability.
     *
     * @param  advertiseName   Name to be advertised.
     * @param  listIsEmtpy     Indicates whether the advertise list is empty.
     */
    void DisableAdvertisement(const qcc::String& advertiseName, bool listIsEmpty);

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

  protected:
    /**
     * Thread entry point.
     *
     * @param arg  Unused thread entry arg.
     */
    qcc::ThreadReturn STDCALL Run(void* arg);

  private:

    class BTAccessor;
    BusAttachment& bus;                                      /**< The message bus for this transport */
    BTAccessor* btAccessor;                        /**< Object for accessing the Bluetooth device */
    std::map<qcc::String, qcc::String> serverArgs; /**< Map of server configuration args */
    std::vector<BTEndpoint*> threadList;            /**< List of active BT endpoints */
    qcc::Mutex threadListLock;                     /**< Mutex that protects threadList */
    TransportListener* listener;
    bool transportIsStopping;                      /**< The transport has recevied a stop request */

};

}

#endif
