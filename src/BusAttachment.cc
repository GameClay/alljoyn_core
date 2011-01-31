/**
 * @file
 * BusAttachment is the top-level object responsible for connecting to and optionally managing a message bus.
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
#include <qcc/Debug.h>
#include <qcc/Util.h>
#include <qcc/Event.h>
#include <qcc/String.h>
#include <qcc/Timer.h>
#include <qcc/atomic.h>
#include <qcc/XmlElement.h>
#include <qcc/StringSource.h>

#include <assert.h>
#include <algorithm>

#include <alljoyn/BusAttachment.h>
#include <alljoyn/BusListener.h>
#include <alljoyn/DBusStd.h>
#include <alljoyn/AllJoynStd.h>

#include "AuthMechanism.h"
#include "AuthMechAnonymous.h"
#include "AuthMechDBusCookieSHA1.h"
#include "AuthMechExternal.h"
#include "AuthMechSRP.h"
#include "AuthMechRSA.h"
#include "AuthMechLogon.h"
#include "Transport.h"
#include "TransportList.h"
#include "TCPTransport.h"
#include "UnixTransport.h"
#include "BusEndpoint.h"
#include "LocalTransport.h"
#include "PeerState.h"
#include "KeyStore.h"
#include "BusInternal.h"
#include "AllJoynPeerObj.h"
#include "XmlHelper.h"

#define QCC_MODULE "ALLJOYN"


using namespace std;
using namespace qcc;

namespace ajn {

BusAttachment::Internal::Internal(const char* appName, BusAttachment& bus, TransportFactoryContainer& factories,
                                  Router* router, bool allowRemoteMessages, const char* listenAddresses) :
    application(appName ? appName : "unknown"),
    bus(bus),
    listenersLock(),
    listeners(),
    transportList(bus, factories),
    keyStore(application),
    authManager(keyStore),
    globalGuid(qcc::GUID()),
    msgSerial(qcc::Rand32()),
    router(router ? router : new ClientRouter),
    peerStateTable(NULL),
    localEndpoint(transportList.GetLocalTransport()->GetLocalEndpoint()),
    timer("timer"),
    dispatcher("dispatcher"),
    allowRemoteMessages(allowRemoteMessages),
    listenAddresses(listenAddresses ? listenAddresses : ""),
    stopLock(),
    stopCount(0)
{
    /*
     * Bus needs a pointer to this internal object.
     */
    bus.busInternal = this;

    /*
     * Create the standard interfaces
     */
    QStatus status = org::freedesktop::DBus::CreateInterfaces(bus);
    if (ER_OK != status) {
        QCC_LogError(status, ("Cannot create %s interface", org::freedesktop::DBus::InterfaceName));
    }
    status = org::alljoyn::Bus::CreateInterfaces(bus);
    if (ER_OK != status) {
        QCC_LogError(status, ("Cannot create %s interface", org::alljoyn::Bus::InterfaceName));
    }
    /* Register bus client authentication mechanisms */
    authManager.RegisterMechanism(AuthMechDBusCookieSHA1::Factory, AuthMechDBusCookieSHA1::AuthName());
    authManager.RegisterMechanism(AuthMechExternal::Factory, AuthMechExternal::AuthName());
    authManager.RegisterMechanism(AuthMechAnonymous::Factory, AuthMechAnonymous::AuthName());
}

BusAttachment::Internal::~Internal()
{
    delete router;
}

class LocalTransportFactoryContainer : public TransportFactoryContainer {
  public:
    LocalTransportFactoryContainer()
    {
#ifdef QCC_OS_WINDOWS
        Add(new TransportFactory<TCPTransport>("tcp", true));
#else
        Add(new TransportFactory<UnixTransport>("unix", true));
#endif

    }
} localTransportsContainer;

BusAttachment::BusAttachment(const char* applicationName, bool allowRemoteMessages) :
    isStarted(false),
    busInternal(new Internal(applicationName, *this, localTransportsContainer, NULL, allowRemoteMessages, NULL))
{
    QCC_DbgTrace(("BusAttachment client constructor (%p)", this));
}

BusAttachment::BusAttachment(Internal* busInternal) :
    isStarted(false),
    busInternal(busInternal)
{
    QCC_DbgTrace(("BusAttachment daemon constructor"));
}

BusAttachment::~BusAttachment(void)
{
    QCC_DbgTrace(("BusAttachment Destructor (%p)", this));

    Stop(true);

    /* Wait for ALL callers of BusAttachment::Stop() to exit before deleting the object */
    while (busInternal->stopCount) {
        qcc::Sleep(50);
    }

    delete busInternal;
    busInternal = NULL;
}

QStatus BusAttachment::Start()
{
    QStatus status;

    QCC_DbgTrace(("BusAttachment::Start()"));

    if (isStarted) {
        status = ER_BUS_BUS_ALREADY_STARTED;
        QCC_LogError(status, ("BusAttachment::Start already started"));
    } else {
        /* Start the timer */
        status = busInternal->timer.Start();
        if (ER_OK == status) {
            busInternal->peerStateTable = new PeerStateTable;
            /* Start the transports */
            status = busInternal->transportList.Start(busInternal->GetListenAddresses());
        }
        if (ER_OK == status) {
            isStarted = true;
        } else {
            delete busInternal->peerStateTable;
            busInternal->peerStateTable = NULL;
        }

        /* Start the alljoyn signal dispatcher */
        busInternal->dispatcher.Start();
    }
    return status;
}

QStatus BusAttachment::Connect(const char* connectSpec, RemoteEndpoint** newep)
{
    QStatus status;
    bool isDaemon = busInternal->GetRouter().IsDaemon();

    if (!isStarted) {
        status = ER_BUS_BUS_NOT_STARTED;
    } else if (IsConnected() && !isDaemon) {
        status = ER_BUS_ALREADY_CONNECTED;
    } else {

        /* Get or create transport for connection */
        Transport* trans = busInternal->transportList.GetTransport(connectSpec);
        if (trans) {
            status = trans->Connect(connectSpec, newep);
        } else {
            status = ER_BUS_TRANSPORT_NOT_AVAILABLE;
        }

        /* If this is a client (non-daemon) bus attachment, then register signal handlers for BusListener */
        if ((ER_OK == status) && !isDaemon) {
            const InterfaceDescription* iface = GetInterface(org::freedesktop::DBus::InterfaceName);
            assert(iface);
            status = RegisterSignalHandler(busInternal,
                                           static_cast<MessageReceiver::SignalHandler>(&BusAttachment::Internal::AllJoynSignalHandler),
                                           iface->GetMember("NameOwnerChanged"),
                                           NULL);

            if (ER_OK == status) {
                Message reply(*this);
                MsgArg arg("s", "type='signal',interface='org.freedesktop.DBus'");
                const ProxyBusObject& dbusObj = this->GetDBusProxyObj();
                status = dbusObj.MethodCall(org::freedesktop::DBus::InterfaceName, "AddMatch", &arg, 1, reply);
            }

            /* Register org.alljoyn.Bus signal handler */
            const InterfaceDescription* ajIface = GetInterface(org::alljoyn::Bus::InterfaceName);
            if (ER_OK == status) {
                assert(ajIface);
                status = RegisterSignalHandler(busInternal,
                                               static_cast<MessageReceiver::SignalHandler>(&BusAttachment::Internal::AllJoynSignalHandler),
                                               ajIface->GetMember("FoundName"),
                                               NULL);
            }
            if (ER_OK == status) {
                assert(ajIface);
                status = RegisterSignalHandler(busInternal,
                                               static_cast<MessageReceiver::SignalHandler>(&BusAttachment::Internal::AllJoynSignalHandler),
                                               ajIface->GetMember("LostAdvertisedName"),
                                               NULL);
            }
            if (ER_OK == status) {
                assert(ajIface);
                status = RegisterSignalHandler(busInternal,
                                               static_cast<MessageReceiver::SignalHandler>(&BusAttachment::Internal::AllJoynSignalHandler),
                                               ajIface->GetMember("BusConnectionLost"),
                                               NULL);
            }
            if (ER_OK == status) {
                Message reply(*this);
                MsgArg arg("s", "type='signal',interface='org.alljoyn.Bus'");
                const ProxyBusObject& dbusObj = this->GetDBusProxyObj();
                status = dbusObj.MethodCall(org::freedesktop::DBus::InterfaceName, "AddMatch", &arg, 1, reply);
            }
        }
    }
    if (ER_OK != status) {
        QCC_LogError(status, ("BusAttachment::Connect failed"));
    }
    return status;
}

QStatus BusAttachment::Disconnect(const char* connectSpec)
{
    QStatus status;
    bool isDaemon = busInternal->GetRouter().IsDaemon();

    if (!isStarted) {
        status = ER_BUS_BUS_NOT_STARTED;
    } else if (!isDaemon && !IsConnected()) {
        status = ER_BUS_NOT_CONNECTED;
    } else {
        /* Terminate transport for connection */
        Transport* trans = busInternal->transportList.GetTransport(connectSpec);

        if (trans) {
            status = trans->Disconnect(connectSpec);
        } else {
            status = ER_BUS_TRANSPORT_NOT_AVAILABLE;
        }

        /* Unregister signal handlers if this is a client-side bus attachment */
        if ((ER_OK == status) && !isDaemon) {
            const InterfaceDescription* dbusIface = GetInterface(org::freedesktop::DBus::InterfaceName);
            if (dbusIface) {
                UnRegisterSignalHandler(this,
                                        static_cast<MessageReceiver::SignalHandler>(&BusAttachment::Internal::AllJoynSignalHandler),
                                        dbusIface->GetMember("NameOwnerChanged"),
                                        NULL);
            }
            const InterfaceDescription* alljoynIface = GetInterface(org::alljoyn::Bus::InterfaceName);
            if (alljoynIface) {
                UnRegisterSignalHandler(this,
                                        static_cast<MessageReceiver::SignalHandler>(&BusAttachment::Internal::AllJoynSignalHandler),
                                        alljoynIface->GetMember("FoundName"),
                                        NULL);
            }
            if (alljoynIface) {
                UnRegisterSignalHandler(this,
                                        static_cast<MessageReceiver::SignalHandler>(&BusAttachment::Internal::AllJoynSignalHandler),
                                        alljoynIface->GetMember("LostAdvertisedName"),
                                        NULL);
            }
            if (alljoynIface) {
                UnRegisterSignalHandler(this,
                                        static_cast<MessageReceiver::SignalHandler>(&BusAttachment::Internal::AllJoynSignalHandler),
                                        alljoynIface->GetMember("BusConnectionLost"),
                                        NULL);
            }
        }
    }

    if (ER_OK != status) {
        QCC_LogError(status, ("BusAttachment::Disconnect failed"));
    }
    return status;
}

QStatus BusAttachment::Stop(bool blockUntilStopped)
{
    QStatus status = ER_OK;
    QCC_DbgTrace(("BusAttachment::Stop"));

    /* Get the lock that ensures that Stop is only executed once */
    IncrementAndFetch(&busInternal->stopCount);
    busInternal->stopLock.Lock();

    if (isStarted) {

        /* Stop all of the transports and the bus timer */
        QStatus status = busInternal->transportList.Stop();
        if (ER_OK != status) {
            QCC_LogError(status, ("TransportList::Stop() failed"));
        }
        status = busInternal->timer.Stop();
        if (ER_OK != status) {
            QCC_LogError(status, ("Timer::Stop() failed"));
        }
        status = busInternal->dispatcher.Stop();
        if (ER_OK != status) {
            QCC_LogError(status, ("Dispatcher::Stop() failed"));
        }

        /* Persist keystore */
        busInternal->keyStore.Store();

        if (blockUntilStopped) {
            WaitStop();
        }
        isStarted = false;
    }

    busInternal->stopLock.Unlock();
    DecrementAndFetch(&busInternal->stopCount);

    return status;
}

void BusAttachment::WaitStop()
{
    busInternal->timer.Join();
    busInternal->dispatcher.Join();
    busInternal->transportList.Join();

    /* Now the bus has stopped we can cleanup the peer state */
    /* TODO: busInternal should clean and deallocate its members by itself. This is dangerous. */
    delete busInternal->peerStateTable;
    busInternal->peerStateTable = NULL;
}

QStatus BusAttachment::CreateInterface(const char* name, InterfaceDescription*& iface, bool secure)
{
    if (NULL != GetInterface(name)) {
        iface = NULL;
        return ER_BUS_IFACE_ALREADY_EXISTS;
    }
    StringMapKey key = String(name);
    InterfaceDescription intf(name, secure);
    iface = &(busInternal->ifaceDescriptions.insert(pair<StringMapKey, InterfaceDescription>(key, intf)).first->second);
    return ER_OK;
}

QStatus BusAttachment::DeleteInterface(InterfaceDescription& iface)
{
    /* Get the (hopefully) unactivated interface */
    map<StringMapKey, InterfaceDescription>::iterator it = busInternal->ifaceDescriptions.find(StringMapKey(iface.GetName()));
    if ((it != busInternal->ifaceDescriptions.end()) && !it->second.isActivated) {
        busInternal->ifaceDescriptions.erase(it);
        return ER_OK;
    } else {
        return ER_BUS_NO_SUCH_INTERFACE;
    }
}

const InterfaceDescription* BusAttachment::GetInterface(const char* name) const
{
    map<StringMapKey, InterfaceDescription>::const_iterator it = busInternal->ifaceDescriptions.find(StringMapKey(name));
    if ((it != busInternal->ifaceDescriptions.end()) && it->second.isActivated) {
        return &(it->second);
    } else {
        return NULL;
    }
}

void BusAttachment::RegisterKeyStoreListener(KeyStoreListener& listener)
{
    busInternal->keyStore.SetListener(listener);
}

void BusAttachment::ClearKeyStore()
{
    busInternal->keyStore.Clear();
}

const qcc::String& BusAttachment::GetUniqueName() const
{
    return busInternal->localEndpoint.GetUniqueName();
}

const qcc::String& BusAttachment::GetGlobalGUIDString() const
{
    return busInternal->GetGlobalGUID().ToString();
}

const ProxyBusObject& BusAttachment::GetDBusProxyObj()
{
    return busInternal->localEndpoint.GetDBusProxyObj();
}

const ProxyBusObject& BusAttachment::GetAllJoynProxyObj()
{
    return busInternal->localEndpoint.GetAllJoynProxyObj();
}

QStatus BusAttachment::RegisterSignalHandler(MessageReceiver* receiver,
                                             MessageReceiver::SignalHandler signalHandler,
                                             const InterfaceDescription::Member* member,
                                             const char* srcPath)
{
    return busInternal->localEndpoint.RegisterSignalHandler(receiver, signalHandler, member, srcPath);
}

QStatus BusAttachment::UnRegisterSignalHandler(MessageReceiver* receiver,
                                               MessageReceiver::SignalHandler signalHandler,
                                               const InterfaceDescription::Member* member,
                                               const char* srcPath)
{
    return busInternal->localEndpoint.UnRegisterSignalHandler(receiver, signalHandler, member, srcPath);
}

bool BusAttachment::IsConnected() const {
    return busInternal->router->IsBusRunning();
}

QStatus BusAttachment::RegisterBusObject(BusObject& obj) {
    return busInternal->localEndpoint.RegisterBusObject(obj);
}

void BusAttachment::DeregisterBusObject(BusObject& object)
{
    busInternal->localEndpoint.DeregisterBusObject(object);
}

QStatus BusAttachment::EnablePeerSecurity(const char* authMechanisms,
                                          AuthListener* listener,
                                          const char* keyStoreFileName)
{
    QStatus status = busInternal->keyStore.Load(keyStoreFileName);
    if (status == ER_OK) {
        /* Register peer-to-peer authentication mechanisms */
        busInternal->authManager.RegisterMechanism(AuthMechSRP::Factory, AuthMechSRP::AuthName());
        busInternal->authManager.RegisterMechanism(AuthMechRSA::Factory, AuthMechRSA::AuthName());
        busInternal->authManager.RegisterMechanism(AuthMechLogon::Factory, AuthMechLogon::AuthName());
        /* Validate the list of auth mechanisms */
        status =  busInternal->authManager.CheckNames(authMechanisms);
        if (status == ER_OK) {
            AllJoynPeerObj* peerObj = busInternal->localEndpoint.GetPeerObj();
            peerObj->SetupPeerAuthentication(authMechanisms, listener);
        }
    }
    return status;
}

QStatus BusAttachment::AddLogonEntry(const char* authMechanism, const char* userName, const char* password)
{
    if (!authMechanism) {
        return ER_BAD_ARG_2;
    }
    if (!userName) {
        return ER_BAD_ARG_3;
    }
    if (strcmp(authMechanism, "ALLJOYN_SRP_LOGON") == 0) {
        return AuthMechLogon::AddLogonEntry(busInternal->keyStore, userName, password);
    } else {
        return ER_BUS_INVALID_AUTH_MECHANISM;
    }
}

void BusAttachment::RegisterBusListener(BusListener& listener)
{
    busInternal->listenersLock.Lock();
    busInternal->listeners.push_back(&listener);
    busInternal->listenersLock.Unlock();
}

void BusAttachment::UnRegisterBusListener(BusListener& listener)
{
    busInternal->listenersLock.Lock();
    list<BusListener*>::iterator it = std::find(busInternal->listeners.begin(), busInternal->listeners.end(), &listener);
    if (it != busInternal->listeners.end()) {
        busInternal->listeners.erase(it);
    }
    busInternal->listenersLock.Unlock();
}

QStatus BusAttachment::NameHasOwner(const char* name, bool& hasOwner)
{
    if (!IsConnected()) {
        return ER_BUS_NOT_CONNECTED;
    }

    Message reply(*this);
    MsgArg arg("s", name);
    const ProxyBusObject& dbusObj = this->GetDBusProxyObj();
    QStatus status = dbusObj.MethodCall(org::freedesktop::DBus::InterfaceName, "NameHasOwner", &arg, 1, reply);
    if (ER_OK == status) {
        if (reply->GetType() == MESSAGE_METHOD_RET) {
            hasOwner = reply->GetArg(0)->v_bool;
        } else if (reply->GetType() == MESSAGE_ERROR) {
            status = ER_BUS_REPLY_IS_ERROR_MESSAGE;
            String errMsg;
            const char* errName = reply->GetErrorName(&errMsg);
            QCC_LogError(status, ("%s.NameHasOwner returned ERROR_MESSAGE (error=%s, \"%s\")",
                                  org::freedesktop::DBus::InterfaceName,
                                  errName,
                                  errMsg.c_str()));
        } else {
            status = ER_FAIL;
        }
    }
    return status;
}

QStatus BusAttachment::ConnectToRemoteBus(const char* busAddr, uint32_t& disposition)
{
    if (!IsConnected()) {
        return ER_BUS_NOT_CONNECTED;
    }

    Message reply(*this);
    MsgArg arg("s", busAddr);
    const ProxyBusObject& alljoynObj = this->GetAllJoynProxyObj();
    QStatus status = alljoynObj.MethodCall(org::alljoyn::Bus::InterfaceName, "Connect", &arg, 1, reply);
    if (ER_OK == status) {
        if (reply->GetType() == MESSAGE_METHOD_RET) {
            disposition = reply->GetArg(0)->v_uint32;
        } else if (reply->GetType() == MESSAGE_ERROR) {
            status = ER_BUS_REPLY_IS_ERROR_MESSAGE;
            String errMsg;
            const char* errName = reply->GetErrorName(&errMsg);
            QCC_LogError(status, ("%s.Connect returned ERROR_MESSAGE (error=%s, \"%s\")",
                                  org::alljoyn::Bus::InterfaceName,
                                  errName,
                                  errMsg.c_str()));
        } else {
            status = ER_FAIL;
        }
    }
    return status;
}

QStatus BusAttachment::DisconnectFromRemoteBus(const char* busAddr, uint32_t& disposition)
{
    if (!IsConnected()) {
        return ER_BUS_NOT_CONNECTED;
    }

    Message reply(*this);
    MsgArg arg("s", busAddr);
    const ProxyBusObject& alljoynObj = this->GetAllJoynProxyObj();
    QStatus status = alljoynObj.MethodCall(org::alljoyn::Bus::InterfaceName, "Disconnect", &arg, 1, reply);
    if (ER_OK == status) {
        if (reply->GetType() == MESSAGE_METHOD_RET) {
            disposition = reply->GetArg(0)->v_uint32;
        } else if (reply->GetType() == MESSAGE_ERROR) {
            status = ER_BUS_REPLY_IS_ERROR_MESSAGE;
            String errMsg;
            const char* errName = reply->GetErrorName(&errMsg);
            QCC_LogError(status, ("%s.Disconnect returned ERROR_MESSAGE (error=%s, \"%s\")",
                                  org::alljoyn::Bus::InterfaceName,
                                  errName,
                                  errMsg.c_str()));
        } else {
            status = ER_FAIL;
        }
    }
    return status;
}

void BusAttachment::Internal::AllJoynSignalHandler(const InterfaceDescription::Member* member,
                                                   const char* srcPath,
                                                   Message& message)
{
    /* Call listeners back on a non-Rx thread */
    Alarm alarm(0, static_cast<AlarmListener*>(this), 0, new Message(message));
    dispatcher.AddAlarm(alarm);
}

void BusAttachment::Internal::AlarmTriggered(const Alarm& alarm)
{
    /* Dispatch thread for BusListener callbacks */
    Message& msg = *(reinterpret_cast<Message*>(alarm.GetContext()));
    size_t numArgs;
    const MsgArg* args;
    msg->GetArgs(numArgs, args);

    if (0 == strcmp("FoundName", msg->GetMemberName())) {
        listenersLock.Lock();
        list<BusListener*>::iterator it = listeners.begin();
        while (it != listeners.end()) {
            (*it++)->FoundName(args[0].v_string.str, args[1].v_string.str, args[2].v_string.str, args[3].v_string.str);
        }
        listenersLock.Unlock();
    } else if (0 == strcmp("LostAdvertisedName", msg->GetMemberName())) {
        listenersLock.Lock();
        list<BusListener*>::iterator it = listeners.begin();
        while (it != listeners.end()) {
            (*it++)->LostAdvertisedName(args[0].v_string.str, args[1].v_string.str, args[2].v_string.str, args[3].v_string.str);
        }
        listenersLock.Unlock();
    } else if (0 == strcmp("BusConnectionLost", msg->GetMemberName())) {
        listenersLock.Lock();
        list<BusListener*>::iterator it = listeners.begin();
        while (it != listeners.end()) {
            (*it++)->BusConnectionLost(args[0].v_string.str);
        }
        listenersLock.Unlock();
    } else if (0 == strcmp("NameOwnerChanged", msg->GetMemberName())) {
        listenersLock.Lock();
        list<BusListener*>::iterator it = listeners.begin();
        while (it != listeners.end()) {
            (*it++)->NameOwnerChanged(args[0].v_string.str,
                                      (0 < args[1].v_string.len) ? args[1].v_string.str : NULL,
                                      (0 < args[2].v_string.len) ? args[2].v_string.str : NULL);
        }
        listenersLock.Unlock();
    } else {
        QCC_LogError(ER_FAIL, ("Unrecognized signal \"%s.%s\" received", msg->GetInterface(), msg->GetMemberName()));
    }
    delete &msg;
}

uint32_t BusAttachment::GetTimestamp()
{
    return qcc::GetTimestamp();
}

QStatus BusAttachment::CreateInterfacesFromXml(const char* xml)
{
    StringSource source(xml);

    /* Parse the XML to update this ProxyBusObject instance (plus any new children and interfaces) */
    XmlParseContext pc(source);
    QStatus status = XmlElement::Parse(pc);
    if (status == ER_OK) {
        XmlHelper xmlHelper(this, "BusAttachment");
        status = xmlHelper.AddInterfaceDefinitions(pc.root);
    }
    return status;
}

}
