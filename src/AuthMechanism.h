#ifndef _ALLJOYN_AUTHMECHANISM_H
#define _ALLJOYN_AUTHMECHANISM_H
/**
 * @file
 * This file defines the base class for authentication mechanisms and the authentication
 * Mechanism manager
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

#ifndef __cplusplus
#error Only include AuthMechanism.h in C++ code.
#endif

#include <qcc/platform.h>
#include <qcc/String.h>
#include <qcc/KeyBlob.h>

#include <map>

#include "ProtectedAuthListener.h"
#include "KeyStore.h"

#include <Status.h>

namespace ajn {

/**
 * Base class for authentication mechanisms that can be registered with the AllJoynAuthManager
 */
class AuthMechanism {
  public:

    /** Authentication type  */
    typedef enum {
        CHALLENGER, /**< A server usually provides the challenges */
        RESPONDER   /**< A client usually provide the responses */
    } AuthRole;

    /**
     * Enumeration for authentication status
     */
    typedef enum {
        ALLJOYN_AUTH_OK,       ///< Indicates the authentication exchange is complete
        ALLJOYN_AUTH_CONTINUE, ///< Indicates the authentication exchange is continuing
        ALLJOYN_AUTH_RETRY,    ///< Indicates the authentication failed but should be retried
        ALLJOYN_AUTH_FAIL,     ///< Indicates the authentication failed
        ALLJOYN_AUTH_ERROR     ///< Indicates the authentication challenge or response was badly formed
    } AuthResult;

    /**
     * Initialize this authentication mechanism. This method is called by the SASL engine
     * immediately after the authentication constructor is called. Classes that define this method
     * should call this base class method.
     *
     * @param authRole   Indicates if the authentication method is initializing as a challenger or a responder.
     * @param authPeer   The bus name of the remote peer that is being authenticated.
     *
     * @return ER_OK if the authentication mechanism was succesfully initialized.
     */
    virtual QStatus Init(AuthRole authRole, const qcc::String& authPeer) {
        this->authPeer = authPeer;
        this->authRole = authRole;
        ++authCount;
        return ER_OK;
    }

    /**
     * Challenges flow from servers to clients.
     *
     * Process a response from a client and returns a challenge.
     *
     * @param response Response from client
     * @param result   Returns:
     *                  - ALLJOYN_AUTH_OK  if the authentication is complete,
     *                  - ALLJOYN_AUTH_CONTINUE if the authentication is incomplete,
     *                  - ALLJOYN_AUTH_ERROR    if the response was rejected.
     */
    virtual qcc::String Challenge(const qcc::String& response, AuthResult& result) = 0;

    /**
     * Request the initial challenge. Will be an empty string if this authentication mechanism does not send an initial challenge.
     *
     * @param result  Returns:
     *                - ALLJOYN_AUTH_OK        if the authentication is complete,
     *                - ALLJOYN_AUTH_CONTINUE  if the authentication is incomplete
     *                - ALLJOYN_AUTH_FAIL      if the authentication conversation failed on startup
     * @return returns a string with the initial challenge
     */
    virtual qcc::String InitialChallenge(AuthResult& result) { result = ALLJOYN_AUTH_CONTINUE; return ""; }

    /**
     * Responses flow from clients to servers.
     *
     * Process a challenge and generate a response
     *
     * @param challenge Challenge provided by the server
     * @param result    Returns:
     *                      - ALLJOYN_AUTH_OK  if the authentication is complete,
     *                      - ALLJOYN_AUTH_CONTINUE if the authentication is incomplete,
     *                      - ALLJOYN_AUTH_ERROR    if the response was rejected.
     */
    virtual qcc::String Response(const qcc::String& challenge, AuthResult& result) = 0;

    /**
     * Request the initial response. Will be an empty string if this authentication mechanism does not send an initial response.
     *
     * @param result  Returns:
     *                - ALLJOYN_AUTH_OK        if the authentication is complete,
     *                - ALLJOYN_AUTH_CONTINUE  if the authentication is incomplete
     *                - ALLJOYN_AUTH_FAIL      if the authentication conversation failed on startup
     * @return a string
     */
    virtual qcc::String InitialResponse(AuthResult& result) { result = ALLJOYN_AUTH_CONTINUE; return ""; }

    /**
     * Get the name for the authentication mechanism
     *
     * @return string with the name for the authentication mechanism
     */
    virtual const char* GetName() = 0;

    /**
     * Accessor function to get the master secret from authentication mechanisms that negotiate one.
     *
     * @param secret  The master secret key blob
     *
     * @return   - ER_OK on success.
     *           - ER_BUS_KEY_UNAVAILABLE if unable to get the master secret.
     */
    QStatus GetMasterSecret(qcc::KeyBlob& secret)
    {
        if (masterSecret.IsValid()) {
            secret = masterSecret;
            return ER_OK;
        } else {
            return ER_BUS_KEY_UNAVAILABLE;
        }
    }

    /**
     * Indicates if the authentication mechanism is interactive (i.e. involves application or user
     * input) or is automatic. If an authentication mechanism is not interactive it is not worth
     * making multiple authentication attempts because the result will be the same each time. On the
     * other hand, authentication methods that involve user input, such as password entry would
     * normally allow one or more retries.
     *
     * @return  Returns true if this authentication method involves user interaction.
     */
    virtual bool IsInteractive() { return false; }

    /**
     * Destructor
     */
    virtual ~AuthMechanism() { }

  protected:

    /**
     * Key blob if mechanism negotiates a master secret.
     */
    qcc::KeyBlob masterSecret;

    /**
     * Specifies the expiration time for the master secret
     */
    uint32_t expiration;

    /**
     * Class instance for interacting with user and/or application to obtain a password and other
     * information.
     */
    ProtectedAuthListener& listener;

    /**
     * Constructor
     */
    AuthMechanism(KeyStore& keyStore, ProtectedAuthListener& listener) : expiration(0xFFFFFFFF), listener(listener), keyStore(keyStore), authCount(0) { }

    /**
     * The keyStore
     */
    KeyStore& keyStore;

    /**
     * The number of times this authentication has been attempted.
     */
    uint16_t authCount;

    /**
     * The current role of the authenticating peer.
     */
    AuthRole authRole;

    /**
     * A name for the remote peer that is being authenticated.
     */
    qcc::String authPeer;
};

}
#endif
