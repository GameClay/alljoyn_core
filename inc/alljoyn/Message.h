#ifndef _ALLJOYN_MESSAGE_H
#define _ALLJOYN_MESSAGE_H
/**
 * @file
 * This file defines a class for parsing and generating message bus messages
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

#include <qcc/platform.h>
#include <alljoyn/Session.h>
#include <Status.h>
#include <alljoyn/AllJoynCTypes.h>
#include <alljoyn/MsgArg.h>

#ifdef __cplusplus

#include <qcc/String.h>
#include <qcc/ManagedObj.h>

namespace ajn {

static const size_t ALLJOYN_MAX_NAME_LEN   =     255;  /*!<  The maximum length of certain bus names */
static const size_t ALLJOYN_MAX_ARRAY_LEN  =  131072;  /*!<  DBus limits array length to 2^26. AllJoyn limits it to 2^17 */
static const size_t ALLJOYN_MAX_PACKET_LEN =  (ALLJOYN_MAX_ARRAY_LEN + 4096);  /*!<  DBus limits packet length to 2^27. AllJoyn limits it further to 2^17 + 4096 to allow for 2^17 payload */

/** @name Endianess indicators */
// @{
/** indicates the bus is little endian */
static const uint8_t ALLJOYN_LITTLE_ENDIAN = 'l';
/** indicates the bus is big endian */
static const uint8_t ALLJOYN_BIG_ENDIAN    = 'B';
// @}


/** @name Flag types */
// @{
/** No reply is expected*/
static const uint8_t ALLJOYN_FLAG_NO_REPLY_EXPECTED  = 0x01;
/** Auto start the service */
static const uint8_t ALLJOYN_FLAG_AUTO_START         = 0x02;
/** Allow messages from remote hosts (valid only in Hello message) */
static const uint8_t ALLJOYN_FLAG_ALLOW_REMOTE_MSG   = 0x04;
/** Global (bus-to-bus) broadcast */
static const uint8_t ALLJOYN_FLAG_GLOBAL_BROADCAST   = 0x20;
/** Header is compressed */
static const uint8_t ALLJOYN_FLAG_COMPRESSED         = 0x40;
/** Body is encrypted */
static const uint8_t ALLJOYN_FLAG_ENCRYPTED          = 0x80;
// @}

/** ALLJOYN protocol version */
static const uint8_t ALLJOYN_MAJOR_PROTOCOL_VERSION  = 1;

/*
 * Forward declarations.
 */
class RemoteEndpoint;


/** Message types */
typedef enum {
    MESSAGE_INVALID     = 0, ///< an invalid message type
    MESSAGE_METHOD_CALL = 1, ///< a method call message type
    MESSAGE_METHOD_RET  = 2, ///< a method return message type
    MESSAGE_ERROR       = 3, ///< an error message type
    MESSAGE_SIGNAL      = 4  ///< a signal message type
} AllJoynMessageType;



/** AllJoyn Header field types  */
typedef enum {

    /* Wire-protocol defined header field types */
    ALLJOYN_HDR_FIELD_INVALID = 0,              ///< an invalid header field type
    ALLJOYN_HDR_FIELD_PATH,                     ///< an object path header field type
    ALLJOYN_HDR_FIELD_INTERFACE,                ///< a message interface header field type
    ALLJOYN_HDR_FIELD_MEMBER,                   ///< a member (message/signal) name header field type
    ALLJOYN_HDR_FIELD_ERROR_NAME,               ///< an error name header field type
    ALLJOYN_HDR_FIELD_REPLY_SERIAL,             ///< a reply serial number header field type
    ALLJOYN_HDR_FIELD_DESTINATION,              ///< message destination header field type
    ALLJOYN_HDR_FIELD_SENDER,                   ///< senders well-known name header field type
    ALLJOYN_HDR_FIELD_SIGNATURE,                ///< message signature header field type
    ALLJOYN_HDR_FIELD_HANDLES,                  ///< number of file/socket handles that accompany the message
    /* AllJoyn defined header field types */
    ALLJOYN_HDR_FIELD_TIMESTAMP,                ///< time stamp header field type
    ALLJOYN_HDR_FIELD_TIME_TO_LIVE,             ///< messages time-to-live header field type
    ALLJOYN_HDR_FIELD_COMPRESSION_TOKEN,        ///< message compression token header field type
    ALLJOYN_HDR_FIELD_SESSION_ID,               ///< Session id field type
    ALLJOYN_HDR_FIELD_UNKNOWN                   ///< unknown header field type also used as maximum number of header field types.
} AllJoynFieldType;


/** AllJoyn header fields */
class HeaderFields {

  public:

    /**
     * The header field values.
     */
    MsgArg field[ALLJOYN_HDR_FIELD_UNKNOWN];

    /**
     * Table to identify which header fields can be compressed.
     */
    static const bool Compressible[ALLJOYN_HDR_FIELD_UNKNOWN + 1];

    /**
     * Table to map the header field to a AllJoynTypeId
     */
    static const AllJoynTypeId FieldType[ALLJOYN_HDR_FIELD_UNKNOWN + 1];

    /**
     * Returns a string representation of the header fields.
     *
     * @param indent   Indentation level.
     *
     * @return  The string representation of the header fields.
     */
    qcc::String ToString(size_t indent =  0) const;

    /** Default constructor */
    HeaderFields() { }

    /** Copy constructor */
    HeaderFields(const HeaderFields& other);

    /** Assignment */
    HeaderFields& operator=(const HeaderFields& other);
};


/**
 * Forward definition
 */
class _Message;
class BusAttachment;

/**
 * Message is a reference counted (managed) version of _Message
 */
typedef qcc::ManagedObj<_Message> Message;


/**
 * This class implements the functionality underlying the #Message class. Instances of #_Message should not be declared directly by applications. Rather applications create instances of
 * the class #Message which handles reference counting for the underlying #_Message instance. The members of #_Message are always accessed indirectly via #Message.
 */
class _Message {

    friend class BusObject;
    friend class ProxyBusObject;
    friend class RemoteEndpoint;
    friend class EndpointAuth;
    friend class LocalEndpoint;
    friend class DaemonRouter;
    friend class DBusObj;
    friend class AllJoynObj;
    friend class DeferredMsg;
    friend class AllJoynPeerObj;

  public:
    /**
     * Constructor for a message
     *
     * @param bus  The bus that this message is sent or received on.
     */
    _Message(BusAttachment& bus);

    /**
     * Copy constructor for a message
     *
     * @param other  Message to copy from.
     */
    _Message(const _Message& other);

    /**
     * Determine if message is a broadcast signal.
     *
     * @return  Return true if this is a broadcast signal.
     */
    bool IsBroadcastSignal() const {
        return (GetType() == MESSAGE_SIGNAL) && (hdrFields.field[ALLJOYN_HDR_FIELD_DESTINATION].typeId == ALLJOYN_INVALID);
    }

    /**
     * Messages broadcast to all devices are global broadcast messages.
     *
     * @return  Return true if this is a global broadcast message.
     */
    bool IsGlobalBroadcast() const { return IsBroadcastSignal() && (msgHeader.flags & ALLJOYN_FLAG_GLOBAL_BROADCAST); }

    /**
     * Returns the flags for the message.
     * @return flags for the message
     *
     * @see flag types in Message.h file
     */
    uint8_t GetFlags() const { return msgHeader.flags; }

    /**
     * Return true if message's TTL header indicates that it is expired
     *
     * @param[out] tillExpireMS  Written with number of milliseconds before message expires if non-null
     *                           If message never expires value is set to the maximum uint32_t value.
     *
     * @return Returns true if the message's TTL header indicates that is has expired.
     */
    bool IsExpired(uint32_t* tillExpireMS = NULL) const;

    /**
     * Determine if the message is marked as unreliable. Unreliable messages have a non-zero
     * time-to-live and may be silently discarded.
     *
     * @return  Returns true if the message is unreliabled, that is, has a non-zero time-to-live.
     */
    bool IsUnreliable() const { return ttl != 0; }

    /**
     * Determine if the message was encrypted.
     *
     * @return  Returns true if the message was encrypted.
     */
    bool IsEncrypted() const { return (msgHeader.flags & ALLJOYN_FLAG_ENCRYPTED) != 0; }

    /**
     * Get the name of the authentication mechanism that was used to generate the encryption key if
     * the message is encrypted.
     *
     * @return  the name of an authentication mechanism or an empty string.
     */
    const qcc::String& GetAuthMechanism() const { return authMechanism; }

    /**
     * Return the type of the message
     */
    AllJoynMessageType GetType() const { return (AllJoynMessageType)msgHeader.msgType; }

    /**
     * Return the arguments for this message.
     *
     * @param[out] args  Returns the arguments
     * @param[out] numArgs The number of arguments
     */
    void GetArgs(size_t& numArgs, const MsgArg*& args) { args = msgArgs; numArgs = numMsgArgs; }

    /**
     * Return a specific argument.
     *
     * @param argN  The index of the argument to get.
     *
     * @return
     *      - The argument
     *      - NULL if unmarshal failed or there is not such argument.
     */
    const MsgArg* GetArg(size_t argN = 0) { return (argN < numMsgArgs) ? &msgArgs[argN] : NULL; }

    /**
     * Unpack and return the arguments for this message. This method uses the functionality from
     * MsgArg::Get() see MsgArg.h for documentation.
     *
     * @param signature  The signature to match against the message arguments.
     * @param ...        Pointers to return references to the unpacked values.
     * @return  ER_OK if successful.
     */
    QStatus GetArgs(const char* signature, ...);

    /**
     * Accessor function to get serial number for the message. Usually only important for
     * #MESSAGE_METHOD_CALL for matching up the reply to the call.
     *
     * @return the serial number of the %Message
     */
    uint32_t GetCallSerial() const { return msgHeader.serialNum; }

    /**
     * Get a reference to all of the header fields for this message.
     *
     * @return A const reference to the header fields for this message.
     */
    const HeaderFields& GetHeaderFields() const { return hdrFields; }

    /**
     * Accessor function to get the signature for this message
     * @return
     *      - The AllJoyn SIGNATURE string stored in the AllJoyn header field
     *      - An empty string if unable to find the AllJoyn signature
     */
    const char* GetSignature() const {
        if (hdrFields.field[ALLJOYN_HDR_FIELD_SIGNATURE].typeId == ALLJOYN_SIGNATURE) {
            return hdrFields.field[ALLJOYN_HDR_FIELD_SIGNATURE].v_signature.sig;
        } else {
            return "";
        }
    }

    /**
     * Accessor function to get the object path for this message
     * @return
     *      - The AllJoyn object path string stored in the AllJoyn header field
     *      - An empty string if unable to find the AllJoyn object path
     */
    const char* GetObjectPath() const {
        if (hdrFields.field[ALLJOYN_HDR_FIELD_PATH].typeId == ALLJOYN_OBJECT_PATH) {
            return hdrFields.field[ALLJOYN_HDR_FIELD_PATH].v_objPath.str;
        } else {
            return "";
        }
    }

    /**
     * Accessor function to get the interface for this message
     * @return
     *      - The AllJoyn interface string stored in the AllJoyn header field
     *      - An empty string if unable to find the interface
     */
    const char* GetInterface() const {
        if (hdrFields.field[ALLJOYN_HDR_FIELD_INTERFACE].typeId == ALLJOYN_STRING) {
            return hdrFields.field[ALLJOYN_HDR_FIELD_INTERFACE].v_string.str;
        } else {
            return "";
        }
    }

    /**
     * Accessor function to get the member (method/signal) name for this message
     * @return
     *      - The AllJoyn member (methoud/signal) name string stored in the AllJoyn header field
     *      - An empty string if unable to find the member name
     */
    const char* GetMemberName() const {
        if (hdrFields.field[ALLJOYN_HDR_FIELD_MEMBER].typeId == ALLJOYN_STRING) {
            return hdrFields.field[ALLJOYN_HDR_FIELD_MEMBER].v_string.str;
        } else {
            return "";
        }
    }

    /**
     * Accessor function to get the reply serial number for the message. Only meaningful for #MESSAGE_METHOD_RET
     * @return
     *      - The serial number for the message stored in the AllJoyn header field
     *      - Zero if unable to find the serial number. Note that 0 is an invalid serial number.
     */
    uint32_t GetReplySerial() const {
        if (hdrFields.field[ALLJOYN_HDR_FIELD_REPLY_SERIAL].typeId == ALLJOYN_UINT32) {
            return hdrFields.field[ALLJOYN_HDR_FIELD_REPLY_SERIAL].v_uint32;
        } else {
            return 0;
        }
    }

    /**
     * Accessor function to get the sender for this message.
     *
     * @return
     *      - The senders well-known name string stored in the AllJoyn header field.
     *      - An empty string if the message did not specify a sender.
     */
    const char* GetSender() const {
        if (hdrFields.field[ALLJOYN_HDR_FIELD_SENDER].typeId == ALLJOYN_STRING) {
            return hdrFields.field[ALLJOYN_HDR_FIELD_SENDER].v_string.str;
        } else {
            return "";
        }
    }

    /**
     * Get the unique name of the endpoint that the message was recevied on.
     *
     * @return
     *     - The unique name of the endpoint that the message was received on.
     */
    const char* GetRcvEndpointName() const {
        return rcvEndpointName.c_str();
    }

    /**
     * Accessor function to get the destination for this message
     * @return
     *      - The message destination string stored in the AllJoyn header field.
     *      - An empty string if unable to find the message destination.
     */
    const char* GetDestination() const {
        if (hdrFields.field[ALLJOYN_HDR_FIELD_DESTINATION].typeId == ALLJOYN_STRING) {
            return hdrFields.field[ALLJOYN_HDR_FIELD_DESTINATION].v_string.str;
        } else {
            return "";
        }
    }

    /**
     * Accessor function to get the compression token for the message.
     *
     * @return
     *      - Compression token for the message stored in the AllJoyn header field
     *      - 0 'zero' if there is no compression token.
     */
    uint32_t GetCompressionToken() const {
        if (hdrFields.field[ALLJOYN_HDR_FIELD_COMPRESSION_TOKEN].typeId == ALLJOYN_UINT32) {
            return hdrFields.field[ALLJOYN_HDR_FIELD_COMPRESSION_TOKEN].v_uint32;
        } else {
            return 0;
        }
    }

    /**
     * Accessor function to get the session id for the message.
     *
     * @return
     *      - Session id for the message
     *      - 0 'zero' if sender did not specify a session
     */
    uint32_t GetSessionId() const {
        if (hdrFields.field[ALLJOYN_HDR_FIELD_SESSION_ID].typeId == ALLJOYN_UINT32) {
            return hdrFields.field[ALLJOYN_HDR_FIELD_SESSION_ID].v_uint32;
        } else {
            return 0;
        }
    }

    /**
     * If the message is an error message returns the error name and optionally the error message string
     * @param[out] errorMessage
     *                      - Return the error message string stored
     *                      - leave errorMessage unchanged if error message string not found
     * @return
     *      - If error detected return error name stored in the AllJoyn header field
     *      - NULL if error not detected
     */
    const char* GetErrorName(qcc::String* errorMessage = NULL) const;

    /**
     * Destructor for the %_Message class.
     */
    ~_Message();

    /**
     * Returns an XML string representation of the message
     * @return an XML string representation of the message
     */
    qcc::String ToString() const;

    /**
     * Returns a string that provides a brief description of the message
     * @return a brief description of the message
     */
    qcc::String Description() const;

    /**
     * Returns the timestamp (in milliseconds) for this message. If the message header contained a
     * timestamp this is the estimated timestamp for when the message was sent by the remote device,
     * otherwise it is the timestamp for when the message was unmarshaled. Note that the timestamp
     * is always relative to local time.
     *
     * @return The timestamp for this message.
     */
    uint32_t GetTimeStamp() { return timestamp; };

    /**
     * Equality operator for messages. Messages are equivalent iff they are the same message.
     *
     * @param other  The other message to compare.
     *
     * @return  Returns true if this message is the same message as the other message.
     */
    bool operator==(const _Message& other) { return this == &other; }

  protected:

    /*
     * These methods and members are protected rather than private to facilitate unit testing.
     */
    /// @cond ALLJOYN_DEV
    /**
     * @internal
     * Turn a method call message into a method reply message
     *
     * @param args        The arguments for the reply (can be NULL)
     * @param numArgs     The number of arguments
     * @return
     *      - #ER_OK if successful
     *      - An error status otherwise
     */
    QStatus ReplyMsg(const MsgArg* args,
                     size_t numArgs);

    /**
     * @internal
     * Turn a method call message into an error message
     *
     * @param errorName   The name of this error
     * @param description Informational string describing details of the error
     * @return
     *      - #ER_OK if successful
     *      - An error status otherwise
     */
    QStatus ErrorMsg(const char* errorName,
                     const char* description);

    /**
     * @internal
     * Turn a method call message into an error message
     *
     * @param status      The status code for this error
     * @return
     *      - #ER_OK if successful
     *      - An error status otherwise
     */
    QStatus ErrorMsg(QStatus status);

    /**
     * @internal
     * Compose a new internally generated error message.
     *
     * @param errorName   The name of this error
     * @param replySerial The serial number the method call this message is replying to.
     */
    void ErrorMsg(const char* errorName,
                  uint32_t replySerial);

    /**
     * @internal
     * Compose a new internally generated error message from a status code
     *
     * @param status      The status code for this error
     * @param replySerial The serial number the method call this message is replying to.
     */
    void ErrorMsg(QStatus status,
                  uint32_t replySerial);

    /**
     * @internal
     * Compose a method call message
     *
     * @param signature   The signature (checked against the args)
     * @param destination The destination for this message
     * @param sessionId   The sessionId to use for this method call or 0 for any
     * @param objPath     The object the method call is being sent to
     * @param iface       The interface for the method (can be NULL)
     * @param methodName  The name of the method to call
     * @param serial      Returns the serial number for this method call
     * @param args        The method call argument list (can be NULL)
     * @param numArgs     The number of arguments
     * @param flags       A logical OR of the AllJoyn flags
     * @return
     *      - #ER_OK if successful
     *      - An error status otherwise
     */
    QStatus CallMsg(const qcc::String& signature,
                    const qcc::String& destination,
                    SessionId sessionId,
                    const qcc::String& objPath,
                    const qcc::String& iface,
                    const qcc::String& methodName,
                    uint32_t& serial,
                    const MsgArg* args,
                    size_t numArgs,
                    uint8_t flags);

    /**
     * @internal
     * Compose a signal message
     *
     * @param signature   The signature (checked against the args)
     * @param destination The destination for this message
     * @param sessionId   The sessionId to use for this signal msg or 0 for any
     * @param objPath     The object sending the signal
     * @param iface       The interface for the method (can be NULL)
     * @param signalName  The name of the signal being sent
     * @param args        The signal argument list (can be NULL)
     * @param numArgs     The number of arguments
     * @param flags       A logical OR of the AllJoyn flags.
     * @param timeToLive  Time-to-live in milliseconds. Signals that cannot be sent within this time
     *                    limit are discarded. Zero indicates reliable delivery.
     * @return
     *      - #ER_OK if successful
     *      - An error status otherwise
     */
    QStatus SignalMsg(const qcc::String& signature,
                      const char* destination,
                      SessionId sessionId,
                      const qcc::String& objPath,
                      const qcc::String& iface,
                      const qcc::String& signalName,
                      const MsgArg* args,
                      size_t numArgs,
                      uint8_t flags,
                      uint16_t timeToLive);


    /**
     * @internal
     * Unmarshal the message arguments.
     *
     * @param expectedSignature       The expected signature for this message.
     * @param expectedReplySignature  The expected reply signature for this message if it is a
     *                                method call message or NULL otherwise.
     *
     * @return
     *         - #ER_OK if the message was unmarshaled
     *         - Error status indicating why the unmarshal failed.
     */
    QStatus UnmarshalArgs(const qcc::String& expectedSignature,
                          const char* expectedReplySignature = NULL);

    /**
     * @internal
     * Reads and unmarshals a message from a source. Only the message header is unmarshaled at this
     * time.
     *
     * @param endpoint       The endpoint to marshal the message data from.
     * @param checkSender    True if message's sender field shold be validated against the endpoint's unique name.
     * @param pedantic       Perform detailed checks on the header fields.
     * @param timeout        If non-zero, a timeout in milliseconds to wait for a message to unmarshal.
     * @return
     *      - #ER_OK if successful
     *      - An error status otherwise
     */
    QStatus Unmarshal(RemoteEndpoint& endpoint, bool checkSender, bool pedantic = true, uint32_t timeout = 0);

    /**
     * @internal
     * Deliver a marshaled message to an sink.
     *
     * @param endpoint   Endpoint to receive marshaled message.
     * @return
     *      - #ER_OK if successful
     *      - An error status otherwise
     */
    QStatus Deliver(RemoteEndpoint& endpoint);

    /**
     * @internal
     */
    QStatus ReMarshal(const char* senderName, bool newSerial = false);

    /// @endcond
  private:

    /**
     * Assignment operator.
     *
     * @param other   RHS of assignment.
     */
    _Message operator=(const _Message& other);

    /**
     * Add the expansion rule described in the message args to the remote endpoint from which this message
     * was received.
     *
     * @param token         The expansion token.
     * @param expansionArg  The the list of arguments that describes the expansion.
     *
     * @return
     *      - #ER_OK if the expansion rule was succesfully parsed and added.
     *      - An error status otherwise
     */
    QStatus AddExpansionRule(uint32_t token, const MsgArg* expansionArg);

    /**
     * Get a MsgArg that describes the header expansion of a given compression token.
     *
     * @param token        The compression token.
     * @param expansionArg [out] Returns a MsgArg that describes the expansion. The MsgArg has the
     *                     signature "a(yv)".
     *
     * @return - #ER_OK if the expansion is available.
     *         - #ER_BUS_CANNOT_EXPAND_MESSAGE if there is not expansion for the specified token.
     */
    QStatus GetExpansion(uint32_t token, MsgArg& expansionArg);

    /**
     * Compose the special hello method call required to establish a connection
     *
     * @param      isBusToBus   true iff connection attempt is between two AllJoyn instances (bus joining).
     * @param      allowRemote  true iff connection allows messages from remote devices.
     * @param[out] serial  Returns the serial number for this method call
     *
     * @return
     *      - #ER_OK if hello method call was sent successfully.
     *      - An error status otherwise
     */
    QStatus HelloMessage(bool isBusToBus, bool allowRemote, uint32_t& serial);

    /**
     * Compose the reply to the hello method call
     *
     * @param isBusToBus  true iff connection attempt is between two AllJoyn instances (bus joining).
     * @param uniqueName  The new unique name for the sender.
     * @return
     *      - #ER_OK if reply to the hello method call was successful
     *      - An error status otherwise
     */
    QStatus HelloReply(bool isBusToBus, const qcc::String& uniqueName);

    typedef struct MessageHeader {
        char endian;           ///< The endian-ness of this message
        uint8_t msgType;       ///< Indicates if the message is method call, signal, etc.
        uint8_t flags;         ///< Flag bits
        uint8_t majorVersion;  ///< Major version of this message
        uint32_t bodyLen;      ///< Length of the body data
        uint32_t serialNum;    ///< serial of this message
        uint32_t headerLen;    ///< Length of the header fields
        MessageHeader() : endian(0), msgType(MESSAGE_INVALID), flags(0), majorVersion(0), bodyLen(0), serialNum(0), headerLen(0) { }
    } MessageHeader;


#if (QCC_TARGET_ENDIAN == QCC_LITTLE_ENDIAN)
    static const char myEndian = ALLJOYN_LITTLE_ENDIAN; ///< Native endianness of host system we are running on: little endian.
#else
    static const char myEndian = ALLJOYN_BIG_ENDIAN;    ///< Native endianness of host system we are running on: big endian.
#endif

    BusAttachment& bus;          ///< The bus this message was received or will be sent on.

    bool endianSwap;             ///< true if endianness will be swapped.

    MessageHeader msgHeader;     ///< Current message header.

    uint64_t* msgBuf;            ///< Pointer to the current msg buffer (uint64_t to ensure 8 byte alignment).
    MsgArg* msgArgs;             ///< Pointer to the unmarshaled arguments.
    uint8_t numMsgArgs;          ///< Number of message args (signature cannot be longer than 255 chars).

    size_t bufSize;              ///< The current allocated size of the msg buffer.
    uint8_t* bufEOD;             ///< End of data currently in buffer.
    uint8_t* bufPos;             ///< Pointer to the position in buffer.
    uint8_t* bodyPtr;            ///< Pointer to start of message body.

    uint16_t ttl;                ///< Time to live
    uint32_t timestamp;          ///< Timestamp (local time) for messages with a ttl.

    qcc::String replySignature;  ///< Expected reply signature for a method call

    qcc::String authMechanism;   ///< For secure messages indicates the authentication mechanism that was used

    qcc::String rcvEndpointName; ///< Name of Endpoint that received this message.

    qcc::SocketFd* handles;      ///< Array of file/socket descriptors.
    size_t numHandles;           ///< Number of handles in the handles array
    bool encrypt;                ///< True if the message is to be encrypted

    /**
     * The header fields for this message. Which header fields are present depends on the message
     * type defined in the message header.
     */
    HeaderFields hdrFields;

    /* Internal methods unmarshal side */

    void ClearHeader();
    QStatus ParseValue(MsgArg* arg, const char*& sigPtr);
    QStatus ParseStruct(MsgArg* arg, const char*& sigPtr);
    QStatus ParseDictEntry(MsgArg* arg, const char*& sigPtr);
    QStatus ParseArray(MsgArg* arg, const char*& sigPtr);
    QStatus ParseSignature(MsgArg* arg);
    QStatus ParseVariant(MsgArg* arg);

    /**
     * Check that the header fields are valid. This check is automatically performed when a header
     * is successfully unmarshalled.
     *
     * @param pedantic   Perform more detailed checks on the header fields.
     *
     * @return
     *      - #ER_OK if the header fields are valid
     *      - an error indicating why it is not.
     */
    QStatus HeaderChecks(bool pedantic);

    /* Internal methods marshal side */

    QStatus EncryptMessage();

    QStatus MarshalMessage(const qcc::String& signature,
                           const qcc::String& destination,
                           AllJoynMessageType msgType,
                           const MsgArg* args,
                           uint8_t numArgs,
                           uint8_t flags,
                           SessionId sessionId);

    QStatus MarshalArgs(const MsgArg* arg, size_t numArgs);
    void MarshalHeaderFields();
    size_t ComputeHeaderLen();

    /**
     * Get string representation of the message
     * @return string representation of the message
     */
    qcc::String ToString(const MsgArg* args, size_t numArgs) const;

};

}

extern "C" {
#endif /* #ifdef __cplusplus */

/** Message types */
typedef enum {
    ALLJOYN_MESSAGE_INVALID     = 0, ///< an invalid message type
    ALLJOYN_MESSAGE_METHOD_CALL = 1, ///< a method call message type
    ALLJOYN_MESSAGE_METHOD_RET  = 2, ///< a method return message type
    ALLJOYN_MESSAGE_ERROR       = 3, ///< an error message type
    ALLJOYN_MESSAGE_SIGNAL      = 4  ///< a signal message type
} alljoyn_messagetype;

/**
 * Create a message object.
 *
 * @param bus  The bus that this message is sent or received on.
 */
extern AJ_API alljoyn_message alljoyn_message_create(alljoyn_busattachment bus);

/**
 * Destroy a message object.
 *
 * @param msg The message to destroy
 */
extern AJ_API void alljoyn_message_destroy(alljoyn_message msg);

/**
 * Return a specific argument.
 *
 * @param msg   The message from which to extract an argument.
 * @param argN  The index of the argument to get.
 *
 * @return
 *      - The argument
 *      - NULL if unmarshal failed or there is not such argument.
 */
extern AJ_API const alljoyn_msgargs alljoyn_message_getarg(alljoyn_message msg, size_t argN);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
