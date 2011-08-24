/**
 * @file
 * LaunchdTransport is a specialization of UnixTransport for launchd created sockets.
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
#ifndef _ALLJOYN_LAUNCHDTRANSPORT_H
#define _ALLJOYN_LAUNCHDTRANSPORT_H

#ifndef __cplusplus
#error Only include LaunchdTransport.h in C++ code.
#endif

#include "UnixTransport.h"

namespace ajn {

/**
 * @brief A class for Launchd Transports used in clients and services.
 *
 * The LaunchdTransport class has different incarnations depending on whether or
 * not an instantiated endpoint using the transport resides in a daemon, or on
 * a service or client.  The differences between these versions revolves around
 * whether or not a server thread is listening; and routing and discovery. This
 * class provides a specialization of class UnixTransport for use by clients and
 * services.
 */
class LaunchdTransport : public UnixTransport {

  public:
    /**
     * Create a Launchd Transport.
     *
     * @param bus  The bus associated with this transport.
     */
    LaunchdTransport(BusAttachment& bus)
        : UnixTransport(bus) { }

    /**
     * Destructor
     */
    ~LaunchdTransport() { }

    /**
     * Normalize a transport specification.
     * Given a transport specification, convert it into a form which is guaranteed to have a one-to-one
     * relationship with a transport.
     *
     * @param inSpec    Input transport connect spec.
     * @param outSpec   Output transport connect spec.
     * @param argMap    Parsed parameter map.
     *
     * @return ER_OK if successful.
     */
    QStatus NormalizeTransportSpec(const char* inSpec, qcc::String& outSpec, std::map<qcc::String, qcc::String>& argMap) const;

    /**
     * Connect to a specified remote AllJoyn/DBus address.
     *
     * @param connectSpec    Transport specific key/value args used to configure the client-side endpoint.
     *                       The form of this string is @c "<transport>:<key1>=<val1>,<key2>=<val2>..."
     *                             - Valid transport is @c "launchd". All others ignored.
     *                             - Valid keys are:
     *                                 - @c env = Environment variable with path name for AF_UNIX socket
     * @param opts           Requested sessions opts.
     * @param newep          [OUT] Endpoint created as a result of successful connect.
     * @return
     *      - ER_OK if successful.
     *      - an error status otherwise.
     */
    QStatus Connect(const char* connectSpec, const SessionOpts& opts, RemoteEndpoint** newep);

    /**
     * Disconnect from a specified AllJoyn/DBus address.
     *
     * @param connectSpec    The connectSpec used in Connect.
     *
     * @return
     *      - ER_OK if successful.
     *      - an error status otherwise.
     */
    QStatus Disconnect(const char* connectSpec);

    /**
     * Returns the name of this transport
     */
    const char* GetTransportName() const { return TransportName(); }

    /**
     * Name of transport used in transport specs.
     *
     * @return name of transport: @c "launchd".
     */
    static const char* TransportName() { return "launchd"; }

  private:
    QStatus GetUnixTransportSpec(const char* launchdConnectSpec, qcc::String& unixConnectSpec);

};

}

#endif
