/**
 * @file
 * A VirtualEndpoint is a representation of an AllJoyn endpoint that exists behind a remote
 * AllJoyn daemon.
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
#include <vector>
#include "VirtualEndpoint.h"
#include <alljoyn/Message.h>
#include <Status.h>

#define QCC_MODULE "ALLJOYN"

using namespace std;
using namespace qcc;

namespace ajn {

VirtualEndpoint::VirtualEndpoint(const char* uniqueName, RemoteEndpoint& busEndpoint)
    : BusEndpoint(BusEndpoint::ENDPOINT_TYPE_VIRTUAL),
    m_uniqueName(uniqueName)
{
    m_b2bEndpoints.push_back(&busEndpoint);
}

QStatus VirtualEndpoint::PushMessage(Message& msg)
{
    QStatus status = ER_BUS_NO_ROUTE;
    m_b2bEndpointsLock.Lock();
    vector<RemoteEndpoint*>::iterator it = m_b2bEndpoints.begin();
    while (it != m_b2bEndpoints.end()) {
        status = (*it++)->PushMessage(msg);
        if (ER_OK == status) {
            break;
        }
    }
    m_b2bEndpointsLock.Unlock();
    return status;
}

bool VirtualEndpoint::AddBusToBusEndpoint(RemoteEndpoint& endpoint)
{
    QCC_DbgTrace(("VirtualEndpoint::AddBusToBusEndpoint(this=%s, b2b=%s)", GetUniqueName().c_str(), endpoint.GetUniqueName().c_str()));

    m_b2bEndpointsLock.Lock();
    vector<RemoteEndpoint*>::iterator it = m_b2bEndpoints.begin();
    bool found = false;
    while (it != m_b2bEndpoints.end()) {
        if (*it == &endpoint) {
            found = true;
            break;
        }
        ++it;
    }
    if (!found) {
        m_b2bEndpoints.push_back(&endpoint);
    }
    m_b2bEndpointsLock.Unlock();
    return !found;
}

bool VirtualEndpoint::RemoveBusToBusEndpoint(RemoteEndpoint& endpoint)
{
    QCC_DbgTrace(("VirtualEndpoint::RemoveBusToBusEndpoint(this=%s, b2b=%s)", GetUniqueName().c_str(), endpoint.GetUniqueName().c_str()));

    m_b2bEndpointsLock.Lock();
    vector<RemoteEndpoint*>::iterator it = m_b2bEndpoints.begin();
    while (it != m_b2bEndpoints.end()) {
        if (*it == &endpoint) {
            m_b2bEndpoints.erase(it);
            break;
        }
        ++it;
    }
    bool isEmpty = m_b2bEndpoints.empty();
    m_b2bEndpointsLock.Unlock();
    return isEmpty;
}

bool VirtualEndpoint::CanUseRoute(const RemoteEndpoint& b2bEndpoint) const
{
    bool isFound = false;
    m_b2bEndpointsLock.Lock();
    vector<RemoteEndpoint*>::const_iterator it = m_b2bEndpoints.begin();
    while (it != m_b2bEndpoints.end()) {
        if (*it == &b2bEndpoint) {
            isFound = true;
            break;
        }
        ++it;
    }
    m_b2bEndpointsLock.Unlock();
    return isFound;
}

}
