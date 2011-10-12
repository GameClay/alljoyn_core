/**
 * @file
 * BusListener is an abstract base class (interface) implemented by users of the
 * AllJoyn API in order to asynchronously receive bus  related event information.
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
#ifndef _ALLJOYN_BUSLISTENER_H
#define _ALLJOYN_BUSLISTENER_H

#include <alljoyn/TransportMask.h>
#include <alljoyn/AllJoynCTypes.h>

#ifdef __cplusplus

namespace ajn {

/**
 * Foward declaration.
 */
class BusAttachment;

/**
 * Abstract base class implemented by AllJoyn users and called by AllJoyn to inform
 * users of bus related events.
 */
class BusListener {
  public:
    /**
     * Virtual destructor for derivable class.
     */
    virtual ~BusListener() { }

    /**
     * Called by the bus when the listener is registered. This give the listener implementation the
     * opportunity to save a reference to the bus.
     *
     * @param bus  The bus the listener is registered with.
     */
    virtual void ListenerRegistered(BusAttachment* bus) { }

    /**
     * Called by the bus when the listener is unegistered.
     */
    virtual void ListenerUnregistered() { }

    /**
     * Called by the bus when an external bus is discovered that is advertising a well-known name
     * that this attachment has registered interest in via a DBus call to org.alljoyn.Bus.FindAdvertisedName
     *
     * @param name         A well known name that the remote bus is advertising.
     * @param transport    Transport that received the advertisment.
     * @param namePrefix   The well-known name prefix used in call to FindAdvertisedName that triggered this callback.
     */
    virtual void FoundAdvertisedName(const char* name, TransportMask transport, const char* namePrefix) { }

    /**
     * Called by the bus when an advertisement previously reported through FoundName has become unavailable.
     *
     * @param name         A well known name that the remote bus is advertising that is of interest to this attachment.
     * @param transport    Transport that stopped receiving the given advertised name.
     * @param namePrefix   The well-known name prefix that was used in a call to FindAdvertisedName that triggered this callback.
     */
    virtual void LostAdvertisedName(const char* name, TransportMask transport, const char* namePrefix) { }

    /**
     * Called by the bus when the ownership of any well-known name changes.
     *
     * @param busName        The well-known name that has changed.
     * @param previousOwner  The unique name that previously owned the name or NULL if there was no previous owner.
     * @param newOwner       The unique name that now owns the name or NULL if the there is no new owner.
     */
    virtual void NameOwnerChanged(const char* busName, const char* previousOwner, const char* newOwner) { }

    /**
     * Called when a BusAttachment this listener is registered with is stopping.
     */
    virtual void BusStopping() { }

    /**
     * Called when a BusAttachment this listener is registered with is has become disconnected from
     * the bus.
     */
    virtual void BusDisconnected() { }

};

}

extern "C" {
#endif /* #ifdef __cplusplus */

/**
 * Type for the ListenerRegistered callback.
 */
typedef void (*alljoyn_buslistener_listener_registered_ptr)(const void* context, alljoyn_busattachment bus);

/**
 * Type for the ListenerUnregistered callback.
 */
typedef void (*alljoyn_buslistener_listener_unregistered_ptr)(const void* context);

/**
 * Type for the FoundAdvertisedName callback.
 */
typedef void (*alljoyn_buslistener_found_advertised_name_ptr)(const void* context, const char* name, alljoyn_transportmask transport, const char* namePrefix);

/**
 * Type for the LostAdvertisedName callback.
 */
typedef void (*alljoyn_buslistener_lost_advertised_name_ptr)(const void* context, const char* name, alljoyn_transportmask transport, const char* namePrefix);

/**
 * Type for the NameOwnerChanged callback.
 */
typedef void (*alljoyn_buslistener_name_owner_changed_ptr)(const void* context, const char* busName, const char* previousOwner, const char* newOwner);


/**
 * Type for the BusStopping callback.
 */
typedef void (*alljoyn_buslistener_bus_stopping_ptr)(const void* context);

/**
 * Type for the BusDisconnected callback.
 */
typedef void (*alljoyn_buslistener_bus_disconnected_ptr)(const void* context);

/**
 * Struct containing callbacks used for creation of an alljoyn_buslistener.
 */
typedef struct {
    alljoyn_buslistener_listener_registered_ptr listener_registered;
    alljoyn_buslistener_listener_unregistered_ptr listener_unregistered;
    alljoyn_buslistener_found_advertised_name_ptr found_advertised_name;
    alljoyn_buslistener_lost_advertised_name_ptr lost_advertised_name;
    alljoyn_buslistener_name_owner_changed_ptr name_owner_changed;
    alljoyn_buslistener_bus_stopping_ptr bus_stopping;
    alljoyn_buslistener_bus_disconnected_ptr bus_disconnected;
} alljoyn_buslistener_callbacks;

/**
 * Create a BusListener which will trigger the provided callbacks, passing along the provided context.
 *
 * @param callbacks Callbacks to trigger for associated events.
 * @param context   Context to pass to callback functions
 *
 * @return Handle to newly allocated BusListener.
 */
extern AJ_API alljoyn_buslistener alljoyn_buslistener_create(const alljoyn_buslistener_callbacks* callbacks, const void* context);

/**
 * Destroy a BusListener.
 *
 * @param listener BusListener to destroy.
 */
extern AJ_API void alljoyn_buslistener_destroy(alljoyn_buslistener listener);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
