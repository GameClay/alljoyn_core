/**
 * @file
 * Data structures used for the AllJoyn lightweight Name Service
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

#ifndef _NAME_SERVICE_H
#define _NAME_SERVICE_H

#ifndef __cplusplus
#error Only include NameService.h in C++ code.
#endif

#include <vector>
#include <list>

#include <qcc/String.h>
#include <qcc/Thread.h>
#include <qcc/Mutex.h>
#include <qcc/Socket.h>

#include <Status.h>
#include <Callback.h>

#include "NsProtocol.h"

/**
 * @brief Experimental Feature to allow broadcast of Name Service packets
 *
 * Some Access Points, notably Cisco Aironet 1140s, are configured to throw away
 * IPv4 multicast packets by default.  There doesn't seem to be a configuration
 * item available to convince these APs to forward IPv4 multicast.  They do,
 * however, consider support of broadcast to be mandatory since many protocols
 * depend on it.  Because of this we allow the NameService to broadcast its
 * WHO-HAS and IS-AT packets.  We do this over a subnet directed broadcast so
 * we have control over which links the packets go out.
 *
 * Somewhat counter-intuitively, it is the higher-end access points that tend
 * to be more restrictive about multicast, and the more you pay for your access
 * point, the more knobs you get to turn that will give AllJoyn heartburn.  For
 * example, the Cisco Unified Wireless Network (CUWN) Wireless Lan Controllers
 * (WLCs) include settings to turn on or off IPv4 muticast, limit the rate at
 * which IGMP packets are forwarded, and limit the rate at which multicast
 * packets in general are forwarded.
 *
 * This can result in strangely unpredictable discovery behavior as viewed by a
 * user so we are experimenting with just falling back to broadcast for IPv4
 * discovery packets even though networking gurus may be shocked at seeing such
 * "old-fashioned" point to multi-point packets on a modern network.
 *
 * To enable this feature define NS_BROADCAST to be non-zero.
 */
#define NS_BROADCAST 1

namespace ajn {

/**
 * @brief API to provide an implementation dependent Name Service for AllJoyn.
 *
 * The basic goal of this class is to provide a way for AllJoyn daemons, clients
 * and services to find an IP address and socket to use when connecting to
 * other daemons, clients and services.
 *
 * To first approximation, what we want is to allow a user of AllJoyn to search
 * for IP addresses and ports of daemons that provide some AllJoyn service, as
 * defined by a well-known or bus name.
 *
 * For example, a client may come up and ask, "where is an AllJoyn daemon that
 * implements the org.freedesktop.yadda bus name?  The name service may
 * respond, for example, "one is at IP address 10.0.0.1, listening on port
 * 9955 and another is at IP address 10.0.0.2, listening on port 9955".  The
 * client can then do a TCP connect to one of those addresses and ports.
 */
class NameService : public qcc::Thread {
  public:
    /**
     * @brief The module name of the name service, for use in the configuration
     * database.
     */
    static const char* MODULE_NAME;

    /**
     * @brief The property name used to define the interfaces (e.g., eth0) used
     * in discovery.
     */
    static const char* INTERFACES_PROPERTY;

    /**
     * @brief The property value used to specify the wildcard interface name.
     */
    static const char* INTERFACES_WILDCARD;

#if NS_BROADCAST
    /**
     * @brief The property name used to define the interfaces (e.g., eth0) used
     * in discovery.
     */
    static const char* BROADCAST_PROPERTY;
#endif

    /**
     * @brief The maximum size of a name, in general.
     */
    static const uint32_t MAX_NAME_SIZE = 255;

    /**
     * @brief The default time for which an advertisement is valid, in seconds.
     */
    static const uint32_t DEFAULT_DURATION = (120);

    /**
     * @brief The time at which an advertising daemon will retransmit its
     * advertisements.  The advertising daemon should retransmit three
     * times during a default advertisement lifetime.  The 2/3 is really
     * two thirds.  This means that when the countdown time reaches two thirds
     * of the default duration value, one third of the time has expired and we
     * will retransmit.  This, in turn, means we retransmit twice before a
     * remote daemon times out an entry since the timer is set to back to
     * DEFAULT_DURATION after every retransmission.  Units are seconds.
     */
    static const uint32_t RETRANSMIT_TIME = (DEFAULT_DURATION * 2 / 3);

    /**
     * @brief The time at which a daemon using an advertisement begins to think
     * that a remote daemon may be history.  The remote daemon is supposed to
     * retransmit its well-known names periodically.  If we don't receive one
     * of those keepalives, we will start to poke the remote daemon for a
     * keepalive.  Units are seconds.
     */
    static const uint32_t QUESTION_TIME = (DEFAULT_DURATION / 4);

    /**
     * @brief The interval at which the local service will ask a remote daemon
     * if it is alive.
     *
     * When
     */
    static const uint32_t QUESTION_MODULUS = 10;

    /**
     * @brief The number of times we resend WhoHas requests.
     *
     * Legacy 802.11 MACs do not do backoff and retransmission of packets
     * destined for multicast addresses.  Therefore if there is a collision
     * on the air, a multicast packet will be silently dropped.  We get
     * no indication of this at all up at the Socket level.  To avoid this
     * unfortunately common occurrence, which would force a user to wait
     * for the next successful retransmission of exported names, we resend
     * each Locate request this many times.
     */
    static const uint32_t NUMBER_RETRIES = 2;

    /**
     * The time value indicating the time between Locate retries.  Units are
     * seconds.
     */
    static const uint32_t RETRY_INTERVAL = 5;

    /**
     * The modulus indicating the minimum time between interface lazy updates.
     * Units are seconds.
     */
    static const uint32_t LAZY_UPDATE_MIN_INTERVAL = 5;

    /**
     * The modulus indicating the maximum time between interface lazy updates.
     * Units are seconds.
     */
    static const uint32_t LAZY_UPDATE_MAX_INTERVAL = 15;

    /**
     * @brief The time value indicating an advertisement is valid forever.
     */
    static const uint32_t DURATION_INFINITE = 255;

    /**
     * @brief The maximum size of the payload of a name service message.
     *
     * An easy choice for this number would be 64K - 8 bytes (the max size of
     * a UDP payload).  The problem is we need to allocate a buffer of that
     * size on the receiver, and we really expect that payloads will be quite
     * small.
     *
     * Another option is to look at the maximum (or minimum) MTUs of the
     * interfaces over which we will send messages.  This leads to possibly
     * confusing behaviors as different combinations of MTUs in different
     * machines can successfully support different numbers of names at
     * different times based on different configurations.
     *
     * It seems better to have a hard limit that can be easily worked around
     * than a possibly confusing limit that imlplies flakiness.  We can always
     * support 1500 bytes through UDP fragmentation, and we will be using
     * IP and multicast-capable devices, so we expect an MTU of 1500 in the
     * typical case.  So we just work with that as a compromise.  We then take
     * the typical MTU and subtrace UDP, IP and Ethernet Type II overhead.
     *
     * 1500 - 8 -20 - 18 = 1454
     *
     * TODO:  This should probably end up a configurable item for a daemon in
     * case we underestimated the numbers and sizes of exported names.
     */
    static const size_t NS_MESSAGE_MAX = (1454);

    /**
     * @brief Which protocol is of interest.  When making discovery calls, the
     * client must choose whether it is interested in IPv4 or IPv6 addresses.
     * Use these constants to specify which is desired.
     */
    enum Protocol {
        UNSPEC = 0, /** Unspecified */
        IPV4 = 1,   /**< Return the address in IPv4 suitable form */
        IPV6 = 2    /**< Return the address in IPv6 suitable form */
    };

    /**
     * @internal
     *
     * @brief Construct a name service object.
     */
    NameService();

    /**
     * @internal
     *
     * @brief Destroy a name service object.
     */
    virtual ~NameService();

    class IfConfigEntry {
      public:
        qcc::String m_name;
        qcc::String m_addr;
#if NS_BROADCAST
        uint32_t m_prefixlen;
#endif
        uint32_t m_family;

        static const uint32_t UP = 1;
        static const uint32_t BROADCAST = 2;
        static const uint32_t DEBUG = 4;
        static const uint32_t LOOPBACK = 8;
        static const uint32_t POINTOPOINT = 16;
        static const uint32_t RUNNING = 32;
        static const uint32_t NOARP = 64;
        static const uint32_t PROMISC = 128;
        static const uint32_t NOTRAILERS = 256;
        static const uint32_t ALLMULTI = 512;
        static const uint32_t MASTER = 1024;
        static const uint32_t SLAVE = 2048;
        static const uint32_t MULTICAST = 4096;
        static const uint32_t PORTSEL = 8192;
        static const uint32_t AUTOMEDIA = 16384;
        static const uint32_t DYNAMIC = 32768;

        uint32_t m_flags;
        uint32_t m_mtu;
        uint32_t m_index;
    };

    /**
     * @brief Initialize the name service.
     *
     * Some operations relating to initializing the name service and
     * arranging the communication with an underlying network can fail.
     * These operations are broken out into an Init method so we can
     * return an error condition.  You may be able to try and Init()
     * at a later time if an error is returned.
     *
     * @param guid The daemon guid of the daemon using this service.
     * @param enableIPv4 If true, advertise and listen over interfaces that
     *     have IPv4 addresses assigned.  If false, this bit trumps any
     *     OpenInterface() requests for specific IPv4 addresses.
     * @param enableIPv6 If true, advertise and listen over interfaces that
     *     have IPv6 addresses assigned.  If false, this bit trumps any
     *     OpenInterface() requests for specific IPv6 addresses.
     * @param disableBroadcast If true, do not send IPv4 subnet directed
     *     broadcasts.  Note that sending these broadcasts is often the only
     *     way to get IPv4 name service  packets out on APs that block
     *     multicast.
     * @param loopback If true, receive our own advertisements.
     *     Typically used for test programs to listen to themselves talk.
     *
     * @return Status of the operation.  Returns ER_OK on success.
     *
     * @see OpenInterface()
     */
    QStatus Init(
        const qcc::String & guid,
        bool enableIPv4,
        bool enableIPv6,
#if NS_BROADCAST
        bool disableBroadcast,
#endif
        bool loopback = false);

    /**
     * @brief Provide parameters to define the general operation of the protocol.
     *
     * @warning Calling this method is not recommended unless for testing.
     */
    void SetCriticalParameters(uint32_t tDuration, uint32_t tRetransmit, uint32_t tQuestion,
                               uint32_t modulus, uint32_t retries);

    /**
     * @brief Get information regarding the network interfaces on the
     * host.
     *
     * @param entries A vector of IfConfigEntry that will be filled out
     *     with information on the found network interfaces.
     *
     * @return Status of the operation.  Returns ER_OK on success.
     *
     * @see IfConfigEntry
     */
    QStatus IfConfig(std::vector<IfConfigEntry>& entries);

    /**
     * @brief Tell the name service to begin listening and transmitting
     * on the provided network interface.
     *
     * There may be a choice of network interfaces available to run the
     * name service protocol over.  A user of the name service can
     * find these interfaces and explore their charactersistics using
     * the IfConfig method.  When it is decided to actually use one of
     * the interfaces, one can pass the m_name (interface name) variable
     * provided in the selected IfConfigEntry into this method to enable
     * the name service for that interface (or pass a configured name).
     *
     * If the interface is not IFF_UP, the name service will periodically
     * check to see if one comes up and will begin to use it whenever it
     * can.
     *
     * @param name The interface name of the interface to start talking to
     *     and listening on (e.g., eth1 or wlan0).
     *
     * @return Status of the operation.  Returns ER_OK on success.
     *
     * @see IfConfig
     * @see IfConfigEntry
     */
    QStatus OpenInterface(const qcc::String& name);

    /**
     * @brief Tell the name service to begin listening and transmitting
     * on the provided network interface.
     *
     * There may be a choice of network interfaces available to run the
     * name service protocol over.  A user of the name service can
     * find these interfaces and explore their charactersistics using
     * the IfConfig method.  When it is decided to actually use one of
     * the interfaces, pass an IPAddress constructed using the m_addr
     * (interface address) variable provided in the selected
     * IfConfigEntry into this method to enable the name service for
     * that address.
     *
     * If there is no interface that is IFF_UP with the specific address
     * the name service will periodically check to see if one comes up
     * and will begin to use it whenever it can.
     *
     * @param name The interface name of the interface to start talking to
     *     and listening on (e.g., eth1 or wlan0).
     *
     * @return Status of the operation.  Returns ER_OK on success.
     *
     * @see IfConfig
     * @see IfConfigEntry
     */
    QStatus OpenInterface(const qcc::IPAddress& address);

    /**
     * @brief Tell the name service to stop listening and transmitting
     * on the provided network interface.
     *
     * @param address The interface name of the interface to stop talking
     *     to and listening on.
     *
     * @return Status of the operation.  Returns ER_OK on success.
     *
     * @see OpenInterface
     */
    QStatus CloseInterface(const qcc::String& name);

    /**
     * @brief Tell the name service to stop listening and transmitting
     * on the provided network interface.
     *
     * @param address The IP address the interface to stop talking
     *     to and listening on.
     *
     * @return Status of the operation.  Returns ER_OK on success.
     *
     * @see OpenInterface
     */
    QStatus CloseInterface(const qcc::IPAddress& address);

    /**
     * Allow a user to select what kind of retry policy should be used when
     * trying to locate names.  There really isn't one obvious policy.  Consider
     * what happens if the question is locate well-known name N from local
     * daemon L.  If the locate is transmitted and all remote daemons having N
     * hear the WHO-HAS and respond, we certainly do not need to retransmit.
     * If all but one remote daemons that have N hear the question, and respond,
     * is it okay to decide not to ping the remaining daemon?  We cannot
     * possibly know that this situation has happened.  To try and ping a
     * remote daemon that could have missed our request, we would necessarily
     * have to just continue retrying.  One can imagine the case where a
     * single response from any remote daemon would satisfy a user "enough" to
     * satisfy an end-user.  One could imagine a situation where one of a
     * list of names would be satisfactory (several services that accomplish
     * basically the same thing).  One could imagine a situation where the
     * entire list must be found to do the useful thing.
     *
     * To avoid trying to make a single pronouncement on the best way to do
     * things, we provide a selectable policy.
     */
    enum LocatePolicy {
        ALWAYS_RETRY = 1,       /**< Always send the default number of retries */
        RETRY_UNTIL_PARTIAL,    /**< Retry until we get at least one of the names, or run out of retries */
        RETRY_UNTIL_COMPLETE    /**< Retry until we get all of the names, or run out of retries */
    };

    /**
     * @brief Express an interest in locating instances of AllJoyn daemons
     * which support the provided well-known name.
     *
     * Calling this method will result in a name resolution request being
     * multicast to the local subnet.  Other instances of the name service that
     * know about daemons that match the constraints will respond to this
     * request.
     *
     * Responses to this request will be filtered after the first response from
     * each remote daemon.  If, for some reason, the local daemon wants to be
     * re-notified of remote names, it can call this method.  In that case, all
     * state information regarding previous notifications will be dropped and
     * the daemon will get a single repeat notification for each remote name.
     *
     * If users of the name service are interested in being notified of these
     * of services, they are expected to set the Found callback function using
     * SetFoundCallback().
     *
     * @see SetFoundCallback()
     *
     * There is also a corresponding Lost callback used to be notified when
     * services vanish off the net.
     *
     * @see SetLostCallback()
     * @see LocatePolicy
     *
     * @param[in] wkn The AllJoyn well-known name to find (e.g.,
     *     "org.freedesktop.Sensor").  Wildcards are supported in the
     *     sense of Linux shell wildcards.  See fnmatch(3C) for details.
     * @param[in] policy The retransmission policy for this Locate event.
     *
     * @return Status of the operation.  Returns ER_OK on success.
     */
    QStatus Locate(const qcc::String& wkn, LocatePolicy policy = ALWAYS_RETRY);

    /**
     * @brief Set the Callback for notification of discovery events.
     *
     * When using an asynchronous service discovery process, a caller will
     * need to specify how to called back when a service appears, disappears
     * or reaffirms its existence on the network.  This method provides the
     * mechanism for specifying the callback.
     *
     * The method singature for the Callback method must be:
     *
     * @code
     *     void Found(const qcc::String &, const qcc::String &, std::vector<qcc::String> &, uint8_t);
     * @endcode
     *
     * The first parameter is the address and port of the found service,
     * formatted as a bus address the way AllJoyn likes, for example,
     * "tcp:addr=192.168.0.1,port=9955".  The second parameter is the daemon
     * guid string exported by the remote daemon service, or the empty string
     * if that daemon didn't bother to export the string.  The third parameter
     * is a vector of qcc::String that represent the well-known names that the
     * remote daemon is referring to, for example, "org.freedesktop.Yadda".
     * The fourth parameter is the timer value.  A timer value of zero indicates
     * that the names provided in the vectoror qcc::String are no longer
     * available.  A timer value of 255 indicates that the names provided should
     * be interpreted as always available, to the extent that is possible.
     * A timer value between 0 and 255 indicates the number of seconds that the
     * name is expected to be valid.  There will be keepalive messages provided
     * which may extend this time periodically.
     *
     * The typical idiom for using the callback facility is to define a method
     * to receive a found callback in your class and instantiate your class
     * (and this one) somewhere:
     *
     * @code
     *     class MyClass:
     *     public:
     *         void Found(const qcc::String &s, const qcc::String &g, std::vector<qcc::String>& wkn, uint8_t timer) {}
     *     }
     *
     *     ...
     *
     *     MyClass myClass;
     *     NameService nameService;
     * @endcode
     *
     * The callback is connected using the SetCallback() method of this
     * class.
     *
     * @code
     *    nameService.SetCallback(new CallbackImpl<MyClass, void,
     *         const qcc::String &, const qcc::String &, std::vector<qcc::string>, uint8_t>(MyClass, myclass));
     * @endcode
     *
     * From this point forward, all changes to services specified by the
     * Locate() call will be set to you through the callback mechanism.
     *
     * To stop notifications, set the callback (cb) to the pointer value 0.
     *
     * @warning The callback will be in the context of a different thread than
     * your thread, so your Found callback code must be multithread safe (or aware
     * at least).
     *
     * @warning Services may come and go constanly during real network operation.
     * Just because a service was found on the network it does not mean that there
     * will be a service waiting on the provided IP address and port.  This
     * service may be gone by the time you connect; and this is a perfectly
     * legal and reasonable situation.
     *
     * @param[in] cb the Callback pointer.
     */
    void SetCallback(Callback<void, const qcc::String&, const qcc::String&, std::vector<qcc::String>&, uint8_t>* cb);

    /**
     * @brief Set the endpoint information for the current daemon.
     *
     * If an AllJoyn daemon wants to advertise its presence on the local subnet(s)
     * it must call this method before making any actual advertisements in order
     * to set its IPv4 address (if any), its IPv6 address (if any) and port.
     *
     * Addresses must be provided in presentation format (dotted decimal for
     * IPV4 or colon-separated hex for IPV6).  It must also provide the port on
     * which it may be contacted.
     *
     * In order to avoid confusion on the network, this method may only be called
     * once.
     *
     * @param[in] ipv4address The IPv4 address of the calling daemon.  If this
     *     address is provided it is placed all advertisements and trumps the
     *     default "IPv4 advertisements get same interface IPv6 address if IFF_UP"
     *     and "IPv6 advertisements get same interface IPv4 address if IFF_UP"
     *     behavior.
     * @param[in] ipv6address The IPv6 address of the calling daemon.  If this
     *     address is provided it is placed all advertisements and trumps the
     *     default "IPv4 advertisements get same interface IPv6 address if IFF_UP"
     *     and "IPv6 advertisements get same interface IPv4 address if IFF_UP"
     *     behavior.
     * @param[in] port The port on which the calling daemon listens.  Must be
     *     the same for IPv4 and IPv6.
     *
     * @return Status of the operation.  Returns ER_OK on success.
     *
     * @see Init()
     */
    QStatus SetEndpoints(const qcc::String& ipv4address, const qcc::String& ipv6address, uint16_t port);

    /**
     * @brief Set the endpoint information for the current daemon.
     *
     * If an AllJoyn daemon wants to advertise its presence on the local subnet(s)
     * it must call this method before making any actual advertisements in order
     * to set its IPv4 address (if any), its IPv6 address (if any) and port.
     *
     * Addresses must be provided in presentation format (dotted decimal for
     * IPV4 or colon-separated hex for IPV6).  It must also provide the port on
     * which it may be contacted.
     *
     * In order to avoid confusion on the network, this method may only be called
     * once.
     *
     * @param[out] ipv4address The IPv4 address of the daemon.  If this address
     *     has been provided to the name service in a previous call to
     *     SetEndpoints it will be returned, otherwise the referenced string
     *     will be set to empty.
     * @param[out] ipv6address The IPv6 address of the daemon.  If this address
     *     has been provided to the name service in a previous call to
     *     SetEndpoints it will be returned, otherwise the referenced string
     *     will be set to empty.
     * @param[out] port The port on which the calling daemon listens.  Must have
     *     been previously set in a call to SetEndpoints.
     *
     * @return Status of the operation.  Returns ER_OK on success.
     *
     * @see Init()
     */
    QStatus GetEndpoints(qcc::String& ipv4address, qcc::String& ipv6address, uint16_t& port);

    /**
     * @brief Advertise an AllJoyn daemon service.
     *
     * If an AllJoyn daemon wants to advertise the presence of a well-known name
     * on the local subnet(s) it calls this function.  It must have previously
     * provided an appropriately formatted address in presentation format (IPV4 or
     * IPV6) and port over which it may be contacted.
     *
     * This method allows the caller to specify a single well-known interface
     * name supported by the exporting AllJoyn. If the AllJoyn supports multiple
     * interfaces, it is more efficient to call the overloaded method which
     * takes a vector of string.
     *
     * @see SetEndpoints()
     *
     * @param[in] wkn The AllJoyn interface (e.g., "org.freedesktop.Sensor").
     *
     * @return Status of the operation.  Returns ER_OK on success.
     */
    QStatus Advertise(const qcc::String& wkn);

    /**
     * @brief Cancel an AllJoyn daemon service advertisement.
     *
     * If an AllJoyn daemon wants to cancel an advertisement of a well-known name
     * on the local subnet(s) it calls this function.
     *
     * @param[in] wkn The AllJoyn interface (e.g., "org.freedesktop.Sensor").
     *
     * @return Status of the operation.  Returns ER_OK on success.
     */
    QStatus Cancel(const qcc::String& wkn);

    /**
     * @internal
     * @brief Advertise an AllJoyn daemon service.
     *
     * If an AllJoyn daemon wants to advertise the presence of a well-known name
     * on the local subnet(s) it calls this function.  It must have previously
     * provided an appropriately formatted address in presentation format (IPV4 or
     * IPV6) and port over which it may be contacted.
     *
     * This method allows the caller to specify multiple well-known interface
     * name supported by the exporting AllJoyn.  If the AllJoyn supports multiple
     * interfaces, this is the preferred method.
     *
     * @see SetEndpoints()
     *
     * @param[in] wkn A vector of AllJoyn well-known names (e.g., "org.freedesktop.Sensor").
     *
     * @return Status of the operation.  Returns ER_OK on success.
     */
    QStatus Advertise(std::vector<qcc::String>& wkn);

    /**
     * @brief Cancel an AllJoyn daemon service advertisement.
     *
     * If an AllJoyn daemon wants to cancel an advertisement of a well-known name
     * on the local subnet(s) it calls this function.
     *
     * @param[in] wkn A vector of AllJoyn well-known names (e.g., "org.freedesktop.Sensor").
     *
     * @return Status of the operation.  Returns ER_OK on success.
     */
    QStatus Cancel(std::vector<qcc::String>& wkn);

    /**
     * @brief Returns a count of the number of names currently being advertised
     *
     * @return  Returns the number of names currently being advertised
     */
    size_t NumAdvertisements() { return m_advertised.size(); }

  private:
    /**
     * @brief The IPv4 multicast address for the  multicast name service.
     * Should eventually be registered with IANA.
     *
     * @see http://www.iana.org/assignments/multicast-addresses/
     */
    static const char* IPV4_MULTICAST_GROUP;

    /**
     * @brief The IPv6 multicast address for the  multicast name service.
     * Should eventually be registered with IANA.
     *
     * @see http://www.iana.org/assignments/multicast-addresses/
     */
    static const char* IPV6_MULTICAST_GROUP;

    /**
     * @brief The port number for the  multicast name service.
     * Should eventually be registered with IANA.
     *
     * @see http://www.iana.org/assignments/multicast-addresses/
     */
    static const uint16_t MULTICAST_PORT;

#if NS_BROADCAST
    /**
     * @brief The IPv4 broadcast address for the fallback case when Access
     * Points disable multicast.
     */
    static const char* IPV4_GLOBAL_BROADCAST_ADDR;

    /**
     * @brief The port number for the broadcast name service packets.
     * Typically the same port as the multicast case, but can be made
     * different (with a litle work).
     */
    static const uint16_t BROADCAST_PORT;
#endif

    /**
     * @brief
     * Private notion of what state the implementation object is in.
     */
    enum State {
        IMPL_INVALID,           /**< Should never be seen on a constructed object */
        IMPL_SHUTDOWN,          /**< Nothing is running and object may be destroyed */
        IMPL_INITIALIZING,      /**< Object is in the process of coming up and may be inconsistent */
        IMPL_RUNNING,           /**< Object is running and ready to go */
    };

    /**
     * @internal
     * @brief State variable to indicate what the implementation is doing or is
     * capable of doing.
     */
    State m_state;

    class InterfaceSpecifier {
      public:
        qcc::String m_interfaceName;        /**< The interface (cf. eth0) we want to talk to */
        qcc::IPAddress m_interfaceAddr;     /**< The address (cf. 1.2.3.4) we want to talk to */
    };

    class LiveInterface : public InterfaceSpecifier {
      public:
        qcc::IPAddress m_address;           /**< The address of the interface we are talking to */
#if NS_BROADCAST
        uint32_t m_prefixlen;               /**< The address prefix (cf netmask) of the interface we are talking to */
#endif
        qcc::SocketFd m_sockFd;             /**< The socket we are using to talk over */
        uint32_t m_mtu;                     /**< The MTU of the protocol/device we are using */
        uint32_t m_index;                   /**< The interface index of the protocol/device we are using if IPv6 */
    };

    /**
     * @internal
     * @brief A vector of information specifying any interfaces we may want to
     * send or receive multicast packets over.  Interfaces in this vector may
     * be up or down, or may be completely unrelated to any interface in the
     * actual host system.  These are what the user is telling us to use.
     */
    std::vector<InterfaceSpecifier> m_requestedInterfaces;

    /**
     * @internal
     * @brief A vector of information specifying any interfaces we have actually
     * decided to send or receive multicast packets over.  Interfaces in this
     * must have been up when they were added, but may have since gone down.
     * These are interfaces we decided to use based on what the user told us
     * to use.
     */
    std::vector<LiveInterface> m_liveInterfaces;

    /**
     * @internal
     * @brief Mutex object used to protect various lists that may be accessed
     * by multiple threads.
     */
    qcc::Mutex m_mutex;

    /**
     * Main thread entry point.
     *
     * @param arg  Unused thread entry arg.
     */
    qcc::ThreadReturn STDCALL Run(void* arg);

    /**
     * @internal
     * @brief Queue a protocol message for transmission out on the multicast
     * group.
     */
    void QueueProtocolMessage(Header& header);

    /**
     * @internal
     * @brief Send a protocol message out on the multicast group.
     */
#if NS_BROADCAST
    void SendProtocolMessage(
        qcc::SocketFd sockFd,
        qcc::IPAddress interfaceAddress,
        uint32_t interfaceAddressPrefixLen,
        bool sockFdIsIPv4,
        Header& header);
#else
    void SendProtocolMessage(qcc::SocketFd sockFd, bool sockFdIsIpv4, Header& header);
#endif

    /**
     * @internal
     * @brief Do something with a received protocol message.
     */
    void HandleProtocolMessage(uint8_t const* const buffer, uint32_t nbytes, qcc::IPAddress address);

    /**
     * @internal
     * @brief Do something with a received protocol question.
     */
    void HandleProtocolQuestion(WhoHas whoHas, qcc::IPAddress address);

    /**
     * @internal
     * @brief Do something with a received protocol answer.
     */
    void HandleProtocolAnswer(IsAt isAt, uint32_t timer, qcc::IPAddress address);

    Callback<void, const qcc::String&, const qcc::String&, std::vector<qcc::String>&, uint8_t>* m_callback;

    /**
     * @internal
     * @brief A list of all of the names that the user has advertised.
     */
    std::list<qcc::String> m_advertised;

    /**
     * @internal
     * @brief The daemon GUID string of the daemon assoicated with this instance
     * of the name service.
     */
    qcc::String m_guid;

    /**
     * @internal
     * @brief The IPv4 address of the daemon assoicated with this instance of the
     * name service (the daemon's IPv4 address).
     */
    qcc::String m_ipv4address;

    /**
     * @internal
     * @brief The IPv6 address of the daemon assoicated with this instance of the
     * name service (the daemon's IPv6 address).

     */
    qcc::String m_ipv6address;

    /**
     * @internal
     * @brief The port assoicated with this instance of the name service
     * (the daemon port).
     */
    uint16_t m_port;

    /**
     * @internal
     * @brief The time remaining before a set of advertisements must be
     * retransmitted over the multicast link.
     */
    uint32_t m_timer;

    /**
     * @internal
     * @brief Perform periodic protocol maintenance.  Called once per second
     * from the main listener loop.
     */
    void DoPeriodicMaintenance(void);

    /**
     * @internal
     * @brief Retransmit exported advertisements.
     */
    void Retransmit(void);

    /**
     * @internal
     * @brief Vector of name service messages reflecting recent locate
     * requests.  Since wifi MACs don't retry multicast after collision
     * we need to support some form of retry, even though we never get
     * an indication that our send failed.
     */
    std::list<Header> m_retry;

    /**
     * @internal
     * @brief Retry locate requests.
     */
    void Retry(void);

    uint32_t m_tDuration;
    uint32_t m_tRetransmit;
    uint32_t m_tQuestion;
    uint32_t m_modulus;
    uint32_t m_retries;

    /**
     * @internal
     * @brief Listen to our own advertisements if true.
     */
    bool m_loopback;

#if NS_BROADCAST
    /**
     * @internal
     * @brief Send name service packets via IPv4 subnet directed broadcast if
     * true.
     */
    bool m_broadcast;
#endif

    /**
     * @internal
     * @brief Advertise and listen over IPv4 if true.
     */
    bool m_enableIPv4;

    /**
     * @internal
     * @brief Advertise and listen over IPv6 if true.
     */
    bool m_enableIPv6;

    /**
     * @internal
     * @brief Advertise IPv4 address assigned to this interface when multicasting
     * over IPv6 sockets in m_overrideIpv6 mode.  Used  to compensate for broken
     * Android phones that don't support IPv4 multicast.
     */
    qcc::String m_overrideInterface;

    /**
     * @internal
     * @brief Use all available interfaces whenever they may be up if true.
     * Think INADDR_ANY or in6addr_any.
     */
    bool m_any;

    /**
     * @internal
     * @brief Event used to wake up the main name service thread and tell it
     * that a message has been queued on the outbound message list.
     */
    qcc::Event m_wakeEvent;

    /**
     * @internal
     * @brief Set to true to force a lazy update cycle if the open interfaces
     * change.
     */
    bool m_forceLazyUpdate;

    /**
     * @internal
     * @brief A list of name service messages queued for transmission out on
     * the multicast group.
     */
    std::list<Header> m_outbound;

#if defined(QCC_OS_WINDOWS)
    /**
     * @internal @brief A socket to hold to keep winsock initialized
     * as long as the name service is alive.
     */
    qcc::SocketFd m_refSockFd;
#endif

    /**
     * @internal
     * @brief Tear down all live interfaces and remove them from the
     * corresponding list.
     */
    void ClearLiveInterfaces(void);

    /**
     * @internal
     * @brief Make sure that we have socket open to talk and listen to as many
     * of our desired interfaces as possible.
     */
    void LazyUpdateInterfaces(void);
};

} // namespace ajn

#endif // _NAME_SERVICE_H
