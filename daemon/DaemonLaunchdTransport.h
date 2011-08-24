/**
 * @file
 * LaunchdTransport is a specialization of class UnixTransport for daemons talking
 * over Unix domain sockets created by launchd.
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
#ifndef _ALLJOYN_DAEMONLAUNCHDTRANSPORT_H
#define _ALLJOYN_DAEMONLAUNCHDTRANSPORT_H

#ifndef __cplusplus
#error Only include DaemonLaunchdTransport.h in C++ code.
#endif

#include "DaemonUnixTransport.h"

namespace ajn {

/**
 * @brief A class for Launchd Transports used in daemons.
 *
 * The LaunchdTransport class has different incarnations depending on whether or
 * not an instantiated endpoint using the transport resides in a daemon, or on a
 * service or client.  The differences between these versions revolves around
 * routing and discovery. This class provides a specialization of class
 * Transport for use by daemons.
 */
class DaemonLaunchdTransport : public DaemonUnixTransport {

  public:
    /**
     * Create a Launchd domain socket based Transport for use by daemons.
     *
     * @param bus  The bus associated with this transport.
     */
    DaemonLaunchdTransport(BusAttachment& bus)
        : DaemonUnixTransport(bus) { }

    /**
     * Destructor
     */
    ~DaemonLaunchdTransport() { }

    /**
     * @internal
     * @brief Normalize a transport specification.
     *
     * Given a transport specification, convert it into a form which is guaranteed to
     * have a one-to-one relationship with a connection instance.
     *
     * @param inSpec    Input transport connect spec.
     * @param outSpec   Output transport connect spec.
     * @param argMap    Parsed parameter map.
     *
     * @return ER_OK if successful.
     */
    QStatus NormalizeTransportSpec(const char* inSpec, qcc::String& outSpec, std::map<qcc::String, qcc::String>& argMap) const;

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

  protected:
    virtual QStatus ListenFd(std::map<qcc::String, qcc::String>& serverArgs, qcc::SocketFd& listenFd);
};

} // namespace ajn

#endif // _ALLJOYN_DAEMONUNIXTRANSPORT_H
