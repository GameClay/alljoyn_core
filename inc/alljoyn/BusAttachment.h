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
#include <alljoyn/Session.h>
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
     * The root tag of the XML can be a \<node\> or a standalone \<interface\> tag. To initialize more
     * than one interface the interfaces need to be nested in a \<node\> tag.
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
     * Returns true if the mesage bus has been requested to stop.
     */
    bool IsStopping() { return isStopping; }

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
     * Request a well-known name.
     * This method is a shortcut/helper that issues an org.freedesktop.DBus.RequestName method call to the local daemon
     * and interprets the response.
     *
     * @param[in]  requestedName  Well-known name being requested.
     * @param[in]  flags          Bitmask of DBUS_NAME_FLAG_* defines (see DBusStd.h)
     * @param[out] disposition    DBUS_REQUEST_NAME_REPLY_* response to org.freedesktop.DBus.RequestName (see DBusStd.h).
     *                            disposition is only valid if return value is #ER_OK.
     *
     * @return
     *      - #ER_OK if daemon response was received. ER_OK indicates that disposition is valid for inspection.
     *      - #ER_BUS_NOT_CONNECTED if a connection has not been made with a local bus.
     *      - Other error status codes indicating a failure.
     */
    QStatus RequestName(const char* requestedName, uint32_t flags, uint32_t& disposition);

    /**
     * Release a previously requeted well-known name.
     * This method is a shortcut/helper that issues an org.freedesktop.DBus.ReleaseName method call to the local daemon
     * and interprets the response.
     *
     * @param[in]  name          Well-known name being released.
     * @param[out] disposition   DBUS_RELEASE_NAME_REPLY_* response to org.freedesktop.DBus.ReleaseName (see DBusStd.h).
     *                           disposition is only valid if return value is #ER_OK.
     *
     * @return
     *      - #ER_OK if daemon response was received. ER_OK indicates that disposition is valid for inspection.
     *      - #ER_BUS_NOT_CONNECTED if a connection has not been made with a local bus.
     *      - Other error status codes indicating a failure.
     */
    QStatus ReleaseName(const char* name, uint32_t& disposition);

    /**
     * Add a DBus match rule.
     * This method is a shortcut/helper that issues an org.freedesktop.DBus.AddMatch method call to the local daemon.
     *
     * @param[in]  rule          Match rule to be added (see DBus specification for format of this string).
     *
     * @return
     *      - #ER_OK if the AddMatch request was successful.
     *      - #ER_BUS_NOT_CONNECTED if a connection has not been made with a local bus.
     *      - Other error status codes indicating a failure.
     */
    QStatus AddMatch(const char* rule);

    /**
     * Advertise the existence of a well-known name to other (possibly disconnected) AllJoyn daemons.
     *
     * This method is a shortcut/helper that issues an org.codeauora.AllJoyn.Bus.AdvertisedName method call to the local daemon
     * and interprets the response.
     *
     * @param[in]  name          the well-known name to advertise. (Must be owned by the caller via RequestName).
     * @param[in]  transports    Set of transports to use for sending advertisment.
     * @param[out] disposition   ALLJOYN_ADVERTISEDNAME_REPLY_* constant from AllJoynStd.h
     *
     * @return
     *      - #ER_OK if daemon response was received. ER_OK indicates that disposition is valid for inspection.
     *      - #ER_BUS_NOT_CONNECTED if a connection has not been made with a local bus.
     *      - Other error status codes indicating a failure.
     */
    QStatus AdvertiseName(const char* name, TransportMask transports, uint32_t& disposition);

    /**
     * Stop advertising the existence of a well-known name to other AllJoyn daemons.
     *
     * This method is a shortcut/helper that issues an org.codeauora.AllJoyn.Bus.CancelAdvertiseName method call to the local daemon
     * and interprets the response.
     *
     * @param[in]  name          A well-known name that was previously advertised via AdvertiseName.
     * @param[out] disposition   ALLJOYN_CANCELADVERTISENAME_REPLY_* constant from AllJoynStd.h
     *
     * @return
     *      - #ER_OK if daemon response was received. ER_OK indicates that disposition is valid for inspection.
     *      - #ER_BUS_NOT_CONNECTED if a connection has not been made with a local bus.
     *      - Other error status codes indicating a failure.
     */
    QStatus CancelAdvertiseName(const char* name, uint32_t& disposition);

    /**
     * Register interest in a well-known name prefix for the purpose of discovery.
     * This method is a shortcut/helper that issues an org.codeauora.AllJoyn.Bus.FindAdvertisedName method call to the local daemon
     * and interprets the response.
     *
     * @param[in]  namePrefix    Well-known name prefix that application is interested in receiving BusListener::FoundAdvertisedName
     *                           notifications about.
     * @param[out] disposition   ALLJOYN_FINDADVERTISEDNAME_REPLY_* constant from AllJoynStd.h
     *
     * @return
     *      - #ER_OK if daemon response was received. ER_OK indicates that disposition is valid for inspection.
     *      - #ER_BUS_NOT_CONNECTED if a connection has not been made with a local bus.
     *      - Other error status codes indicating a failure.
     */
    QStatus FindAdvertisedName(const char* namePrefix, uint32_t& disposition);

    /**
     * Cancel interest in a well-known name prefix that was previously registered with FindAdvertisedName.
     * This method is a shortcut/helper that issues an org.codeauora.AllJoyn.Bus.CancelFindAdvertisedName method call to the local daemon
     * and interprets the response.
     *
     * @param[in]  namePrefix    Well-known name prefix that application is no longer interested in receiving
     *                           BusListener::FoundAdvertisedName notifications about.
     * @param[out] disposition   ALLJOYN_CANCELFINDADVERTISEDNAME_REPLY_* constant from AllJoynStd.h
     *
     * @return
     *      - #ER_OK if daemon response was received. ER_OK indicates that disposition is valid for inspection.
     *      - #ER_BUS_NOT_CONNECTED if a connection has not been made with a local bus.
     *      - Other error status codes indicating a failure.
     */
    QStatus CancelFindAdvertisedName(const char* namePrefix, uint32_t& disposition);

    /**
     * Make a SessionPort available for external BusAttachments to join.
     *
     * Each BusAttachment binds its own set of SessionPorts. Session joiners use the bound session
     * port along with the name of the attachement to create a persistent logical connection (called
     * a Session) with the original BusAttachment.
     *
     * A SessionPort and bus name form a unique identifier that BusAttachments use when joining a
     * session.
     *
     * SessionPort values can be pre-arranged between AllJoyn services and their clients (well-known
     * SessionPorts).
     *
     * Once a session is joined using one of the service's well-known SessionPorts, the service may
     * bind additional SessionPorts (dyanamically) and share these SessionPorts with the joiner over
     * the original session. The joiner can then create additional sessions with the service by
     * calling JoinSession with these dynamic SessionPort ids.
     *
     * @param[in,out] sessionPort   SessionPort value to bind or SESSION_PORT_ANY to allow this method
     *                              to choose an available port. On successful return, this value
     *                              contains the chosen SessionPort.
     *
     * @param[in]     opts          Session options that joiners must agree to in order to
     *                              successfully join the session.
     *
     * @param[out]    disposition   @ref BindSessionPortReplyAnchor "ALLJOYN_BINDSESSIONPORT_REPLY_*"
     *                              constant from AllJoynStd.h
     *
     * @return
     *      - #ER_OK if daemon response was received. ER_OK indicates that disposition is valid for inspection.
     *      - #ER_BUS_NOT_CONNECTED if a connection has not been made with a local bus.
     *      - Other error status codes indicating a failure.
     */
    QStatus BindSessionPort(SessionPort& sessionPort, const SessionOpts& opts, uint32_t& disposition);

    /**
     * Join a session.
     * This method is a shortcut/helper that issues an org.codeauora.AllJoyn.Bus.JoinSession method call to the local daemon
     * and interprets the response.
     *
     * @param[in]  sessionHost   Bus name of attachment that is hosting the session to be joined.
     * @param[in]  sessionPort   SessionPort of sessionHost to be joined.
     * @param[out] disposition   @ref JoinSessionReplyAnchor "ALLJOYN_JOINSESSION_REPLY_*" constant from AllJoynStd.h.
     * @param[out] sessionId     Unique identifier for session. Valid if disposition is ALLJOYN_CREATESESSION_REPLY_SUCCESS.
     * @param[out] opts          Session options.
     * @return
     *      - #ER_OK if daemon response was received. ER_OK indicates that disposition is valid for inspection.
     *      - #ER_BUS_NOT_CONNECTED if a connection has not been made with a local bus.
     *      - Other error status codes indicating a failure.
     */
    QStatus JoinSession(const char* sessionHost, SessionPort sessionPort, uint32_t& disposition, SessionId& sessionId, SessionOpts& opts);

    /**
     * Leave an existing session.
     * This method is a shortcut/helper that issues an org.codeauora.AllJoyn.Bus.LeaveSession method call to the local daemon
     * and interprets the response.
     *
     * @param[in]  sessionId     Session id.
     * @param[out] disposition   @ref LeaveSessionReplyAnchor "ALLJOYN_LEAVESESSION_REPLY_*" constant from AllJoynStd.h.
     * @return
     *      - #ER_OK if daemon response was received. ER_OK indicates that disposition is valid for inspection.
     *      - #ER_BUS_NOT_CONNECTED if a connection has not been made with a local bus.
     *      - Other error status codes indicating a failure.
     */
    QStatus LeaveSession(const SessionId& sessionId, uint32_t& disposition);

    /**
     * Get the file descriptor for a raw (non-message based) session.
     *
     * @param sessionId   Id of an existing streamming session.
     * @param sockFd      [OUT] Socket file descriptor for session.
     * @return ER_OK if successful.
     */
    QStatus GetSessionFd(SessionId sessionId, qcc::SocketFd& sockFd);

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
