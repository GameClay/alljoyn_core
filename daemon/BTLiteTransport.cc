/**
 * @file
 * AllJoynBTTransport is an implementation of AllJoynTransport that uses Bluetooth.
 *
 * This implementation uses the message bus to talk to the Bluetooth subsystem.
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

#include <limits>
#include <map>
#include <vector>

#include <qcc/platform.h>
#include <qcc/Event.h>
#include <qcc/String.h>
#include <qcc/Debug.h>
#include <qcc/Log.h>
#include <qcc/Logger.h>
#include <qcc/Socket.h>
#include <qcc/SocketStream.h>

#include <alljoyn/BusAttachment.h>
#include <Status.h>

#include "BTLiteTransport.h"

#define QCC_MODULE "ALLJOYN_BT_LITE"


using namespace std;

namespace ajn {

#define BUS_NAME_TTL numeric_limits<uint8_t>::max()
BTLiteController* z_btLiteController = NULL;

class BTLiteEndpoint : public RemoteEndpoint {
  public:
    /**
     * BluetoothLite endpoint constructor
     */
    BTLiteEndpoint(BusAttachment& bus, const qcc::String uid, bool incoming,
                   const qcc::String connectSpec, qcc::SocketFd sock) :
        RemoteEndpoint(bus, incoming, connectSpec, m_stream, "btlite"),
        uniqueID(uid),
        btAddr(connectSpec),
        m_stream(sock) { }

    virtual ~BTLiteEndpoint() { }
    qcc::String& GetUniqueID() { return uniqueID; }


  private:
    qcc::String uniqueID;
    qcc::String btAddr;
    qcc::SocketStream m_stream;
};


BTLiteTransport::BTLiteTransport(BusAttachment& bus) : bus(bus) {
    QCC_DbgPrintf(("BTLiteTransport::BTLiteTransport()"));
}


BTLiteTransport::~BTLiteTransport()
{
    QCC_DbgPrintf(("BTLiteTransport::~BTLiteTransport()"));
    /* Stop the thread */
    Stop();
    Join();
}

void* BTLiteTransport::Run(void* arg)
{
    QStatus status = ER_OK;
    return (void*) status;
}


QStatus BTLiteTransport::Start()
{
    QCC_DbgPrintf(("BTLiteTransport::Start()"));
    if (z_btLiteController != NULL) {
        z_btLiteController->SetTransport(this);
        z_btLiteController->EnsureDiscoverable();
    }

    return ER_OK;
}


QStatus BTLiteTransport::Stop(void)
{
    QCC_DbgPrintf(("BTLiteTransport::Stop()"));
    if (z_btLiteController == NULL) {
        return ER_BUS_TRANSPORT_NOT_AVAILABLE;
    }

    list<BTLiteEndpoint*>::iterator eit;

    /* Stop any endpoints that are running */
    m_endpointListLock.Lock();
    for (eit = m_endpointList.begin(); eit != m_endpointList.end(); ++eit) {
        (*eit)->Stop();
    }
    m_endpointListLock.Unlock();

    return ER_OK;
}


QStatus BTLiteTransport::Join(void)
{
    QCC_DbgPrintf(("BTLiteTransport::Join()"));
    if (z_btLiteController == NULL) {
        return ER_BUS_TRANSPORT_NOT_AVAILABLE;
    }

    m_endpointListLock.Lock();
    while (m_endpointList.size() > 0) {
        m_endpointListLock.Unlock();
        qcc::Sleep(50);
        m_endpointListLock.Lock();
    }
    m_endpointListLock.Unlock();
    return Thread::Join();
}


void BTLiteTransport::EnableDiscovery(const char* namePrefix)
{
    QCC_DbgPrintf(("BTLiteTransport::EnableDiscovery(namePrefix = \"%s\")", namePrefix));
    if (z_btLiteController != NULL) {
        z_btLiteController->EnableDiscovery(namePrefix);
    }
}


void BTLiteTransport::DisableDiscovery(const char* namePrefix)
{
    QCC_DbgPrintf(("BTLiteTransport::DisableDiscovery(namePrefix = \"%s\")", namePrefix));
    if (z_btLiteController == NULL) {
        z_btLiteController->DisableDiscovery(namePrefix);
    }
}


QStatus BTLiteTransport::EnableAdvertisement(const qcc::String& advertiseName)
{
    qcc::Log(LOG_DEBUG, "BTLiteTransport::EnableAdvertisement(%s)", advertiseName.c_str());
    if (z_btLiteController == NULL) {
        return ER_FAIL;
    }

    z_btLiteController->EnableAdvertisement(advertiseName);
    return ER_OK;
}


void BTLiteTransport::DisableAdvertisement(const qcc::String& advertiseName, bool nameListEmpty)
{
    QCC_DbgPrintf(("BTLiteTransport::DisableAdvertisement(advertiseName = %s, nameListEmpty = %s)", advertiseName.c_str(), nameListEmpty ? "true" : "false"));
    if (z_btLiteController != NULL) {
        z_btLiteController->DisableAdvertisement(advertiseName);
    }
}

QStatus BTLiteTransport::Disconnect(const char* connectSpec)
{
    QCC_DbgPrintf(("BTLiteTransport::Disconnect(connectSpec = \"%s\")", connectSpec));
    if (z_btLiteController == NULL) {
        return ER_BUS_TRANSPORT_NOT_AVAILABLE;
    }

    return ER_OK;
}

QStatus BTLiteTransport::StartListen(const char* listenSpec)
{
    QCC_DbgPrintf(("BTLiteTransport::StartListen(listenSpec = \"%s\")", listenSpec));
    if (z_btLiteController == NULL) {
        return ER_BUS_TRANSPORT_NOT_AVAILABLE;
    }

    z_btLiteController->StartListen();
    return ER_OK;
}

QStatus BTLiteTransport::StopListen(const char* listenSpec)
{
    QCC_DbgPrintf(("BTLiteTransport::StopListen(listenSpec = \"%s\")", listenSpec));
    if (z_btLiteController == NULL) {
        return ER_BUS_TRANSPORT_NOT_AVAILABLE;
    }
    return ER_OK;
}

void BTLiteTransport::EndpointExit(RemoteEndpoint* endpoint)
{
    if (z_btLiteController == NULL) {
        return;
    }

    BTLiteEndpoint* ep = static_cast<BTLiteEndpoint*>(endpoint);
    QCC_DbgPrintf(("BTLiteTransport::EndpointExit(endpoint => \"%s\" - \"%s\")",
                   ep->GetRemoteGUID().ToShortString().c_str(),
                   ep->GetConnectSpec().c_str()));

    z_btLiteController->EndpointExit(ep->GetUniqueID());

    /* Remove endpoint from endpoint list */
    m_endpointListLock.Lock();
    list<BTLiteEndpoint*>::iterator eit = find(m_endpointList.begin(), m_endpointList.end(), ep);
    if (eit != m_endpointList.end()) {
        m_endpointList.erase(eit);
    }
    m_endpointListLock.Unlock();

    delete ep;
}

QStatus BTLiteTransport::Connect(const char* connectSpec, RemoteEndpoint** newep)
{
    QCC_DbgPrintf(("BTLiteTransport::Connect(connectSpec = \"%s\")", connectSpec));
    QStatus status = ER_FAIL;
    bool isConnected = false;
    if (z_btLiteController == NULL) {
        return ER_BUS_TRANSPORT_NOT_AVAILABLE;
    }

    String spec(connectSpec);
    qcc::String uniqueID;

    uniqueID = z_btLiteController->Connect(spec);

    if (uniqueID.size() <= 0) {
        QCC_LogError(status, ("z_btLiteController->Connect(): Failed"));
        return ER_FAIL;
    }

    SocketFd sockFd = -1;
    qcc::String ipAddr = "127.0.0.1";
    int port = 9527;

    status = Socket(QCC_AF_INET, QCC_SOCK_STREAM, sockFd);
    if (status == ER_OK) {
        status = qcc::Connect(sockFd, ipAddr, port);
        if (status == ER_OK) {
            isConnected = true;
        } else {
            QCC_LogError(status, ("BTLiteTransport::Connect(): Failed"));
        }
    } else {
        QCC_LogError(status, ("BTLiteTransport::Connect(): qcc::Socket() failed"));
    }


    /*
     * The underling transport mechanism is started, but we need to create a
     * BTLiteEndpoint object that will orchestrate the movement of data across the
     * transport.
     */
    BTLiteEndpoint* conn = NULL;
    if (status == ER_OK) {
        conn = new BTLiteEndpoint(bus, uniqueID, false, spec, sockFd);
        m_endpointListLock.Lock();
        m_endpointList.push_back(conn);
        m_endpointListLock.Unlock();

        /* Initialized the features for this endpoint */
        conn->GetFeatures().isBusToBus = true;
        conn->GetFeatures().allowRemote = bus.GetInternal().AllowRemoteMessages();
        conn->GetFeatures().handlePassing = false;

        qcc::String authName;
        status = conn->Establish("ANONYMOUS", authName);
        if (ER_OK == status) {
            conn->SetListener(this);
            status = conn->Start();
        }

        /*
         * We put the endpoint into our list of active endpoints to make life
         * easier reporting problems up the chain of command behind the scenes
         * if we got an error during the authentincation process and the endpoint
         * startup.  If we did get an error, we need to remove the endpoint if it
         * is still there and the endpoint exit callback didn't kill it.
         */
        if (status != ER_OK && conn) {
            QCC_LogError(status, ("BTLiteTransport::Establish() failed"));
            m_endpointListLock.Lock();
            list<BTLiteEndpoint*>::iterator i = find(m_endpointList.begin(), m_endpointList.end(), conn);
            if (i != m_endpointList.end()) {
                m_endpointList.erase(i);
            }
            m_endpointListLock.Unlock();
            delete conn;
            conn = NULL;
        }
    }

    /*
     * If we got an error, we need to cleanup the socket and zero out the
     * returned endpoint.  If we got this done without a problem, we return
     * a pointer to the new endpoint.
     */
    if (status != ER_OK) {
        if (isConnected) {
            qcc::Shutdown(sockFd);
        }
        if (sockFd >= 0) {
            qcc::Close(sockFd);
        }
        if (newep) {
            *newep = NULL;
        }
    } else {
        if (newep) {
            *newep = conn;
        }
    }

    return status;
}

RemoteEndpoint* BTLiteTransport::LookupEndpoint(const qcc::String& busName)
{
    RemoteEndpoint* ep = NULL;
    list<BTLiteEndpoint*>::iterator eit;
    m_endpointListLock.Lock();
    for (eit = m_endpointList.begin(); eit != m_endpointList.end(); ++eit) {
        if ((*eit)->GetRemoteName() == busName) {
            ep = *eit;
            break;
        }
    }
    m_endpointListLock.Unlock();
    return ep;
}

QStatus BTLiteTransport::FoundName(std::vector<qcc::String>& nameList, String& guid, String& busAddr) {
    QCC_DbgPrintf(("BTLiteTransport::FoundName()"));
    QStatus status = ER_OK;
    if (listener != NULL) {
        listener->FoundNames(busAddr, guid, TRANSPORT_BLUETOOTH_LITE, &nameList, BUS_NAME_TTL);
    }
    return status;
}

qcc::String BTLiteTransport::GetGlobalGUID() {
    return bus.GetInternal().GetGlobalGUID().ToString();
}

void BTLiteTransport::Accepted(qcc::String uniqueID) {
    SocketFd sockFd = -1;
    bool isConnected = false;
    qcc::String ipAddr = "127.0.0.1";
    int port = 9527;
    QStatus status = ER_OK;

    status = Socket(QCC_AF_INET, QCC_SOCK_STREAM, sockFd);
    if (status == ER_OK) {
        status = qcc::Connect(sockFd, ipAddr, port);
        if (status == ER_OK) {
            isConnected = true;
        } else {
            QCC_LogError(status, ("BTLiteTransport::Connect(): Failed"));
        }
    } else {
        QCC_LogError(status, ("BTLiteTransport::Connect(): qcc::Socket() failed"));
    }

    /*
     * The underling transport mechanism is started, but we need to create a
     * BTLiteEndpoint object that will orchestrate the movement of data across the
     * transport.
     */
    BTLiteEndpoint* conn = NULL;
    if (status == ER_OK) {
        conn = new BTLiteEndpoint(bus, uniqueID, true, "dummySpec", sockFd);
        m_endpointListLock.Lock();
        m_endpointList.push_back(conn);
        m_endpointListLock.Unlock();

        /* Initialized the features for this endpoint */
        conn->GetFeatures().isBusToBus = false;
        conn->GetFeatures().allowRemote = bus.GetInternal().AllowRemoteMessages();
        conn->GetFeatures().handlePassing = false;
        qcc::String authName;
        status = conn->Establish("ANONYMOUS", authName);
        if (ER_OK == status) {
            conn->SetListener(this);
            status = conn->Start();
        }

        /*
         * We put the endpoint into our list of active endpoints to make life
         * easier reporting problems up the chain of command behind the scenes
         * if we got an error during the authentincation process and the endpoint
         * startup.  If we did get an error, we need to remove the endpoint if it
         * is still there and the endpoint exit callback didn't kill it.
         */
        if (status != ER_OK && conn) {
            QCC_LogError(status, ("BTLiteTransport::Accepted(): Start BTLiteEndpoint failed"));

            m_endpointListLock.Lock();
            list<BTLiteEndpoint*>::iterator i = find(m_endpointList.begin(), m_endpointList.end(), conn);
            if (i != m_endpointList.end()) {
                m_endpointList.erase(i);
            }
            m_endpointListLock.Unlock();
            delete conn;
            conn = NULL;
        }
    }

    /*
     * If we got an error, we need to cleanup the socket and zero out the
     * returned endpoint.  If we got this done without a problem, we return
     * a pointer to the new endpoint.
     */
    if (status != ER_OK) {
        if (isConnected) {
            qcc::Shutdown(sockFd);
        }
        if (sockFd >= 0) {
            qcc::Close(sockFd);
        }
    }
}

}
