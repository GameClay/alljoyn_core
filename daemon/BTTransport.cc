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

#include <qcc/platform.h>

#include <limits>
#include <map>
#include <vector>

#include <qcc/Event.h>
#include <qcc/String.h>

#include <alljoyn/BusAttachment.h>
#include <alljoyn/QosInfo.h>

#include "BDAddress.h"
#include "BTController.h"
#include "BTTransport.h"

#if defined QCC_OS_GROUP_POSIX
#include "bt_bluez/BTAccessor.h"
#elif defined QCC_OS_GROUP_WINDOWS
#error Windows support to be implemented
#endif

#include <Status.h>

#define QCC_MODULE "ALLJOYN_BT"


using namespace std;
using namespace qcc;
using namespace ajn;
using namespace ajn::bluez;

namespace ajn {


#define BUS_NAME_TTL numeric_limits<uint8_t>::max()

/*****************************************************************************/


BTTransport::BTTransport(BusAttachment& bus) :
    Thread("BTTransport"),
    bus(bus),
    transportIsStopping(false),
    btmActive(false)
{
    btQos.proximity = QosInfo::PROXIMITY_PHYSICAL;
    btQos.traffic = QosInfo::TRAFFIC_MESSAGES;
    btQos.transports = QosInfo::TRANSPORT_BLUETOOTH;

    btController = new BTController(bus, *this);
    QStatus status = btController->Init();
    if (status == ER_OK) {
        btAccessor = new BTAccessor(this, bus.GetGlobalGUIDString());
        btmActive = true;
    }
}


BTTransport::~BTTransport()
{
    /* Stop the thread */
    Stop();
    Join();

    delete btController;
    if (btmActive) {
        delete btAccessor;
    }
}

QStatus BTTransport::NormalizeTransportSpec(const char* inSpec, qcc::String& outSpec, map<qcc::String, qcc::String>& argMap)
{
    if (!btmActive) {
        return ER_BUS_TRANSPORT_NOT_AVAILABLE;
    }

    QStatus status = ParseArguments("bluetooth", inSpec, argMap);
    if (status == ER_OK) {
        map<qcc::String, qcc::String>::iterator it;
        outSpec = "bluetooth:";

        it = argMap.find("addr");
        if (it == argMap.end()) {
            status = ER_FAIL;
            QCC_LogError(status, ("'addr=' must be specified for 'bluetooth:'"));
        } else {
            outSpec.append("addr=");
            outSpec += it->second;

            it = argMap.find("channel");
            outSpec.append(",channel=");
            if (it == argMap.end()) {
                outSpec += U32ToString(BTController::INVALID_CHANNEL);
            } else {
                outSpec += it->second;
            }

            it = argMap.find("psm");
            outSpec.append(",psm=");
            if (it == argMap.end()) {
                outSpec.append("0x");
                outSpec += U32ToString(BTController::INVALID_PSM, 16);
            } else {
                outSpec += it->second;
            }
        }
    }
    return status;
}

void* BTTransport::Run(void* arg)
{
    if (!btmActive) {
        return (void*)ER_BUS_TRANSPORT_NOT_AVAILABLE;
    }

    QStatus status = ER_OK;
    vector<Event*> signaledEvents;
    vector<Event*> checkEvents;

    while (!IsStopping()) {
        Event* l2capEvent = btAccessor->GetL2CAPConnectEvent();
        if (l2capEvent) {
            checkEvents.push_back(l2capEvent);
        }

        Event* rfcommEvent = btAccessor->GetRFCOMMConnectEvent();
        if (rfcommEvent) {
            checkEvents.push_back(rfcommEvent);
        }

        checkEvents.push_back(&stopEvent);

        /* wait for something to happen */
        QCC_DbgTrace(("waiting for incoming connection ..."));
        status = Event::Wait(checkEvents, signaledEvents);
        if (ER_OK != status) {
            QCC_LogError(status, ("Event::Wait failed"));
            break;
        }

        /* Iterate over signaled events */
        for (vector<Event*>::iterator it = signaledEvents.begin(); it != signaledEvents.end(); ++it) {
            if (*it != &stopEvent) {
                /* Accept a new connection */
                qcc::String authName;
                RemoteEndpoint* conn(btAccessor->Accept(bus, *it));
                if (!conn) {
                    continue;
                }

                /* Initialized the features for this endpoint */
                conn->GetFeatures().isBusToBus = false;
                conn->GetFeatures().allowRemote = false;
                conn->GetFeatures().handlePassing = false;

                threadListLock.Lock();
                threadList.push_back(conn);
                threadListLock.Unlock();
                QCC_DbgPrintf(("BTTransport::Run: Calling conn->Establish() [for accepted connection]"));
                status = conn->Establish("ANONYMOUS", authName);
                if (ER_OK == status) {
                    QCC_DbgPrintf(("Starting endpoint [for accepted connection]"));
                    conn->SetListener(this);
                    status = conn->Start();
                }

                if (ER_OK != status) {
                    QCC_LogError(status, ("Error starting RemoteEndpoint"));
                    EndpointExit(conn);
                    conn = NULL;
                }
            } else {
                (*it)->ResetEvent();
            }
        }
        signaledEvents.clear();
        checkEvents.clear();
    }
    return (void*) status;
}


QStatus BTTransport::Start()
{
    QCC_DbgTrace(("BTTransport::Start()"));
    if (!btmActive) {
        return ER_BUS_TRANSPORT_NOT_AVAILABLE;
    }

    return btAccessor->Start();
}


QStatus BTTransport::Stop(void)
{
    if (!btmActive) {
        return ER_BUS_TRANSPORT_NOT_AVAILABLE;
    }

    transportIsStopping = true;

    vector<RemoteEndpoint*>::iterator eit;

    bool isStopping = IsStopping();

    if (!isStopping) {
        btAccessor->Stop();
    }

    /* Stop any endpoints that are running */
    threadListLock.Lock();
    for (eit = threadList.begin(); eit != threadList.end(); ++eit) {
        (*eit)->Stop();
    }
    threadListLock.Unlock();

    return ER_OK;
}


QStatus BTTransport::Join(void)
{
    if (!btmActive) {
        return ER_BUS_TRANSPORT_NOT_AVAILABLE;
    }

    /* Wait for the thread list to empty out */
    threadListLock.Lock();
    while (threadList.size() > 0) {
        threadListLock.Unlock();
        qcc::Sleep(50);
        threadListLock.Lock();
    }
    threadListLock.Unlock();
    return Thread::Join();
}


void BTTransport::EnableDiscovery(const char* namePrefix)
{
    QCC_DbgTrace(("BTTransport::EnableDiscovery(namePrefix = \"%s\")", namePrefix));
    if (!btmActive) {
        return;
    }

    qcc::String name(namePrefix);
    QStatus status;

    status = btController->AddFindName(name);

    if (status != ER_OK) {
        QCC_LogError(status, ("BTTransport::EnableDiscovery"));
    }
}


void BTTransport::DisableDiscovery(const char* namePrefix)
{
    QCC_DbgTrace(("BTTransport::DisableDiscovery(namePrefix = \"%s\")", namePrefix));
    if (!btmActive) {
        return;
    }

    qcc::String name(namePrefix);
    QStatus status;

    status = btController->RemoveFindName(name);

    if (status != ER_OK) {
        QCC_LogError(status, ("BTTransport::DisableDiscovery"));
    }
}


QStatus BTTransport::EnableAdvertisement(const qcc::String& advertiseName, const QosInfo& advQos)
{
    QCC_DbgTrace(("BTTransport::EnableAdvertisement(%s, <%x, %x, %x>)", advertiseName.c_str(), advQos.traffic, advQos.proximity, advQos.transports));
    if (!btmActive) {
        return ER_FAIL;
    }

    /* Do nothing if qos doesn't include bluetooth */
    QStatus status = ER_BUS_INCOMPATIBLE_QOS;
    if (btQos.IsCompatible(advQos)) {
        if (advQos.IsCompatible(btQos)) {
            status = btController->AddAdvertiseName(advertiseName);

            if (status != ER_OK) {
                QCC_LogError(status, ("BTTransport::EnableAdvertisement"));
            }
        }
    }
    return status;
}


void BTTransport::DisableAdvertisement(const qcc::String& advertiseName, const QosInfo* advQos, bool nameListEmpty)
{
    QCC_DbgTrace(("BTTransport::DisableAdvertisement(advertiseName = %s, nameListEmpty = %s)", advertiseName.c_str(), nameListEmpty ? "true" : "false"));
    if (!btmActive) {
        return;
    }

    /* Do nothing if qos doesn't include bluetooth */
    if (!advQos || advQos->IsCompatible(btQos)) {
        QStatus status;

        status = btController->RemoveAdvertiseName(advertiseName);

        if (status != ER_OK) {
            QCC_LogError(status, ("BTTransport::DisableAdvertisement"));
        }
    }
}


QStatus BTTransport::Connect(const char* connectSpec, RemoteEndpoint** newep)
{
    QCC_DbgTrace(("BTTransport::Connect(connectSpec = \"%s\")", connectSpec));
    if (!btmActive) {
        return ER_BUS_TRANSPORT_NOT_AVAILABLE;
    }

    QStatus status;

    map<qcc::String, qcc::String> argMap;
    qcc::String normSpec;
    qcc::String addrArg;

    BDAddress bdAddr;
    uint8_t channel;
    uint16_t psm;

    /* Parse connectSpec */
    status = NormalizeTransportSpec(connectSpec, normSpec, argMap);
    if (ER_OK != status) {
        QCC_LogError(status, ("parsing bluetooth arguments: \"%s\"", connectSpec));
        goto exit;
    }

    addrArg = argMap["addr"];

    if (addrArg.empty()) {
        status = ER_BUS_BAD_TRANSPORT_ARGS;
        QCC_LogError(status, ("Address not specified."));
        goto exit;
    }

    status = bdAddr.FromString(addrArg);
    if (status != ER_OK) {
        status = ER_BUS_BAD_TRANSPORT_ARGS;
        QCC_LogError(status, ("Badly formed Bluetooth device address \"%s\"", argMap["addr"].c_str()));
        goto exit;
    }

    channel = StringToU32(argMap["channel"], 0, BTController::INVALID_CHANNEL);
    psm = StringToU32(argMap["psm"], 0, BTController::INVALID_PSM);

    status = Connect(bdAddr, channel, psm, newep);

exit:
    return status;
}

QStatus BTTransport::Disconnect(const char* connectSpec)
{
    QCC_DbgTrace(("BTTransport::Disconnect(connectSpec = \"%s\")", connectSpec));
    if (!btmActive) {
        return ER_BUS_TRANSPORT_NOT_AVAILABLE;
    }

    /* Normalize and parse the connect spec */
    qcc::String spec;
    map<qcc::String, qcc::String> argMap;
    QStatus status = NormalizeTransportSpec(connectSpec, spec, argMap);

    if (ER_OK == status) {
        if (argMap.find("addr") != argMap.end()) {
            BDAddress addr(argMap["addr"]);

            status = Disconnect(addr);
        }
    }
    return status;
}

// @@ TODO
QStatus BTTransport::StartListen(const char* listenSpec)
{
    QCC_DbgTrace(("BTTransport::StartListen(listenSpec = \"%s\")", listenSpec));
    if (!btmActive) {
        return ER_BUS_TRANSPORT_NOT_AVAILABLE;
    }

    /*
     * Bluetooth listens are managed by the Master node in a piconet
     */
    return ER_OK;
}

// @@ TODO
QStatus BTTransport::StopListen(const char* listenSpec)
{
    QCC_DbgTrace(("BTTransport::StopListen(listenSpec = \"%s\")", listenSpec));
    if (!btmActive) {
        return ER_BUS_TRANSPORT_NOT_AVAILABLE;
    }

    /*
     * Bluetooth listens are managed by the Master node in a piconet
     */
    return ER_OK;
}

void BTTransport::EndpointExit(RemoteEndpoint* endpoint)
{
    if (!btmActive) {
        return;
    }

    QCC_DbgTrace(("BTTransport::EndpointExit(endpoint => \"%s\" - \"%s\")",
                  endpoint->GetRemoteGUID().ToShortString().c_str(),
                  endpoint->GetConnectSpec().c_str()));

    /* Remove thread from thread list */
    threadListLock.Lock();
    vector<RemoteEndpoint*>::iterator eit = find(threadList.begin(), threadList.end(), endpoint);
    if (eit != threadList.end()) {
        threadList.erase(eit);
    }
    threadListLock.Unlock();

    delete endpoint;
}


void BTTransport::DeviceChange(const BDAddress& adBdAddr,
                               uint32_t newUUIDRev,
                               uint32_t oldUUIDRev,
                               bool lost)
{
    if (btmActive) {
        btController->ProcessDeviceChange(adBdAddr, newUUIDRev, oldUUIDRev, lost);
    }
}


/********************************************************/

QStatus BTTransport::StartFind(uint32_t ignoreUUIDRev, uint32_t duration)
{
    return btAccessor->StartDiscovery(ignoreUUIDRev, duration);
}


QStatus BTTransport::StopFind()
{
    return btAccessor->StopDiscovery();
}


QStatus BTTransport::StartAdvertise(uint32_t uuidRev,
                                    const BDAddress& bdAddr,
                                    uint8_t channel,
                                    uint16_t psm,
                                    const AdvertiseInfo& adInfo,
                                    uint32_t duration)
{
    QStatus status = btAccessor->SetSDPInfo(uuidRev, bdAddr, channel, psm, adInfo);
    if (status == ER_OK) {
        status = btAccessor->StartDiscoverability(duration);
    }
    return status;
}


QStatus BTTransport::StopAdvertise()
{
    BDAddress bdAddr;
    AdvertiseInfo adInfo;
    btAccessor->SetSDPInfo(BTController::INVALID_UUIDREV, bdAddr, BTController::INVALID_CHANNEL, BTController::INVALID_PSM, adInfo);
    btAccessor->StopDiscoverability();
    return ER_OK;  // This will ensure that the topology manager stays in the right state.
}


void BTTransport::FoundNamesChange(const qcc::String& guid,
                                   const vector<String>& names,
                                   const BDAddress& bdAddr,
                                   uint8_t channel,
                                   uint16_t psm,
                                   bool lost)
{
    if (listener) {
        qcc::String busAddr("bluetooth:addr=" + bdAddr.ToString() +
                            ",channel=" + U32ToString(channel) +
                            ",psm=0x" + U32ToString(psm, 16));

        listener->FoundNames(busAddr, guid, btQos, &names, lost ? 0 : BUS_NAME_TTL);
    }
}


QStatus BTTransport::StartListen(BDAddress& addr,
                                 uint8_t& channel,
                                 uint16_t& psm)
{
    if (!btmActive) {
        return ER_BUS_TRANSPORT_NOT_AVAILABLE;
    }

    QStatus status;
    status = btAccessor->StartConnectable(addr, channel, psm);
    if (status == ER_OK) {
        QCC_DbgHLPrintf(("Listening on addr: %s  channel = %d  psm = %04x", addr.ToString().c_str(), channel, psm));
        Thread::Start();
    }
    return status;
}


void BTTransport::StopListen()
{
    Thread::Stop();
    Thread::Join();
    btAccessor->StopConnectable();
    QCC_DbgHLPrintf(("Stopped listening"));
}

QStatus BTTransport::GetDeviceInfo(const BDAddress& addr,
                                   BDAddress& connAddr,
                                   uint32_t& uuidRev,
                                   uint8_t& channel,
                                   uint16_t& psm,
                                   BluetoothDeviceInterface::AdvertiseInfo& adInfo)
{
    if (!btmActive) {
        return ER_BUS_TRANSPORT_NOT_AVAILABLE;
    }

    return btAccessor->GetDeviceInfo(addr, &connAddr, &uuidRev, &channel, &psm, &adInfo);
}



QStatus BTTransport::Connect(const BDAddress& bdAddr,
                             uint8_t channel,
                             uint16_t psm,
                             RemoteEndpoint** newep)
{
    QStatus status;
    RemoteEndpoint* conn = NULL;
    bool useLocal = btController->OKToConnect();

    if (useLocal) {
        qcc::String authName;

        btController->PrepConnect();

        conn = btAccessor->Connect(bus, bdAddr, channel, psm);
        status = conn ? ER_OK : ER_FAIL;

        if (status != ER_OK) {
            goto exit;
        }

        /* Initialized the features for this endpoint */
        conn->GetFeatures().isBusToBus = true;
        conn->GetFeatures().allowRemote = bus.GetInternal().AllowRemoteMessages();
        conn->GetFeatures().handlePassing = false;

        threadListLock.Lock();
        threadList.push_back(conn);
        threadListLock.Unlock();
        QCC_DbgPrintf(("BTTransport::Connect: Calling conn->Establish() [bdAddr = %s, ch = %u, psm = %u]",
                       bdAddr.ToString().c_str(), channel, psm));
        status = conn->Establish("ANONYMOUS", authName);
        if (ER_OK != status) {
            QCC_LogError(status, ("BTEndpoint::Establish failed"));
            EndpointExit(conn);
            goto exit;
        }

        QCC_DbgPrintf(("Starting endpoint [bdAddr = %s, ch = %u, psm = %u]", bdAddr.ToString().c_str(), channel, psm));
        /* Start the endpoint */
        conn->SetListener(this);
        status = conn->Start();
        if (ER_OK != status) {
            QCC_LogError(status, ("BTEndpoint::Start failed"));
            EndpointExit(conn);
            goto exit;
        }

        /* If transport is closing, then don't allow any new endpoints */
        if (transportIsStopping) {
            status = ER_BUS_TRANSPORT_NOT_STARTED;
        }
    } else {
        qcc::String delegate;

        status = btController->ProxyConnect(bdAddr, channel, psm, &delegate);

        if (status == ER_OK) {
            threadListLock.Lock();
            std::vector<RemoteEndpoint*>::const_iterator it;
            for (it = threadList.begin(); it != threadList.end(); ++it) {
                if ((*it)->GetRemoteName() == delegate) {
                    conn = *it;
                }
            }
            threadListLock.Unlock();
        }
    }

exit:
    if (status == ER_OK) {
        if (newep) {
            *newep = conn;
        }
    } else {
        if (newep) {
            *newep = NULL;
        }
    }

    if (useLocal) {
        btController->PostConnect(status, conn);
    }

    return status;
}


QStatus BTTransport::Disconnect(const BDAddress& bdAddr)
{
    QStatus status = btAccessor->Disconnect(bdAddr);
    if (status == ER_BUS_BAD_TRANSPORT_ARGS) {
        status = btController->ProxyDisconnect(bdAddr);
    }

    return status;
}

}
