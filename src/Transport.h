/**
 * @file
 * Transport is an abstract base class implemented by physical
 * media interfaces such as TCP, UNIX, Local and Bluetooth.
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
#ifndef _ALLJOYN_TRANSPORT_H
#define _ALLJOYN_TRANSPORT_H

#ifndef __cplusplus
#error Only include Transport.h in C++ code.
#endif

#include <qcc/platform.h>
#include <qcc/String.h>
#include <map>
#include <vector>
#include <alljoyn/Message.h>
#include <alljoyn/TransportMask.h>
#include <alljoyn/Session.h>
#include <Status.h>

namespace ajn {

class RemoteEndpoint;


/**
 * %TransportListener is an abstract base class that provides aynchronous notifications about
 * transport related events.
 */
class TransportListener {
  public:
    /**
     * Virtual destructor for derivable class.
     */
    virtual ~TransportListener() { }

    /**
     * Called when a transport has found a bus to connect to with a set of bus names.
     *
     * @param busAddr   The address of the bus formatted as a string that can be passed to
     *                  createEndpoint
     * @param guid      GUID associated with this advertisement.
     * @param transport Transport that sent the advertisment.
     * @param names    The list of bus names that the bus has advertised or NULL if transport cannot determine list.
     * @param timer    Time to live for this set of names. (0 implies that the name is gone.)
     */
    virtual void FoundNames(const qcc::String& busAddr,
                            const qcc::String& guid,
                            TransportMask transport,
                            const std::vector<qcc::String>* names,
                            uint8_t timer) = 0;

    /**
     * Called when a transport gets a surprise disconnect from a remote bus.
     *
     * @param busAddr       The address of the bus formatted as a string.
     */
    virtual void BusConnectionLost(const qcc::String& busAddr) = 0;

    /**
     * Get a list of the currently advertised names for this transport listener
     *
     * @param names  A vector containing the advertised names.
     */
    virtual void GetAdvertisedNames(std::vector<qcc::String>& names) = 0;

};


/**
 * %Transport is an abstract base class implemented by physical
 * media interfaces such as TCP, UNIX, Local and Bluetooth.
 */
class Transport {
  public:

    /**
     * Destructor
     */
    virtual ~Transport() { }

    /**
     * Start the transport and associate it with a router.
     *
     * @return
     *      - ER_OK if successful.
     *      - an error status otherwise.
     */
    virtual QStatus Start() = 0;

    /**
     * Stop the transport.
     *
     * @return
     *      - ER_OK if successful.
     *      - an error status otherwise.
     */
    virtual QStatus Stop() = 0;

    /**
     * Pend the caller until the transport stops.
     *
     * @return
     *      - ER_OK if successful
     *      - an error status otherwise.
     */
    virtual QStatus Join() = 0;

    /**
     * Determine if this transport is running. Running means Start() has been called.
     *
     * @return  Returns true if the transport is running.
     */
    virtual bool IsRunning() = 0;

    /**
     * Get the transport mask for this transport
     *
     * @return the TransportMask for this transport.
     */
    virtual TransportMask GetTransportMask() = 0;

    /**
     * Get the listen spec for a given set of session options.
     *
     * @return listenSpec (busAddr) to use for given session options (empty string if
     *         session opts are incompatible with this transport.
     */
    virtual qcc::String GetListenAddress(const SessionOpts& opts) { return ""; }

    /**
     * Normalize a transport specification.
     * Given a transport specification, convert it into a form which is guaranteed to have a one-to-one
     * relationship with a transport.
     *
     * @param inSpec    Input transport connect spec.
     * @param outSpec   Output transport connect spec.
     * @param argMap    Parsed parameter map.
     * @return ER_OK if successful.
     */
    virtual QStatus NormalizeTransportSpec(const char* inSpec,
                                           qcc::String& outSpec,
                                           std::map<qcc::String, qcc::String>& argMap) = 0;

    /**
     * Connect to a specified remote AllJoyn/DBus address.
     *
     * @param connectSpec    Transport specific key/value args used to configure the client-side endpoint.
     *                       The form of this string is @c "<transport>:<key1>=<val1>,<key2>=<val2>..."
     * @return
     *      - ER_OK if successful.
     *      - an error status otherwise.
     */
    virtual QStatus Connect(const char* connectSpec, RemoteEndpoint** newep) = 0;

    /**
     * Disconnect from a specified AllJoyn/DBus address.
     *
     * @param connectSpec    The connectSpec used in Connect.
     *
     * @return
     *      - ER_OK if successful.
     *      - an error status otherwise.
     */
    virtual QStatus Disconnect(const char* connectSpec) = 0;

    /**
     * Start listening for incomming connections on a specified bus address.
     *
     * @param listenSpec  Transport specific key/value args that specify the physical interface to listen on.
     *                    The form of this string is "<transport>:<key1>=<val1>,<key2>=<val2>...[;]"
     *
     * @return ER_OK if successful.
     */
    virtual QStatus StartListen(const char* listenSpec) = 0;

    /**
     * Stop listening for incomming connections on a specified bus address.
     *
     * @param listenSpec  Transport specific key/value args that specify the physical interface to listen on.
     *                    The form of this string is @c "<transport>:<key1>=<val1>,<key2>=<val2>...[;]"
     *
     * @return
     *      - ER_OK if successful.
     *      - an error status otherwise.
     */
    virtual QStatus StopListen(const char* listenSpec) = 0;

    /**
     * Set a listener for transport related events.
     * There can only be one listener set at a time. Setting a listener
     * implicitly removes any previously set listener.
     *
     * @param listener  Listener for transport related events.
     */
    virtual void SetListener(TransportListener* listener) = 0;

    /**
     * Start discovering remotely advertised names that match prefix.
     *
     * @param namePrefix    Well-known name prefix.
     *
     * @return
     *      - ER_OK if successful.
     *      - an error status otherwise (returns ER_BUS_NO_LISTENER by default).
     */
    virtual void EnableDiscovery(const char* namePrefix) = 0;

    /**
     * Stop discovering remotely advertised names that match prefix.
     *
     * @param namePrefix    Well-known name prefix.
     *
     */
    virtual void DisableDiscovery(const char* namePrefix) = 0;

    /**
     * Start advertising a well-known name with the given quality of service.
     *
     * @param advertiseName   Well-known name to add to list of advertised names.
     *
     * @return
     *      - ER_OK if successful.
     *      - an error status otherwise.
     */
    virtual QStatus EnableAdvertisement(const qcc::String& advertiseName) = 0;

    /**
     * Stop advertising a well-known name with a given quality of service.
     *
     * @param advertiseName   Well-known name to remove from list of advertised names.
     * @param nameListEmpty   Indicates whether advertise name list is completely empty (safe to disable OTA advertising).
     */
    virtual void DisableAdvertisement(const qcc::String& advertiseName, bool nameListEmpty) = 0;

    /**
     * Returns the name of the transport
     */
    virtual const char* GetTransportName() = 0;

    /**
     * Indicates whether this transport may be used for a connection between
     * an application and the daemon on the same machine or not.
     *
     * @return  true indicates this transport may be used for local connections.
     */
    virtual bool LocallyConnectable() const = 0;

    /**
     * Indicates whether this transport may be used for a connection between
     * an application and the daemon on a different machine or not.
     *
     * @return  true indicates this transport may be used for external connections.
     */
    virtual bool ExternallyConnectable() const = 0;

    /**
     * Helper used to parse client/server arg strings
     *
     * @param transportName  Name of transport to match in args.
     * @param args      Transport argument string of form "<transport>:<key0>=<val0>,<key1>=<val1>[;]"
     * @param argMap    [OUT] A maps or args matching the given transport name.
     * @return ER_OK if successful.
     */
    static QStatus ParseArguments(const char* transportName,
                                  const char* args,
                                  std::map<qcc::String, qcc::String>& argMap);
};

}

#endif
