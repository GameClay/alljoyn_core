/**
 * @file BusAttachment.h is the top-level object responsible for connecting to a
 * message bus.
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
#ifndef _ALLJOYN_BUSATTACHMENT_H
#define _ALLJOYN_BUSATTACHMENT_H

#ifndef __cplusplus
#error Only include BusAttachment.h in C++ code.
#endif

#include <qcc/platform.h>

#include <qcc/String.h>
#include <alljoyn/KeyStoreListener.h>
#include <alljoyn/AuthListener.h>
#include <alljoyn/BusListener.h>
#include <alljoyn/BusObject.h>
#include <alljoyn/ProxyBusObject.h>
#include <alljoyn/InterfaceDescription.h>

#include <Status.h>

namespace ajn {

class RemoteEndpoint;

/**
 * %BusAttachment is the top-level object responsible for connecting to and optionally managing a message bus.
 */
class BusAttachment : public MessageReceiver {
  public:

    /**
     * Construct a BusAttachment.
     *
     * @param applicationName       Name of the application.
     * @param allowRemoteMessages   True if this attachment is allowed to receive messages from remote devices.
     */
    BusAttachment(const char* applicationName, bool allowRemoteMessages = false);

    /** Destructor */
    virtual ~BusAttachment();

    /**
     * Create an interface description with a given name.
     *
     * This method informs AllJoyn of a new interface that will (presumably) be implemented
     * by one or more local or remote bus objects.
     *
     * Typically, interfaces that are implemented by BusObjects are created here.
     * Interfaces that are implemented by remote objects are added automatically by
     * the bus if they are not already present via ProxyBusObject::IntrospectRemoteObject().
     *
     * Because interfaces are added both explicitly (via this method) and implicitly
     * (via @c ProxyBusObject::IntrospectRemoteObject), there is the possibility that creating
     * an interface here will fail because the interface already exists. If this happens, the
     * ER_BUS_IFACE_ALREADY_EXISTS will be returned and NULL will be returned in the iface [OUT]
     * parameter.
     *
     * Interfaces created with this method need to be activated using InterfaceDescription::Activate()
     * once all of the methods, signals, etc have been added to the interface. The interface will
     * be unaccessible (via BusAttachment::GetInterfaces() or BusAttachment::GetInterface()) until
     * it is activated.
     *
     * @param name   The requested interface name.
     * @param[out] iface
     *      - Interface description
     *      - NULL if cannot be created.
     * @param secure If true the interface is secure and method calls and signals will be encrypted.
     *
     * @return
     *      - #ER_OK if creation was successful.
     *      - #ER_BUS_IFACE_ALREADY_EXISTS if requested interface already exists
     * @see ProxyBusObject::IntrospectRemoteObject, InterfaceDescription::Activate, BusAttachment::GetInterface
     */
    QStatus CreateInterface(const char* name, InterfaceDescription*& iface, bool secure = false);

    /**
     * Initialize one more interface descriptions from an XML string in DBus introspection format.
     * The root tag of the XML can be a <node> or a standalone <interface> tag. To initialize more
     * than one interface the interfaces need to be nested in a <node> tag.
     *
     * Note that when this method fails during parsing, the return code will be set accordingly.
     * However, any interfaces which were successfully parsed prior to the failure may be registered
     * with the bus.
     *
     * @param xml     An XML string in DBus introspection format.
     *
     * @return
     *      - #ER_OK if parsing is completely successful.
     *      - An error status otherwise.
     */
    QStatus CreateInterfacesFromXml(const char* xml);

    /**
     * Returns the existing activated InterfaceDescriptions.
     *
     * @param ifaces     A pointer to an InterfaceDescription array to receive the interfaces. Can be NULL in
     *                   which case no interfaces are returned and the return value gives the number
     *                   of interface available.
     * @param numIfaces  The size of the InterfaceDescription array. If this value is smaller than the total
     *                   number of interfaces only numIfaces will be returned.
     *
     * @return  The number of interfaces returned or the total number of interfaces if ifaces is NULL.
     */
    size_t GetInterfaces(const InterfaceDescription** ifaces = NULL, size_t numIfaces = 0) const;

    /**
     * Retrieve an existing activated InterfaceDescription.
     *
     * @param name       Interface name
     * @return
     *      - A pointer to the registered interface
     *      - NULL if interface doesn't exist
     */
    const InterfaceDescription* GetInterface(const char* name) const;

    /**
     * Delete an interface description with a given name.
     *
     * Deleting an interface is only allowed if that interface has never been activated.
     *
     * @param iface  The un-activated interface to be deleted.
     * @return
     *      - #ER_OK if deletion was successful
     *      - #ER_BUS_NO_SUCH_INTERFACE if interface was not found
     * @return
     *      - #ER_OK on success
     *      - #ER_BUS_NO_SUCH_INTERFACE if interface is not found
     */
    QStatus DeleteInterface(InterfaceDescription& iface);

    /**
     * Start the message bus.
     *
     * This method only begins the process of starting the bus. Sending and receiving messages
     * cannot begin until the bus is connected.
     *
     * There are two ways to determine whether the bus is currently connected:
     *    -# BusAttachment::IsConnected() returns true
     *    -# BusObject::ObjectRegistered() is called by the bus
     *
     * @return
     *      - #ER_OK if successful.
     *      - #ER_BUS_BUS_ALREADY_STARTED if already started
     *      - Other error status codes indicating a failure
     */
    QStatus Start();

    /**
     * Stop the message bus.
     *
     * @param blockUntilStopped   Block the caller until the bus is stopped
     * @return
     *      - #ER_OK if successful.
     *      - An error status if unable to stop the message bus
     */
    QStatus Stop(bool blockUntilStopped = true);

    /**
     * Returns true if the mesage bus has been started.
     */
    bool IsStarted() { return isStarted; }

    /**
     * Wait for the message bus to be stopped. This method blocks the calling thread until another thread
     * calls the Stop() method. Return immediately if the message bus has not been started.
     */
    void WaitStop();

    /**
     * Connect to a remote bus address.
     *
     * @param connectSpec  A transport connection spec string of the form:
     *                     @c "<transport>:<param1>=<value1>,<param2>=<value2>...[;]"
     * @param newep        FOR INTERNAL USE ONLY - External users must set to NULL (the default)
     *
     * @return
     *      - #ER_OK if successful.
     *      - An error status otherwise
     */
    QStatus Connect(const char* connectSpec, RemoteEndpoint** newep = NULL);

    /**
     * Disconnect a remote bus address connection.
     *
     * @param connectSpec  The transport connection spec used to connect.
     * @return
     *          - #ER_OK if successful
     *          - #ER_BUS_BUS_NOT_STARTED if the bus is not started
     *          - #ER_BUS_NOT_CONNECTED if the %BusAttachment is not connected to the bus
     *          - Other error status codes indicating a failure
     */
    QStatus Disconnect(const char* connectSpec);

    /**
     * Indicate whether bus is currently connected.
     *
     * Messages can only be sent or received when the bus is connected.
     *
     * @return true if the bus is connected.
     */
    bool IsConnected() const;

    /**
     * Register a BusObject
     *
     * @param obj      BusObject to register.
     * @return
     *      - #ER_OK if successful.
     *      - #ER_BUS_BAD_OBJ_PATH for a bad object path
     */
    QStatus RegisterBusObject(BusObject& obj);

    /**
     * De-register a BusObject
     *
     * @param object  Object to be deregistered.
     */
    void DeregisterBusObject(BusObject& object);

    /**
     * Get the org.freedesktop.DBus proxy object.
     *
     * @return org.freedesktop.DBus proxy object
     */
    const ProxyBusObject& GetDBusProxyObj();

    /**
     * Get the org.alljoyn.Bus proxy object.
     *
     * @return org.alljoyn.Bus proxy object
     */
    const ProxyBusObject& GetAllJoynProxyObj();

    /**
     * Get the unique name of this BusAttachment.
     *
     * @return The unique name of this BusAttachment.
     */
    const qcc::String& GetUniqueName() const;

    /**
     * Get the guid of the local daemon as a string
     *
     * @return GUID of local AllJoyn daemon as a string.
     */
    const qcc::String& GetGlobalGUIDString() const;

    /**
     * Register a signal handler.
     *
     * Signals are forwarded to the signalHandler if sender, interface, member and path
     * qualifiers are ALL met.
     *
     * @param receiver       The object receiving the signal.
     * @param signalHandler  The signal handler method.
     * @param member         The interface/member of the signal.
     * @param srcPath        The object path of the emitter of the signal or NULL for all paths.
     * @return #ER_OK
     */
    QStatus RegisterSignalHandler(MessageReceiver* receiver,
                                  MessageReceiver::SignalHandler signalHandler,
                                  const InterfaceDescription::Member* member,
                                  const char* srcPath);

    /**
     * Un-Register a signal handler.
     *
     * Remove the signal handler that was registered with the given parameters.
     *
     * @param receiver       The object receiving the signal.
     * @param signalHandler  The signal handler method.
     * @param member         The interface/member of the signal.
     * @param srcPath        The object path of the emitter of the signal or NULL for all paths.
     * @return #ER_OK
     */
    QStatus UnRegisterSignalHandler(MessageReceiver* receiver,
                                    MessageReceiver::SignalHandler signalHandler,
                                    const InterfaceDescription::Member* member,
                                    const char* srcPath);

    /**
     * Enable peer-to-peer security. This function must be called by applications that want to use
     * secure interfaces. This bus must have been started by calling BusAttachment::Start() before this
     * function is called.
     *
     * @param authMechanisms  The authentication mechanism(s) to use for peer-to-peer authentication.
     *                        If this parameter is NULL peer-to-peer authentication is disabled.
     *
     * @param listener        Passes password and other authentication related requests to the application.
     *
     * @param keyStoreFileName Optional parameter to specify the filename of the default key store.  The
     *                         default value is the applicationName parameter of BusAttachment().
     *
     * @return
     *      - #ER_OK if peer security was enabled.
     *      - #ER_BUS_BUS_NOT_STARTED BusAttachment::Start has not be called
     */
    QStatus EnablePeerSecurity(const char* authMechanisms, AuthListener* listener,
                               const char* keyStoreFileName = NULL);

    /**
     * Register an object that will receive bus event notifications.
     *
     * @param listener  Object instance that will receive bus event notifications.
     */
    virtual void RegisterBusListener(BusListener& listener);

    /**
     * UnRegister an object that was previously registered with RegisterBusListener.
     *
     * @param listener  Object instance to un-register as a listener.
     */
    virtual void UnRegisterBusListener(BusListener& listener);

    /**
     * Set a key store listener to listen for key store load and store requests.
     * This overrides the internal key store listener.
     *
     * @param listener  The key store listener to set.
     */
    void RegisterKeyStoreListener(KeyStoreListener& listener);

    /**
     * Clears all stored keys from the key store. All store keys and authentication information is
     * deleted and cannot be recovered. Any passwords or other credentials will need to be reentered
     * when establishing secure peer connections.
     */
    void ClearKeyStore();

    /**
     * Adds a logon entry string for the requested authentication mechanism to the key store. This
     * allows an authenticating server to generate offline authentication credentials for securely
     * logging on a remote peer using a user-name and password credentials pair. This only applies
     * to authentication mechanisms that support a user name + password logon functionality.
     *
     * @param authMechanism The authentication mechanism.
     * @param userName      The user name to use for generating the logon entry.
     * @param password      The password to use for generating the logon entry. If the password is
     *                      NULL the logon entry is deleted from the key store.
     *
     * @return
     *      - #ER_OK if the logon entry was generated.
     *      - #ER_BUS_INVALID_AUTH_MECHANISM if the authentication mechanism does not support
     *                                       logon functionality.
     *      - #ER_BAD_ARG_2 indicates a null string was used as the user name.
     *      - #ER_BAD_ARG_3 indicates a null string was used as the password.
     *      - Other error status codes indicating a failure
     */
    QStatus AddLogonEntry(const char* authMechanism, const char* userName, const char* password);

    /**
     * Request the local AllJoyn daemon to connect with a remote AllJoyn daemon.
     * This method is a shortcut/helper that issues an org.codeauora.AllJoyn.Connect method call to the daemon
     * and interprets the response.
     *
     * @param[in]  busAddr      The bus address of the remote daemon.
     * @param[out] disposition  \ref ALLJOYN_CONNECT_REPLY_SUCCESS "ALLJOYN_CONNECT_REPLY_*" code from AllJoynStd.h
     * @return
     *      - #ER_OK if connect request was issued. ER_OK indicates that disposition is valid for inspection.
     *      - #ER_BUS_NOT_CONNECTED if a connection has not been made with a local bus.
     *      - Other error status codes indicating a failure.
     */
    QStatus ConnectToRemoteBus(const char* busAddr, uint32_t& disposition);

    /**
     * Request the local AllJoyn daemon to disconnect with a remote AllJoyn daemon.
     * This method is a shortcut/helper that issues an org.codeauora.AllJoyn.Disconnect method call to the daemon
     * and interprets the response. Disconnection from the remote bus will happen when all local AllJoyn clients
     * that had previously connected to the remote bus issue this method call or disconnect from the local daemon.
     *
     * @param[in]  busAddr      The bus address of the remote daemon used in a previous call to ConnectToRemoteBus.
     * @param[out] disposition  @ref ALLJOYN_DISCONNECT_REPLY_SUCCESS "ALLJOYN_DISCONNECT_REPLY_*" code from AllJoynStd.h
     * @return
     *      - #ER_OK if Disconnect request was issued. ER_OK indicates that disposition is valid for inspection.
     *      - #ER_BUS_NOT_CONNECTED if a connection has not been made with a local bus.
     */
    QStatus DisconnectFromRemoteBus(const char* busAddr, uint32_t& disposition);

    /**
     * Determine whether a given well-known name exists on the bus.
     * This method is a shortcut/helper that issues an org.freedesktop.DBus.NameHasOwner method call to the daemon
     * and interprets the response.
     *
     * @param[in]  name       The well known name that the caller is inquiring about.
     * @param[out] hasOwner   If return is ER_OK, indicates whether name exists on the bus. If return is not ER_OK, param is not modified.
     * @return
     *      - #ER_OK if name ownership was able to be determined.
     *      - An error status otherwise
     */
    QStatus NameHasOwner(const char* name, bool& hasOwner);

    /**
     * Returns the current non-absolute real-time clock used internally by AllJoyn. This value can be
     * compared with the timestamps on messages to calculate the time since a timestamped message
     * was sent.
     *
     * @return  The current timestamp in milliseconds.
     */
    static uint32_t GetTimestamp();

    /// @cond ALLJOYN_DEV
    /**
     * @internal
     * Class for internal state for bus attachment.
     */
    class Internal;

    /**
     * @internal
     * Get a pointer to the internal BusAttachment state.
     *
     * @return A pointer to the internal state.
     */
    Internal& GetInternal() { return *busInternal; }

    /**
     * @internal
     * Get a pointer to the internal BusAttachment state.
     *
     * @return A pointer to the internal state.
     */
    const Internal& GetInternal() const { return *busInternal; }
    /// @endcond
  protected:
    /// @cond ALLJOYN_DEV
    /**
     * @internal
     * Construct a BusAttachment.
     *
     * @param internal  Internal state.
     */
    BusAttachment(Internal* internal);
    /// @endcond
  private:

    /**
     * Assignment operator is private.
     */
    BusAttachment& operator=(const BusAttachment& other) { return *this; }

    /**
     * Copy constructor is private.
     */
    BusAttachment(const BusAttachment& other) { }

    bool isStarted;           /**< Indicates if the bus has been started */
    bool isStopping;          /**< Indicates Stop has been called */
    Internal* busInternal;    /**< Internal state information */
};

}

#endif
