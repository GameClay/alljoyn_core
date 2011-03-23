/**
 * @file
 *
 * This file implements the default bus listener.
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

#include <queue>

#include <qcc/String.h>
#include <qcc/Event.h>
#include <qcc/Mutex.h>
#include <qcc/Thread.h>
#include <qcc/Debug.h>

#include <alljoyn/BusListener.h>
#include <alljoyn/BusAttachment.h>
#include <alljoyn/SimpleBusListener.h>


#define QCC_MODULE "ALLJOYN"


using namespace qcc;
using namespace std;

namespace ajn {

static inline const char* CopyIn(qcc::String& dest, const char* src)
{
    if (src) {
        dest = src;
        return dest.c_str();
    } else {
        return NULL;
    }
}

SimpleBusListener::BusEvent& SimpleBusListener::BusEvent::operator=(const BusEvent& other)
{
    eventType = other.eventType;
    switch (eventType) {
    case FOUND_ADVERTISED_NAME:
        foundAdvertisedName.name = CopyIn(strings[0], other.foundAdvertisedName.name);
        foundAdvertisedName.transport = other.foundAdvertisedName.transport;
        foundAdvertisedName.namePrefix = CopyIn(strings[1], other.foundAdvertisedName.namePrefix);
        break;

    case LOST_ADVERTISED_NAME:
        lostAdvertisedName.name = CopyIn(strings[0], other.lostAdvertisedName.name);
        lostAdvertisedName.namePrefix = CopyIn(strings[1], other.lostAdvertisedName.namePrefix);
        break;

    case NAME_OWNER_CHANGED:
        nameOwnerChanged.busName = CopyIn(strings[0], other.nameOwnerChanged.busName);
        nameOwnerChanged.previousOwner = CopyIn(strings[1], other.nameOwnerChanged.previousOwner);
        nameOwnerChanged.newOwner = CopyIn(strings[2], other.nameOwnerChanged.newOwner);
        break;

    case SESSION_LOST:
        sessionLost.sessionId = other.sessionLost.sessionId;
        break;

    case ACCEPT_SESSION_JOINER:
        acceptSessionJoiner.sessionPort = other.acceptSessionJoiner.sessionPort;
        acceptSessionJoiner.joiner = CopyIn(strings[1], other.acceptSessionJoiner.joiner);
        sessionOpts = *other.acceptSessionJoiner.sessionOpts;
        acceptSessionJoiner.sessionOpts = &sessionOpts;
        break;

    default:
        break;
    }
    return *this;
};

class SimpleBusListener::Internal {
  public:
    Internal() : bus(NULL), acceptEvent(NULL), accepted(false), waiter(false) { }
    Event waitEvent;
    qcc::Mutex lock;
    std::queue<BusEvent> eventQueue;
    BusAttachment* bus;
    Event* acceptEvent;
    bool accepted;
    bool waiter;

    void QueueEvent(BusEvent& ev) {
        lock.Lock();
        eventQueue.push(ev);
        waitEvent.SetEvent();
        lock.Unlock();
    }
};

SimpleBusListener::SimpleBusListener(uint32_t enabled) : enabled(enabled), internal(*(new Internal))
{
}

void SimpleBusListener::FoundAdvertisedName(const char* name, TransportMask transport, const char* namePrefix)
{
    if (enabled & FOUND_ADVERTISED_NAME) {
        BusEvent busEvent;
        busEvent.eventType = FOUND_ADVERTISED_NAME;
        busEvent.foundAdvertisedName.name = name;
        busEvent.foundAdvertisedName.transport = transport;
        busEvent.foundAdvertisedName.namePrefix = namePrefix;
        internal.QueueEvent(busEvent);
    }
}

void SimpleBusListener::LostAdvertisedName(const char* name, const char* namePrefix)
{
    if (enabled & LOST_ADVERTISED_NAME) {
        BusEvent busEvent;
        busEvent.eventType = LOST_ADVERTISED_NAME;
        busEvent.lostAdvertisedName.name = name;
        busEvent.lostAdvertisedName.namePrefix = namePrefix;
        internal.QueueEvent(busEvent);
    }
}

void SimpleBusListener::NameOwnerChanged(const char* busName, const char* previousOwner, const char* newOwner)
{
    if (enabled & NAME_OWNER_CHANGED) {
        BusEvent busEvent;
        busEvent.eventType = NAME_OWNER_CHANGED;
        busEvent.nameOwnerChanged.busName = busName;
        busEvent.nameOwnerChanged.previousOwner = previousOwner;
        busEvent.nameOwnerChanged.newOwner = newOwner;
        internal.QueueEvent(busEvent);
    }
}

void SimpleBusListener::SessionLost(const SessionId& sessionId)
{
    if (enabled & SESSION_LOST) {
        BusEvent busEvent;
        busEvent.eventType = SESSION_LOST;
        busEvent.sessionLost.sessionId = sessionId;
        internal.QueueEvent(busEvent);
    }
}

bool SimpleBusListener::AcceptSessionJoiner(SessionPort sessionPort, const char* joiner, const SessionOpts& opts)
{
    bool ret = false;
    if (enabled & ACCEPT_SESSION_JOINER) {
        BusEvent busEvent;
        busEvent.eventType = ACCEPT_SESSION_JOINER;
        busEvent.acceptSessionJoiner.sessionPort = sessionPort;
        busEvent.acceptSessionJoiner.joiner = joiner;
        busEvent.acceptSessionJoiner.sessionOpts = &opts;
        internal.lock.Lock();
        internal.QueueEvent(busEvent);
        if (!internal.acceptEvent) {
            Event acceptEvent;
            internal.acceptEvent = &acceptEvent;
            internal.lock.Unlock();
            Event::Wait(acceptEvent);
            ret = internal.accepted;
        } else {
            internal.acceptEvent->SetEvent();
            internal.acceptEvent = NULL;
            internal.lock.Unlock();
        }
    }
    return false;
}

QStatus SimpleBusListener::AcceptSessionJoiner(bool accept)
{
    QStatus status = ER_BUS_NO_SESSION;
    if (enabled & ACCEPT_SESSION_JOINER) {
        internal.lock.Lock();
        internal.waiter = true;
        internal.waitEvent.ResetEvent();
        if (internal.acceptEvent) {
            internal.accepted = accept;
            internal.acceptEvent->SetEvent();
            internal.acceptEvent = NULL;
            status = ER_OK;
        }
        /*
         * If we accepted the session joiner wait for the join to complete
         */
        if ((status == ER_OK) && accept) {
            internal.lock.Unlock();
            status = Event::Wait(internal.waitEvent, 10000);
            internal.lock.Lock();
            internal.waitEvent.ResetEvent();
        }
        internal.waiter = false;
        internal.lock.Unlock();
    }
    return status;
}

void SimpleBusListener::SessionJoined(SessionPort sessionPort, SessionId id, const char* joiner)
{
    if (enabled & ACCEPT_SESSION_JOINER) {
        internal.waitEvent.SetEvent();
    }
}

void SimpleBusListener::SetFilter(uint32_t enabled)
{
    internal.lock.Lock();
    this->enabled = enabled;
    if (internal.acceptEvent) {
        AcceptSessionJoiner(false);
    }
    /*
     * Save all queued events that pass the filter.
     */
    size_t pass = 0;
    while (internal.eventQueue.size() != pass) {
        BusEvent ev = internal.eventQueue.front();
        internal.eventQueue.pop();
        if (ev.eventType & enabled) {
            internal.eventQueue.push(ev);
            ++pass;
        }
    }
    if (pass == 0) {
        internal.waitEvent.ResetEvent();
    }
    internal.lock.Unlock();
}

QStatus SimpleBusListener::WaitForEvent(BusEvent& busEvent, uint32_t timeout)
{
    QStatus status = ER_OK;
    internal.lock.Lock();
    busEvent.eventType = NO_EVENT;
    if (!internal.bus) {
        status = ER_BUS_WAIT_FAILED;
        QCC_LogError(status, ("Listener has not been registered with a bus attachment"));
        goto ExitWait;
    }
    if (internal.bus->IsStopping() || !internal.bus->IsStarted()) {
        status = ER_BUS_WAIT_FAILED;
        QCC_LogError(status, ("Bus is not running"));
        goto ExitWait;
    }
    if (internal.waiter) {
        status = ER_BUS_WAIT_FAILED;
        QCC_LogError(status, ("Another thread is already waiting"));
        goto ExitWait;
    }
    if (internal.acceptEvent) {
        status = ER_BUS_WAIT_FAILED;
        QCC_LogError(status, ("An ACCEPT_SESSION_JOINER event has not been accepted"));
        goto ExitWait;
    }
    if (internal.eventQueue.empty() && timeout) {
        internal.waiter = true;
        internal.lock.Unlock();
        status = Event::Wait(internal.waitEvent, (timeout == 0xFFFFFFFF) ? Event::WAIT_FOREVER : timeout);
        internal.lock.Lock();
        internal.waitEvent.ResetEvent();
        internal.waiter = false;
    }
    if (!internal.eventQueue.empty()) {
        busEvent = internal.eventQueue.front();
        internal.eventQueue.pop();
    }

ExitWait:
    internal.lock.Unlock();
    return status;
}

void SimpleBusListener::BusStopping()
{
    /*
     * Unblock any waiting thread
     */
    internal.waitEvent.SetEvent();
    if (internal.acceptEvent) {
        internal.acceptEvent->SetEvent();
    }
}

void SimpleBusListener::ListenerUnRegistered()
{
    internal.lock.Lock();
    internal.bus = NULL;
    internal.lock.Unlock();
}

void SimpleBusListener::ListenerRegistered(BusAttachment* bus)
{
    internal.lock.Lock();
    internal.bus = bus;
    internal.lock.Unlock();
}

SimpleBusListener::~SimpleBusListener()
{
    /*
     * Unblock any threads waiting
     */
    internal.lock.Lock();
    internal.waitEvent.SetEvent();
    if (internal.acceptEvent) {
        internal.acceptEvent->SetEvent();
    }
    internal.lock.Unlock();
    delete &internal;
}

}
