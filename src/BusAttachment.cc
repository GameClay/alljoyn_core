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
#include <qcc/FileStream.h>

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
#include "SessionInternal.h"
#include "Transport.h"
#include "TransportList.h"
#include "TCPTransport.h"
#include "BusUtil.h"
#include "UnixTransport.h"
#include "BusEndpoint.h"
#include "LocalTransport.h"
#include "PeerState.h"
#include "KeyStore.h"
#include "BusInternal.h"
#include "AllJoynPeerObj.h"
#include "XmlHelper.h"
#include "LaunchdTransport.h"

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
    globalGuid(qcc::GUID128()),
    msgSerial(1),
    router(router ? router : new ClientRouter),
    localEndpoint(transportList.GetLocalTransport()->GetLocalEndpoint()),
    timer("BusTimer", true),
    dispatcher("BusDispatcher", true, 4),
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
    status = org::alljoyn::CreateInterfaces(bus);
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
    /*
     * Make sure that all threads that might possibly access this object have been joined.
     */
    timer.Join();
    dispatcher.Join();
    transportList.Join();
    delete router;
    router = NULL;
}

class LocalTransportFactoryContainer : public TransportFactoryContainer {
  public:
    LocalTransportFactoryContainer()
    {
#if defined(QCC_OS_WINDOWS)
        Add(new TransportFactory<TCPTransport>("tcp", true));
#elif defined(QCC_OS_DARWIN)
        Add(new TransportFactory<LaunchdTransport>("launchd", true));
#else
        Add(new TransportFactory<UnixTransport>("unix", true));
#endif
    }
} localTransportsContainer;

BusAttachment::BusAttachment(const char* applicationName, bool allowRemoteMessages) :
    hasStarted(false),
    isStarted(false),
    isStopping(false),
    busInternal(new Internal(applicationName, *this, localTransportsContainer, NULL, allowRemoteMessages, NULL)),
    joinObj(this)
{
    QCC_DbgTrace(("BusAttachment client constructor (%p)", this));
}

BusAttachment::BusAttachment(Internal* busInternal) :
    hasStarted(false),
    isStarted(false),
    isStopping(false),
    busInternal(busInternal),
    joinObj(this)
{
    QCC_DbgTrace(("BusAttachment daemon constructor"));
}

BusAttachment::~BusAttachment(void)
{
    QCC_DbgTrace(("BusAttachment Destructor (%p)", this));

    StopInternal(true);

    /*
     * Other threads may be attempting to stop the bus. We need to wait for ALL
     * callers of BusAttachment::StopInternal() to exit before deleting the
     * object
     */
    while (busInternal->stopCount) {
        /*
         * We want to allow other calling threads to complete.  This means we
         * need to yield the CPU.  Sleep(0) yields the CPU to all threads of
         * equal or greater priority.  Other callers may be of lesser priority
         * so We need to yield the CPU to them, too.  We need to get ourselves
         * off of the ready queue, so we need to really execute a sleep.  The
         * Sleep(1) will translate into a mimimum sleep of one scheduling quantum
         * which is, for example, one Jiffy in Liux which is 1/250 second or
         * 4 ms.  It's not as arbitrary as it might seem.
         */
        qcc::Sleep(1);
    }

    delete busInternal;
    busInternal = NULL;
}

QStatus BusAttachment::Start()
{
    QStatus status;

    QCC_DbgTrace(("BusAttachment::Start()"));

    /*
     * The variable isStarted indicates that the bus has been Start()ed, and has
     * not yet been Stop()ed.  As soon as a Join is completed, isStarted is set
     * to false.  We want to prevent the bus attachment from being started
     * multiple times to prevent very hard to debug problems where users try to
     * reuse bus attachments in the mistaken belief that it will somehow be more
     * efficient.  There are three state variables here and we check them all
     * separately (in order to be specific with error messages) before
     * continuing to allow a Start.
     */
    if (hasStarted) {
        status = ER_BUS_BUS_ALREADY_STARTED;
        QCC_LogError(status, ("BusAttachment::Start(): Start may not ever be called more than once"));
        return status;
    }

    if (isStarted) {
        status = ER_BUS_BUS_ALREADY_STARTED;
        QCC_LogError(status, ("BusAttachment::Start(): Start called, but currently started."));
        return status;
    }

    if (isStopping) {
        status = ER_BUS_STOPPING;
        QCC_LogError(status, ("BusAttachment::Start(): Start called while stopping"));
        return status;
    }

    isStarted = hasStarted = true;

    /*
     * Start the alljoyn signal dispatcher first because the dispatcher thread is responsible,
     * via the Internal::ThreadListener, for stopping the timer thread and the transports.
     */
    status = busInternal->dispatcher.Start();
    if (ER_OK == status) {
        /* Start the timer */
        status = busInternal->timer.Start();
    }

    if (ER_OK == status) {
        /* Start the transports */
        status = busInternal->transportList.Start(busInternal->GetListenAddresses());
    }

    if ((status == ER_OK) && isStopping) {
        status = ER_BUS_STOPPING;
        QCC_LogError(status, ("BusAttachment::Start bus was stopped while starting"));
    }

    if (status != ER_OK) {
        QCC_LogError(status, ("BusAttachment::Start failed to start"));
        busInternal->dispatcher.Stop();
        busInternal->timer.Stop();
        busInternal->transportList.Stop();
        WaitStopInternal();
    }
    return status;
}

#if defined(QCC_OS_ANDROID)
static qcc::String bundleConnectSpec;
static bool isBundleDaemonStarted = false;

/* We only try to load alternate daemons on Android platform with steps:
 * 1) Try to connect to the preinstalled (BT capable daemon) whose connect path is unix:abstract=alljoyn
 * 2) If #1 fails, issue an Intent to start the daemon APK, wait for it to launch and then try to connect
 *    to this daemon. The connect path is unix:abstract=alljoyn-service.
 * 3) If #2 fails, look for a daemon that may have been bundled with the application itself and launch/connect to it if found.
 *    The connect path is unix:abstract=alljoyn-{UUID}
 */
QStatus BusAttachment::TryAlternativeDaemon(RemoteEndpoint** newep)
{
    QCC_DbgTrace(("BusAttachment::TryAlternativeDaemon()"));
    QStatus status = ER_FAIL;
    bool shouldWaitReady = false;
    const uint32_t TRY_PERIOD_IN_MS = 100;
    const uint32_t MAX_CONNECT_TRIES = 3;

    /* Try Android apk daemon*/
    qcc::String apkConnSpec = "unix:abstract=alljoyn-service";
    QCC_DbgPrintf(("BusAttachment::TryAlternativeDaemon::try apkConnSpec = %s", apkConnSpec.c_str()));

    status = TryConnect(apkConnSpec.c_str(), newep);
    /* Maybe the APK daemon is installed but not started yet, so try issue an intent to start it*/
    if (ER_OK != status) {
        if (system("am startservice -W -a org.alljoyn.bus.START_DAEMON") != -1) {
            uint32_t numOfTries = 0;
            /* Try connect to the APK daemon again every 100ms*/
            while (numOfTries < MAX_CONNECT_TRIES && status != ER_OK) {
                QCC_DbgPrintf(("Wait %d ms before trying connect", TRY_PERIOD_IN_MS));
                qcc::Event timerEvent(TRY_PERIOD_IN_MS, 0);
                qcc::Event::Wait(timerEvent, TRY_PERIOD_IN_MS);
                status = TryConnect(apkConnSpec.c_str(), newep);
                numOfTries++;
            }
        }
    }

    if (ER_OK == status) {
        this->connectSpec = apkConnSpec; /* Save the connect spec so that Disconnect() will use it*/
        return status;
    }

    /* Next try the daemon bundled with the application. When we issue an intent to start the BundleDaemonService,
     * the connect spec is saved in the global variable bundleConnectSpec. We should use the saved bundleConnectSpec to connect to the
     * daemon if daemon is already running.
     */

    /* If bundle daemon connect spec is not set yet, then generate a unique connect spec using UUID*/
    if (bundleConnectSpec.empty()) {
        qcc::GUID128 specGuid;
        bundleConnectSpec = "unix:abstract=alljoyn-" + specGuid.ToString();
    }

    if (!isBundleDaemonStarted) {
        /* To start the BundleDaemonService of the application associated with this process, we should explicitly give the component name of the service.
         * The component name includes the application package name and the service name. Here we read the package name from file /proc/${PID}/cmdline.
         * Android uses package name as process name by default*/
        qcc::String packageName;
        const uint32_t MAX_PID_STR_SIZE = 32;
        char pidStr [MAX_PID_STR_SIZE];
        pid_t pid = getpid();
        snprintf(pidStr, MAX_PID_STR_SIZE, "%d", pid);
        qcc::String fileName = "/proc/";
        fileName += pidStr;
        fileName += "/cmdline";
        QCC_DbgHLPrintf(("Read fileName %s ", fileName.c_str()));
        FileSource source(fileName);
        const uint32_t maxBufferLen = 128;
        char pkgBuffer[maxBufferLen];
        uint32_t actualRead = 0;
        if (source.IsValid()) {
            source.PullBytes(pkgBuffer, maxBufferLen - 1, actualRead);
            pkgBuffer[actualRead] = '\0';
            packageName += pkgBuffer;

            while (actualRead == (maxBufferLen - 1)) {
                actualRead = 0;
                source.PullBytes(pkgBuffer, maxBufferLen - 1, actualRead);
                pkgBuffer[actualRead] = '\0';
                packageName += pkgBuffer;
            }
        } else {
            /*It may fail to read from the file /proc/${PID}/cmdline if Android locks the dir /proc in the future, then fallback to use the application name*/
            QCC_DbgHLPrintf(("Fail to read file %s, use application name instead.", fileName.c_str()));
            packageName = busInternal->application;
        }

        QCC_DbgHLPrintf(("BusAttachment::Try to start bundle daemon: packageName =%s\n", packageName.c_str()));
        qcc::String intent = "am startservice -W -n "; /* Using "-W" will wait until the launch finishes*/
        intent += packageName;
        intent += "/org.alljoyn.bus.alljoyn.BundleDaemonService  -d ";
        intent += bundleConnectSpec.c_str(); /*We piggyback the connect spec in the Data Uri of the intent*/
        QCC_DbgHLPrintf(("Send intent = %s to start BundleDaemonService", intent.c_str()));
        if (system(intent.c_str()) == -1) {
            QCC_LogError(status, ("BusAttachment::Connect fail to start bundle daemon via system() call"));
        } else {
            isBundleDaemonStarted = true;
            shouldWaitReady = true;
        }
    }

    /* Try connect to the bundle daemon*/
    if (shouldWaitReady) {
        uint32_t numOfTries = 0;
        while (numOfTries < MAX_CONNECT_TRIES && status != ER_OK) {
            QCC_DbgPrintf(("Wait %d ms before trying connect", TRY_PERIOD_IN_MS));
            qcc::Event timerEvent(TRY_PERIOD_IN_MS, 0);
            qcc::Event::Wait(timerEvent, TRY_PERIOD_IN_MS);
            status = TryConnect(bundleConnectSpec.c_str(), newep);
            numOfTries++;
        }
    } else {
        status = TryConnect(bundleConnectSpec.c_str(), newep);
    }

    if (status == ER_OK) {
        this->connectSpec = bundleConnectSpec; /* Save the connect spec so that Disconnect() will use it*/
    }

    return status;
}

#endif

QStatus BusAttachment::TryConnect(const char* connectSpec, RemoteEndpoint** newep)
{
    QCC_DbgTrace(("BusAttachment::TryConnect to %s", connectSpec));
    QStatus status = ER_OK;
    /* Get or create transport for connection */
    Transport* trans = busInternal->transportList.GetTransport(connectSpec);
    if (trans) {
        SessionOpts emptyOpts;
        status = trans->Connect(connectSpec, emptyOpts, newep);
    } else {
        status = ER_BUS_TRANSPORT_NOT_AVAILABLE;
    }
    return status;
}

QStatus BusAttachment::Connect(const char* connectSpec, RemoteEndpoint** newep)
{
    QStatus status;
    bool isDaemon = busInternal->GetRouter().IsDaemon();

    if (!isStarted) {
#if OPTIONAL_START
        /*
         * Optional implies it may not already be called.
         */
        QCC_DbgTrace(("BusAttachment::Connect(): Exercising optional Start()"));
        status = Start();
#else
        status = ER_BUS_BUS_NOT_STARTED;
#endif
    } else if (isStopping) {
        status = ER_BUS_STOPPING;
        QCC_LogError(status, ("BusAttachment::Connect cannot connect while bus is stopping"));
    } else if (IsConnected() && !isDaemon) {
        status = ER_BUS_ALREADY_CONNECTED;
    } else {
        this->connectSpec = connectSpec;
        status = TryConnect(connectSpec, newep);

#if defined(QCC_OS_ANDROID)
        /* If the connect sepc is "unix:abstract=alljoyn", then try other daemon options with the precedence of preinstalled daemon > APK daemon > bundle daemon */
        if (status != ER_OK && !isDaemon && (strcmp(connectSpec, "unix:abstract=alljoyn") == 0)) {
            status = TryAlternativeDaemon(newep);
        }
#endif
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
                                               ajIface->GetMember("FoundAdvertisedName"),
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
                                               ajIface->GetMember("SessionLost"),
                                               NULL);
            }
            if (ER_OK == status) {
                assert(ajIface);
                status = RegisterSignalHandler(busInternal,
                                               static_cast<MessageReceiver::SignalHandler>(&BusAttachment::Internal::AllJoynSignalHandler),
                                               ajIface->GetMember("MPSessionChanged"),
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
    } else if (isStopping) {
        status = ER_BUS_STOPPING;
        QCC_LogError(status, ("BusAttachment::Diconnect cannot disconnect while bus is stopping"));
    } else if (!isDaemon && !IsConnected()) {
        status = ER_BUS_NOT_CONNECTED;
    } else {
        /* Terminate transport for connection */
        Transport* trans = busInternal->transportList.GetTransport(this->connectSpec.c_str());

        if (trans) {
            status = trans->Disconnect(this->connectSpec.c_str());
        } else {
            status = ER_BUS_TRANSPORT_NOT_AVAILABLE;
        }

        /* Unregister signal handlers if this is a client-side bus attachment */
        if ((ER_OK == status) && !isDaemon) {
            const InterfaceDescription* dbusIface = GetInterface(org::freedesktop::DBus::InterfaceName);
            if (dbusIface) {
                UnregisterSignalHandler(busInternal,
                                        static_cast<MessageReceiver::SignalHandler>(&BusAttachment::Internal::AllJoynSignalHandler),
                                        dbusIface->GetMember("NameOwnerChanged"),
                                        NULL);
            }
            const InterfaceDescription* alljoynIface = GetInterface(org::alljoyn::Bus::InterfaceName);
            if (alljoynIface) {
                UnregisterSignalHandler(busInternal,
                                        static_cast<MessageReceiver::SignalHandler>(&BusAttachment::Internal::AllJoynSignalHandler),
                                        alljoynIface->GetMember("FoundAdvertisedName"),
                                        NULL);
            }
            if (alljoynIface) {
                UnregisterSignalHandler(busInternal,
                                        static_cast<MessageReceiver::SignalHandler>(&BusAttachment::Internal::AllJoynSignalHandler),
                                        alljoynIface->GetMember("LostAdvertisedName"),
                                        NULL);
            }
            if (alljoynIface) {
                UnregisterSignalHandler(busInternal,
                                        static_cast<MessageReceiver::SignalHandler>(&BusAttachment::Internal::AllJoynSignalHandler),
                                        alljoynIface->GetMember("SessionLost"),
                                        NULL);
            }
            if (alljoynIface) {
                UnregisterSignalHandler(busInternal,
                                        static_cast<MessageReceiver::SignalHandler>(&BusAttachment::Internal::AllJoynSignalHandler),
                                        alljoynIface->GetMember("MPSessionChanged"),
                                        NULL);
            }
        }
    }

    if (ER_OK != status) {
        QCC_LogError(status, ("BusAttachment::Disconnect failed"));
    }
    return status;
}

QStatus BusAttachment::Stop(void)
{
    return StopInternal(false);
}

/*
 * Note if called with blockUntilStopped == false this function must not do anything that might block.
 * Because we don't know what kind of cleanup various transports may do on Stop() the transports are
 * stopped on the ThreadExit callback for the dispatch thread.
 */
QStatus BusAttachment::StopInternal(bool blockUntilStopped)
{
    QStatus status = ER_OK;
    if (isStarted) {
        isStopping = true;
        /*
         * Let bus listeners know the bus is stopping.
         */
        busInternal->listenersLock.Lock(MUTEX_CONTEXT);
        list<BusListener*>::iterator it = busInternal->listeners.begin();
        while (it != busInternal->listeners.end()) {
            (*it++)->BusStopping();
        }
        busInternal->listenersLock.Unlock(MUTEX_CONTEXT);
        /* Stop the timer thread */
        status = busInternal->timer.Stop();
        if (ER_OK != status) {
            QCC_LogError(status, ("Timer::Stop() failed"));
        }
        /* Stop the dispatcher */
        status = busInternal->dispatcher.Stop();
        if (ER_OK != status) {
            QCC_LogError(status, ("Dispatcher::Stop() failed"));
        }
        /* Stop the transport list */
        status = busInternal->transportList.Stop();
        if (ER_OK != status) {
            QCC_LogError(status, ("TransportList::Stop() failed"));
        }

        if ((status == ER_OK) && blockUntilStopped) {
            WaitStopInternal();
        }
    }
    return status;
}

QStatus BusAttachment::Join()
{
    QCC_DbgTrace(("BusAttachment::Join"));
    WaitStopInternal();
    return ER_OK;
}

void BusAttachment::WaitStopInternal()
{
    QCC_DbgTrace(("BusAttachment::WaitStopInternal"));
    if (isStarted) {
        /*
         * We use a combination of a mutex and a counter to ensure that all threads that are
         * blocked waiting for the bus attachment to stop are actually blocked.
         */
        IncrementAndFetch(&busInternal->stopCount);
        busInternal->stopLock.Lock(MUTEX_CONTEXT);

        /*
         * In the case where more than one thread has called WaitStopInternal() the first thread in will
         * clear the isStarted flag.
         */
        if (isStarted) {
            busInternal->timer.Join();
            busInternal->dispatcher.Join();
            busInternal->transportList.Join();

            /* Clear peer state */
            busInternal->peerStateTable.Clear();

            /* Persist keystore */
            busInternal->keyStore.Store();

            isStarted = false;
            isStopping = false;
        }

        busInternal->stopLock.Unlock(MUTEX_CONTEXT);
        DecrementAndFetch(&busInternal->stopCount);
    }
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

size_t BusAttachment::GetInterfaces(const InterfaceDescription** ifaces, size_t numIfaces) const
{
    size_t count = 0;
    map<qcc::StringMapKey, InterfaceDescription>::const_iterator it;
    for (it = busInternal->ifaceDescriptions.begin(); it != busInternal->ifaceDescriptions.end(); it++) {
        if (it->second.isActivated) {
            if (ifaces && (count < numIfaces)) {
                ifaces[count] = &(it->second);
            }
            ++count;
        }
    }
    return count;
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

QStatus BusAttachment::RegisterKeyStoreListener(KeyStoreListener& listener)
{
    return busInternal->keyStore.SetListener(listener);
}

void BusAttachment::ClearKeyStore()
{
    busInternal->keyStore.Clear();
}

const qcc::String BusAttachment::GetUniqueName() const
{
    /*
     * Cannot have a valid unique name if not connected to the bus.
     */
    if (!IsConnected()) {
        return "";
    }
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

const ProxyBusObject& BusAttachment::GetAllJoynDebugObj()
{
    return busInternal->localEndpoint.GetAllJoynDebugObj();
}

QStatus BusAttachment::RegisterSignalHandler(MessageReceiver* receiver,
                                             MessageReceiver::SignalHandler signalHandler,
                                             const InterfaceDescription::Member* member,
                                             const char* srcPath)
{
    return busInternal->localEndpoint.RegisterSignalHandler(receiver, signalHandler, member, srcPath);
}

QStatus BusAttachment::UnregisterSignalHandler(MessageReceiver* receiver,
                                               MessageReceiver::SignalHandler signalHandler,
                                               const InterfaceDescription::Member* member,
                                               const char* srcPath)
{
    return busInternal->localEndpoint.UnregisterSignalHandler(receiver, signalHandler, member, srcPath);
}

QStatus BusAttachment::UnregisterAllHandlers(MessageReceiver* receiver)
{
    return busInternal->localEndpoint.UnregisterAllHandlers(receiver);
}

bool BusAttachment::IsConnected() const {
    return busInternal->router->IsBusRunning();
}

QStatus BusAttachment::RegisterBusObject(BusObject& obj) {
    return busInternal->localEndpoint.RegisterBusObject(obj);
}

void BusAttachment::UnregisterBusObject(BusObject& object)
{
    busInternal->localEndpoint.UnregisterBusObject(object);
}

QStatus BusAttachment::EnablePeerSecurity(const char* authMechanisms,
                                          AuthListener* listener,
                                          const char* keyStoreFileName,
                                          bool isShared)
{
    QStatus status = ER_OK;

    /* If there are no auth mechanisms peer security is being disabled. */
    if (authMechanisms) {
        status = busInternal->keyStore.Init(keyStoreFileName, isShared);
        if (status == ER_OK) {
            /* Register peer-to-peer authentication mechanisms */
            busInternal->authManager.RegisterMechanism(AuthMechSRP::Factory, AuthMechSRP::AuthName());
            busInternal->authManager.RegisterMechanism(AuthMechRSA::Factory, AuthMechRSA::AuthName());
            busInternal->authManager.RegisterMechanism(AuthMechLogon::Factory, AuthMechLogon::AuthName());
            /* Validate the list of auth mechanisms */
            status =  busInternal->authManager.CheckNames(authMechanisms);
        }
    }
    if (status == ER_OK) {
        AllJoynPeerObj* peerObj = busInternal->localEndpoint.GetPeerObj();
        if (peerObj) {
            peerObj->SetupPeerAuthentication(authMechanisms, authMechanisms ? listener : NULL);
        } else {
            return ER_FAIL;
        }
    }
    return status;
}

bool BusAttachment::IsPeerSecurityEnabled()
{
    AllJoynPeerObj* peerObj = busInternal->localEndpoint.GetPeerObj();
    if (peerObj) {
        return peerObj->AuthenticationEnabled();
    } else {
        return false;
    }
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

QStatus BusAttachment::RequestName(const char* requestedName, uint32_t flags)
{
    if (!IsConnected()) {
        return ER_BUS_NOT_CONNECTED;
    }

    Message reply(*this);
    MsgArg args[2];
    size_t numArgs = ArraySize(args);

    MsgArg::Set(args, numArgs, "su", requestedName, flags);

    const ProxyBusObject& dbusObj = this->GetDBusProxyObj();
    QStatus status = dbusObj.MethodCall(org::freedesktop::DBus::InterfaceName, "RequestName", args, numArgs, reply);
    if (ER_OK == status) {
        uint32_t disposition;
        status = reply->GetArgs("u", &disposition);
        if (ER_OK == status) {
            switch (disposition) {
            case DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER :
                break;

            case DBUS_REQUEST_NAME_REPLY_IN_QUEUE :
                status = ER_DBUS_REQUEST_NAME_REPLY_IN_QUEUE;
                break;

            case DBUS_REQUEST_NAME_REPLY_EXISTS :
                status = ER_DBUS_REQUEST_NAME_REPLY_EXISTS;
                break;

            case DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER:
                status = ER_DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER;
                break;

            default:
                status = ER_BUS_UNEXPECTED_DISPOSITION;
                break;
            }
        }
    } else {
        String errMsg;
        const char* errName = reply->GetErrorName(&errMsg);
        QCC_LogError(status, ("%s.RequestName returned ERROR_MESSAGE (error=%s, \"%s\")",
                              org::freedesktop::DBus::InterfaceName,
                              errName,
                              errMsg.c_str()));
    }
    return status;
}

QStatus BusAttachment::ReleaseName(const char* name)
{
    if (!IsConnected()) {
        return ER_BUS_NOT_CONNECTED;
    }

    Message reply(*this);
    MsgArg args[1];
    size_t numArgs = ArraySize(args);

    MsgArg::Set(args, numArgs, "s", name);

    const ProxyBusObject& dbusObj = this->GetDBusProxyObj();
    QStatus status = dbusObj.MethodCall(org::freedesktop::DBus::InterfaceName, "ReleaseName", args, numArgs, reply);
    if (ER_OK == status) {
        uint32_t disposition;
        status = reply->GetArgs("u", &disposition);
        if (ER_OK == status) {
            switch (disposition) {
            case DBUS_RELEASE_NAME_REPLY_RELEASED:
                break;

            case DBUS_RELEASE_NAME_REPLY_NON_EXISTENT:
                status = ER_DBUS_RELEASE_NAME_REPLY_NON_EXISTENT;
                break;

            case DBUS_RELEASE_NAME_REPLY_NOT_OWNER:
                status = ER_DBUS_RELEASE_NAME_REPLY_NOT_OWNER;
                break;

            default:
                status = ER_BUS_UNEXPECTED_DISPOSITION;
                break;
            }
        }
    } else {
        String errMsg;
        const char* errName = reply->GetErrorName(&errMsg);
        QCC_LogError(status, ("%s.ReleaseName returned ERROR_MESSAGE (error=%s, \"%s\")",
                              org::freedesktop::DBus::InterfaceName,
                              errName,
                              errMsg.c_str()));
    }
    return status;
}

QStatus BusAttachment::AddMatch(const char* rule)
{
    if (!IsConnected()) {
        return ER_BUS_NOT_CONNECTED;
    }

    Message reply(*this);
    MsgArg args[1];
    size_t numArgs = ArraySize(args);

    MsgArg::Set(args, numArgs, "s", rule);

    const ProxyBusObject& dbusObj = this->GetDBusProxyObj();
    QStatus status = dbusObj.MethodCall(org::freedesktop::DBus::InterfaceName, "AddMatch", args, numArgs, reply);
    if (ER_OK != status) {
        String errMsg;
        const char* errName = reply->GetErrorName(&errMsg);
        QCC_LogError(status, ("%s.AddMatch returned ERROR_MESSAGE (error=%s, \"%s\")",
                              org::freedesktop::DBus::InterfaceName,
                              errName,
                              errMsg.c_str()));
    }
    return status;
}

QStatus BusAttachment::RemoveMatch(const char* rule)
{
    if (!IsConnected()) {
        return ER_BUS_NOT_CONNECTED;
    }

    Message reply(*this);
    MsgArg args[1];
    size_t numArgs = ArraySize(args);

    MsgArg::Set(args, numArgs, "s", rule);

    const ProxyBusObject& dbusObj = this->GetDBusProxyObj();
    QStatus status = dbusObj.MethodCall(org::freedesktop::DBus::InterfaceName, "RemoveMatch", args, numArgs, reply);
    if (ER_OK != status) {
        String errMsg;
        const char* errName = reply->GetErrorName(&errMsg);
        QCC_LogError(status, ("%s.RemoveMatch returned ERROR_MESSAGE (error=%s, \"%s\")",
                              org::freedesktop::DBus::InterfaceName,
                              errName,
                              errMsg.c_str()));
    }
    return status;
}

QStatus BusAttachment::FindAdvertisedName(const char* namePrefix)
{
    if (!IsConnected()) {
        return ER_BUS_NOT_CONNECTED;
    }

    Message reply(*this);
    MsgArg args[1];
    size_t numArgs = ArraySize(args);

    MsgArg::Set(args, numArgs, "s", namePrefix);

    const ProxyBusObject& alljoynObj = this->GetAllJoynProxyObj();
    QStatus status = alljoynObj.MethodCall(org::alljoyn::Bus::InterfaceName, "FindAdvertisedName", args, numArgs, reply);
    if (ER_OK == status) {
        uint32_t disposition;
        status = reply->GetArgs("u", &disposition);
        if (ER_OK == status) {
            switch (disposition) {
            case ALLJOYN_FINDADVERTISEDNAME_REPLY_SUCCESS:
                break;

            case ALLJOYN_FINDADVERTISEDNAME_REPLY_ALREADY_DISCOVERING:
                status = ER_ALLJOYN_FINDADVERTISEDNAME_REPLY_ALREADY_DISCOVERING;
                break;

            case ALLJOYN_FINDADVERTISEDNAME_REPLY_FAILED:
                status = ER_ALLJOYN_FINDADVERTISEDNAME_REPLY_FAILED;
                break;

            default:
                status = ER_BUS_UNEXPECTED_DISPOSITION;
                break;
            }
        }
    } else {
        String errMsg;
        const char* errName = reply->GetErrorName(&errMsg);
        QCC_LogError(status, ("%s.FindAdvertisedName returned ERROR_MESSAGE (error=%s, \"%s\")",
                              org::alljoyn::Bus::InterfaceName,
                              errName,
                              errMsg.c_str()));
    }
    return status;
}

QStatus BusAttachment::CancelFindAdvertisedName(const char* namePrefix)
{
    if (!IsConnected()) {
        return ER_BUS_NOT_CONNECTED;
    }

    Message reply(*this);
    MsgArg args[1];
    size_t numArgs = ArraySize(args);

    MsgArg::Set(args, numArgs, "s", namePrefix);

    const ProxyBusObject& alljoynObj = this->GetAllJoynProxyObj();
    QStatus status = alljoynObj.MethodCall(org::alljoyn::Bus::InterfaceName, "CancelFindAdvertisedName", args, numArgs, reply);
    if (ER_OK == status) {
        uint32_t disposition;
        status = reply->GetArgs("u", &disposition);
        if (ER_OK == status) {
            switch (disposition) {
            case ALLJOYN_CANCELFINDADVERTISEDNAME_REPLY_SUCCESS:
                break;

            case ALLJOYN_CANCELFINDADVERTISEDNAME_REPLY_FAILED:
                status = ER_ALLJOYN_CANCELFINDADVERTISEDNAME_REPLY_FAILED;
                break;

            default:
                status = ER_BUS_UNEXPECTED_DISPOSITION;
                break;
            }
        }
    } else {
        String errMsg;
        const char* errName = reply->GetErrorName(&errMsg);
        QCC_LogError(status, ("%s.CancelFindAdvertisedName returned ERROR_MESSAGE (error=%s, \"%s\")",
                              org::alljoyn::Bus::InterfaceName,
                              errName,
                              errMsg.c_str()));
    }
    return status;
}

QStatus BusAttachment::AdvertiseName(const char* name, TransportMask transports)
{
    if (!IsConnected()) {
        return ER_BUS_NOT_CONNECTED;
    }

    Message reply(*this);
    MsgArg args[2];
    size_t numArgs = ArraySize(args);

    MsgArg::Set(args, numArgs, "sq", name, transports);

    const ProxyBusObject& alljoynObj = this->GetAllJoynProxyObj();
    QStatus status = alljoynObj.MethodCall(org::alljoyn::Bus::InterfaceName, "AdvertiseName", args, numArgs, reply);
    if (ER_OK == status) {
        int32_t disposition;
        status = reply->GetArgs("u", &disposition);
        if (ER_OK == status) {
            switch (disposition) {
            case ALLJOYN_ADVERTISENAME_REPLY_SUCCESS:
                break;

            case ALLJOYN_ADVERTISENAME_REPLY_ALREADY_ADVERTISING:
                status = ER_ALLJOYN_ADVERTISENAME_REPLY_ALREADY_ADVERTISING;
                break;

            case ALLJOYN_ADVERTISENAME_REPLY_FAILED:
                status = ER_ALLJOYN_ADVERTISENAME_REPLY_FAILED;
                break;

            default:
                status = ER_BUS_UNEXPECTED_DISPOSITION;
                break;
            }
        }
    } else {
        String errMsg;
        const char* errName = reply->GetErrorName(&errMsg);
        QCC_LogError(status, ("%s.AdvertiseName returned ERROR_MESSAGE (error=%s, \"%s\")",
                              org::alljoyn::Bus::InterfaceName,
                              errName,
                              errMsg.c_str()));
    }
    return status;
}

QStatus BusAttachment::CancelAdvertiseName(const char* name, TransportMask transports)
{
    if (!IsConnected()) {
        return ER_BUS_NOT_CONNECTED;
    }

    Message reply(*this);
    MsgArg args[2];
    size_t numArgs = ArraySize(args);

    MsgArg::Set(args, numArgs, "sq", name, transports);

    const ProxyBusObject& alljoynObj = this->GetAllJoynProxyObj();
    QStatus status = alljoynObj.MethodCall(org::alljoyn::Bus::InterfaceName, "CancelAdvertiseName", args, numArgs, reply);
    if (ER_OK == status) {
        uint32_t disposition;
        status = reply->GetArgs("u", &disposition);
        if (ER_OK == status) {
            switch (disposition) {
            case ALLJOYN_CANCELADVERTISENAME_REPLY_SUCCESS:
                break;

            case ALLJOYN_CANCELADVERTISENAME_REPLY_FAILED:
                status = ER_ALLJOYN_CANCELADVERTISENAME_REPLY_FAILED;
                break;

            default:
                status = ER_BUS_UNEXPECTED_DISPOSITION;
                break;
            }
        }
    } else {
        String errMsg;
        const char* errName = reply->GetErrorName(&errMsg);
        QCC_LogError(status, ("%s.CancelAdvertiseName returned ERROR_MESSAGE (error=%s, \"%s\")",
                              org::alljoyn::Bus::InterfaceName,
                              errName,
                              errMsg.c_str()));
    }
    return status;
}

void BusAttachment::RegisterBusListener(BusListener& listener)
{
    busInternal->listenersLock.Lock(MUTEX_CONTEXT);
    busInternal->listeners.push_back(&listener);
    /* Let listener know which bus attachment it has been registered on */
    listener.ListenerRegistered(this);
    busInternal->listenersLock.Unlock(MUTEX_CONTEXT);
}

void BusAttachment::UnregisterBusListener(BusListener& listener)
{
    busInternal->listenersLock.Lock(MUTEX_CONTEXT);
    list<BusListener*>::iterator it = std::find(busInternal->listeners.begin(), busInternal->listeners.end(), &listener);
    if (it != busInternal->listeners.end()) {
        busInternal->listeners.erase(it);
        listener.ListenerUnregistered();
    }
    busInternal->listenersLock.Unlock(MUTEX_CONTEXT);
}

QStatus BusAttachment::NameHasOwner(const char* name, bool& hasOwner)
{
    if (!IsConnected()) {
        return ER_BUS_NOT_CONNECTED;
    }

    Message reply(*this);
    MsgArg arg("s", name);
    QStatus status = this->GetDBusProxyObj().MethodCall(org::freedesktop::DBus::InterfaceName, "NameHasOwner", &arg, 1, reply);
    if (ER_OK == status) {
        status = reply->GetArgs("b", &hasOwner);
    } else {
        String errMsg;
        const char* errName = reply->GetErrorName(&errMsg);
        QCC_LogError(status, ("%s.NameHasOwner returned ERROR_MESSAGE (error=%s, \"%s\")",
                              org::freedesktop::DBus::InterfaceName,
                              errName,
                              errMsg.c_str()));
    }
    return status;
}

QStatus BusAttachment::SetDaemonDebug(const char* module, uint32_t level)
{
    if (!IsConnected()) {
        return ER_BUS_NOT_CONNECTED;
    }

    Message reply(*this);
    MsgArg args[2];
    size_t argsSize = ArraySize(args);
    MsgArg::Set(args, argsSize, "su", module, level);
    QStatus status = this->GetAllJoynDebugObj().MethodCall(org::alljoyn::Daemon::Debug::InterfaceName, "SetDebugLevel", args, argsSize, reply);
    if (status != ER_OK) {
        String errMsg;
        reply->GetErrorName(&errMsg);
        if (errMsg == "ER_BUS_NO_SUCH_OBJECT") {
            status = ER_BUS_NO_SUCH_OBJECT;
        }
    }
    return status;
}

QStatus BusAttachment::BindSessionPort(SessionPort& sessionPort, const SessionOpts& opts, SessionPortListener& listener)
{
    if (!IsConnected()) {
        return ER_BUS_NOT_CONNECTED;
    }

    Message reply(*this);
    MsgArg args[2];

    args[0].Set("q", sessionPort);
    SetSessionOpts(opts, args[1]);

    QStatus status = this->GetAllJoynProxyObj().MethodCall(org::alljoyn::Bus::InterfaceName, "BindSessionPort", args, ArraySize(args), reply);
    if (status != ER_OK) {
        String errMsg;
        const char* errName = reply->GetErrorName(&errMsg);
        QCC_LogError(status, ("%s.BindSessionPort returned ERROR_MESSAGE (error=%s, \"%s\")",
                              org::alljoyn::Bus::InterfaceName,
                              errName,
                              errMsg.c_str()));

    } else {
        SessionPort tempPort;
        uint32_t disposition;
        status = reply->GetArgs("uq", &disposition, &tempPort);
        if (status == ER_OK) {
            switch (disposition) {
            case ALLJOYN_BINDSESSIONPORT_REPLY_SUCCESS:
                sessionPort = tempPort;
                break;

            case ALLJOYN_BINDSESSIONPORT_REPLY_ALREADY_EXISTS:
                status = ER_ALLJOYN_BINDSESSIONPORT_REPLY_ALREADY_EXISTS;
                break;

            case ALLJOYN_BINDSESSIONPORT_REPLY_INVALID_OPTS:
                status = ER_ALLJOYN_BINDSESSIONPORT_REPLY_INVALID_OPTS;
                break;

            default:
            case ALLJOYN_BINDSESSIONPORT_REPLY_FAILED:
                status = ER_ALLJOYN_BINDSESSIONPORT_REPLY_FAILED;
                break;
            }
        }
        if (status == ER_OK) {
            busInternal->listenersLock.Lock(MUTEX_CONTEXT);
            busInternal->sessionPortListeners[sessionPort] = &listener;
            busInternal->listenersLock.Unlock(MUTEX_CONTEXT);
        }
    }
    return status;
}

QStatus BusAttachment::UnbindSessionPort(SessionPort sessionPort)
{
    if (!IsConnected()) {
        return ER_BUS_NOT_CONNECTED;
    }

    Message reply(*this);
    MsgArg args[1];

    args[0].Set("q", sessionPort);

    QStatus status = this->GetAllJoynProxyObj().MethodCall(org::alljoyn::Bus::InterfaceName, "UnbindSessionPort", args, ArraySize(args), reply);
    if (status != ER_OK) {
        String errMsg;
        const char* errName = reply->GetErrorName(&errMsg);
        QCC_LogError(status, ("%s.UnbindSessionPort returned ERROR_MESSAGE (error=%s, \"%s\")",
                              org::alljoyn::Bus::InterfaceName,
                              errName,
                              errMsg.c_str()));

    } else {
        uint32_t disposition;
        status = reply->GetArgs("u", &disposition);
        if (status == ER_OK) {
            switch (disposition) {
            case ALLJOYN_UNBINDSESSIONPORT_REPLY_SUCCESS:
                status = ER_OK;
                break;

            case ALLJOYN_UNBINDSESSIONPORT_REPLY_BAD_PORT:
                status = ER_ALLJOYN_UNBINDSESSIONPORT_REPLY_BAD_PORT;
                break;

            case ALLJOYN_UNBINDSESSIONPORT_REPLY_FAILED:
            default:
                status = ER_ALLJOYN_UNBINDSESSIONPORT_REPLY_FAILED;
                break;
            }
        }
        if (status == ER_OK) {
            busInternal->sessionListenersLock.Lock(MUTEX_CONTEXT);
            busInternal->sessionPortListeners.erase(sessionPort);
            busInternal->sessionListenersLock.Unlock(MUTEX_CONTEXT);
        }
    }
    return status;
}

struct _JoinSessionMethodCBContext {
    BusAttachment::JoinSessionAsyncCB* callback;
    SessionListener* sessionListener;
    void* context;
    _JoinSessionMethodCBContext(BusAttachment::JoinSessionAsyncCB* callback, SessionListener* sessionListener, void* context)
        : callback(callback), sessionListener(sessionListener), context(context) { }
};

QStatus BusAttachment::JoinSessionAsync(const char* sessionHost, SessionPort sessionPort, SessionListener* sessionListener,
                                        const SessionOpts& opts, BusAttachment::JoinSessionAsyncCB* callback, void* context)
{
    if (!IsConnected()) {
        return ER_BUS_NOT_CONNECTED;
    }
    if (!IsLegalBusName(sessionHost)) {
        return ER_BUS_BAD_BUS_NAME;
    }

    MsgArg args[3];
    size_t numArgs = 2;

    MsgArg::Set(args, numArgs, "sq", sessionHost, sessionPort);
    SetSessionOpts(opts, args[2]);

    const ProxyBusObject& alljoynObj = this->GetAllJoynProxyObj();
    QStatus status = alljoynObj.MethodCallAsync(org::alljoyn::Bus::InterfaceName,
                                                "JoinSession",
                                                busInternal,
                                                static_cast<MessageReceiver::ReplyHandler>(&BusAttachment::Internal::JoinSessionMethodCB),
                                                args, ArraySize(args),
                                                reinterpret_cast<void*>(new _JoinSessionMethodCBContext(callback, sessionListener, context)));
    return status;
}

void BusAttachment::Internal::JoinSessionMethodCB(Message& reply, void* context)
{
    /* Dispatch reply */
    QStatus status = DispatchMessage(*this, reply, context);
    if (status != ER_OK) {
        QCC_LogError(status, ("DispatchMessage for JoinSessionMethodCB failed"));
    }
}

void BusAttachment::Internal::DoJoinSessionMethodCB(Message& reply, void* context)
{
    _JoinSessionMethodCBContext* ctx = reinterpret_cast<_JoinSessionMethodCBContext*>(context);

    QStatus status = ER_FAIL;
    SessionId sessionId = 0;
    SessionOpts opts;
    if (reply->GetType() == MESSAGE_METHOD_RET) {
        const MsgArg* replyArgs;
        size_t na;
        reply->GetArgs(na, replyArgs);
        assert(na == 3);
        uint32_t disposition = replyArgs[0].v_uint32;
        sessionId = replyArgs[1].v_uint32;
        status = GetSessionOpts(replyArgs[2], opts);
        if (status == ER_OK) {
            switch (disposition) {
            case ALLJOYN_JOINSESSION_REPLY_SUCCESS:
                break;

            case ALLJOYN_JOINSESSION_REPLY_NO_SESSION:
                status = ER_ALLJOYN_JOINSESSION_REPLY_NO_SESSION;
                break;

            case ALLJOYN_JOINSESSION_REPLY_UNREACHABLE:
                status = ER_ALLJOYN_JOINSESSION_REPLY_UNREACHABLE;
                break;

            case ALLJOYN_JOINSESSION_REPLY_CONNECT_FAILED:
                status = ER_ALLJOYN_JOINSESSION_REPLY_CONNECT_FAILED;
                break;

            case ALLJOYN_JOINSESSION_REPLY_REJECTED:
                status = ER_ALLJOYN_JOINSESSION_REPLY_REJECTED;
                break;

            case ALLJOYN_JOINSESSION_REPLY_BAD_SESSION_OPTS:
                status = ER_ALLJOYN_JOINSESSION_REPLY_BAD_SESSION_OPTS;
                break;

            case ALLJOYN_JOINSESSION_REPLY_ALREADY_JOINED:
                status = ER_ALLJOYN_JOINSESSION_REPLY_ALREADY_JOINED;
                break;

            case ALLJOYN_JOINSESSION_REPLY_FAILED:
                status = ER_ALLJOYN_JOINSESSION_REPLY_FAILED;
                break;

            default:
                status = ER_BUS_UNEXPECTED_DISPOSITION;
                break;
            }
        }
    } else if (reply->GetType() == MESSAGE_ERROR) {
        status = ER_BUS_REPLY_IS_ERROR_MESSAGE;
        String errMsg;
        const char* errName = reply->GetErrorName(&errMsg);
        sessionId = 0;
        QCC_LogError(status, ("%s.JoinSession returned ERROR_MESSAGE (error=%s, \"%s\")",
                              org::alljoyn::Bus::InterfaceName,
                              errName,
                              errMsg.c_str()));
    }
    if (ctx->sessionListener && (status == ER_OK)) {
        listenersLock.Lock(MUTEX_CONTEXT);
        sessionListeners[sessionId] = ctx->sessionListener;
        listenersLock.Unlock(MUTEX_CONTEXT);
    }

    /* Call the callback */
    ctx->callback->JoinSessionCB(status, sessionId, opts, ctx->context);
    delete ctx;
}

QStatus BusAttachment::JoinSession(const char* sessionHost, SessionPort sessionPort, SessionListener* listener, SessionId& sessionId, SessionOpts& opts)
{
    if (!IsConnected()) {
        return ER_BUS_NOT_CONNECTED;
    }
    if (!IsLegalBusName(sessionHost)) {
        return ER_BUS_BAD_BUS_NAME;
    }

    Message reply(*this);
    MsgArg args[3];
    size_t numArgs = 2;

    MsgArg::Set(args, numArgs, "sq", sessionHost, sessionPort);
    SetSessionOpts(opts, args[2]);

    const ProxyBusObject& alljoynObj = this->GetAllJoynProxyObj();
    QStatus status = alljoynObj.MethodCall(org::alljoyn::Bus::InterfaceName, "JoinSession", args, ArraySize(args), reply);
    if (ER_OK == status) {
        const MsgArg* replyArgs;
        size_t na;
        reply->GetArgs(na, replyArgs);
        assert(na == 3);
        uint32_t disposition = replyArgs[0].v_uint32;
        sessionId = replyArgs[1].v_uint32;
        status = GetSessionOpts(replyArgs[2], opts);
        if (status != ER_OK) {
            sessionId = 0;
        } else {
            switch (disposition) {
            case ALLJOYN_JOINSESSION_REPLY_SUCCESS:
                break;

            case ALLJOYN_JOINSESSION_REPLY_NO_SESSION:
                status = ER_ALLJOYN_JOINSESSION_REPLY_NO_SESSION;
                break;

            case ALLJOYN_JOINSESSION_REPLY_UNREACHABLE:
                status = ER_ALLJOYN_JOINSESSION_REPLY_UNREACHABLE;
                break;

            case ALLJOYN_JOINSESSION_REPLY_CONNECT_FAILED:
                status = ER_ALLJOYN_JOINSESSION_REPLY_CONNECT_FAILED;
                break;

            case ALLJOYN_JOINSESSION_REPLY_REJECTED:
                status = ER_ALLJOYN_JOINSESSION_REPLY_REJECTED;
                break;

            case ALLJOYN_JOINSESSION_REPLY_BAD_SESSION_OPTS:
                status = ER_ALLJOYN_JOINSESSION_REPLY_BAD_SESSION_OPTS;
                break;

            case ALLJOYN_JOINSESSION_REPLY_ALREADY_JOINED:
                status = ER_ALLJOYN_JOINSESSION_REPLY_ALREADY_JOINED;
                break;

            case ALLJOYN_JOINSESSION_REPLY_FAILED:
                status = ER_ALLJOYN_JOINSESSION_REPLY_FAILED;
                break;

            default:
                status = ER_BUS_UNEXPECTED_DISPOSITION;
                break;
            }
        }
    } else {
        String errMsg;
        const char* errName = reply->GetErrorName(&errMsg);
        sessionId = 0;
        QCC_LogError(status, ("%s.JoinSession returned ERROR_MESSAGE (error=%s, \"%s\")",
                              org::alljoyn::Bus::InterfaceName,
                              errName,
                              errMsg.c_str()));
    }
    if (listener && (status == ER_OK)) {
        busInternal->listenersLock.Lock(MUTEX_CONTEXT);
        busInternal->sessionListeners[sessionId] = listener;
        busInternal->listenersLock.Unlock(MUTEX_CONTEXT);
    }
    return status;
}

QStatus BusAttachment::LeaveSession(const SessionId& sessionId)
{
    if (!IsConnected()) {
        return ER_BUS_NOT_CONNECTED;
    }

    Message reply(*this);
    MsgArg arg("u", sessionId);
    const ProxyBusObject& alljoynObj = this->GetAllJoynProxyObj();
    QStatus status = alljoynObj.MethodCall(org::alljoyn::Bus::InterfaceName, "LeaveSession", &arg, 1, reply);
    if (ER_OK == status) {
        uint32_t disposition;
        status = reply->GetArgs("u", &disposition);
        if (ER_OK == status) {
            switch (disposition) {
            case ALLJOYN_LEAVESESSION_REPLY_SUCCESS:
                break;

            case ALLJOYN_LEAVESESSION_REPLY_NO_SESSION:
                status = ER_ALLJOYN_LEAVESESSION_REPLY_NO_SESSION;
                break;

            case ALLJOYN_LEAVESESSION_REPLY_FAILED:
                status = ER_ALLJOYN_LEAVESESSION_REPLY_FAILED;
                break;

            default:
                status = ER_BUS_UNEXPECTED_DISPOSITION;
                break;
            }
        }
    } else {
        String errMsg;
        const char* errName = reply->GetErrorName(&errMsg);
        QCC_LogError(status, ("%s.LeaveSession returned ERROR_MESSAGE (error=%s, \"%s\")",
                              org::alljoyn::Bus::InterfaceName,
                              errName,
                              errMsg.c_str()));
    }
    return status;
}

QStatus BusAttachment::GetSessionFd(SessionId sessionId, SocketFd& sockFd)
{
    QCC_DbgTrace(("BusAttachment::GetSessionFd sessionId:%d", sessionId));
    if (!IsConnected()) {
        return ER_BUS_NOT_CONNECTED;
    }

    sockFd = qcc::INVALID_SOCKET_FD;

    Message reply(*this);
    MsgArg arg("u", sessionId);
    const ProxyBusObject& alljoynObj = this->GetAllJoynProxyObj();
    QStatus status = alljoynObj.MethodCall(org::alljoyn::Bus::InterfaceName, "GetSessionFd", &arg, 1, reply);
    if (ER_OK == status) {
        status = reply->GetArgs("h", &sockFd);
        if (status == ER_OK) {
            status = qcc::SocketDup(sockFd, sockFd);
            if (status == ER_OK) {
                status = qcc::SetBlocking(sockFd, false);
                if (status != ER_OK) {
                    qcc::Close(sockFd);
                }
            }
        }
    } else {
        String errMsg;
        const char* errName = reply->GetErrorName(&errMsg);
        QCC_LogError(status, ("%s.GetSessionFd returned ERROR_MESSAGE (error=%s, \"%s\")",
                              org::alljoyn::Bus::InterfaceName,
                              errName,
                              errMsg.c_str()));
    }
    return status;
}

QStatus BusAttachment::SetLinkTimeout(SessionId sessionId, uint32_t& linkTimeout)
{
    if (!IsConnected()) {
        return ER_BUS_NOT_CONNECTED;
    }

    Message reply(*this);
    MsgArg args[2];

    args[0].Set("u", sessionId);
    args[1].Set("u", linkTimeout);

    QStatus status = this->GetAllJoynProxyObj().MethodCall(org::alljoyn::Bus::InterfaceName, "SetLinkTimeout", args, ArraySize(args), reply);
    if (status != ER_OK) {
        String errMsg;
        const char* errName = reply->GetErrorName(&errMsg);
        QCC_LogError(status, ("%s.SetLinkTimeout returned ERROR_MESSAGE (error=%s, \"%s\")",
                              org::alljoyn::Bus::InterfaceName,
                              errName,
                              errMsg.c_str()));
        status = ER_ALLJOYN_SETLINKTIMEOUT_REPLY_NOT_SUPPORTED;
    } else {
        uint32_t disposition;
        uint32_t replyLinkTimeout = 0;
        status = reply->GetArgs("uu", &disposition, &replyLinkTimeout);
        if (status == ER_OK) {
            switch (disposition) {
            case ALLJOYN_SETLINKTIMEOUT_REPLY_SUCCESS:
                linkTimeout = replyLinkTimeout;
                break;

            case ALLJOYN_SETLINKTIMEOUT_REPLY_NO_DEST_SUPPORT:
                status = ER_ALLJOYN_SETLINKTIMEOUT_REPLY_NO_DEST_SUPPORT;
                break;

            case ALLJOYN_SETLINKTIMEOUT_REPLY_NO_SESSION:
                status = ER_BUS_NO_SESSION;
                break;

            default:
            case ALLJOYN_SETLINKTIMEOUT_REPLY_FAILED:
                status = ER_ALLJOYN_SETLINKTIMEOUT_REPLY_FAILED;
                break;
            }
        }
    }
    return status;
}

QStatus BusAttachment::Internal::DispatchMessage(AlarmListener& listener, Message& msg, void* context, uint32_t delay)
{
    QStatus status;

    if (!bus.isStarted || !dispatcher.IsRunning()) {
        status = ER_BUS_BUS_NOT_STARTED;
    } else if (bus.isStopping) {
        status = ER_BUS_STOPPING;
    } else {
        pair<Message, void*>* alarmContext = new pair<Message, void*>(msg, context);
        Alarm alarm(delay, &listener, 0, alarmContext);
        status = dispatcher.AddAlarm(alarm);
        if (status != ER_OK) {
            delete alarmContext;
        }
    }
    return status;
}

QStatus BusAttachment::Internal::Dispatch(AlarmListener& listener, void* context, uint32_t delay)
{
    QStatus status;

    if (!bus.isStarted || !dispatcher.IsRunning()) {
        status = ER_BUS_BUS_NOT_STARTED;
    } else if (bus.isStopping) {
        status = ER_BUS_STOPPING;
    } else {
        Alarm alarm(delay, &listener, 0, context);
        status = dispatcher.AddAlarm(alarm);
    }
    return status;
}

void BusAttachment::Internal::RemoveDispatchListener(AlarmListener& listener)
{
    dispatcher.RemoveAlarmsWithListener(listener);
}

void BusAttachment::Internal::LocalEndpointDisconnected()
{
    listenersLock.Lock(MUTEX_CONTEXT);
    list<BusListener*>::iterator it = listeners.begin();
    while (it != listeners.end()) {
        (*it++)->BusDisconnected();
    }
    listenersLock.Unlock(MUTEX_CONTEXT);
}

void BusAttachment::Internal::AllJoynSignalHandler(const InterfaceDescription::Member* member,
                                                   const char* srcPath,
                                                   Message& message)
{
    /* Call listeners back on a non-Rx thread */
    DispatchMessage(*this, message, NULL);
}

void BusAttachment::Internal::AlarmTriggered(const Alarm& alarm, QStatus reason)
{
    /* Dispatch thread for BusListener callbacks */
    pair<Message, void*>* alarmContext = reinterpret_cast<pair<Message, void*>*>(alarm.GetContext());
    Message& msg = alarmContext->first;
    size_t numArgs;
    const MsgArg* args;
    msg->GetArgs(numArgs, args);

    if (reason == ER_OK) {
        if (msg->GetType() == MESSAGE_SIGNAL) {
            if (0 == strcmp("FoundAdvertisedName", msg->GetMemberName())) {
                listenersLock.Lock(MUTEX_CONTEXT);
                list<BusListener*>::iterator it = listeners.begin();
                while (it != listeners.end()) {
                    (*it++)->FoundAdvertisedName(args[0].v_string.str, args[1].v_uint16, args[2].v_string.str);
                }
                listenersLock.Unlock(MUTEX_CONTEXT);
            } else if (0 == strcmp("LostAdvertisedName", msg->GetMemberName())) {
                listenersLock.Lock(MUTEX_CONTEXT);
                list<BusListener*>::iterator it = listeners.begin();
                while (it != listeners.end()) {
                    (*it++)->LostAdvertisedName(args[0].v_string.str, args[1].v_uint16, args[2].v_string.str);
                }
                listenersLock.Unlock(MUTEX_CONTEXT);
            } else if (0 == strcmp("SessionLost", msg->GetMemberName())) {
                sessionListenersLock.Lock(MUTEX_CONTEXT);
                SessionId id = static_cast<SessionId>(args[0].v_uint32);
                map<SessionId, SessionListener*>::iterator slit = sessionListeners.find(id);
                if ((slit != sessionListeners.end()) && slit->second) {
                    slit->second->SessionLost(id);
                }
                sessionListenersLock.Unlock(MUTEX_CONTEXT);
            } else if (0 == strcmp("NameOwnerChanged", msg->GetMemberName())) {
                listenersLock.Lock(MUTEX_CONTEXT);
                list<BusListener*>::iterator it = listeners.begin();
                while (it != listeners.end()) {
                    (*it++)->NameOwnerChanged(args[0].v_string.str,
                                              (0 < args[1].v_string.len) ? args[1].v_string.str : NULL,
                                              (0 < args[2].v_string.len) ? args[2].v_string.str : NULL);
                }
                listenersLock.Unlock(MUTEX_CONTEXT);
            } else if (0 == strcmp("MPSessionChanged", msg->GetMemberName())) {
                sessionListenersLock.Lock(MUTEX_CONTEXT);
                SessionId id = static_cast<SessionId>(args[0].v_uint32);
                const char* member = args[1].v_string.str;
                map<SessionId, SessionListener*>::iterator slit = sessionListeners.find(id);
                if ((slit != sessionListeners.end()) && slit->second) {
                    if (args[2].v_bool) {
                        slit->second->SessionMemberAdded(id, member);
                    } else {
                        slit->second->SessionMemberRemoved(id, member);
                    }
                }
                sessionListenersLock.Unlock(MUTEX_CONTEXT);
            } else {
                QCC_DbgPrintf(("Unrecognized signal \"%s.%s\" received", msg->GetInterface(), msg->GetMemberName()));
            }
        } else if (msg->GetType() == MESSAGE_METHOD_RET) {
            DoJoinSessionMethodCB(msg, alarmContext->second);
        }
    }
    delete alarmContext;
}

uint32_t BusAttachment::GetTimestamp()
{
    return qcc::GetTimestamp();
}

QStatus BusAttachment::SetSessionListener(SessionId id, SessionListener* listener)
{
    return busInternal->SetSessionListener(id, listener);
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

bool BusAttachment::Internal::CallAcceptListeners(SessionPort sessionPort, const char* joiner, const SessionOpts& opts)
{
    bool isAccepted = false;

    /* Call sessionPortListener */
    listenersLock.Lock(MUTEX_CONTEXT);
    map<SessionPort, SessionPortListener*>::iterator it = sessionPortListeners.find(sessionPort);
    if (it != sessionPortListeners.end()) {
        isAccepted = it->second->AcceptSessionJoiner(sessionPort, joiner, opts);
    } else {
        QCC_LogError(ER_FAIL, ("Unable to find sessionPortListener for port=%d", sessionPort));
    }
    listenersLock.Unlock(MUTEX_CONTEXT);

    return isAccepted;
}

void BusAttachment::Internal::CallJoinedListeners(SessionPort sessionPort, SessionId sessionId, const char* joiner)
{
    /* Call sessionListener */
    listenersLock.Lock(MUTEX_CONTEXT);
    map<SessionPort, SessionPortListener*>::iterator it = sessionPortListeners.find(sessionPort);
    if (it != sessionPortListeners.end()) {
        /* Add entry to sessionListeners */
        if (sessionListeners.find(sessionId) == sessionListeners.end()) {
            sessionListeners[sessionId] = NULL;
        }
        /* Notify user */
        it->second->SessionJoined(sessionPort, sessionId, joiner);
    } else {
        QCC_LogError(ER_FAIL, ("Unable to find sessionPortListener for port=%d", sessionPort));
    }
    listenersLock.Unlock(MUTEX_CONTEXT);
}

QStatus BusAttachment::Internal::SetSessionListener(SessionId id, SessionListener* listener)
{
    QStatus status = ER_BUS_NO_SESSION;
    listenersLock.Lock(MUTEX_CONTEXT);
    map<SessionId, SessionListener*>::iterator it = sessionListeners.find(id);
    if (it != sessionListeners.end()) {
        sessionListeners[id] = listener;
        status = ER_OK;
    }
    listenersLock.Unlock(MUTEX_CONTEXT);
    return status;
}


QStatus BusAttachment::GetPeerGUID(const char* name, qcc::String& guid)
{
    PeerStateTable* peerTable = busInternal->GetPeerStateTable();
    qcc::String peerName;
    if (name && *name) {
        peerName = name;
    } else {
        peerName = GetUniqueName();
    }
    if (peerTable->IsKnownPeer(peerName)) {
        guid = peerTable->GetPeerState(peerName)->GetGuid().ToString();
        return ER_OK;
    } else {
        return ER_BUS_NO_PEER_GUID;
    }
}

QStatus BusAttachment::ReloadKeyStore()
{
    return busInternal->keyStore.Reload();
}

QStatus BusAttachment::ClearKeys(const qcc::String& guid)
{
    if (!qcc::GUID128::IsGUID(guid)) {
        return ER_INVALID_GUID;
    } else {
        qcc::GUID128 g(guid);
        if (busInternal->keyStore.HasKey(g)) {
            return busInternal->keyStore.DelKey(g);
        } else {
            return ER_BUS_KEY_UNAVAILABLE;
        }
    }
}

QStatus BusAttachment::SetKeyExpiration(const qcc::String& guid, uint32_t timeout)
{
    if (timeout == 0) {
        return ClearKeys(guid);
    }
    if (!qcc::GUID128::IsGUID(guid)) {
        return ER_INVALID_GUID;
    } else {
        qcc::GUID128 g(guid);
        uint64_t millis = 1000ull * timeout;
        Timespec expiration(millis, TIME_RELATIVE);
        return busInternal->keyStore.SetKeyExpiration(g, expiration);
    }
}

QStatus BusAttachment::GetKeyExpiration(const qcc::String& guid, uint32_t& timeout)
{
    if (!qcc::GUID128::IsGUID(guid)) {
        return ER_INVALID_GUID;
    } else {
        qcc::GUID128 g(guid);
        Timespec expiration;
        QStatus status = busInternal->keyStore.GetKeyExpiration(g, expiration);
        if (status == ER_OK) {
            int64_t deltaMillis = expiration - Timespec(0, TIME_RELATIVE);
            if (deltaMillis < 0) {
                timeout = 0;
            } else if (deltaMillis > (0xFFFFFFFFll * 1000)) {
                timeout = 0xFFFFFFFF;
            } else {
                timeout = (uint32_t)((deltaMillis + 500ull) / 1000ull);
            }
        }
        return status;
    }
}

}
