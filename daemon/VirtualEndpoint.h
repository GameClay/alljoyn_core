/**
 * @file
 * A VirtualEndpoint is a representation of an AllJoyn endpoint that exists behind a remote
 * AllJoyn daemon.
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
#ifndef _ALLJOYN_VIRTUALENDPOINT_H
#define _ALLJOYN_VIRTUALENDPOINT_H

#include <qcc/platform.h>

#include "RemoteEndpoint.h"

#include <qcc/String.h>
#include <alljoyn/Message.h>

#include <Status.h>

namespace ajn {

/**
 * %VirtualEndpoint is an alias for a remote bus connection that exists
 * behind a remote AllJoyn daemon.
 */
class VirtualEndpoint : public BusEndpoint {
  public:

    /**
     * Constructor
     *
     * @param uniqueName      Unique name for this endpoint.
     * @param busEndpoint     The first endpoint of the bus-to-bus connection responsible for this virtual endpoint.
     */
    VirtualEndpoint(const char* uniqueName, RemoteEndpoint& busEndpoint);

    /**
     * Send an outgoing message.
     *
     * @param msg   Message to be sent.
     * @return
     *      - ER_OK if successful.
     *      - An error status otherwise
     */
    QStatus PushMessage(Message& msg);

    /**
     * Get unique bus name.
     *
     * @return
     *      - unique bus name
     *      - empty if server has not yet assigned one (client-side).
     */
    const qcc::String& GetUniqueName() const { return m_uniqueName; }

    /**
     * Return the user id of the endpoint.
     *
     * @return  User ID number.
     */
    uint32_t GetUserId() const { return 0; };

    /**
     * Return the group id of the endpoint.
     *
     * @return  Group ID number.
     */
    uint32_t GetGroupId() const { return 0; }

    /**
     * Return the process id of the endpoint.
     *
     * @return  Process ID number.
     */
    uint32_t GetProcessId() const { return 0; }

    /**
     * Indicates if the endpoint supports reporting UNIX style user, group, and process IDs.
     *
     * @return  'true' if UNIX IDs supported, 'false' if not supported.
     */
    bool SupportsUnixIDs() const { return false; }

    /**
     * Get the BusToBus endpoint associated with this virtual endpoint.
     *
     * @return The current (top of queue) bus-to-bus endpoint.
     */
    RemoteEndpoint* GetBusToBusEndpoint() { return m_b2bEndpoints.front(); }

    /**
     * Add an alternate bus-to-bus endpoint that can route for this endpoint.
     *
     * @param endpoint   A bus-to-bus endpoint that can route to this virutual endpoint.
     * @return  true if endpoint was added.
     */
    bool AddBusToBusEndpoint(RemoteEndpoint& endpoint);

    /**
     * Remove a bus-to-bus endpoint that can route for thie virtual endpoint.
     *
     * @param endpoint   Bus-to-bus endpoint to remove from list of routes
     * @return  true iff virtual endpoint has no bus-to-bus endpoint and should be removed.
     */
    bool RemoveBusToBusEndpoint(RemoteEndpoint& endpoint);

    /**
     * Return true iff the given bus-to-bus endpoint can potentially be used to route
     * messages for this virtual endpoint.
     *
     * @param b2bEndpoint   B2B endpoint being checked for suitability as a route for this virtual endpoint.
     * @return true iff the B2B endpoint can be used to route messages for this virtual endpoint.
     */
    bool CanUseRoute(const RemoteEndpoint& b2bEndpoint) const;

    /**
     * Indicate whether this endpoint is allowed to receive messages from remote devices.
     * VirtualEndpoints are always allowed to receive remote messages.
     *
     * @return true
     */
    bool AllowRemoteMessages() { return true; }

    /**
     * Get the number of bus to bus endpoints associated with the virtual endpoint.
     *
     * @return Number of bus to bus endpoints associated with the virtual endpoint.
     */
    size_t GetBusToBusEndpointCount() const {
        m_b2bEndpointsLock.Lock();
        size_t cnt = m_b2bEndpoints.size();
        m_b2bEndpointsLock.Unlock();
        return cnt;
    }

  private:

    qcc::String m_uniqueName;                     /**< The unique name for this endpoint */
    std::vector<RemoteEndpoint*> m_b2bEndpoints;   /**< Set of b2bEndpoints that can route for this virtual endpoint */
    mutable qcc::Mutex m_b2bEndpointsLock;        /**< Lock that protects m_b2bEndpoints */
};

}

#endif
