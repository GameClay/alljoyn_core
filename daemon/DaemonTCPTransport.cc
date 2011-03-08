/**
 * @file
 * DaemonTCPTransport is an implementation of TCPTransportBase for daemons.
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
#include <qcc/IPAddress.h>
#include <qcc/Socket.h>
#include <qcc/SocketStream.h>
#include <qcc/String.h>
#include <qcc/StringUtil.h>

#include <alljoyn/BusAttachment.h>
#include <alljoyn/QosInfo.h>

#include "BusInternal.h"
#include "RemoteEndpoint.h"
#include "Router.h"
#include "ConfigDB.h"
#include "NameService.h"
#include "DaemonTCPTransport.h"

#define QCC_MODULE "ALLJOYN"

using namespace std;
using namespace qcc;

namespace ajn {

/*
 * An endpoint class to handle the details of authenticating a connection in a
 * way that avoids denial of service attacks.
 */
class DaemonTCPEndpoint : public RemoteEndpoint {
  public:
    enum AuthState {
        ILLEGAL = 0,
        INITIALIZED,
        AUTHENTICATING,
        FAILED,
        SUCCEEDED
    };

    DaemonTCPEndpoint(DaemonTCPTransport* transport, BusAttachment& bus, bool incoming,
                      const qcc::String connectSpec, qcc::SocketFd sock, const qcc::IPAddress& ipAddr, uint16_t port)
        :
        RemoteEndpoint(bus, incoming, connectSpec, m_stream, "tcp"),
        m_transport(transport),
        m_state(INITIALIZED),
        m_tStart(qcc::Timespec(0)),
        m_authThread(),
        m_stream(sock),
        m_ipAddr(ipAddr),
        m_port(port),
        m_wasSuddenDisconnect(!incoming) { }

    ~DaemonTCPEndpoint() { }

    void SetStartTime(qcc::Timespec tStart) { m_tStart = tStart; }
    qcc::Timespec GetStartTime(void) { return m_tStart; }
    QStatus Authenticate(void);
    void Abort(void);
    const qcc::IPAddress& GetIPAddress() { return m_ipAddr; }
    uint16_t GetPort() { return m_port; }
    bool IsFailed(void) { return m_state == FAILED; }
    bool IsSuddenDisconnect() { return m_wasSuddenDisconnect; }
    void SetSuddenDisconnect(bool val) { m_wasSuddenDisconnect = val; }

  private:
    class AuthThread : public qcc::Thread {
        qcc::ThreadReturn STDCALL Run(void* arg);

      public:
        AuthThread() : Thread("auth") { }
    };

    DaemonTCPTransport* m_transport;  /**< The server holding the connection */
    volatile AuthState m_state;       /**< The state of the endpoint authentication process */
    qcc::Timespec m_tStart;           /**< Timestamp indicating when the authentication process started */
    AuthThread m_authThread;          /**< Thread used to do blocking calls during startup */
    qcc::SocketStream m_stream;       /**< Stream used by authentication code */
    qcc::IPAddress m_ipAddr;          /**< Remote IP address. */
    uint16_t m_port;                  /**< Remote port. */
    bool m_wasSuddenDisconnect;       /**< If true, assumption is that any disconnect is unexpected due to lower level error */
};

QStatus DaemonTCPEndpoint::Authenticate(void)
{
    /*
     * Start the authentication thread.  The first parameter is the pointer to
     * the connection object and the second parameter is the listener.  The
     * listener must be NULL since nobody must care if this thread exits.  If
     * the authentication process succeeds, our thread will set up the listener
     * for the resulting RemoteEndpoint threads.
     */
    QStatus status = m_authThread.Start(this, NULL);
    if (status != ER_OK) {
        m_state = FAILED;
    }
    return status;
}

void DaemonTCPEndpoint::Abort(void)
{
    m_authThread.Stop();
    m_authThread.Join();
}

void* DaemonTCPEndpoint::AuthThread::Run(void* arg)
{
    DaemonTCPEndpoint* conn = reinterpret_cast<DaemonTCPEndpoint*>(arg);
    conn->m_state = AUTHENTICATING;

    /*
     * We're running an authentication process here and we are cooperating
     * with the main server thread.  This thread is running in an object
     * that is allocated on the heap, and the server is managing these
     * objects so we need to coordinate getting all of this cleaned up.
     *
     * There is a state variable that only we write.  The server thread only
     * reads this variable, so there are no data sharing issues.  If there is
     * an authentication failure, this thread sets that state variable to
     * FAILED and then exits.  The server holds a list of currently
     * authenticating connections and will look for FAILED connections when it
     * runs its Accept loop.  If it finds one, it will then delete the
     * connection which will cause a Join() to this thread.  Since we set FAILED
     * immediately before exiting, there will be no problem having the server
     * block waiting for the Join() to complete.  We fail authentication here
     * and let the server clean up after us, lazily.
     *
     * If we succeed in the authentication process, we set the state variable
     * to SUCEEDED and then call back into the server telling it that we are
     * up and running.  It needs to take us off of the list of authenticating
     * connections and put us on the list of running connections.  This thread
     * will quickly go away and will be replaced by the Rx- and TxThreads of
     * the running RemoteEndpoint.
     *
     * If we are running an authentication process, we are probably ultimately
     * blocked on a socket.  We expect that if the server is asked to shut
     * down, it will run through its list of authenticating connections and
     * Abort() each one.  That will cause a thread Stop() which should unblock
     * all of the reads and return an error which will eventually pop out here
     * with an authentication failure.
     *
     * Finally, if the server decides we've spent too much time here and we are
     * actually a denial of service attack, it can close us down by doing a
     * Stop which will pop out of here as an authentication failure as well.
     */
    uint8_t byte;
    size_t nbytes;

    /*
     * Eat the first byte of the stream.  This is required to be zero by the
     * DBus protocol.  It is used in the Unix socket implementation to carry
     * out-of-band capabilities, but is discarded here.  We do this here since
     * it involves a read that can block.
     */
    QStatus status = conn->m_stream.PullBytes(&byte, 1, nbytes);
    if ((status != ER_OK) || (nbytes != 1) || (byte != 0)) {
        conn->m_stream.Close();
        conn->m_state = FAILED;
        return (void*)ER_FAIL;
    }

    /* Initialized the features for this endpoint */
    conn->GetFeatures().isBusToBus = false;
    conn->GetFeatures().isBusToBus = false;
    conn->GetFeatures().handlePassing = false;

    /* Run the actual connection authentication code. */
    qcc::String authName;
    status = conn->Establish("ANONYMOUS", authName);
    if (status != ER_OK) {
        conn->m_stream.Close();
        conn->m_state = FAILED;
        QCC_LogError(status, ("Failed to authenticate TCP endpoint"));
        return (void*)status;
    }

    /* Authentication succeeded, crank up the transport. */
    conn->SetListener(conn->m_transport);
    status = conn->Start();
    if (status != ER_OK) {
        conn->m_stream.Close();
        conn->m_state = FAILED;
        QCC_LogError(status, ("Failed to start TCP endpoint"));
        return (void*)status;
    }

    /* Tell the server that the authentication succeeded and that the connection is up. */
    conn->m_state = SUCCEEDED;
    conn->m_transport->Authenticated(conn);
    return (void*)status;
}


DaemonTCPTransport::DaemonTCPTransport(BusAttachment& bus)
    : Thread("DaemonTCPTransport"), m_bus(bus), m_ns(0), m_stopping(false), m_listener(0), m_foundCallback(m_listener)
{
    /*
     * We know we are daemon code, so we'd better be running with a daemon
     * router.  This is assumed elsewhere.
     */
    assert(m_bus.GetInternal().GetRouter().IsDaemon());
}

DaemonTCPTransport::~DaemonTCPTransport()
{
    Stop();
    Join();
}

void DaemonTCPTransport::Authenticated(DaemonTCPEndpoint* conn)
{
    QCC_DbgTrace(("TCPTransport::Authenticated()"));
    m_endpointListLock.Lock();
    list<DaemonTCPEndpoint*>::iterator i = find(m_authList.begin(), m_authList.end(), conn);
    assert(i != m_authList.end() && "TCPTransportBase::Authenticated(): Can't find connection");
    m_authList.erase(i);
    m_endpointList.push_back(conn);
    m_endpointListLock.Unlock();
}

QStatus DaemonTCPTransport::Start()
{
    m_stopping = false;

    /*
     * Start up an instance of the lightweight name service and tell it what
     * GUID we think we are and whether or not to advertise over IPv4 and or
     * IPv6
     */
    m_ns = new NameService;
    qcc::String guidStr = m_bus.GetInternal().GetGlobalGUID().ToString();
    QStatus status = m_ns->Init(guidStr, true, true);
    if (status != ER_OK) {
        QCC_LogError(status, ("DaemonTCPTransport::Start(): Error starting name service"));
        return status;
    }

    /*
     * Tell the name service to call us back on our FoundCallback method when
     * we hear about a new well-known bus name.
     */
    assert(m_ns);
    m_ns->SetCallback(
        new CallbackImpl<FoundCallback, void, const qcc::String&, const qcc::String&, std::vector<qcc::String>&, uint8_t>
            (&m_foundCallback, &FoundCallback::Found));

    /*
     * Start the server accept loop through the thread base class
     */
    return Thread::Start();
}

QStatus DaemonTCPTransport::Stop(void)
{
    m_stopping = true;

    /*
     * Tell the server accept loop thread to shut down through the thead
     * base class.
     */
    QStatus status = Thread::Stop();
    if (status != ER_OK) {
        QCC_LogError(status, ("DaemonTCPTransport::Stop(): Failed to Stop() server thread"));
        return status;
    }

    m_endpointListLock.Lock();

    /*
     * Ask any running endpoints to shut down and exit their threads.
     */
    for (list<DaemonTCPEndpoint*>::iterator i = m_endpointList.begin(); i != m_endpointList.end(); ++i) {
        (*i)->Stop();
    }

    /*
     * Ask any authenticating endpoints to shut down and exit their threads.
     * Note the use of Meyers' idiom for deleting iterators while iterating
     * and be careful.  This is possible since the Abort() call does a Join
     * on the authenticator thread.  An endpoint is either on the authenticating
     * list or on the endpoint list, so we need to delete them here.
     */
    for (list<DaemonTCPEndpoint*>::iterator i = m_authList.begin(); i != m_authList.end();) {
        DaemonTCPEndpoint* ep = *i;
        m_authList.erase(i++);
        ep->Abort();
        delete ep;
    }

    m_endpointListLock.Unlock();

    return ER_OK;
}

QStatus DaemonTCPTransport::Join(void)
{
    /*
     * Wait for the server accept loop thread to exit.
     */
    QStatus status = Thread::Join();
    if (status != ER_OK) {
        QCC_LogError(status, ("DaemonTCPTransport::Join(): Failed to Join() server thread"));
        return status;
    }

    /*
     * A call to Stop() above will ask all of the endpoints to stop.  We still
     * need to wait here until all of the threads running in those endpoints
     * actually stop running.  When a remote endpoint thead exits the endpoint
     * will call back into our EndpointExit() and have itself removed from the
     * list.  We poll for the all-exited condition, yielding the CPU to let
     * the endpoint therad wake and exit.
     */
    m_endpointListLock.Lock();
    while (m_endpointList.size() > 0) {
        m_endpointListLock.Unlock();
        qcc::Sleep(50);
        m_endpointListLock.Lock();
    }

    /*
     * All authenticating  endpoints have been removed from their list and
     * were deleted in Stop() so the list must be empty.
     */
    assert(m_authList.size() == 0);

    m_endpointListLock.Unlock();

    m_stopping = false;

    return ER_OK;
}

void DaemonTCPTransport::EndpointExit(RemoteEndpoint* ep)
{
    DaemonTCPEndpoint* tep = static_cast<DaemonTCPEndpoint*>(ep);
    assert(tep);

    /*
     * This is a callback driven from the remote endpoint thread exit function.
     * Our DaemonTCPEndpoint inherits from class RemoteEndpoint and so when
     * either of the threads (transmit or receive) of one of our endpoints exits
     * for some reason, we get called back here.
     */
    QCC_DbgTrace(("DaemonTCPTransport::EndpointExit()"));

    /* Remove the dead endpoint from the live endpoint list */
    m_endpointListLock.Lock();
    list<DaemonTCPEndpoint*>::iterator i = find(m_endpointList.begin(), m_endpointList.end(), tep);
    if (i != m_endpointList.end()) {
        m_endpointList.erase(i);
    }
    m_endpointListLock.Unlock();

    /*
     * The endpoint can exit if it was asked to by us in response to a Disconnect()
     * from higher level code, or if it got an error from the underlying transport.
     * We need to notify upper level code if the disconnect is due to an event from
     * the transport.
     */
    if (m_listener && tep->IsSuddenDisconnect()) {
        m_listener->BusConnectionLost(tep->GetConnectSpec());
    }

    delete tep;
}

void* DaemonTCPTransport::Run(void* arg)
{
    /*
     * We need to find the defaults for our connection limits.  These limits
     * can be specified in the configuration database with corresponding limits
     * used for DBus.  If any of those are present, we use them, otherwise we
     * provide some hopefully reasonable defaults.
     */
    ConfigDB* config = ConfigDB::GetConfigDB();

    /*
     * tTimeout is the maximum amount of time we allow incoming connections to
     * mess about while they should be authenticating.  If they take longer
     * than this time, we feel free to disconnect them as deniers of service.
     */
    uint32_t authTimeoutConfig = config->GetLimit("auth_timeout");
    Timespec tTimeout = authTimeoutConfig ? authTimeoutConfig : ALLJOYN_AUTH_TIMEOUT_DEFAULT;

    /*
     * maxAuth is the maximum number of incoming connections that can be in
     * the process of authenticating.  If starting to authenticate a new
     * connection would mean exceeding this number, we drop the new connection.
     */
    uint32_t maxAuthConfig = config->GetLimit("max_incomplete_connections_tcp");
    uint32_t maxAuth = maxAuthConfig ? maxAuthConfig : ALLJOYN_MAX_INCOMPLETE_CONNECTIONS_TCP_DEFAULT;

    /*
     * maxConn is the maximum number of active connections possible over the
     * TCP transport.  If starting to process a new connection would mean
     * exceeding this number, we drop the new connection.
     */
    uint32_t maxConnConfig = config->GetLimit("max_completed_connections_tcp");
    uint32_t maxConn = maxConnConfig ? maxConnConfig : ALLJOYN_MAX_INCOMPLETE_CONNECTIONS_TCP_DEFAULT;

    QStatus status = ER_OK;

    while (!IsStopping()) {

        /*
         * Each time through the loop we create a set of events to wait on.
         * We need to wait on the stop event and all of the SocketFds of the
         * addresses and ports we are listening on.  If the list changes, the
         * code that does the change Alert()s this thread and we wake up and
         * re-evaluate the list of SocketFds.
         */
        m_listenFdsLock.Lock();
        vector<Event*> checkEvents, signaledEvents;
        checkEvents.push_back(&stopEvent);
        for (list<pair<qcc::String, SocketFd> >::const_iterator i = m_listenFds.begin(); i != m_listenFds.end(); ++i) {
            checkEvents.push_back(new Event(i->second, Event::IO_READ, false));
        }
        m_listenFdsLock.Unlock();

        /*
         * We have our list of events, so now wait for something to happen
         * on that list (or get alerted).
         */
        signaledEvents.clear();

        status = Event::Wait(checkEvents, signaledEvents);
        if (ER_OK != status) {
            QCC_LogError(status, ("Event::Wait failed"));
            break;
        }

        /*
         * We're back from our Wait() so something has happened.  Iterate over
         * the vector of signaled events to find out which event(s) got
         * bugged
         */
        for (vector<Event*>::iterator i = signaledEvents.begin(); i != signaledEvents.end(); ++i) {
            /*
             * Reset an Alert() (or Stop())
             */
            if (*i == &stopEvent) {
                stopEvent.ResetEvent();
                continue;
            }

            /*
             * If the current event is not the stop event, it reflects one of
             * the SocketFds we are waiting on for incoming connections.  Go
             * ahead and Accept() the new connection on the current SocketFd.
             */
            IPAddress remoteAddr;
            uint16_t remotePort;
            SocketFd newSock;

            status = Accept((*i)->GetFD(), remoteAddr, remotePort, newSock);
            if (status == ER_OK) {
                /*
                 * We have a request for a new connection.  We need to
                 * Authenticate before naively allowing, and we can't do
                 * blocking calls here, so we need to spin up a thread to
                 * handle them.  We can't allow a malicious user to cause
                 * us to spin up threads till we kill the phone, so we
                 * have a list of pending authorizations.  We also need to
                 * time out possibly malicious connection requests that will
                 * never complete, so we can timeout the least recently used
                 * request.  Finally, we need to lazily clean up any
                 * connections that have failed authentication.
                 *
                 * Does not handle rollover, but a Timespec holds uint32_t
                 * worth of seconds that derives from the startup time of the
                 * system in the Posix case or the number of seconds since
                 * jan 1, 1970 in the Windows case.  This is 136 years worth
                 * of seconds which means we're okay `till the year 2106.
                 */
                Timespec tNow;
                GetTimeNow(&tNow);

                QCC_DbgHLPrintf(("DaemonTCPTransport connect request"));

                m_endpointListLock.Lock();

                /*
                 * See if there are any connections with failed authentications
                 * on our list that we can clean up.  We do this every time
                 * through just to conserve resources (memory).  Note that if
                 * the state of the endpoint is FAILED, any thread running there
                 * will never touch the endpoint object so it is safe to delete.
                 * Note the use of Meyers' idiom to erase while iterating.
                 */
                for (list<DaemonTCPEndpoint*>::iterator j = m_authList.begin(); j != m_authList.end();) {
                    if ((*j)->IsFailed()) {
                        delete *j;
                        m_authList.erase(j++);
                        QCC_DbgHLPrintf(("DaemonTCPTransport::Run(): Scavenging failed authentication"));
                    } else {
                        ++j;
                    }
                }

                /*
                 * If the pending authentication list is still full, see if
                 * there any pending connections on the list that can be removed
                 * (timed out).  Note that we join any thread that may be
                 * working behind the scenes in the endpoint with the Abort().
                 */
                assert(m_authList.size() + m_endpointList.size() <= maxConn);
                if (m_authList.size() == maxAuth) {
                    DaemonTCPEndpoint* conn = m_authList.back();
                    if (conn->GetStartTime() + tTimeout < tNow) {
                        QCC_DbgHLPrintf(("DaemonTCPTransport::Run(): Scavenging slow authenticator"));
                        m_authList.pop_back();
                        conn->Abort();
                        delete conn;
                    }
                }

                /*
                 * We've scavenged any slots we can, so now do we have a slot
                 * available for a new connection?  If so, use it.
                 */
                if ((m_authList.size() < maxAuth) && (m_authList.size() + m_endpointList.size() < maxConn)) {
                    DaemonTCPEndpoint* conn = new DaemonTCPEndpoint(this, m_bus, true, "", newSock, remoteAddr, remotePort);
                    Timespec tNow;
                    GetTimeNow(&tNow);
                    conn->SetStartTime(tNow);
                    m_authList.push_front(conn);
                    status = conn->Authenticate();
                } else {
                    qcc::Shutdown(newSock);
                    qcc::Close(newSock);
                    status = ER_AUTH_FAIL;
                    QCC_LogError(status, ("DaemonTCPTransport::Run(): No slot for new connection"));
                }

                m_endpointListLock.Unlock();
            } else if (ER_WOULDBLOCK == status) {
                status = ER_OK;
            }

            if (ER_OK != status) {
                QCC_LogError(status, ("DaemonTCPTransport::Run(): Error accepting new connection. Ignoring..."));
            }
        }

        /*
         * We're going to loop back and create a new list of checkEvents that
         * reflect the current state, so we need to delete the checkEvents we
         * created on this iteration.
         */
        for (vector<Event*>::iterator i = checkEvents.begin(); i != checkEvents.end(); ++i) {
            if (*i != &stopEvent) {
                delete *i;
            }
        }
    }

    QCC_DbgPrintf(("DaemonTCPTransport::Run is exiting status=%s", QCC_StatusText(status)));
    return (void*) status;
}

/*
 * The default address for use in listen specs.  INADDR_ANY means to listen
 * for TCP connections on any interfaces that are currently up or any that may
 * come up in the future.
 */
static const char* ADDR_DEFAULT = "0.0.0.0";

/*
 * The default port for use in listen specs.  This port is used by the TCP
 * listener to listen for incoming connection requests.
 */
static const uint16_t PORT_DEFAULT = 9955;

QStatus DaemonTCPTransport::NormalizeListenSpec(const char* inSpec, qcc::String& outSpec, map<qcc::String, qcc::String>& argMap)
{
    /*
     * Take the string in inSpec, which must start with "tcp:" and parse it,
     * looking for comma-separated "key=value" pairs and initialize the
     * argMap with those pairs.
     */
    QStatus status = ParseArguments("tcp", inSpec, argMap);
    if (status != ER_OK) {
        return status;
    }

    map<qcc::String, qcc::String>::iterator i = argMap.find("addr");
    if (i == argMap.end()) {
        qcc::IPAddress addr(ADDR_DEFAULT);
        qcc::String addrString = addr.ToString();
        argMap["addr"] = addrString;
        outSpec = "tcp:addr=" + addrString;
    } else {
        /*
         * We have a value associated with the "addr" key.  Run it through
         * a conversion function to make sure it's a valid value.
         */
        IPAddress addr;
        status = addr.SetAddress(i->second);
        if (status == ER_OK) {
            i->second = addr.ToString();
            outSpec = "tcp:addr=" + i->second;
        } else {
            return ER_BUS_BAD_TRANSPORT_ARGS;
        }
    }

    i = argMap.find("port");
    if (i == argMap.end()) {
        qcc::String portString = U32ToString(PORT_DEFAULT);
        argMap["port"] = portString;
        outSpec += ",port=" + portString;
    } else {
        /*
         * We have a value associated with the "port" key.  Run it through
         * a conversion function to make sure it's a valid value.
         */
        uint32_t port = StringToU32(i->second);
        if (port > 0 && port <= 0xffff) {
            i->second = U32ToString(port);
            outSpec += ",port=" + i->second;
        } else {
            return ER_BUS_BAD_TRANSPORT_ARGS;
        }
    }

    return ER_OK;
}

QStatus DaemonTCPTransport::NormalizeTransportSpec(const char* inSpec, qcc::String& outSpec, map<qcc::String, qcc::String>& argMap)
{
    /*
     * Unlike a listenSpec a transportSpec (actually a connectSpec) must have
     * a specific address (INADDR_ANY isn't a valid IP address to connect to).
     */
    QStatus status = NormalizeListenSpec(inSpec, outSpec, argMap);
    if (status != ER_OK) {
        return status;
    }

    /*
     * Since the only difference between a transportSpec and a listenSpec is
     * the presence of the address, we just check for the default address
     * and fail if we find it.
     */
    map<qcc::String, qcc::String>::iterator i = argMap.find("addr");
    assert(i != argMap.end());
    if (i->second == ADDR_DEFAULT) {
        return ER_BUS_BAD_TRANSPORT_ARGS;
    }

    return ER_OK;
}

QStatus DaemonTCPTransport::Connect(const char* connectSpec, RemoteEndpoint** newep)
{
    QCC_DbgHLPrintf(("DaemonTCPTransport::Connect(): %s", connectSpec));

    QStatus status;
    bool isConnected = false;

    /*
     * Don't bother trying to create new endpoints if we're shutting down.
     */
    if (IsRunning() == false || m_stopping == true) {
        return ER_BUS_TRANSPORT_NOT_STARTED;
    }

    /*
     * Parse and normalize the connectArgs.  When connecting to the outside
     * world, there are no reasonable defaults and so the addr and port keys
     * MUST be present.
     */
    qcc::String normSpec;
    map<qcc::String, qcc::String> argMap;
    status = NormalizeTransportSpec(connectSpec, normSpec, argMap);
    if (ER_OK != status) {
        QCC_LogError(status, ("TCPTransport::Connect(): Invalid TCP connect spec \"%s\"", connectSpec));
        return status;
    }

    IPAddress ipAddr(argMap.find("addr")->second); // Guaranteed to be there.
    uint16_t port = StringToU32(argMap["port"]);   // Guaranteed to be there.

    /*
     * This is a new not previously satisfied connection request, so attempt
     * to connect to the remote TCP address and port specified in the connectSpec.
     */
    SocketFd sockFd = -1;
    status = Socket(QCC_AF_INET, QCC_SOCK_STREAM, sockFd);
    if (status == ER_OK) {

        /*
         * Got a socket, now tell TCP to connect to the remote address and port.
         */
        status = qcc::Connect(sockFd, ipAddr, port);
        if (status == ER_OK) {
            /*
             * We have a TCP connection established, but DBus (the wire protocol
             * of which we are using) requires that every connection,
             * irrespective of transport, start with a single zero byte.  This
             * is so that the Unix-domain socket transport used by DBus can pass
             * SCM_RIGHTS out-of-band when that byte is sent.
             */
            uint8_t nul = 0;
            size_t sent;

            status = Send(sockFd, &nul, 1, sent);
            if (status != ER_OK) {
                QCC_LogError(status, ("TCPTransport::Connect(): Failed to send initial NUL byte"));
            }
            isConnected = true;
        } else {
            QCC_LogError(status, ("TCPTransport::Connect(): Failed"));
        }
    } else {
        QCC_LogError(status, ("TCPTransport::Connect(): qcc::Socket() failed"));
    }

    /*
     * The underling transport mechanism is started, but we need to create a
     * TCPEndpoint object that will orchestrate the movement of data across the
     * transport.
     */
    DaemonTCPEndpoint* conn = NULL;
    if (status == ER_OK) {
        conn = new DaemonTCPEndpoint(this, m_bus, false, normSpec, sockFd, ipAddr, port);
        m_endpointListLock.Lock();
        m_endpointList.push_back(conn);
        m_endpointListLock.Unlock();

        /* Initialized the features for this endpoint */
        conn->GetFeatures().isBusToBus = true;
        conn->GetFeatures().allowRemote = m_bus.GetInternal().AllowRemoteMessages();
        conn->GetFeatures().handlePassing = false;

        qcc::String authName;
        status = conn->Establish("ANONYMOUS", authName);
        if (status == ER_OK) {
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
            QCC_LogError(status, ("DaemonTCPTransport::Connect(): Start TCPEndpoint failed"));

            m_endpointListLock.Lock();
            list<DaemonTCPEndpoint*>::iterator i = find(m_endpointList.begin(), m_endpointList.end(), conn);
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

QStatus DaemonTCPTransport::Disconnect(const char* connectSpec)
{
    QCC_DbgHLPrintf(("DaemonTCPTransport::Disconnect(): %s", connectSpec));

    /*
     * Higher level code tells us which connection is refers to by giving us the
     * same connect spec it used in the Connect() call.  We have to determine the
     * address and port in exactly the same way
     */
    qcc::String normSpec;
    map<qcc::String, qcc::String> argMap;
    QStatus status = NormalizeTransportSpec(connectSpec, normSpec, argMap);
    if (ER_OK != status) {
        QCC_LogError(status, ("DaemonTCPTransport::Disconnect(): Invalid TCP connect spec \"%s\"", connectSpec));
        return status;
    }

    IPAddress ipAddr(argMap.find("addr")->second); // Guaranteed to be there.
    uint16_t port = StringToU32(argMap["port"]);   // Guaranteed to be there.

    /*
     * Stop the remote endpoint.  Be careful here since calling Stop() on the
     * TCPEndpoint is going to cause the transmit and receive threads of the
     * underlying RemoteEndpoint to exit, which will cause our EndpointExit()
     * to be called, which will walk the list of endpoints and delete the one
     * we are stopping.  Once we poke ep->Stop(), the pointer to ep must be
     * considered dead.
     */
    status = ER_BUS_BAD_TRANSPORT_ARGS;
    m_endpointListLock.Lock();
    for (list<DaemonTCPEndpoint*>::iterator i = m_endpointList.begin(); i != m_endpointList.end(); ++i) {
        if ((*i)->GetPort() == port && (*i)->GetIPAddress() == ipAddr) {
            DaemonTCPEndpoint* ep = *i;
            ep->SetSuddenDisconnect(false);
            m_endpointListLock.Unlock();
            return ep->Stop();
        }
    }
    m_endpointListLock.Unlock();
    return status;
}

/*
 * The default interface for the name service to use.  The wildcard character
 * means to listen and transmit over all interfaces that are up and multicast
 * capable, with any IP address they happen to have.
 */
static const char* INTERFACES_DEFAULT = "*";

QStatus DaemonTCPTransport::StartListen(const char* listenSpec)
{
    /*
     * Don't bother trying to create new listeners if we're stopped or shutting
     * down.
     */
    if (IsRunning() == false || m_stopping == true) {
        return ER_BUS_TRANSPORT_NOT_STARTED;
    }

    /*
     * Normalize the listen spec.  Although this looks like a connectSpec it is
     * different in that reasonable defaults are possible.
     */
    qcc::String normSpec;
    map<qcc::String, qcc::String> argMap;
    QStatus status = NormalizeListenSpec(listenSpec, normSpec, argMap);
    if (status != ER_OK) {
        QCC_LogError(status, ("DaemonTCPTransport::StartListen(): Invalid TCP listen spec \"%s\"", listenSpec));
        return status;
    }

    QCC_DbgPrintf(("DaemonTCPTransport::StartListen(): addr = \"%s\", port = \"%s\"",
                   argMap["addr"].c_str(), argMap["port"].c_str()));

    m_listenFdsLock.Lock();

    /*
     * Check to see if the requested address and port is already being listened
     * to.  The normalized listen spec is saved to define the instance of the
     * listener.
     */
    for (list<pair<qcc::String, SocketFd> >::iterator i = m_listenFds.begin(); i != m_listenFds.end(); ++i) {
        if (i->first == normSpec) {
            m_listenFdsLock.Unlock();
            return ER_BUS_ALREADY_LISTENING;
        }
    }

    /*
     * Figure out what local address and port the listener should use.
     */
    IPAddress listenAddr;
    listenAddr.SetAddress(argMap["addr"]);
    uint16_t listenPort = StringToU32(argMap["port"]);

    /*
     * The name service is very flexible about what to advertise.  Empty
     * strings tell the name service to use IP addreses discovered from
     * addresses returned in socket receive calls.  Providing explicit IPv4
     * or IPv6 addresses trumps this and allows us to advertise one interface
     * over a name service running on another.  The name service allows
     * this, but we don't use the feature.
     *
     * N.B. This means that if we listen on a specific IP address and advertise
     * over other interfaces (which do not have that IP address assigned) by
     * providing, for example the wildcard interface, we will be advertising
     * services on addresses do not listen on.
     */
    assert(m_ns);
    m_ns->SetEndpoints("", "", listenPort);

    /*
     * Get the configuration item telling us which network interfaces we
     * should run the name service over.  The item can specify an IP address,
     * in which case the name service waits until that particular address comes
     * up and then uses the corresponding net device if it is multicast-capable.
     * The item can also specify an interface name.  In this case the name
     * service waits until it finds the interface IFF_UP and multicast capable
     * with an assigned IP address and then starts using the interface.  If the
     * configuration item contains "*" (the wildcard) it is interpreted as
     * meaning all multicast-capable interfaces.  If the configuration item is
     * empty (not assigned in the configuration database) it defaults to "*".
     */
    qcc::String interfaces = ConfigDB::GetConfigDB()->GetProperty(NameService::MODULE_NAME, NameService::INTERFACES_PROPERTY);
    if (interfaces.size() == 0) {
        interfaces = INTERFACES_DEFAULT;
    }

    while (interfaces.size()) {
        qcc::String currentInterface;
        size_t i = interfaces.find(",");
        if (i != qcc::String::npos) {
            currentInterface = interfaces.substr(0, i);
            interfaces = interfaces.substr(i + 1, interfaces.size() - i - 1);
        } else {
            currentInterface = interfaces;
            interfaces.clear();
        }

        /*
         * Be careful about just wanging the current interface string into an
         * IP address to see what it is, since SetAddress() will try to
         * interpret a string that doesn't work as an IP address as a host name.
         * This means possibly contacting a domain name server, and going out
         * to the network which may not have a DNS.  We certainly don't want
         * that, so we do a crude out-of-band check here.  We assume that an
         * IPv4 address has at least one "." in it and an IPv6 address has at
         * least one ':' in it.
         */
        i = currentInterface.find_first_of(".:");
        if (i != qcc::String::npos) {
            IPAddress currentAddress(currentInterface);
            status = m_ns->OpenInterface(currentAddress);
        } else {
            status = m_ns->OpenInterface(currentInterface);
        }

        if (status != ER_OK) {
            QCC_LogError(status, ("DaemonTCPTransport::StartListen(): OpenInterface() failed for %s", currentInterface.c_str()));
        }
    }

    /*
     * Create the TCP listener socket and set SO_REUSEADDR so we don't have
     * to wait for four minutes to relaunch the daemon if it crashes.
     *
     * XXX We should enable IPv6 listerners.
     */
    SocketFd listenFd = -1;
    status = Socket(QCC_AF_INET, QCC_SOCK_STREAM, listenFd);
    if (status != ER_OK) {
        m_listenFdsLock.Unlock();
        QCC_LogError(status, ("DaemonTCPTransport::StartListen(): Socket() failed"));
        return status;
    }

    uint32_t yes = 1;
    if (setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes)) < 0) {
        status = ER_OS_ERROR;
        m_listenFdsLock.Unlock();
        QCC_LogError(status, ("DaemonTCPTransport::StartListen(): setsockopt(SO_REUSEADDR) failed"));
        return status;
    }

    /*
     * Bind the socket to the listen address and start listening for incoming
     * connections on it.
     */
    status = Bind(listenFd, listenAddr, listenPort);
    if (status == ER_OK) {
        status = qcc::Listen(listenFd, 0);
        if (status == ER_OK) {
            QCC_DbgPrintf(("DaemonTCPTransport::StartListen(): Listening on %s:%d", argMap["addr"].c_str(), listenPort));
            m_listenFds.push_back(pair<qcc::String, SocketFd>(normSpec, listenFd));
        } else {
            QCC_LogError(status, ("DaemonTCPTransport::StartListen(): Listen failed"));
        }
    } else {
        QCC_LogError(status, ("DaemonTCPTransport::StartListen(): Failed to bind to %s:%d", listenAddr.ToString().c_str(),
                              listenPort));
    }

    m_listenFdsLock.Unlock();

    /*
     * Signal the (probably) waiting run thread so it will wake up and add this
     * new socket to its list of sockets it is waiting for connections on.
     */
    if (status == ER_OK) {
        Alert();
    }

    return status;
}

QStatus DaemonTCPTransport::StopListen(const char* listenSpec)
{
    /*
     * Normalize the listen spec.  We are going to use the name string that was
     * put together for the StartListen call to find the listener instance to
     * stop, so we need to do it exactly the same way.
     */
    qcc::String normSpec;
    map<qcc::String, qcc::String> argMap;
    QStatus status = NormalizeListenSpec(listenSpec, normSpec, argMap);
    if (status != ER_OK) {
        QCC_LogError(status, ("DaemonTCPTransport::StopListen(): Invalid TCP listen spec \"%s\"", listenSpec));
        return status;
    }

    /*
     * Find the (single) listen spec and remove it from the list of active FDs
     * used by the server accept loop (run thread).
     */
    m_listenFdsLock.Lock();
    status = ER_BUS_BAD_TRANSPORT_ARGS;
    qcc::SocketFd stopFd = -1;
    for (list<pair<qcc::String, SocketFd> >::iterator i = m_listenFds.begin(); i != m_listenFds.end(); ++i) {
        if (i->first == normSpec) {
            stopFd = i->second;
            m_listenFds.erase(i);
            status = ER_OK;
            break;
        }
    }
    m_listenFdsLock.Unlock();

    /*
     * If we took a socketFD off of the list of active FDs, we need to tear it
     * down and alert the server accept loop that the list of FDs on which it
     * is listening has changed.
     */
    if (status == ER_OK) {
        qcc::Shutdown(stopFd);
        qcc::Close(stopFd);

        Alert();
    }

    return status;
}

void DaemonTCPTransport::EnableDiscovery(const char* namePrefix)
{
    /*
     * When a bus name is advertised, the source may append a string that
     * identifies a specific instance of advertised name.  For example, one
     * might advertise something like
     *
     *   com.mycompany.myproduct.0123456789ABCDEF
     *
     * as a specific instance of the bus name,
     *
     *   com.mycompany.myproduct
     *
     * Clients of the system will want to be able to discover all specific
     * instances, so they need to do a wildcard search for bus name strings
     * that match the non-specific name, for example,
     *
     *   com.mycompany.myproduct*
     *
     * We automatically append the name service wildcard character to the end
     * of the provided string (which we call the namePrefix) before sending it
     * to the name service which forwards the request out over the net.
     */
    String starPrefix = namePrefix;
    starPrefix.append('*');

    assert(m_ns);
    QStatus status = m_ns->Locate(starPrefix);
    if (status != ER_OK) {
        QCC_LogError(status, ("Failure enable discovery for \"%s\" on TCP", namePrefix));
    }
}

QStatus DaemonTCPTransport::EnableAdvertisement(const qcc::String& advertiseName, const QosInfo& advQos)
{
    QStatus status = ER_BUS_INCOMPATIBLE_QOS;

    // TODO: This filtering is too simplistic. Need to consider WWAN and other TCP types
    if (advQos.transports & QosInfo::TRANSPORT_WLAN) {
        /*
         * Give the provided name to the name service and have it start advertising
         * the name on the network as reachable through the daemon having this
         * transport.  The name service handles periodic retransmission of the name
         * and manages the coming and going of network interfaces for us.
         */
        assert(m_ns);
        status = m_ns->Advertise(advertiseName);
        if (status != ER_OK) {
            QCC_LogError(status, ("DaemonTCPTransport::EnableAdvertisment(%s) failure", advertiseName.c_str()));
        }
    }
    return status;
}

void DaemonTCPTransport::DisableAdvertisement(const qcc::String& advertiseName, const QosInfo* advQos, bool nameListEmpty)
{
    if (!advQos || (advQos->transports & QosInfo::TRANSPORT_WLAN)) {
        /*
         * Tell the name service to stop advertising the provided name on the
         * network as reachable through the daemon having this transport.  The name
         * service sends out a no-longer-here message and stops periodic
         * retransmission of the name as a result of the Cancel() call.
         */
        assert(m_ns);
        QStatus status = m_ns->Cancel(advertiseName);
        if (status != ER_OK) {
            QCC_LogError(status, ("Failure stop advertising \"%s\" for TCP", advertiseName.c_str()));
        }
    }
}

void DaemonTCPTransport::FoundCallback::Found(const qcc::String& busAddr, const qcc::String& guid,
                                              std::vector<qcc::String>& nameList, uint8_t timer)
{
    /*
     * Whenever the name service receives a message indicating that a bus-name
     * is out on the network somewhere, it sends a message back to us via this
     * callback.  In order to avoid duplication of effort, the name service does
     * not manage a cache of names, but delegates that to the daemon having this
     * transport.  If the timer parameter is non-zero, it indicates that the
     * nameList (actually a vector of bus-name Strings) can be expected to be
     * valid for the value of timer in seconds.  If timer is zero, it means that
     * the bus names in the nameList are no longer available and should be
     * flushed out of the daemon name cache.
     *
     * The name service does not have a cache and therefore cannot time out
     * entries, but also delegates that task to the daemon.  It is expected that
     * remote daemons will send keepalive messages that the local daemon will
     * recieve, also via this callback.
     *
     * Our job here is just to pass the messages on up the stack to the daemon.
     *
     * XXX Currently this transport has no clue how to handle an advertised
     * IPv6 address so we filter them out.  We should support IPv6.
     */
    String a("addr=");
    String p(",port=");

    size_t i = busAddr.find(a);
    if (i == String::npos) {
        return;
    }
    i += a.size();

    size_t j = busAddr.find(p);
    if (j == String::npos) {
        return;
    }

    String s = busAddr.substr(i, j - i);

    IPAddress addr;
    QStatus status = addr.SetAddress(s);
    if (status != ER_OK) {
        return;
    }

    if (addr.IsIPv4() != true) {
        return;
    }

    // TODO: Qos for TCP is currenlty fixed (hardcoded). However, this may change once tcp transport
    //       can be used for both local and global (Internet-wide) connections
    QosInfo qos;
    qos.traffic = QosInfo::TRAFFIC_MESSAGES | QosInfo::TRAFFIC_RAW_RELIABLE;
    qos.proximity = QosInfo::PROXIMITY_ANY;
    qos.transports = QosInfo::TRANSPORT_WLAN;

    if (m_listener) {
        m_listener->FoundNames(busAddr, guid, qos, &nameList, timer);
    }
}

} // namespace ajn
