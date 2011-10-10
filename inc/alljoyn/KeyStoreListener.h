/**
 * @file
 * The KeyStoreListener class handled requests to load or store the key store.
 */

/******************************************************************************
 * Copyright 2010-2011, Qualcomm Innovation Center, Inc.
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
#ifndef _ALLJOYN_KEYSTORE_LISTENER_H
#define _ALLJOYN_KEYSTORE_LISTENER_H

#include <qcc/platform.h>
#include <alljoyn/AllJoynCTypes.h>
#include <Status.h>

#ifdef __cplusplus

#include <qcc/String.h>

namespace ajn {

/**
 * Forward declaration.
 */
class KeyStore;

/**
 * An application can provide a key store listener to override the default key store Load and Store
 * behavior. This will override the default key store behavior.
 */
class KeyStoreListener {

  public:
    /**
     * Virtual destructor for derivable class.
     */
    virtual ~KeyStoreListener() { }

    /**
     * This method is called when a key store needs to be loaded.
     * @remark The application must call <tt>#PutKeys</tt> to put the new key store data into the
     * internal key store.
     *
     * @param keyStore   Reference to the KeyStore to be loaded.
     *
     * @return
     *      - #ER_OK if the load request was satisfied
     *      - An error status otherwise
     *
     */
    virtual QStatus LoadRequest(KeyStore& keyStore) = 0;

    /**
     * Put keys into the key store from an encrypted byte string.
     *
     * @param keyStore  The keyStore to put to. This is the keystore indicated in the LoadRequest call.
     * @param source    The byte string containing the encrypted key store contents.
     * @param password  The password required to decrypt the key data
     *
     * @return
     *      - #ER_OK if successful
     *      - An error status otherwise
     *
     */
    QStatus PutKeys(KeyStore& keyStore, const qcc::String& source, const qcc::String& password);

    /**
     * This method is called when a key store needs to be stored.
     * @remark The application must call <tt>#GetKeys</tt> to obtain the key data to be stored.
     *
     * @param keyStore   Reference to the KeyStore to be stored.
     *
     * @return
     *      - #ER_OK if the store request was satisfied
     *      - An error status otherwise
     */
    virtual QStatus StoreRequest(KeyStore& keyStore) = 0;

    /**
     * Get the current keys from the key store as an encrypted byte string.
     *
     * @param keyStore  The keyStore to get from. This is the keystore indicated in the StoreRequest call.
     * @param sink      The byte string to write the keys to.
     * @return
     *      - #ER_OK if successful
     *      - An error status otherwise
     */
    QStatus GetKeys(KeyStore& keyStore, qcc::String& sink);

};

}

extern "C" {
#endif /* #ifdef __cplusplus */

/**
 * Type for the LoadRequest callback.
 */
typedef QStatus (*alljoyn_keystorelistener_loadrequest_ptr)(const void* context, alljoyn_keystore keyStore);

/**
 * Type for the StoreRequest callback.
 */
typedef QStatus (*alljoyn_keystorelistener_storerequest_ptr)(const void* context, alljoyn_keystore keyStore);

/**
 * Structure used during alljoyn_keystorelistener_create to provide callbacks into C.
 */
typedef struct {
    alljoyn_keystorelistener_loadrequest_ptr load_request;
    alljoyn_keystorelistener_storerequest_ptr store_request;
} alljoyn_keystorelistener_callbacks;

/**
 * Create a KeyStoreListener
 *
 * @param callbacks  Callbacks to trigger for associated events.
 * @param context    Context to pass along to callback functions.
 */
alljoyn_keystorelistener alljoyn_keystorelistener_create(const alljoyn_keystorelistener_callbacks* callbacks, const void* context);

/**
 * Destroy a KeyStoreListener
 *
 * @param listener The KeyStoreListener to destroy.
 */
void alljoyn_keystorelistener_destroy(alljoyn_keystorelistener listener);

/**
 * Put keys into the key store from an encrypted byte string.
 *
 * @param listener  The KeyStoreListener into which to put the keys.
 * @param keyStore  The keyStore to put to. This is the keystore indicated in the LoadRequest call.
 * @param source    The byte string containing the encrypted key store contents.
 * @param password  The password required to decrypt the key data
 *
 * @return
 *      - #ER_OK if successful
 *      - An error status otherwise
 *
 */
QStatus alljoyn_keystorelistener_putkeys(alljoyn_keystorelistener listener, alljoyn_keystore keyStore, const char* source, const char* password);

/**
 * Get the current keys from the key store as an encrypted byte string.
 *
 * @param listener  The KeyStoreListener from which to get the keys.
 * @param keyStore  The keyStore to get from. This is the keystore indicated in the StoreRequest call.
 * @param sink      The byte string to write the keys to.
 * @param sink_sz   The size of the byte string provided.
 * @return
 *      - #ER_OK if successful
 *      - An error status otherwise
 */
QStatus alljoyn_keystorelistener_getkeys(alljoyn_keystorelistener listener, alljoyn_keystore keyStore, char* sink, size_t sink_sz);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
