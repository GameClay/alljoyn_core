/**
 * @file
 * SASLEngine is a utility class that provides authentication functions
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
#ifndef _ALLJOYN_SASLENGINE_H
#define _ALLJOYN_SASLENGINE_H

#include <qcc/platform.h>

#include <set>

#include <qcc/String.h>
#include <qcc/GUID.h>
#include <qcc/KeyBlob.h>

#include <alljoyn/BusAttachment.h>

#include "AuthMechanism.h"

namespace ajn {

/**
 * %SASLEngine is a utility class that implements the state machine for SASL-based authentication
 * mechanisms
 */
class SASLEngine {
  public:

    /** Authentication state   */

    typedef enum {
        ALLJOYN_SEND_AUTH_REQ,   ///< Initial responder state,
        ALLJOYN_WAIT_FOR_AUTH,   ///< Initial challenger state
        ALLJOYN_WAIT_FOR_BEGIN,
        ALLJOYN_WAIT_FOR_DATA,
        ALLJOYN_WAIT_FOR_OK,
        ALLJOYN_WAIT_FOR_REJECT,
        ALLJOYN_AUTH_SUCCESS,     ///< Authentication was successful - conversation it over
        ALLJOYN_AUTH_FAILED       ///< Authentication failed - conversation it over
    } AuthState;

    /**
     * Constructor
     *
     * @param bus           The bus
     * @param authRole      Challenger or responder end of the authentication conversation
     * @param mechanisms    The mechanisms to use for this authentication conversation
     * @param listener      Listener for handling password and other authentication related requests.
     */
    SASLEngine(BusAttachment& bus, AuthMechanism::AuthRole authRole, const qcc::String& mechanisms, AuthListener* listener = NULL);

    /**
     * Destructor
     */
    ~SASLEngine();

    /**
     * Advance to the next step in the authentication conversation
     *
     * @param authIn   The authentication string received from the remote endpoint
     * @param authOut  The authentication string to send to the remote endpoint
     * @param state    Returns the current state of the conversation. The conversation is complete
     *                 when the state is ALLJOYN_AUTHENTICATED.
     *
     * @return ER_OK if the conversation has moved forward,
     *         ER_BUS_AUTH_FAIL if converstation ended with an failure to authenticate
     *         ER_BUS_NOT_AUTHENTICATING if the conversation is complete
     */
    QStatus Advance(qcc::String authIn, qcc::String& authOut, AuthState& state);

    /**
     * Returns the name of the authentication last mechanism that was used. If the authentication
     * conversation is complete this is the authentication mechanism that succeeded or failed.
     */
    qcc::String GetMechanism() { return (authMechanism) ? authMechanism->GetName() : ""; }

    /**
     * Get the identifier string received at the end of a succesful authentication conversation
     */
    const qcc::String& GetRemoteId() { return remoteId; };

    /**
     * Set the identifier string to be sent at the end of a succesful authentication conversation
     */
    void SetLocalId(const qcc::String& id) { localId = id; };

    /**
     * Get the master secret from authentication mechanisms that negotiate one.
     *
     * @param secret The master secret key blob.
     *
     * @return   - ER_OK on success
     *           - ER_BUS_KEY_UNAVAILABLE if there is no master secret to get.
     */
    QStatus GetMasterSecret(qcc::KeyBlob& secret) {
        return (authState != ALLJOYN_AUTH_SUCCESS) ? ER_BUS_KEY_UNAVAILABLE : authMechanism->GetMasterSecret(secret);
    }

  private:

    /**
     * Default constructor is private
     */
    SASLEngine();

    /**
     * The bus object
     */
    BusAttachment& bus;

    /**
     * Indicates if this is a challenger or a responder
     */
    AuthMechanism::AuthRole authRole;

    /**
     * Listener for handling interactive authentication methods.
     */
    AuthListener* listener;

    /**
     * Set of available authentication method names
     */
    std::set<qcc::String> authSet;

    /**
     * Count of number of times the state machine has been advanced.
     */
    uint16_t authCount;

    /**
     * Current authentication mechanism
     */
    AuthMechanism* authMechanism;

    /**
     * Current state machine state
     */
    AuthState authState;

    /**
     * Identifier string received from remote authenticated endpoint
     */
    qcc::String remoteId;

    /**
     * Identifier string to send to remote authenticated endpoint
     */
    qcc::String localId;

    /**
     * Internal methods
     */

    QStatus Response(qcc::String& inStr, qcc::String& outStr);
    QStatus Challenge(qcc::String& inStr, qcc::String& outStr);
    QStatus NewAuthRequest(qcc::String& authCmd);
};

}

#endif
