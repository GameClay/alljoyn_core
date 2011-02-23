/**
 * @file
 * SimpleBusListener is a syncronous bus listener that fits the need of applications that can
 * handled all bus events from the main thread.
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
#ifndef _ALLJOYN_SIMPLEBUSLISTENER_H
#define _ALLJOYN_SIMPLEBUSLISTENER_H

#ifndef __cplusplus
#error Only include SimpleBusListener.h in C++ code.
#endif

#include <alljoyn/Session.h>
#include <alljoyn/QosInfo.h>
#include <alljoyn/BusListener.h>

namespace ajn {

/**
 * Helper class that provides a blocking API that allows application threads to wait for bus events.
 */
class SimpleBusListener : public BusListener {
  public:

    /**
     * The various bus events.
     */
    static const uint32_t FOUND_ADVERTISED_NAME = 0x0001;
    static const uint32_t LOST_ADVERTISED_NAME  = 0x0002;
    static const uint32_t NAME_OWNER_CHANGED    = 0x0004;
    static const uint32_t SESSION_LOST          = 0x0008;
    static const uint32_t ACCEPT_SESSION        = 0x0010;
    static const uint32_t ALL_EVENTS            = 0x00FF;
    static const uint32_t NO_EVENT              = 0x0000;

    static const uint32_t FOREVER = -1;

    /**
     * Constructor that intializes a bus listener with specific events enabled.
     *
     * @param enabled   A logical OR of the bus events to be enabled for this listener.
     */
    SimpleBusListener(uint32_t enabled = 0);

    /**
     * Set an event filter. This overrides the events enabled by the constructor. Any queued events
     * that are not enabled are discarded.
     *
     * @param enabled  A logical OR of the bus events to be enabled for this listener.
     */
    void SetFilter(uint32_t enabled);

    /**
     * This union is used to return busEvent information for the bus events.
     */
    class BusEvent {
        friend class SimpleBusListener;
      public:
        uint32_t eventType;             ///< The busEvent type identifies which variant from the union applies.
        union {
            struct {
                const char* name;       ///< well known name that the remote bus is advertising that is of interest to this attachment.
                const QosInfo* advQos;  ///< Advertised quality of service.
                const char* namePrefix; ///< The well-known name prefix used in call to FindAdvertisedName that triggered the busEvent.
            } foundAdvertisedName;
            struct {
                const char* name;       ///< A well known name that the remote bus is advertising that is of interest to this attachment.
                const char* namePrefix; ///< The well-known name prefix that was used in a call to FindAdvertisedName that triggered this callback.
            } lostAdvertisedName;
            struct {
                const char* busName;       ///< The well-known name that has changed.
                const char* previousOwner; ///< The unique name that previously owned the name or NULL if there was no previous owner.
                const char* newOwner;      ///< The unique name that now owns the name or NULL if the there is no new owner.
            } nameOwnerChanged;
            struct {
                SessionId sessionId;     ///< Id of session that was lost.
            } sessionLost;
            struct {
                const char* sessionName; ///< Name of session.
                SessionId id;            ///< Id of session.
                const char* joiner;      ///< Unique name of potential joiner.
                const QosInfo* qos;      ///< Incoming quality of service.
            } acceptSession;
        };

        /**
         * Constructor
         */
        BusEvent() : eventType(0) { }

        /**
         * Copy constructor.
         */
        BusEvent(const BusEvent& other) { *this = other; }

        /**
         * Assignment operator.
         */
        BusEvent& operator=(const BusEvent& other);

      private:

        /**
         * @internal  Storage for the busEvent strings.
         */
        qcc::String strings;
        /**
         * @internal  Storage for quality of service information.
         */
        QosInfo qosInfo;
    };

    /**
     * Wait for a bus event.
     *
     * @param busEvent Returns the busEvent type and related information.
     * @param timeout  A timeout in milliseconds to wait for the busEvent, 0 means don't wait just
     *                 check for an busEvent and return, FOREVER (-1) means wait forever.
     *
     * @return #ER_OK if an even was received.
     *         #ER_TIMEOUT if the wait timed out.
     *         #ER_THREAD_ALERTED if the wait unblocked due to a signal
     */
    QStatus WaitForEvent(BusEvent& busEvent, uint32_t timeout = FOREVER);

    /**
     * On receiving an ACCEPT_SESSION busEvent the application must call this function to accept or
     * rejct the session request. Calling WaitForEvent will automatically reject the session request.
     *
     * @param accept  Select or reject this session request.
     */
    QStatus AcceptSession(bool accept);

    /**
     * Desctructor.
     */
    ~SimpleBusListener();

  private:

    /**
     * Bit mask of events enabled for this listener.
     */
    uint32_t enabled;

    void FoundAdvertisedName(const char* name, const QosInfo& advQos, const char* namePrefix);
    void LostAdvertisedName(const char* name, const char* namePrefix);
    void NameOwnerChanged(const char* busName, const char* previousOwner, const char* newOwner);
    void SessionLost(const SessionId& sessionId);
    bool AcceptSession(const char* sessionName, SessionId id, const char* joiner, const QosInfo& qos);

    /**
     * @internal
     * Internal storage for this class.
     */
    class Internal;
    Internal& internal;

};

}

#endif
