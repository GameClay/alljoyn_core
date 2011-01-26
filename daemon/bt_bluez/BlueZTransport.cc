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


/*
 * TODO:
 *
 * - Check if a discovered device via DeviceFound is already paired.  If so,
 *   don't bother calling CreateDevice, let BlueZ do so and let BlueZ continue
 *   to manage the device.
 *
 * - If we call CreateDevice for a discovered device, but another BlueZ device
 *   manager tool calls CreatePairedDevice, don't remove the device if it does
 *   not have AllJoyn support.  The 'Paired" property will be set if another BlueZ
 *   device manager calls CreatePairedDevice.
 *
 * - Work with BlueZ community to develop a better system to allow autonomous
 *   connections like that needed by AllJoyn.
 *   - Get SDP information without the need to call CreateDevice.
 *   - Add a method to allow BlueZ to update its UUID list for remote devices
 *     without the need to remove the device and re-add it.
 */

#include <qcc/platform.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/socket.h>
#include <unistd.h>
#include <ctype.h>

#include <map>
#include <set>
#include <vector>

#include <qcc/Environ.h>
#include <qcc/ManagedObj.h>
#include <qcc/Socket.h>
#include <qcc/SocketStream.h>
#include <qcc/String.h>
#include <qcc/StringMapKey.h>
#include <qcc/StringSource.h>
#include <qcc/StringUtil.h>
#include <qcc/Util.h>
#include <qcc/XmlElement.h>

#include <alljoyn/BusAttachment.h>
#include <alljoyn/DBusStd.h>
#include <alljoyn/InterfaceDescription.h>
#include <alljoyn/MessageReceiver.h>
#include <alljoyn/MsgArg.h>

#include "BusInternal.h"
#include "BTTransport.h"
#include "Router.h"
#include "RemoteEndpoint.h"
#include "UnixTransport.h"

#define QCC_MODULE "ALLJOYN_BT"

#define SignalHander(a) static_cast<MessageReceiver::SignalHandler>(a)
#define ReplyHander(a) static_cast<MessageReceiver::ReplyHandler>(a)


#define SOL_BLUETOOTH  274
#define SOL_L2CAP        6
#define SOL_RFCOMM      18
#define RFCOMM_CONNINFO  2
#define L2CAP_CONNINFO   2
#define L2CAP_OPTIONS    1
#define BT_SECURITY      4
#define BT_SECURITY_LOW  1
static const int RFCOMM_PROTOCOL_ID = 3;
static const int L2CAP_PROTOCOL_ID = 0;
typedef struct _BDADDR {
    unsigned char b[6];
} BDADDR;
typedef struct _RFCOMM_SOCKADDR {
    uint16_t sa_family;
    BDADDR bdaddr;
    uint8_t channel;
} RFCOMM_SOCKADDR;
typedef struct _L2CAP_SOCKADDR {
    uint16_t sa_family;
    uint16_t psm;
    BDADDR bdaddr;
    uint16_t cid;
} L2CAP_SOCKADDR;
typedef union _BT_SOCKADDR {
    L2CAP_SOCKADDR l2cap;
    RFCOMM_SOCKADDR rfcomm;
} BT_SOCKADDR;
struct l2cap_options {
    uint16_t omtu;
    uint16_t imtu;
    uint16_t flush_to;
    uint8_t mode;
};
using namespace std;
using namespace qcc;
using namespace ajn;

namespace ajn {

const static uint32_t BUS_NAME_TTL(120);                   // 2 minutes
const static uint16_t L2capDefaultMtu = (1 * 1021) + 1011; // 2 x 3DH5

#define MSGBUS_VERSION_NUM_ATTR    0x400
#define MSGBUS_PSM_ATTR            0x401
#define MSGBUS_UCD_PSM_ATTR        0x402
#define MSGBUS_BUS_NAME_ATTR       0x403
#define MSGBUS_RFCOMM_CH_ATTR      0x404
#define MSGBUS_ADVERTISEMENTS_ATTR 0x405
#define MSGBUS_BUS_UUID_ATTR       0x406

#define MAJOR_VERSION  1
#define MINOR_VERSION  0
#define BUILD_NUM      0

#define ALLJOYN_DEVICE_CLASS ((31 << 8) | (60 << 2))

static const uint32_t alljoynVersion = ((MAJOR_VERSION << 24) | (MINOR_VERSION << 16) | BUILD_NUM);
static const char alljoynUUIDBase[] =  "-1c25-481f-9dfb-59193d238280";          // Rest of orig UUID: 09d52497  -- 0->f
#define ALLJOYN_UUID_REV_SIZE (sizeof("12345678") - 1)
#define ALLJOYN_UUID_BASE_SIZE (sizeof(alljoynUUIDBase) - 1)

static const char* bzBusName = "org.bluez";
static const char* bzMgrObjPath = "/";
static const char* bzManagerIfc = "org.bluez.Manager";
static const char* bzServiceIfc = "org.bluez.Service";
static const char* bzAdapterIfc = "org.bluez.Adapter";
static const char* bzDeviceIfc = "org.bluez.Device";

static const char sdpXmlTemplate[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<record>"
    "    <attribute id=\"0x0000\">"
    "        <uint32 value=\"0x4F492354\"/>"
    "    </attribute>"
    "    <attribute id=\"0x0002\">"
    "        <uint32 value=\"0x00000001\"/>"
    "    </attribute>"
    "    <attribute id=\"0x0008\">"
    "        <uint8 value=\"0xFF\"/>"
    "    </attribute>"
    "    <attribute id=\"0x0004\">"
    "        <sequence>"
    "            <sequence>"
    "                <uuid value=\"0x0100\"/>"
    "            </sequence>"
    "        </sequence>"
    "    </attribute>"
    "    <attribute id=\"0x0005\">"
    "        <sequence>"
    "            <uuid value=\"0x00001002\"/>"
    "        </sequence>"
    "    </attribute>"
    "    <attribute id=\"0x0001\">"
    "        <sequence>"
    "            <uuid value=\"%08x%s\"/>" // AllJoyn UUID - filled in later
    "        </sequence>"
    "    </attribute>"
    "    <attribute id=\"0x0400\">"        // AllJoyn Version number
    "        <uint32 value=\"%#08x\"/>"    // filled in later
    "    </attribute>"
    "    <attribute id=\"0x0401\">"
    "        <uint32 value=\"%#08x\"/>"    // Filled in with dynamically determined PSM number
    "    </attribute>"
    "    <attribute id=\"0x0404\">"
    "        <uint32 value=\"%#08x\"/>"    // Filled in with dynamically determined RFCOMM channel number
    "    </attribute>"
    "    <attribute id=\"0x0405\">"
    "        <sequence>%s</sequence>"      // Filled in with advertised names
    "    </attribute>"
    "    <attribute id=\"0x0406\">"
    "        <text value=\"%s\"/>"         // Filled in with bus GUID
    "    </attribute>"
    "    <attribute id=\"0x0100\">"
    "        <text value=\"AllJoyn\"/>"
    "    </attribute>"
    "    <attribute id=\"0x0101\">"
    "        <text value=\"AllJoyn Distributed Message Bus\"/>"
    "    </attribute>"
    "</record>";

struct InterfaceDesc {
    AllJoynMessageType type;
    const char* name;
    const char* inputSig;
    const char* outSig;
    const char* argNames;
    uint8_t annotation;
};

struct InterfaceTable {
    const char* ifcName;
    const InterfaceDesc* desc;
    size_t tableSize;
};


static const InterfaceDesc bzManagerIfcTbl[] = {
    { MESSAGE_METHOD_CALL, "DefaultAdapter",        NULL, "o",     NULL, 0 },
    { MESSAGE_METHOD_CALL, "FindAdapter",           "s",  "o",     NULL, 0 },
    { MESSAGE_METHOD_CALL, "GetProperties",         NULL, "a{sv}", NULL, 0 },
    { MESSAGE_METHOD_CALL, "ListAdapters",          NULL, "ao",    NULL, 0 },
    { MESSAGE_SIGNAL,      "AdapterAdded",          "o",  NULL,    NULL, 0 },
    { MESSAGE_SIGNAL,      "AdapterRemoved",        "o",  NULL,    NULL, 0 },
    { MESSAGE_SIGNAL,      "DefaultAdapterChanged", "o",  NULL,    NULL, 0 },
    { MESSAGE_SIGNAL,      "PropertyChanged",       "sv", NULL,    NULL, 0 }
};

static const InterfaceDesc bzAdapterIfcTbl[] = {
    { MESSAGE_METHOD_CALL, "CancelDeviceCreation", "s",      NULL,    NULL, 0 },
    { MESSAGE_METHOD_CALL, "CreateDevice",         "s",      "o",     NULL, 0 },
    { MESSAGE_METHOD_CALL, "CreatePairedDevice",   "sos",    "o",     NULL, 0 },
    { MESSAGE_METHOD_CALL, "FindDevice",           "s",      "o",     NULL, 0 },
    { MESSAGE_METHOD_CALL, "GetProperties",        NULL,     "a{sv}", NULL, 0 },
    { MESSAGE_METHOD_CALL, "ListDevices",          NULL,     "ao",    NULL, 0 },
    { MESSAGE_METHOD_CALL, "RegisterAgent",        "os",     NULL,    NULL, 0 },
    { MESSAGE_METHOD_CALL, "ReleaseSession",       NULL,     NULL,    NULL, 0 },
    { MESSAGE_METHOD_CALL, "RemoveDevice",         "o",      NULL,    NULL, 0 },
    { MESSAGE_METHOD_CALL, "RequestSession",       NULL,     NULL,    NULL, 0 },
    { MESSAGE_METHOD_CALL, "SetProperty",          "sv",     NULL,    NULL, 0 },
    { MESSAGE_METHOD_CALL, "StartDiscovery",       NULL,     NULL,    NULL, 0 },
    { MESSAGE_METHOD_CALL, "StopDiscovery",        NULL,     NULL,    NULL, 0 },
    { MESSAGE_METHOD_CALL, "UnregisterAgent",      "o",      NULL,    NULL, 0 },
    { MESSAGE_SIGNAL,      "DeviceCreated",        "o",      NULL,    NULL, 0 },
    { MESSAGE_SIGNAL,      "DeviceDisappeared",    "s",      NULL,    NULL, 0 },
    { MESSAGE_SIGNAL,      "DeviceFound",          "sa{sv}", NULL,    NULL, 0 },
    { MESSAGE_SIGNAL,      "DeviceRemoved",        "o",      NULL,    NULL, 0 },
    { MESSAGE_SIGNAL,      "PropertyChanged",      "sv",     NULL,    NULL, 0 }
};

static const InterfaceDesc bzServiceIfcTbl[] = {
    { MESSAGE_METHOD_CALL, "AddRecord",            "s",  "u",  NULL, 0 },
    { MESSAGE_METHOD_CALL, "CancelAuthorization",  NULL, NULL, NULL, 0 },
    { MESSAGE_METHOD_CALL, "RemoveRecord",         "u",  NULL, NULL, 0 },
    { MESSAGE_METHOD_CALL, "RequestAuthorization", "su", NULL, NULL, 0 },
    { MESSAGE_METHOD_CALL, "UpdateRecord",         "us", NULL, NULL, 0 }
};

static const InterfaceDesc bzDeviceIfcTbl[] = {
    { MESSAGE_METHOD_CALL, "CancelDiscovery",     NULL, NULL,    NULL, 0 },
    { MESSAGE_METHOD_CALL, "Disconnect",          NULL, NULL,    NULL, 0 },
    { MESSAGE_METHOD_CALL, "DiscoverServices",    "s",  "a{us}", NULL, 0 },
    { MESSAGE_METHOD_CALL, "GetProperties",       NULL, "a{sv}", NULL, 0 },
    { MESSAGE_METHOD_CALL, "SetProperty",         "sv", NULL,    NULL, 0 },
    { MESSAGE_SIGNAL,      "DisconnectRequested", NULL, NULL,    NULL, 0 },
    { MESSAGE_SIGNAL,      "PropertyChanged",     "sv", NULL,    NULL, 0 }
};

static const InterfaceTable ifcTables[] = {
    { "org.bluez.Manager", bzManagerIfcTbl, ArraySize(bzManagerIfcTbl) },
    { "org.bluez.Adapter", bzAdapterIfcTbl, ArraySize(bzAdapterIfcTbl) },
    { "org.bluez.Service", bzServiceIfcTbl, ArraySize(bzServiceIfcTbl) },
    { "org.bluez.Device",  bzDeviceIfcTbl,  ArraySize(bzDeviceIfcTbl)  }
};


/*
 * Set the L2CAP mtu to something better than the BT 1.0 default value.
 */
static void ConfigL2cap(SocketFd sockFd)
{
    int ret;
    uint8_t secOpt = BT_SECURITY_LOW;
    socklen_t optLen = secOpt;
    ret = setsockopt(sockFd, SOL_BLUETOOTH, BT_SECURITY, &secOpt, optLen);
    QCC_DbgPrintf(("Setting security low: %d - %d: %s", ret, errno, strerror(errno)));

    struct l2cap_options opts;
    optLen = sizeof(opts);
    ret = getsockopt(sockFd, SOL_L2CAP, L2CAP_OPTIONS, &opts, &optLen);
    if (ret != -1) {
        opts.imtu = L2capDefaultMtu;
        opts.omtu = L2capDefaultMtu;
        setsockopt(sockFd, SOL_L2CAP, L2CAP_OPTIONS, &opts, optLen);
        if (ret == -1) {
            QCC_LogError(ER_OS_ERROR, ("Failed to set in/out MTU for L2CAP socket"));
        } else {
            QCC_DbgPrintf(("Set L2CAP mtu to %d", opts.omtu));
        }
    } else {
        QCC_LogError(ER_OS_ERROR, ("Failed to get in/out MTU for L2CAP socket"));
    }
}

class BTSocketStream : public SocketStream {
  public:
    BTSocketStream(SocketFd sock, bool isRfcommSock);
    ~BTSocketStream() { if (buffer) delete[] buffer; }
    QStatus PullBytes(void* buf, size_t reqBytes, size_t& actualBytes, uint32_t timeout = Event::WAIT_FOREVER);
    QStatus PushBytes(const void* buf, size_t numBytes, size_t& numSent);

  private:
    bool isRfcommSock;
    uint8_t* buffer;
    size_t inMtu;
    size_t outMtu;
    size_t offset;
    size_t fill;
};


BTSocketStream::BTSocketStream(SocketFd sock, bool isRfcommSock) :
    SocketStream(sock),
    isRfcommSock(isRfcommSock),
    buffer(NULL),
    offset(0),
    fill(0)
{
    if (!isRfcommSock) {
        struct l2cap_options opts;
        socklen_t optlen = sizeof(opts);
        int ret;
        ret = getsockopt(sock, SOL_L2CAP, L2CAP_OPTIONS, &opts, &optlen);
        if (ret == -1) {
            QCC_LogError(ER_OS_ERROR, ("Failed to get in/out MTU for L2CAP socket, using default of 672"));
            inMtu = 672;
            outMtu = 672;
        } else {
            inMtu = opts.imtu;
            outMtu = opts.omtu;
        }
        buffer = new uint8_t[inMtu];
    }
}


QStatus BTSocketStream::PullBytes(void* buf, size_t reqBytes, size_t& actualBytes, uint32_t timeout)
{
    if (isRfcommSock) {
        return SocketStream::PullBytes(buf, reqBytes, actualBytes, timeout);
    }
    if (!IsConnected()) {
        return ER_FAIL;
    }
    if (reqBytes == 0) {
        actualBytes = 0;
        return ER_OK;
    }

    QStatus status;
    size_t avail = fill - offset;

    if (avail > 0) {
        /* Pull from internal buffer */
        actualBytes = min(avail, reqBytes);
        memcpy(buf, &buffer[offset], actualBytes);
        offset += actualBytes;
        status = ER_OK;
    } else if (reqBytes >= inMtu) {
        /* Pull directly into user buffer */
        status = SocketStream::PullBytes(buf, reqBytes, actualBytes, timeout);
    } else {
        /* Pull into internal buffer */
        status = SocketStream::PullBytes(buffer, inMtu, avail, timeout);
        if (status == ER_OK) {
            actualBytes = min(avail, reqBytes);
            /* Partial copy from internal buffer to user buffer */
            memcpy(buf, buffer, actualBytes);
            fill = avail;
            offset = actualBytes;
        }
    }
    return status;
}


QStatus BTSocketStream::PushBytes(const void* buf, size_t numBytes, size_t& numSent)
{
    // Can only send up to outMtu number of bytes over an L2CAP socket at a time.
    return SocketStream::PushBytes(buf, isRfcommSock ? numBytes : min(numBytes, outMtu), numSent);
}


class BDAddress {
  public:
    BDAddress() : init1(0), init2(0) { }

    BDAddress(const BDAddress& other) : init1(other.init1), init2(other.init2) { }

    BDAddress(const qcc::String& addr) {
        if ((HexStringToBytes(addr, a, sizeof(a)) != sizeof(a)) &&
            (HexStringToBytes(addr, a, sizeof(a), '.') != sizeof(a)) &&
            (HexStringToBytes(addr, a, sizeof(a), ':') != sizeof(a))) {
            init1 = 0;
            init2 = 0;
        }
    }

    BDAddress(const uint8_t* addr, bool bluez = false) { this->CopyFrom(addr, bluez); }

    void CopyFrom(const uint8_t* addr, bool bluez = false) {
        if (bluez) {
            a[0] = addr[5];
            a[1] = addr[4];
            a[2] = addr[3];
            a[3] = addr[2];
            a[4] = addr[1];
            a[5] = addr[0];
        } else {
            a[0] = addr[0];
            a[1] = addr[1];
            a[2] = addr[2];
            a[3] = addr[3];
            a[4] = addr[4];
            a[5] = addr[5];
        }
    }

    void CopyTo(uint8_t* addr, bool bluez = false) const {
        if (bluez) {
            addr[0] = a[5];
            addr[1] = a[4];
            addr[2] = a[3];
            addr[3] = a[2];
            addr[4] = a[1];
            addr[5] = a[0];
        } else {
            addr[0] = a[0];
            addr[1] = a[1];
            addr[2] = a[2];
            addr[3] = a[3];
            addr[4] = a[4];
            addr[5] = a[5];
        }
    }

    qcc::String ToString(char separator = ':') const { return BytesToHexString(a, sizeof(a), true, separator); }

    QStatus FromString(const qcc::String addr) {
        uint32_t tmp1(init1);
        uint16_t tmp2(init2);
        if ((HexStringToBytes(addr, a, sizeof(a)) != sizeof(a)) &&
            (HexStringToBytes(addr, a, sizeof(a), '.') != sizeof(a)) &&
            (HexStringToBytes(addr, a, sizeof(a), ':') != sizeof(a))) {
            init1 = tmp1;
            init2 = tmp2;
            return ER_FAIL;
        }
        return ER_OK;
    }

    BDAddress& operator=(const BDAddress& other) {
        init1 = other.init1;
        init2 = other.init2;
        return *this;
    }

    bool operator==(const BDAddress& other) const {
        return (init1 == other.init1) && (init2 == other.init2);
    }

    bool operator<(const BDAddress& other) const {
        return ((a[0] < other.a[0]) ||
                ((a[0] == other.a[0]) &&
                 ((a[1] < other.a[1]) ||
                  ((a[1] == other.a[1]) &&
                   ((a[2] < other.a[2]) ||
                    ((a[2] == other.a[2]) &&
                     ((a[3] < other.a[3]) ||
                      ((a[3] == other.a[3]) &&
                       ((a[4] < other.a[4]) ||
                        ((a[4] == other.a[4]) &&
                         (a[5] < other.a[5])))))))))));
    }

    bool operator>(const BDAddress& other) const { return other < *this; }

  private:
    union {
        uint8_t a[6];
        struct {
            uint32_t init1;
            uint16_t init2;
        };
    };
};

struct sockaddr_hci {
    sa_family_t family;
    uint16_t dev;
};

static const uint8_t HciSetInquiryParams[] = {
    0x01, 0x1e, 0x0C, 0x04, 0x28, 0x00, 0x14, 0x00
};

static const uint8_t HciSetInquiryInterlaced[] = {
    0x01, 0x43, 0x0C, 0x01, 0x01
};

/*
 * Class for handling commands on an alarm thread.
 */
class AlarmContext {
  public:
    typedef enum {
        CONTINUE_DISCOVERY,
        FIND_DEVICE,
        DISABLE_DISCOVERABILITY
    } CmdType;

    AlarmContext(CmdType cmd, Message& msg) : cmd(cmd), data(new Message(msg)) { }

    AlarmContext(CmdType cmd, void* data = NULL) : cmd(cmd), data(data) { }

    CmdType cmd;
    void* data;
};

/*
 * @param deviceId    The Bluetooth device id
 * @param window      The inquiry window in milliseconds (10 .. 2560)
 * @param interval    The inquiry window in milliseconds (11 .. 2560)
 * @param interlaced  If true use interlaced inquiry.
 */
QStatus ConfigureInquiry(uint16_t deviceId, uint16_t window, uint16_t interval, bool interlaced)
{
    QStatus status = ER_OK;
    uint8_t cmd[8];
    sockaddr_hci addr;
    SocketFd hciFd;
    size_t sent;

    if ((window < 10) | (window > 2560)) {
        status = ER_BAD_ARG_2;
        QCC_LogError(status, ("Inquiry window %d must be in range 10..2560 msecs", window));
        return status;
    }
    if ((interval < 11) | (interval > 2560)) {
        status = ER_BAD_ARG_3;
        QCC_LogError(status, ("Inquiry interval %d must be in range 11..2560m msecs", window));
        return status;
    }
    if (window > interval) {
        status = ER_BAD_ARG_2;
        QCC_LogError(status, ("Inquiry window must be <= to the interval"));
        return status;
    }

    hciFd = (SocketFd)socket(AF_BLUETOOTH, QCC_SOCK_RAW, 1);
    if (!hciFd) {
        status = ER_OS_ERROR;
        QCC_LogError(status, ("Failed to create socket (errno %d)\n", errno));
        return status;
    }

    addr.family = AF_BLUETOOTH;
    addr.dev = deviceId;
    if (bind(hciFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        status = ER_OS_ERROR;
        QCC_LogError(status, ("Failed to bind to BT device id %d socket (errno %d)\n", deviceId, errno));
        goto Exit;
    }

    /*
     * Convert window and interval from millseconds to ticks.
     */
    window = window == 10 ? 0x11 : (uint16_t)(((uint32_t)window * 1000 + 313) / 625);
    interval = (uint16_t)(((uint32_t)interval * 1000 + 313) / 625);

    memcpy(cmd, HciSetInquiryParams, 4);
    cmd[4] =  (uint8_t)(interval & 0xFF);
    cmd[5] =  (uint8_t)(interval >> 8);
    cmd[6] =  (uint8_t)(window & 0xFF);
    cmd[7] =  (uint8_t)(window >> 8);

    status = Send(hciFd, cmd, sizeof(HciSetInquiryParams), sent);
    if (status != ER_OK) {
        QCC_LogError(status, ("Failed to send SetInquiryParams HCI command (errno %d)\n", errno));
        goto Exit;
    }

    memcpy(cmd, HciSetInquiryInterlaced, 4);
    cmd[4] = (uint8_t)interlaced;

    status = Send(hciFd, cmd, sizeof(HciSetInquiryInterlaced), sent);
    if (status != ER_OK) {
        QCC_LogError(status, ("Failed to send SetInquiryInterlaced HCI command (errno %d)\n", errno));
        goto Exit;
    }

Exit:

    close(hciFd);
    return status;
}

class RCProxyBusObject : public ProxyBusObject {
  public:
    RCProxyBusObject(BusAttachment& bus, const char* path) :
        ProxyBusObject(bus, bzBusName, path),
        count(1)
    { }
    void Inc() { IncrementAndFetch(&count); }
    void Dec() { if (DecrementAndFetch(&count) == 0) delete this; }
  private:
    int32_t count;
};

class AdapterObject : public RCProxyBusObject {
  public:
    bool discoverable;
    int id;

    AdapterObject(BusAttachment& bus, const char* path) :
        RCProxyBusObject(bus, path),
        discoverable(false),
        id(0)
    {
        for (size_t i = strlen(path) - 1; (i >= 0) && isdigit(path[i]); --i) {
            id *= 10;
            id += path[i] - '0';
        }

    }
};

class DeviceObject : public RCProxyBusObject, public ProxyBusObject::Listener {
  public:
    BDAddress address;
    AdapterObject* adapterObj;
    uint32_t psm;
    uint32_t channel;
    qcc::String guid;
    uint32_t uuidRev;

    DeviceObject(BusAttachment& bus, BTTransport& transport, const char* path, AdapterObject* adapterObj, const BDAddress& address, bool outgoing) :
        RCProxyBusObject(bus, path),
        address(address),
        adapterObj(adapterObj),
        psm(0),
        channel(~0),
        transport(transport),
        outgoing(outgoing)
    {
        adapterObj->Inc();
        /*
         * We stop discovery after establishing an outgoing connection. This prevents the
         * formation of complex scatternets that prevent other applications from using
         * Bluetooth.
         */
        if (outgoing) {
            transport.DisableDiscovery(NULL);
        }
    }

    ~DeviceObject()
    {
        adapterObj->Dec();
        if (outgoing) {
            transport.EnableDiscovery(NULL);
        }
    }

    static const size_t maxIncoming = 7;

  private:
    BTTransport& transport;
    bool outgoing;
};

class BTEndpoint : public RemoteEndpoint {
  public:

    /**
     * Bluetooth endpoint constructor
     */
    BTEndpoint(BusAttachment& bus, bool incoming, const qcc::String& connectSpec, SocketFd sockFd, DeviceObject* devObj, bool isRfcommSock) :
        RemoteEndpoint(bus, incoming, connectSpec, sockStream, "bz"),
        sockFd(sockFd), sockStream(sockFd, isRfcommSock), devObj(devObj)
    {
        devObj->Inc();
    }

    ~BTEndpoint() { devObj->Dec(); }

    DeviceObject* GetDeviceObject() const { devObj->Inc(); return devObj; }
    SocketFd GetSocketFd() const { return sockFd; }

  private:
    SocketFd sockFd;
    BTSocketStream sockStream;
    DeviceObject* devObj;
};

/*
 * Timeout for various operations
 */
#define BT_DEFAULT_TO      10000
#define BT_GETPROP_TO      3000
#define BT_SDPQUERY_TO     200000
#define BT_CREATE_DEV_TO   200000


class BTTransport::BTAccessor : public MessageReceiver, public ProxyBusObject::Listener, public AlarmListener {
  public:
    BTAccessor(BTTransport* transport, const qcc::String& busGuid);

    bool IsBluetoothAvailable() const { return bluetoothAvailable; }
    bool IsDiscoverable() const { return discoverable; }

    QStatus StartControlBus();
    void StopControlBus();

    QStatus ConnectBlueZ(bool startup = false);
    void DisconnectBlueZ();

    QStatus ListenBlueZ();
    void CancelListenBlueZ();

    BTEndpoint* Accept(BusAttachment& alljoyn, SocketFd listenFd, bool isRfcommSock);
    BTEndpoint* Connect(BusAttachment& alljoyn, const qcc::String& connectSpec);
    QStatus Disconnect(const BDAddress& addr);
    QStatus GetExistingDevice(const BDAddress& addr, bool in, DeviceObject*& dev);

    void FlushFoundNames(const BDAddress& addr, const qcc::String& guid)
    {
        QCC_DbgPrintf(("Flush cached names for %s", addr.ToString().c_str()));
        if (transport->listener) {
            qcc::String busAddr("bluetooth:addr=" + addr.ToString());
            transport->listener->FoundNames(busAddr, guid, NULL, 0);
        }
    }

    void DisconnectComplete(DeviceObject* dev, bool in, bool surpriseDisconnect)
    {
        qcc::String busAddr("bluetooth:addr=" + dev->address.ToString());
        /*
         * For outgoing connections only on a suprise disconnect flush the name cache.
         */
        if (surpriseDisconnect && !in) {
            FlushFoundNames(dev->address, dev->guid);
            deviceLock.Lock();
            foundDevices.erase(dev->address);
            deviceLock.Unlock();
        }
        if (transport->listener && !in) {
            transport->listener->BusConnectionLost(busAddr);
        }
    }

    void StartDiscovery(bool unpause = false);
    void StopDiscovery(bool pause = false);
    void PauseDiscovery() { StopDiscovery(true); }

    void ContinueDiscovery(uint32_t delay) {
        QCC_DbgPrintf(("Discovery will continue in %d seconds", delay));
        Alarm alarm(delay * 1000, this, 0, new AlarmContext(AlarmContext::CONTINUE_DISCOVERY));
        bzBus.GetInternal().GetTimer().AddAlarm(alarm);
    }

    bool Discovering() { return (discoverCount > 0) && !discoverPaused; }

    void StartDiscoverability() { discoverable = true; if (bluetoothAvailable) { SetDiscoverabilityProperty(); transport->Alert(); } }
    void StopDiscoverability() { discoverable = false; if (bluetoothAvailable) { SetDiscoverabilityProperty(); transport->Alert(); } }
    void DelayedStopDiscoverability() {
        Alarm alarm(BUS_NAME_TTL * 1000, this, 0, new AlarmContext(AlarmContext::DISABLE_DISCOVERABILITY));
        bzBus.GetInternal().GetTimer().AddAlarm(alarm);
    }

    void AddAdvertiseName(const qcc::String& advertiseName) { advertiseNames.insert(advertiseName); }
    void RemoveAdvertiseName(const qcc::String& advertiseName) { advertiseNames.erase(advertiseName); }

    void UpdateUUID()
    {
        QCC_DbgPrintf(("Updating UUID"));
        if (++alljoynUUIDRev == 0) {
            ++alljoynUUIDRev;
        }
        UpdateServiceRecord();
    }

    ~BTAccessor()
    {
        AdapterMap::iterator ait(adapterMap.begin());
        while (ait != adapterMap.end()) {
            AdapterObject* doomed(ait->second);
            doomed->Dec();
            ++ait;
        }
        adapterMap.clear();

        delete bzManagerObj;
    }

    /**
     * Function called when an alarm is triggered.
     */
    void AlarmTriggered(const qcc::Alarm& alarm);


    // TODO: Change listenFd to use a mechanism similar to TCPTransport.
    // Should be able to add capability to just listen on a specified dongle
    // if the addr is given and is valid.

    SocketFd l2capLFd;
    SocketFd rfcommLFd;

  private:
    typedef ManagedObj<vector<qcc::String> > AdvertisedNamesList;

    class FoundInfo {
      public:
        FoundInfo() : uuidRev(0), timestamp(0), psm(0), channel(~0), sdpInProgress(false), advertisedNames() { }
        qcc::String guid;
        uint32_t uuidRev;
        uint32_t timestamp;
        uint32_t psm;
        uint32_t channel;
        bool sdpInProgress;
        AdvertisedNamesList advertisedNames;
    };

    typedef map<StringMapKey, AdapterObject*> AdapterMap;

    struct {
        struct {
            struct {
                const InterfaceDescription* interface;
                // Methods (not all; only those needed)
                const InterfaceDescription::Member* DefaultAdapter;
                const InterfaceDescription::Member* ListAdapters;
                // Signals
                const InterfaceDescription::Member* AdapterAdded;
                const InterfaceDescription::Member* AdapterRemoved;
                const InterfaceDescription::Member* DefaultAdapterChanged;
            } Manager;

            struct {
                const InterfaceDescription* interface;
                // Methods (not all; only those needed)
                const InterfaceDescription::Member* AddRecord;
                const InterfaceDescription::Member* RemoveRecord;
            } Service;

            struct {
                const InterfaceDescription* interface;
                // Methods (not all; only those needed)
                const InterfaceDescription::Member* CreateDevice;
                const InterfaceDescription::Member* FindDevice;
                const InterfaceDescription::Member* GetProperties;
                const InterfaceDescription::Member* ListDevices;
                const InterfaceDescription::Member* RemoveDevice;
                const InterfaceDescription::Member* SetProperty;
                const InterfaceDescription::Member* StartDiscovery;
                const InterfaceDescription::Member* StopDiscovery;
                // Signals
                const InterfaceDescription::Member* DeviceCreated;
                const InterfaceDescription::Member* DeviceDisappeared;
                const InterfaceDescription::Member* DeviceFound;
                const InterfaceDescription::Member* DeviceRemoved;
                const InterfaceDescription::Member* PropertyChanged;
            } Adapter;

            struct {
                const InterfaceDescription* interface;
                // Methods (not all; only those needed)
                const InterfaceDescription::Member* DiscoverServices;
                const InterfaceDescription::Member* GetProperties;
                // Signals
                const InterfaceDescription::Member* DisconnectRequested;
                const InterfaceDescription::Member* PropertyChanged;
            } Device;
        } bluez;
    } org;


    /* Adapter management functions. */
    QStatus EnumerateAdapters();
    void AdapterAdded(const char* adapterObjPath, bool sync);
    void AdapterAddedSignalHandler(const InterfaceDescription::Member* member,
                                   const char* sourcePath,
                                   Message& msg);
    void AdapterRemoved(const char* adapterObjPath);
    void AdapterRemovedSignalHandler(const InterfaceDescription::Member* member,
                                     const char* sourcePath,
                                     Message& msg);
    void DefaultAdapterChangedSignalHandler(const InterfaceDescription::Member* member,
                                            const char* sourcePath,
                                            Message& msg);
    void NameOwnerChangedSignalHandler(const InterfaceDescription::Member* member,
                                       const char* sourcePath,
                                       Message& msg);
    void AdapterPropertyChangedSignalHandler(const InterfaceDescription::Member* member,
                                             const char* sourcePath,
                                             Message& msg);
    void CallStartDiscovery();
    void CallStopDiscovery();
    void SetDiscoverabilityProperty();
    QStatus UpdateServiceRecord();
    void AddRecordReplyHandler(Message& message, void* context);
    QStatus RegisterService();
    QStatus DeregisterService();
    void SetInquiryParameters();

    /* Device presence management functions. */
    void DeviceFoundSignalHandler(const InterfaceDescription::Member* member,
                                  const char* sourcePath,
                                  Message& msg);
    void DeviceCreatedSignalHandler(const InterfaceDescription::Member* member,
                                    const char* sourcePath,
                                    Message& msg);

    void RemoveDeviceResponse(Message& msg, void* context);
    void DeviceRemovedSignalHandler(const InterfaceDescription::Member* member,
                                    const char* sourcePath,
                                    Message& msg);
    QStatus LookupDevObjAndAdapter(const BDAddress& bdAddr,
                                   qcc::String& devObjPath,
                                   AdapterObject*& adapter);

    /* Device interaction functions. */
    void DevDisconnectRequestedSignalHandler(const InterfaceDescription::Member* member,
                                             const char* sourcePath,
                                             Message& msg);
    QStatus ProcessSDPXML(XmlParseContext& xmlctx, uint32_t& psm, uint32_t& channel, qcc::String& uuidstr, vector<qcc::String>& names, qcc::String& devBusGuid);

    void FindDevice(void* context);

    /* Helper functions */
    void NullHandler(Message& message, void* context);

    AdapterObject* GetAdapterObject(const qcc::String adapterObjPath)
    {
        AdapterObject* adapter;
        adapterLock.Lock();
        AdapterMap::iterator it(adapterMap.find(adapterObjPath));
        if (it != adapterMap.end()) {
            adapter = it->second;
            adapter->Inc();
        } else {
            adapter = NULL;
        }
        adapterLock.Unlock();
        return adapter;
    }

    AdapterObject* GetDefaultAdapterObject()
    {
        adapterLock.Lock();
        AdapterObject* adapter(defaultAdapterObj);
        if (adapter) {
            adapter->Inc();
        }
        adapterLock.Unlock();
        return adapter;
    }

    AdapterObject* GetAnyAdapterObject()
    {
        adapterLock.Lock();
        AdapterObject* adapter(anyAdapterObj);
        if (adapter) {
            adapter->Inc();
        }
        adapterLock.Unlock();
        return adapter;
    }

    static size_t FindAllJoynUUID(const AllJoynArray& list, qcc::String& uuidString);

    BusAttachment bzBus;
    const qcc::String busGuid;
    bool bluetoothAvailable;

    uint32_t alljoynUUIDRev;
    uint16_t ourPsm;
    uint8_t ourChannel;

    ProxyBusObject* bzManagerObj;
    AdapterObject* defaultAdapterObj;
    AdapterObject* anyAdapterObj;
    AdapterMap adapterMap;
    Mutex adapterLock; // Generic lock for adapter related objects, maps, etc.

    BTTransport* transport;

    uint32_t recordHandle;

    Mutex deviceLock; // Generic lock for device related objects, maps, etc.

    bool discoverable;
    int32_t discoverCount;  // reference count for tracking discover start/stop
    int32_t discoverPaused; // none zero if discovery has been paused

    map<BDAddress, FoundInfo> foundDevices;          // Map of found AllJoyn devices w/ UUID-Rev and expire time.
    set<qcc::String> advertiseNames;
};

void BTTransport::BTAccessor::AlarmTriggered(const Alarm& alarm)
{
    AlarmContext* ctx = reinterpret_cast<AlarmContext*>(alarm.GetContext());
    switch (ctx->cmd) {
    case AlarmContext::CONTINUE_DISCOVERY:
        StartDiscovery(true);
        break;

    case AlarmContext::FIND_DEVICE:
        FindDevice(ctx->data);
        break;

    case AlarmContext::DISABLE_DISCOVERABILITY:
        StopDiscoverability();
        break;
    }
    delete ctx;
}

BTTransport::BTAccessor::BTAccessor(BTTransport* transport, const qcc::String& busGuid) :
    l2capLFd(-1),
    rfcommLFd(-1),
    bzBus("BlueZTransport"),
    busGuid(busGuid),
    bluetoothAvailable(false),
    alljoynUUIDRev(0),
    ourPsm(0),        // Init to invalid PSM number
    ourChannel(0xff), // Init to invalid RFCOMM channel number
    defaultAdapterObj(NULL),
    anyAdapterObj(NULL),
    transport(transport),
    recordHandle(0),
    discoverable(false),
    discoverCount(0),
    discoverPaused(0)
{
    size_t tableIndex, member;

    // Zero is an invalid revision number
    while (alljoynUUIDRev == 0) {
        alljoynUUIDRev = qcc::Rand32();
    }

    // Must be initialized after 'bus' is initialized!
    bzManagerObj = new ProxyBusObject(bzBus, bzBusName, bzMgrObjPath);

    for (tableIndex = 0; tableIndex < ArraySize(ifcTables); ++tableIndex) {
        InterfaceDescription* ifc;
        const InterfaceTable& table(ifcTables[tableIndex]);
        bzBus.CreateInterface(table.ifcName, ifc);

        if (ifc) {
            for (member = 0; member < table.tableSize; ++member) {
                ifc->AddMember(table.desc[member].type,
                               table.desc[member].name,
                               table.desc[member].inputSig,
                               table.desc[member].outSig,
                               table.desc[member].argNames,
                               table.desc[member].annotation);
            }
            ifc->Activate();

            if (table.desc == bzManagerIfcTbl) {
                org.bluez.Manager.interface =             ifc;
                org.bluez.Manager.DefaultAdapter =        ifc->GetMember("DefaultAdapter");
                org.bluez.Manager.ListAdapters =          ifc->GetMember("ListAdapters");
                org.bluez.Manager.AdapterAdded =          ifc->GetMember("AdapterAdded");
                org.bluez.Manager.AdapterRemoved =        ifc->GetMember("AdapterRemoved");
                org.bluez.Manager.DefaultAdapterChanged = ifc->GetMember("DefaultAdapterChanged");

                bzBus.RegisterSignalHandler(this,
                                            SignalHander(&BTTransport::BTAccessor::AdapterAddedSignalHandler),
                                            org.bluez.Manager.AdapterAdded, bzMgrObjPath);

                bzBus.RegisterSignalHandler(this,
                                            SignalHander(&BTTransport::BTAccessor::AdapterRemovedSignalHandler),
                                            org.bluez.Manager.AdapterRemoved, bzMgrObjPath);

                bzBus.RegisterSignalHandler(this,
                                            SignalHander(&BTTransport::BTAccessor::DefaultAdapterChangedSignalHandler),
                                            org.bluez.Manager.DefaultAdapterChanged, bzMgrObjPath);

            } else if (table.desc == bzAdapterIfcTbl) {
                org.bluez.Adapter.interface =         ifc;
                org.bluez.Adapter.CreateDevice =      ifc->GetMember("CreateDevice");
                org.bluez.Adapter.FindDevice =        ifc->GetMember("FindDevice");
                org.bluez.Adapter.GetProperties =     ifc->GetMember("GetProperties");
                org.bluez.Adapter.ListDevices =       ifc->GetMember("ListDevices");
                org.bluez.Adapter.RemoveDevice =      ifc->GetMember("RemoveDevice");
                org.bluez.Adapter.SetProperty =       ifc->GetMember("SetProperty");
                org.bluez.Adapter.StartDiscovery =    ifc->GetMember("StartDiscovery");
                org.bluez.Adapter.StopDiscovery =     ifc->GetMember("StopDiscovery");
                org.bluez.Adapter.DeviceCreated =     ifc->GetMember("DeviceCreated");
                org.bluez.Adapter.DeviceDisappeared = ifc->GetMember("DeviceDisappeared");
                org.bluez.Adapter.DeviceFound =       ifc->GetMember("DeviceFound");
                org.bluez.Adapter.DeviceRemoved =     ifc->GetMember("DeviceRemoved");
                org.bluez.Adapter.PropertyChanged =   ifc->GetMember("PropertyChanged");

            } else if (table.desc == bzServiceIfcTbl) {
                org.bluez.Service.interface =    ifc;
                org.bluez.Service.AddRecord =    ifc->GetMember("AddRecord");
                org.bluez.Service.RemoveRecord = ifc->GetMember("RemoveRecord");

            } else {
                org.bluez.Device.interface =           ifc;
                org.bluez.Device.DiscoverServices =    ifc->GetMember("DiscoverServices");
                org.bluez.Device.GetProperties =       ifc->GetMember("GetProperties");
                org.bluez.Device.DisconnectRequested = ifc->GetMember("DisconnectRequested");
                org.bluez.Device.PropertyChanged =     ifc->GetMember("PropertyChanged");
            }
        }
    }
}


QStatus BTTransport::BTAccessor::StartControlBus()
{
    QCC_DbgTrace(("BTTransport::BTAccessor::SetupControlBus()"));

    QStatus status;

    /* Start the control bus */
    status = bzBus.Start();
    if (status == ER_OK) {
        qcc::String rules[] = {
            qcc::String("type='signal',sender='") + bzBusName + "',interface='" + bzManagerIfc + "'",
            qcc::String("type='signal',sender='") + bzBusName + "',interface='" + bzAdapterIfc + "'",
            qcc::String("type='signal',sender='") + bzBusName + "',interface='" + bzDeviceIfc  + "'",
            qcc::String("type='signal',sender='") + ajn::org::freedesktop::DBus::WellKnownName + "',interface='" + ajn::org::freedesktop::DBus::InterfaceName + "'"
        };

        MsgArg arg;
        Message reply(bzBus);
        const ProxyBusObject& dbusObj = bzBus.GetDBusProxyObj();
        const InterfaceDescription* ifc(bzBus.GetInterface(ajn::org::freedesktop::DBus::InterfaceName));
        const InterfaceDescription::Member* addMatch;
        const InterfaceDescription::Member* nameHasOwner;
        const InterfaceDescription::Member* nameOwnerChanged;

        /* Get environment variable for the system bus */
        Environ* env(Environ::GetAppEnviron());
#ifdef ANDROID
        qcc::String connectArgs(env->Find("DBUS_SYSTEM_BUS_ADDRESS",
                                          "unix:path=/dev/socket/dbus"));
#else
        qcc::String connectArgs(env->Find("DBUS_SYSTEM_BUS_ADDRESS",
                                          "unix:path=/var/run/dbus/system_bus_socket"));
#endif

        assert(ifc);
        if (!ifc) {
            status = ER_FAIL;
            QCC_LogError(status, ("Failed to get DBus interface description from AllJoyn"));
            goto exit;
        }

        addMatch = ifc->GetMember("AddMatch");
        nameHasOwner = ifc->GetMember("NameHasOwner");
        nameOwnerChanged = ifc->GetMember("NameOwnerChanged");

        /* Create the endpoint for talking to the Bluetooth subsystem */
        status = bzBus.Connect(connectArgs.c_str());
        if (status != ER_OK) {
            QCC_LogError(status, ("Failed to create UNIX endpoint"));
            goto exit;
        }

        bzBus.RegisterSignalHandler(this,
                                    SignalHander(&BTTransport::BTAccessor::NameOwnerChangedSignalHandler),
                                    nameOwnerChanged, NULL);

        /* Add Match rules */
        for (size_t i = 0; (status == ER_OK) && (i < ArraySize(rules)); ++i) {
            arg.Set("s", rules[i].c_str());
            status = dbusObj.MethodCall(*addMatch, &arg, 1, reply);
            if (status != ER_OK) {
                QCC_LogError(status, ("Failed to add match rule: \"%s\"", rules[i].c_str()));
                QCC_DbgHLPrintf(("reply msg: %s\n", reply->ToString().c_str()));
            }
        }

        // Find out if the Bluetooth subsystem is running...
        arg.Set("s", bzBusName);
        status = dbusObj.MethodCall(*nameHasOwner, &arg, 1, reply);
        if (status != ER_OK) {
            QCC_LogError(status, ("Failure calling %s.NameHasOwner", ajn::org::freedesktop::DBus::InterfaceName));
            QCC_DbgHLPrintf(("reply msg: %s\n", reply->ToString().c_str()));
            bzBus.Stop();
            bzBus.WaitStop();
        } else if (reply->GetArg(0)->v_bool) {
            status = ConnectBlueZ(true);
        }
    }

exit:
    return status;
}


void BTTransport::BTAccessor::StopControlBus()
{
    bzBus.Stop();
    bzBus.WaitStop();
}


QStatus BTTransport::BTAccessor::ListenBlueZ()
{
    QCC_DbgTrace(("BTTransport::BTAccessor::ListenBlueZ()"));

    QStatus status = ER_OK;
    L2CAP_SOCKADDR l2capAddr;
    int l2capFd = -1;
    RFCOMM_SOCKADDR rfcommAddr;
    int rfcommFd = -1;
    int ret;

    rfcommFd = socket(AF_BLUETOOTH, SOCK_STREAM, RFCOMM_PROTOCOL_ID);
    if (rfcommFd == -1) {
        status = ER_OS_ERROR;
        QCC_LogError(status, ("ListenBlueZ(): Bind socket failed (errno: %d - %s)", errno, strerror(errno)));
        goto exit;
    }
    rfcommLFd = rfcommFd;

    QCC_DbgPrintf(("BTTransport::BTAccessor::ListenBlueZ(): rfcommFd = %d", rfcommFd));

    memset(&rfcommAddr, 0, sizeof(rfcommAddr));
    rfcommAddr.sa_family = AF_BLUETOOTH;

    /* Supposedly BlueZ allows binding to channel 0 to allow reserving the
     * first available RFCOMM channel, but there's no way to know which
     * channel it reserved, so try explicitly reserving each channel number in
     * turn until an unused channel is found. */
    for (ourChannel = 1; (ourChannel < 31); ++ourChannel) {
        rfcommAddr.channel = ourChannel;
        ret = bind(rfcommFd, (struct sockaddr*)&rfcommAddr, sizeof(rfcommAddr));
        if (ret != -1) {
            break;
        }
    }
    if (ret == -1) {
        status = ER_OS_ERROR;
        QCC_LogError(status, ("ConnectBlueZ(): Failed to find an unused RFCOMM channel (bind errno: %d - %s)", errno, strerror(errno)));
        ourChannel = 0xff;
        goto exit;
    }
    QCC_DbgPrintf(("Bound RFCOMM channel: %d", ourChannel));

    ret = listen(rfcommFd, DeviceObject::maxIncoming);
    if (ret == -1) {
        status = ER_OS_ERROR;
        QCC_LogError(status, ("ListenBlueZ(): Listen socket failed (errno: %d - %s)", errno, strerror(errno)));
        goto exit;
    }


    l2capFd = socket(AF_BLUETOOTH, SOCK_SEQPACKET, L2CAP_PROTOCOL_ID);
    if (l2capFd == -1) {
        status = ER_OS_ERROR;
        QCC_LogError(status, ("ListenBlueZ(): Bind socket failed (errno: %d - %s)", errno, strerror(errno)));
        goto exit;
    }
    l2capLFd = l2capFd;

    QCC_DbgPrintf(("BTTransport::BTAccessor::ListenBlueZ(): l2capFd = %d", l2capFd));

    memset(&l2capAddr, 0, sizeof(l2capAddr));
    l2capAddr.sa_family = AF_BLUETOOTH;

    for (ourPsm = 0x1001; (ourPsm < 0x8fff); ourPsm += 2) {
        l2capAddr.psm = ourPsm;  // NOTE: this only works on little-endian
        ret = bind(l2capFd, (struct sockaddr*)&l2capAddr, sizeof(l2capAddr));
        if (ret != -1) {
            break;
        }
    }
    if (ret == -1) {
        status = ER_OS_ERROR;
        QCC_LogError(status, ("ListenBlueZ(): Failed to find an unused PSM (bind errno: %d - %s)", errno, strerror(errno)));
        ourPsm = 0;
        goto exit;
    }

    QCC_DbgPrintf(("Bound PSM: %#04x", ourPsm));
    ConfigL2cap(l2capFd);
    ret = listen(l2capFd, DeviceObject::maxIncoming);
    if (ret == -1) {
        status = ER_OS_ERROR;
        QCC_LogError(status, ("ListenBlueZ(): Listen socket failed (errno: %d - %s)", errno, strerror(errno)));
        goto exit;
    }


    UpdateServiceRecord();

exit:
    if ((status != ER_OK) && ((rfcommLFd != -1) || (l2capLFd != -1))) {
        CancelListenBlueZ();
    }
    return status;
}

void BTTransport::BTAccessor::CancelListenBlueZ()
{
    QCC_DbgTrace(("BTTransport::BTAccessor::CancelListenBlueZ()"));
    if (rfcommLFd != -1) {
        QCC_DbgPrintf(("Closing rfcommFd: %d", rfcommLFd));
        shutdown(rfcommLFd, SHUT_RDWR);
        close(rfcommLFd);
        rfcommLFd = -1;
    }
    if (l2capLFd != -1) {
        QCC_DbgPrintf(("Closing l2capFd: %d", l2capLFd));
        shutdown(l2capLFd, SHUT_RDWR);
        close(l2capLFd);
        l2capLFd = -1;
    }
    ourChannel = 0xff;
    ourPsm = 0;
}

QStatus BTTransport::BTAccessor::ConnectBlueZ(bool startup)
{
    QCC_DbgTrace(("BTTransport::BTAccessor::ConnectBlueZ()"));
    QStatus status = ER_OK;

    if (startup) {
        if (EnumerateAdapters() != ER_OK) {
            // No adapters were found, but we'll tell the upper layers
            // everything is OK so that when an adapter does become
            // available it can be used.
            goto exit;
        }

        if (RegisterService() == ER_OK) {
            bluetoothAvailable = true;
        }
    } else {
        bluetoothAvailable = true;
    }

exit:

    return status;
}


void BTTransport::BTAccessor::DisconnectBlueZ()
{
    QCC_DbgTrace(("BTTransport::BTAccessor::DisconnectBlueZ()"));

    StopDiscoverability();

    bluetoothAvailable = false;
    /*
     * If there are any servers registered, then deregister them and close the FD.
     */
    DeregisterService();
}

BTEndpoint* BTTransport::BTAccessor::Accept(BusAttachment& alljoyn, SocketFd listenFd, bool isRfcommSock)
{
    BTEndpoint* conn(NULL);
    DeviceObject* dev(NULL);
    SocketFd sockFd;
    BT_SOCKADDR remoteAddr;
    socklen_t ralen = sizeof(remoteAddr);
    BDAddress remAddr;
    QStatus status;
    int flags, ret;

    PauseDiscovery();

    sockFd = accept(listenFd, (struct sockaddr*)&remoteAddr, &ralen);
    if (sockFd == -1) {
        status = ER_OS_ERROR;
        if (1 || errno != EBADF) {
            QCC_LogError(status, ("Accept socket failed (errno: %d - %s)",
                                  errno, strerror(errno)));
        }
        goto exit;
    } else {
        QCC_DbgPrintf(("BTTransport::BTAccessor::Accept(listenFd = %d - %s): sockFd = %d", listenFd, isRfcommSock ? "RFCOMM" : "L2CAP", sockFd));
        uint8_t nul = 255;
        size_t recvd;
        status = qcc::Recv(sockFd, &nul, 1, recvd);
        if ((status != ER_OK) || (nul != 0)) {
            status = (status == ER_OK) ? ER_FAIL : status;
            QCC_LogError(status, ("Did not receive initial nul byte"));
            goto exit;
        }
    }

    QCC_DbgPrintf(("Accepted connection from: %02x:%02x:%02x:%02x:%02x:%02x",
                   isRfcommSock ? remoteAddr.rfcomm.bdaddr.b[5] : remoteAddr.l2cap.bdaddr.b[5],
                   isRfcommSock ? remoteAddr.rfcomm.bdaddr.b[4] : remoteAddr.l2cap.bdaddr.b[4],
                   isRfcommSock ? remoteAddr.rfcomm.bdaddr.b[3] : remoteAddr.l2cap.bdaddr.b[3],
                   isRfcommSock ? remoteAddr.rfcomm.bdaddr.b[2] : remoteAddr.l2cap.bdaddr.b[2],
                   isRfcommSock ? remoteAddr.rfcomm.bdaddr.b[1] : remoteAddr.l2cap.bdaddr.b[1],
                   isRfcommSock ? remoteAddr.rfcomm.bdaddr.b[0] : remoteAddr.l2cap.bdaddr.b[0]));

#if 1
    flags = fcntl(sockFd, F_GETFL);
    ret = fcntl(sockFd, F_SETFL, flags | O_NONBLOCK);
    if (ret == -1) {
        status = ER_OS_ERROR;
        QCC_LogError(status, ("Could not set L2CAP socket to non-blocking"));
    }
#endif

    if (isRfcommSock) {
        remAddr.CopyFrom(remoteAddr.rfcomm.bdaddr.b, true);
    } else {
        remAddr.CopyFrom(remoteAddr.l2cap.bdaddr.b, true);
    }

    /*
     * Look for an existing outgoing device object to reuse
     */
    status = GetExistingDevice(remAddr, true, dev);
    if (status != ER_OK) {
        QCC_LogError(status, ("Incoming connection from already connected device \"%s\"", remAddr.ToString().c_str()));
        goto exit;
    }
    if (!dev) {
        qcc::String devObjPath;
        AdapterObject* adapter(NULL);
        status = LookupDevObjAndAdapter(remAddr, devObjPath, adapter);
        if (status != ER_OK) {
            goto exit;
        }
        dev = new DeviceObject(bzBus, *transport, devObjPath.c_str(), adapter, remAddr, false);
        dev->AddInterface(*org.bluez.Device.interface);
        if (adapter) {
            adapter->Dec();
        }
    }

exit:
    if (status != ER_OK) {
        if (sockFd > 0) {
            QCC_DbgPrintf(("Closing sockFd: %d", sockFd));
            shutdown(sockFd, SHUT_RDWR);
            close(sockFd);
            sockFd = -1;
        }
    } else {
        qcc::String connectSpec("bluetooth:addr=" + remAddr.ToString());
        conn = new BTEndpoint(alljoyn, true, connectSpec, sockFd, dev, isRfcommSock);
    }

    if (dev) {
        dev->Dec();
    }

    ContinueDiscovery(1);

    return conn;
}


QStatus BTTransport::BTAccessor::GetExistingDevice(const BDAddress& addr, bool in, DeviceObject*& dev)
{
    assert(dev == NULL);
    QStatus status = ER_OK;
    transport->threadListLock.Lock();
    for (vector<BTEndpoint*>::iterator eit = transport->threadList.begin(); eit != transport->threadList.end(); ++eit) {
        DeviceObject* epDev = (*eit)->GetDeviceObject();
        if (epDev->address == addr) {
            if ((*eit)->IsIncomingConnection() == in) {
                if (dev) {
                    dev->Dec();
                }
                epDev->Dec();
                status = ER_BUS_ALREADY_CONNECTED;
                break;
            }
            assert(dev == NULL);
            dev = epDev;
        } else {
            epDev->Dec();
        }
    }
    transport->threadListLock.Unlock();
    return status;
}


#define MAX_CONNECT_ATTEMPTS  3
#define MAX_CONNECT_WAITS    30

BTEndpoint* BTTransport::BTAccessor::Connect(BusAttachment& alljoyn, const qcc::String& connectSpec)
{
    BTEndpoint* conn(NULL);
    DeviceObject* dev(NULL);
    qcc::String guid;
    map<qcc::String, qcc::String> argMap;
    qcc::String normSpec;
    BDAddress bdAddr;
    int ret;
    int flags;
    int sockFd(-1);
    uint32_t psm = 0;
    uint32_t channel = 0xff;
    BT_SOCKADDR addr;
    QStatus status = ER_OK;

    QCC_DbgTrace(("BTTransport::BTAccessor::Connect(connectSpec = \"%s\")", connectSpec.c_str()));

    /*
     * Stop discovering while we complete the connection.
     */
    PauseDiscovery();

    /* Parse connectSpec */
    status = transport->NormalizeTransportSpec(connectSpec.c_str(), normSpec, argMap);
    if (ER_OK != status) {
        QCC_LogError(status, ("parsing bluetooth arguments: \"%s\"", connectSpec.c_str()));
        goto exit;
    }

    if (argMap["addr"].empty()) {
        status = ER_BUS_BAD_TRANSPORT_ARGS;
        QCC_LogError(status, ("Address not specified."));
        goto exit;
    }

    status = bdAddr.FromString(argMap["addr"]);
    if (status != ER_OK) {
        status = ER_BUS_BAD_TRANSPORT_ARGS;
        QCC_LogError(status, ("Badly formed Bluetooth device address \"%s\"", argMap["addr"].c_str()));
        goto exit;
    } else {
        deviceLock.Lock();
        map<BDAddress, FoundInfo>::iterator it(foundDevices.find(bdAddr));
        if (it != foundDevices.end()) {
            psm = it->second.psm;
            channel = it->second.channel;
            guid = it->second.guid;
        } else {
            status = ER_BUS_CONNECT_FAILED;
        }
        deviceLock.Unlock();
    }

    if (status != ER_OK) {
        QCC_LogError(status, ("Unknown device %s", bdAddr.ToString().c_str()));
        goto exit;
    }

    /*
     * Look for an existing incoming device object to reuse
     */
    status = GetExistingDevice(bdAddr, false, dev);
    if (status != ER_OK) {
        QCC_LogError(status, ("Outgoing connection to already connected device \"%s\"", bdAddr.ToString().c_str()));
        goto exit;
    }
    if (!dev) {
        qcc::String devObjPath;
        AdapterObject* adapter = NULL;
        status = LookupDevObjAndAdapter(bdAddr, devObjPath, adapter);
        if (status != ER_OK) {
            QCC_LogError(status, ("Look up device object and adapter failed for %s", bdAddr.ToString().c_str()));
            goto exit;
        }
        dev = new DeviceObject(bzBus, *transport, devObjPath.c_str(), adapter, bdAddr, true);
        dev->AddInterface(*org.bluez.Device.interface);
        if (adapter) {
            adapter->Dec();
        }
    }
    dev->guid = guid;

    memset(&addr, 0, sizeof(addr));

    if (psm == 0) {
        addr.rfcomm.sa_family = AF_BLUETOOTH;
        addr.rfcomm.channel = channel;
        bdAddr.CopyTo(addr.rfcomm.bdaddr.b, true);
    } else {
        addr.l2cap.sa_family = AF_BLUETOOTH;
        addr.l2cap.psm = psm;  // NOTE: This only works on little-endian systems.
        bdAddr.CopyTo(addr.l2cap.bdaddr.b, true);
    }

    for (int tries = 0; tries < MAX_CONNECT_ATTEMPTS; ++tries) {
        if (psm == 0) {
            sockFd = socket(AF_BLUETOOTH, SOCK_STREAM, RFCOMM_PROTOCOL_ID);
        } else {
            sockFd = socket(AF_BLUETOOTH, SOCK_SEQPACKET, L2CAP_PROTOCOL_ID);
            if (sockFd != -1) {
                ConfigL2cap(sockFd);
            }
        }
        if (sockFd == -1) {
            status = ER_OS_ERROR;
            QCC_LogError(status, ("Create socket failed - %s (errno: %d - %s)",
                                  bdAddr.ToString().c_str(),
                                  errno, strerror(errno)));
            qcc::Sleep(200);
            continue;
        }
        QCC_DbgPrintf(("BTTransport::BTAccessor::Connect(): sockFd = %d PSM = %#04x", sockFd, psm));

        /* Attempt to connect */
        ret = connect(sockFd, (struct sockaddr*)&addr, sizeof(addr));
        if (ret == -1) {
            status = ER_BUS_CONNECT_FAILED;
            close(sockFd);
            sockFd = -1;
            if ((errno == ECONNREFUSED) || (errno == EBADFD)) {
                qcc::Sleep(200);
                continue;
            }
        } else {
            status = ER_OK;
        }
        break;
    }
    if (status != ER_OK) {
        if (psm == 0) {
            QCC_LogError(status, ("Connect to %02x:%02x:%02x:%02x:%02x:%02x (channel %d) failed (errno: %d - %s)",
                                  addr.rfcomm.bdaddr.b[5], addr.rfcomm.bdaddr.b[4], addr.rfcomm.bdaddr.b[3], addr.rfcomm.bdaddr.b[2], addr.rfcomm.bdaddr.b[1], addr.rfcomm.bdaddr.b[0], addr.rfcomm.channel,
                                  errno, strerror(errno)));
        } else {
            QCC_LogError(status, ("Connect to %02x:%02x:%02x:%02x:%02x:%02x (PSM %#04x) failed (errno: %d - %s)",
                                  addr.l2cap.bdaddr.b[5], addr.l2cap.bdaddr.b[4], addr.l2cap.bdaddr.b[3], addr.l2cap.bdaddr.b[2], addr.l2cap.bdaddr.b[1], addr.l2cap.bdaddr.b[0], addr.l2cap.psm,
                                  errno, strerror(errno)));
        }
        goto exit;
    }
    /*
     * BlueZ sockets are badly behaved. Even though the connect returned the
     * connection may not be fully up.  To code around this we poll on
     * getsockup until we get success.
     */
    for (int tries = 0; tries < MAX_CONNECT_WAITS; ++tries) {
        uint8_t opt[8];
        socklen_t optLen = sizeof(opt);
        if (psm == 0) {
            ret = getsockopt(sockFd, SOL_RFCOMM, RFCOMM_CONNINFO, opt, &optLen);
        } else {
            ret = getsockopt(sockFd, SOL_L2CAP, L2CAP_CONNINFO, opt, &optLen);
        }
        if (ret == -1) {
            if (errno == ENOTCONN) {
                qcc::Sleep(100);
            } else {
                status = ER_FAIL;
                QCC_LogError(status, ("Connection failed to come up (errno: %d - %s)", errno, strerror(errno)));
                goto exit;
            }
        } else {
            uint8_t nul = 0;
            size_t sent;
            status = qcc::Send(sockFd, &nul, 1, sent);
            if (status != ER_OK) {
                QCC_LogError(status, ("Failed to send nul byte (errno: %d - %s)", errno, strerror(errno)));
                goto exit;
            }
            if (psm == 0) {
                QCC_DbgPrintf(("BTTransport::BTAccessor::Connect() success sockFd = %d channel = %d", sockFd, channel));
            } else {
                QCC_DbgPrintf(("BTTransport::BTAccessor::Connect() success sockFd = %d psm = %#04x", sockFd, psm));
            }
            break;
        }
    }

#if 1
    flags = fcntl(sockFd, F_GETFL);
    ret = fcntl(sockFd, F_SETFL, flags | O_NONBLOCK);
    if (ret == -1) {
        status = ER_OS_ERROR;
        QCC_LogError(status, ("Could not set socket to non-blocking"));
        goto exit;
    }
#endif

exit:

    if (status != ER_OK) {
        if (sockFd > 0) {
            QCC_DbgPrintf(("Closing sockFd: %d", sockFd));
            shutdown(sockFd, SHUT_RDWR);
            close(sockFd);
            sockFd = -1;
        }
        /*
         * Treat a failed connect the same way we treat a surprise disconnect.
         */
        deviceLock.Lock();
        foundDevices.erase(bdAddr);
        deviceLock.Unlock();
        if (dev) {
            FlushFoundNames(dev->address, dev->guid);
        }
    } else {
        conn = new BTEndpoint(alljoyn, false, connectSpec, sockFd, dev, psm == 0);
    }


    if (dev) {
        dev->Dec();
    }

    ContinueDiscovery(1);

    return conn;
}


QStatus BTTransport::BTAccessor::Disconnect(const BDAddress& addr)
{
    QCC_DbgTrace(("BTTransport::BTAccessor::Disconnect(addr = \"%s\")", addr.ToString().c_str()));
    QStatus status(ER_BUS_BAD_TRANSPORT_ARGS);

    BTEndpoint* ep(NULL);
    vector<BTEndpoint*>::iterator eit;

    transport->threadListLock.Lock();
    for (eit = transport->threadList.begin(); eit != transport->threadList.end(); ++eit) {
        if (!(*eit)->IsIncomingConnection()) {
            DeviceObject* dev = (*eit)->GetDeviceObject();
            if (addr == dev->address) {
                ep = *eit;
            }
            dev->Dec();
            if (ep) {
                status = ep->Stop();
                break;
            }
        }
    }
    transport->threadListLock.Unlock();
    return status;
}


void BTTransport::BTAccessor::StartDiscovery(bool unpause)
{
    QCC_DbgTrace(("BTTransport::BTAccessor::StartDiscovery(%s) discoverCount=%d", unpause ? "unpause" : "", discoverCount));
    bool start;

    deviceLock.Lock();
    if (unpause) {
        assert(discoverPaused > 0);
        --discoverPaused;
        start = (discoverCount > 0) && !discoverPaused;
    } else {
        start = (++discoverCount == 1) && !discoverPaused;
    }

    if (start) {
        uint32_t now(GetTimestamp());
        // Clean out old found devices since upper layers will have forgotten about them anyway.
        map<BDAddress, FoundInfo>::iterator it(foundDevices.begin());
        while (it != foundDevices.end()) {
            if ((now - it->second.timestamp) > (BUS_NAME_TTL * 1000)) {
                foundDevices.erase(it++);
            } else {
                ++it;
            }
        }
        CallStartDiscovery();
    }
    deviceLock.Unlock();
}


void BTTransport::BTAccessor::StopDiscovery(bool pause)
{
    QCC_DbgTrace(("BTTransport::BTAccessor::StopDiscovery(%s)", pause ? "pause" : ""));
    bool stop = false;

    deviceLock.Lock();
    if (pause) {
        stop = discoverCount > 0 && !discoverPaused;
        ++discoverPaused;
    } else {
        assert(discoverCount > 0);
        stop = (--discoverCount == 0) && !discoverPaused;
    }

    if (stop) {
        CallStopDiscovery();
    }
    deviceLock.Unlock();
}


QStatus BTTransport::BTAccessor::LookupDevObjAndAdapter(const BDAddress& bdAddr,
                                                        qcc::String& devObjPath,
                                                        AdapterObject*& adapter)
{
    QCC_DbgTrace(("BTTransport::BTAccessor::LookupDevObjAndAdapter(bdAddr = %s)", bdAddr.ToString().c_str()));
    QStatus status(ER_NONE);
    Message rsp(bzBus);
    qcc::String bdAddrStr(bdAddr.ToString());
    MsgArg arg("s", bdAddrStr.c_str());
    const MsgArg* rspArg;
    AdapterMap::iterator ait;
    list<AdapterObject*> adapterList;

    // Need information from the adapter's properties
    adapterLock.Lock();
    for (ait = adapterMap.begin(); ait != adapterMap.end(); ++ait) {
        adapterList.push_back(ait->second);
        ait->second->Inc();
    }
    adapterLock.Unlock();

    for (list<AdapterObject*>::const_iterator it = adapterList.begin(); it != adapterList.end(); ++it) {
        if (status != ER_OK) {
            status = (*it)->MethodCall(*org.bluez.Adapter.FindDevice, &arg, 1, rsp, BT_DEFAULT_TO);
            if (status == ER_OK) {
                adapter = (*it);
                adapter->Inc();
            } else {
#ifndef NDEBUG
                qcc::String errMsg;
                const char* errName = rsp->GetErrorName(&errMsg);
                QCC_DbgHLPrintf(("LookupDevObjAndAdapter(): FindDevice method call: %s - %s", errName, errMsg.c_str()));
#endif
            }
        }
        (*it)->Dec();
    }

    if (status != ER_OK) {
        // Not found on adapter, so create it on default adapter.
        adapter = GetDefaultAdapterObject();

        if (adapter) {
            status = adapter->MethodCall(*org.bluez.Adapter.CreateDevice, &arg, 1, rsp, BT_CREATE_DEV_TO);
            // Don't decrement the adapter because it is being given back to the caller.
            if (status != ER_OK) {
#ifndef NDEBUG
                qcc::String errMsg;
                const char* errName = rsp->GetErrorName(&errMsg);
                QCC_DbgHLPrintf(("LookupDevObjAndAdapter(): CreateDevice method call: %s - %s", errName, errMsg.c_str()));
#endif
            }
        } else {
            status = ER_FAIL;
        }
    }

    if (status == ER_OK) {
        rspArg = rsp->GetArg(0);
        devObjPath = qcc::String(rspArg->v_string.str, rspArg->v_string.len);
    }


    return status;
}


QStatus BTTransport::BTAccessor::EnumerateAdapters()
{
    QCC_DbgTrace(("BTTransport::BTAccessor::EnumerateAdapters()"));
    QStatus status;
    Message rsp(bzBus);

    status = bzManagerObj->MethodCall(*org.bluez.Manager.ListAdapters, NULL, 0, rsp, BT_DEFAULT_TO);
    if (status == ER_OK) {
        const MsgArg* rspArg(rsp->GetArg(0));
        size_t arraySize(rspArg->v_array.GetNumElements());
        const MsgArg* arrayEntries(rspArg->v_array.GetElements());
        for (size_t i = 0; i < arraySize; ++i) {
            AdapterAdded(arrayEntries[i].v_objPath.str, true);
        }
    } else {
        QCC_LogError(status, ("EnumerateAdapters(): 'ListAdapters' method call failed"));
    }

    status = bzManagerObj->MethodCall(*org.bluez.Manager.DefaultAdapter, NULL, 0, rsp, BT_DEFAULT_TO);
    if (status == ER_OK) {
        const MsgArg* rspArg(rsp->GetArg(0));
        assert(rspArg);
        qcc::String defaultAdapterObjPath(rspArg->v_string.str, rspArg->v_string.len);
        size_t pos(defaultAdapterObjPath.find_last_of('/'));
        if (pos != qcc::String::npos) {
            defaultAdapterObj = GetAdapterObject(defaultAdapterObjPath);
            if (!defaultAdapterObj) {
                status = ER_FAIL;
            } else {
                qcc::String anyAdapterObjPath(defaultAdapterObjPath.substr(0, pos + 1) + "any");
                anyAdapterObj = new AdapterObject(bzBus, anyAdapterObjPath.c_str());
                adapterLock.Lock();
                anyAdapterObj->AddInterface(*org.bluez.Service.interface);
                adapterLock.Unlock();
            }
        } else {
            QCC_DbgHLPrintf(("Invalid object path: \"%s\"", rspArg->v_string.str));
            status = ER_FAIL;
        }
    } else {
        QCC_DbgHLPrintf(("Finding default adapter path failed, most likely no bluetooth device connected (status = %s)",
                         QCC_StatusText(status)));
    }

    return status;
}


void BTTransport::BTAccessor::AdapterAdded(const char* adapterObjPath, bool sync = false)
{
    QCC_DbgTrace(("BTTransport::BTAccessor::AdapterAdded(adapterObjPath = \"%s\")", adapterObjPath));

    if (GetAdapterObject(adapterObjPath)) {
        QCC_LogError(ER_FAIL, ("Adapter %s already exists", adapterObjPath));
        return;
    }

    AdapterObject* newAdapterObj(new AdapterObject(bzBus, adapterObjPath));

    if (newAdapterObj->GetInterface(bzServiceIfc) == NULL) {
        newAdapterObj->AddInterface(*org.bluez.Service.interface);
    }

    adapterLock.Lock();
    adapterMap[newAdapterObj->GetPath()] = newAdapterObj;

    bzBus.RegisterSignalHandler(this,
                                SignalHander(&BTTransport::BTAccessor::DeviceFoundSignalHandler),
                                org.bluez.Adapter.DeviceFound, adapterObjPath);

    bzBus.RegisterSignalHandler(this,
                                SignalHander(&BTTransport::BTAccessor::DeviceCreatedSignalHandler),
                                org.bluez.Adapter.DeviceCreated, adapterObjPath);

    bzBus.RegisterSignalHandler(this,
                                SignalHander(&BTTransport::BTAccessor::DeviceRemovedSignalHandler),
                                org.bluez.Adapter.DeviceRemoved, adapterObjPath);

    bzBus.RegisterSignalHandler(this,
                                SignalHander(&BTTransport::BTAccessor::AdapterPropertyChangedSignalHandler),
                                org.bluez.Adapter.PropertyChanged, adapterObjPath);

    adapterLock.Unlock();
}


void BTTransport::BTAccessor::AdapterAddedSignalHandler(const InterfaceDescription::Member* member,
                                                        const char* sourcePath,
                                                        Message& msg)
{
    QCC_DbgTrace(("BTTransport::BTAccessor::AdapterAddedSignalHandler - signal from \"%s\"",
                  sourcePath));
    AdapterAdded(msg->GetArg(0)->v_objPath.str);
}


void BTTransport::BTAccessor::AdapterRemoved(const char* adapterObjPath)
{
    QCC_DbgTrace(("BTTransport::BTAccessor::AdapterRemoved(adapterObjPath = \"%s\")", adapterObjPath));

    bzBus.UnRegisterSignalHandler(this,
                                  SignalHander(&BTTransport::BTAccessor::DeviceFoundSignalHandler),
                                  org.bluez.Adapter.DeviceFound, adapterObjPath);

    bzBus.UnRegisterSignalHandler(this,
                                  SignalHander(&BTTransport::BTAccessor::DeviceCreatedSignalHandler),
                                  org.bluez.Adapter.DeviceCreated, adapterObjPath);

    bzBus.UnRegisterSignalHandler(this,
                                  SignalHander(&BTTransport::BTAccessor::DeviceRemovedSignalHandler),
                                  org.bluez.Adapter.DeviceRemoved, adapterObjPath);

    bzBus.UnRegisterSignalHandler(this,
                                  SignalHander(&BTTransport::BTAccessor::AdapterPropertyChangedSignalHandler),
                                  org.bluez.Adapter.PropertyChanged, adapterObjPath);

    adapterLock.Lock();
    AdapterMap::iterator ait(adapterMap.find(adapterObjPath));
    if (ait != adapterMap.end()) {
        AdapterObject* doomed(ait->second);
        adapterMap.erase(ait);
        doomed->Dec();
    }
    adapterLock.Unlock();
}


void BTTransport::BTAccessor::AdapterRemovedSignalHandler(const InterfaceDescription::Member* member,
                                                          const char* sourcePath,
                                                          Message& msg)
{
    QCC_DbgTrace(("BTTransport::BTAccessor::AdapterRemovedSignalHandler - signal from \"%s\"", sourcePath));
    AdapterRemoved(msg->GetArg(0)->v_objPath.str);
}


void BTTransport::BTAccessor::DefaultAdapterChangedSignalHandler(const InterfaceDescription::Member* member,
                                                                 const char* sourcePath,
                                                                 Message& msg)
{
    QCC_DbgTrace(("BTTransport::BTAccessor::DefaultAdapterChangedSignalHandler - signal from \"%s\"", sourcePath));

    AdapterObject* newAdapter(NULL);

    /*
     * Temporarily pause discovery while we switch adapters.
     */
    PauseDiscovery();

    adapterLock.Lock();
    AdapterMap::const_iterator it(adapterMap.find(msg->GetArg(0)->v_objPath.str));
    assert(it != adapterMap.end());
    if (it != adapterMap.end()) {
        newAdapter = it->second;
        newAdapter->Inc();
    }
    if (defaultAdapterObj) {
        defaultAdapterObj->Dec();
    }
    defaultAdapterObj = newAdapter;
    adapterLock.Unlock();

    // Need to either create the any adapter object if bluetoothd was started
    // (or the BT HW was powered on) after we started, or bluetoothd was
    // restarted which will result in all the object paths changing.
    qcc::String defaultAdapterObjPath(defaultAdapterObj->GetPath());
    size_t pos(defaultAdapterObjPath.find_last_of('/'));
    qcc::String anyAdapterObjPath(defaultAdapterObjPath.substr(0, pos + 1) + "any");
    AdapterObject* anyAdapter(GetAnyAdapterObject());

    if (!anyAdapter || anyAdapter->GetPath() != anyAdapterObjPath) {
        QCC_DbgPrintf(("Creating \"any\" adapter object"));
        adapterLock.Lock();
        if (anyAdapterObj) {
            anyAdapterObj->Dec();
        }

        anyAdapterObj = new AdapterObject(bzBus, anyAdapterObjPath.c_str());
        anyAdapterObj->AddInterface(*org.bluez.Service.interface);
        adapterLock.Unlock();

        recordHandle = 0;  // just in case

        /*
         * Alert the listen thread that something has changed.
         */
        transport->Alert();

        if (RegisterService() == ER_OK) {
            bluetoothAvailable = true;
            SetDiscoverabilityProperty();
        }
    }

    /*
     * Re-enable discovery on the new default adapter.
     */
    ContinueDiscovery(1);

    if (anyAdapter) {
        anyAdapter->Dec();
    }
}


void BTTransport::BTAccessor::NameOwnerChangedSignalHandler(const InterfaceDescription::Member* member,
                                                            const char* sourcePath,
                                                            Message& msg)
{
    QCC_DbgTrace(("BTTransport::BTAccessor::NameOwnerChangedSignalHandler - %s", msg->GetArg(0)->v_string.str));

    // Only care about changes to org.bluez
    if (strcmp(msg->GetArg(0)->v_string.str, bzBusName) == 0) {

        if (msg->GetArg(1)->v_string.len > 0) {
            // Clean up from old owner

            /* Stop any endpoints that are running */
            vector<BTEndpoint*>::iterator eit;
            transport->threadListLock.Lock();
            for (eit = transport->threadList.begin(); eit != transport->threadList.end(); ++eit) {
                (*eit)->Stop();
            }
            transport->threadListLock.Unlock();

            deviceLock.Lock();
            if (defaultAdapterObj && (discoverCount > 0)) {
                CallStopDiscovery();
            }
            deviceLock.Unlock();

            adapterLock.Lock();
            while (!adapterMap.empty()) {
                AdapterMap::iterator it(adapterMap.begin());
                AdapterObject* doomed(it->second);
                adapterMap.erase(it);
                doomed->Dec();
            }
            if (defaultAdapterObj) {
                defaultAdapterObj->Dec();
            }
            defaultAdapterObj = NULL;
            if (anyAdapterObj) {
                anyAdapterObj->Dec();
            }
            anyAdapterObj = NULL;
            adapterLock.Unlock();

            DisconnectBlueZ();
        }

        if (msg->GetArg(2)->v_string.len > 0) {
            // org.bluez either just started or changed owners.
            ConnectBlueZ();

            // No need to enumerate adapters first.  We'll get AdapterAdded
            // and DefaultAdapterChanged signals.  The DefaultAdapterChanged
            // signal handler will register the service for us.
        }
    }
}


void BTTransport::BTAccessor::AdapterPropertyChangedSignalHandler(const InterfaceDescription::Member* member,
                                                                  const char* sourcePath,
                                                                  Message& msg)
{
    // QCC_DbgTrace(("BTTransport::BTAccessor::AdapterPropertyChangedSignalHandler"));

    const AllJoynString& property(msg->GetArg(0)->v_string);
    const MsgArg& value(*msg->GetArg(1)->v_variant.val);
    AdapterObject* adapter(GetAdapterObject(sourcePath));

    // QCC_DbgPrintf(("New value for property on %s: %s = %s", sourcePath, property.str, value.ToString().c_str()));

    if (adapter) {
        if (strcmp(property.str, "Discoverable") == 0) {
            bool disc = value.v_bool;
            adapter->discoverable = disc;

            if (!disc && discoverable) {
                // Adapter just became UNdiscoverable when it should still be discoverable.
                MsgArg discVal("b", true);
                MsgArg dargs[2];

                dargs[0].Set("s", "Discoverable");
                dargs[1].Set("v", &discVal);

                adapter->MethodCallAsync(*org.bluez.Adapter.SetProperty,
                                         this,
                                         ReplyHandler(&BTTransport::BTAccessor::NullHandler),
                                         dargs, ArraySize(dargs),
                                         NULL, BT_DEFAULT_TO);
            }
        }
        adapter->Dec();
    }
}


void BTTransport::BTAccessor::CallStartDiscovery()
{
    AdapterObject* adapter(GetDefaultAdapterObject());
    if (adapter) {
        QStatus status = adapter->MethodCallAsync(*org.bluez.Adapter.StartDiscovery,
                                                  this,
                                                  ReplyHandler(&BTTransport::BTAccessor::NullHandler),
                                                  NULL, 0,
                                                  NULL, BT_DEFAULT_TO);
        if (status == ER_OK) {
            QCC_DbgPrintf(("Started discovery"));
        } else {
            QCC_LogError(status, ("Call to org.bluez.Adapter.StartDiscovery failed"));
        }
        adapter->Dec();
    }
}


void BTTransport::BTAccessor::CallStopDiscovery()
{
    AdapterObject* adapter(GetDefaultAdapterObject());
    if (adapter) {
        QStatus status = adapter->MethodCallAsync(*org.bluez.Adapter.StopDiscovery,
                                                  this,
                                                  ReplyHandler(&BTTransport::BTAccessor::NullHandler),
                                                  NULL, 0,
                                                  NULL, BT_DEFAULT_TO);
        if (status == ER_OK) {
            QCC_DbgPrintf(("Stopped discovery"));
        } else {
            QCC_LogError(status, ("Called org.bluez.Adapter.StopDiscovery"));
        }
        adapter->Dec();
    }
}


void BTTransport::BTAccessor::SetDiscoverabilityProperty()
{
    QCC_DbgTrace(("BTTransport::BTAccessor::SetDiscoverability(%s)", discoverable ? "true" : "false"));
    QStatus status(ER_OK);
    AdapterMap::iterator adapterIt;
    list<AdapterObject*> adapterList;
    MsgArg discVal("b", discoverable);
    MsgArg dargs[2];

    dargs[0].Set("s", "Discoverable");
    dargs[1].Set("v", &discVal);

    // Only set discoverability for those adapters that are not already set
    // accordingly.  Also, not a good idea to call a method while iterating
    // through the list of adapters since it could change during the time it
    // takes to call the method and holding the lock for that long could be
    // problematic.
    adapterLock.Lock();
    for (adapterIt = adapterMap.begin(); adapterIt != adapterMap.end(); ++adapterIt) {
        if (adapterIt->second->discoverable != discoverable) {
            adapterList.push_back(adapterIt->second);
            adapterIt->second->Inc();
        }
    }
    adapterLock.Unlock();

    for (list<AdapterObject*>::const_iterator it = adapterList.begin(); it != adapterList.end(); ++it) {
        Message reply(bzBus);

        status = (*it)->MethodCallAsync(*org.bluez.Adapter.SetProperty,
                                        this,
                                        ReplyHandler(&BTTransport::BTAccessor::NullHandler),
                                        dargs, ArraySize(dargs),
                                        NULL, BT_DEFAULT_TO);
        if (status != ER_OK) {
            QCC_LogError(status, ("Failed to set 'Discoverable' %s on %s", discoverable ? "true" : "false", (*it)->GetPath().c_str()));
        }
        (*it)->Dec();
    }
}


QStatus BTTransport::BTAccessor::UpdateServiceRecord()
{
    QStatus status = ER_OK;
    QCC_DbgTrace(("BTTransport::BTAccessor::UpdateServiceRecord()"));

    set<qcc::String>::const_iterator it;
    qcc::String nameList;
    for (it = advertiseNames.begin(); it != advertiseNames.end(); ++it) {
        nameList += "<text value=\"" + *it + "\"/>";
    }

    const int sdpXmlSize = (sizeof(sdpXmlTemplate) + ALLJOYN_UUID_BASE_SIZE + sizeof("12") + (2 * sizeof("0x12345678")) + nameList.size() + busGuid.size());
    char* sdpXml(new char[sdpXmlSize]);

    // if (snprintf(sdpXml, sdpXmlSize, sdpXmlTemplate, alljoynUUIDRev, alljoynUUIDBase, alljoynVersion, ourPsm, ourChannel, nameList.c_str(), busGuid.c_str()) > sdpXmlSize) {
    if (snprintf(sdpXml, sdpXmlSize, sdpXmlTemplate, alljoynUUIDRev, alljoynUUIDBase, alljoynVersion, 0, ourChannel, nameList.c_str(), busGuid.c_str()) > sdpXmlSize) {
        status = ER_OUT_OF_MEMORY;
        QCC_LogError(status, ("UpdateServiceRecord(): Allocated SDP XML buffer too small (BUG - this should never happen)"));
        assert("SDP XML buffer too small" == NULL);
    } else {
        AdapterObject* adapter(GetAnyAdapterObject());
        if (adapter) {
            MsgArg arg;

            if (recordHandle) {
                QCC_DbgPrintf(("Removing record handle %x", recordHandle));
                arg.Set("u", recordHandle);

                status = adapter->MethodCallAsync(*org.bluez.Service.RemoveRecord,
                                                  this,
                                                  ReplyHandler(&BTTransport::BTAccessor::NullHandler),
                                                  &arg, 1, NULL, BT_DEFAULT_TO);
                if (status == ER_OK) {
                    recordHandle = 0;
                } else {
                    QCC_LogError(status, ("UpdateServiceRecord(): RemoveRecord method call failed"));
                }
            }

            if (status == ER_OK) {
                arg.Set("s", sdpXml);

                QCC_DbgPrintf(("Adding Record: UUID = %08x%s", alljoynUUIDRev, alljoynUUIDBase));
                status = adapter->MethodCallAsync(*org.bluez.Service.AddRecord,
                                                  this,
                                                  ReplyHandler(&BTTransport::BTAccessor::AddRecordReplyHandler),
                                                  &arg, 1, NULL, BT_DEFAULT_TO);
                if (status != ER_OK) {
                    QCC_LogError(status, ("UpdateServiceRecord(): AddRecord method call failed"));
                }
            }
            adapter->Dec();
        }
    }
    delete [] sdpXml;
    return status;
}


void BTTransport::BTAccessor::AddRecordReplyHandler(Message& message, void* context)
{
    if (message->GetType() == MESSAGE_ERROR) {
        qcc::String errMsg;
        const char* errName = message->GetErrorName(&errMsg);
        QCC_LogError(ER_FAIL, ("UpdateServiceRecord(): AddRecord method call: %s - %s", errName, errMsg.c_str()));
    } else {
        uint32_t newHandle(message->GetArg(0)->v_uint32);
        if (recordHandle == 0) {
            recordHandle = newHandle;
        } else if (recordHandle != newHandle) {
            QCC_DbgPrintf(("Removing extraneous AllJoyn service record (%x).", recordHandle));

            AdapterObject* adapter(GetAnyAdapterObject());
            if (adapter) {
                MsgArg arg("u", recordHandle);
                QStatus status = adapter->MethodCallAsync(*org.bluez.Service.RemoveRecord,
                                                          this,
                                                          ReplyHandler(&BTTransport::BTAccessor::NullHandler),
                                                          &arg, 1, NULL, BT_DEFAULT_TO);
                if (status != ER_OK) {
                    QCC_LogError(status, ("RemoveRecord method call failed"));
                }
                adapter->Dec();
            }

            recordHandle = newHandle;
        }
        QCC_DbgPrintf(("Got record handle %x", recordHandle));
    }


}


QStatus BTTransport::BTAccessor::RegisterService()
{
    QCC_DbgTrace(("BTTransport::BTAccessor::RegisterService()"));
    return UpdateServiceRecord();
}


QStatus BTTransport::BTAccessor::DeregisterService()
{
    QCC_DbgTrace(("BTTransport::BTAccessor::DeregisterService()"));
    QStatus status = ER_OK;
    AdapterObject* adapter(GetAnyAdapterObject());

    if (adapter && (recordHandle != 0)) {
        Message rsp(bzBus);
        MsgArg arg("u", recordHandle);

        QCC_DbgPrintf(("Removing record handle %x", recordHandle));
        status = adapter->MethodCall(*org.bluez.Service.RemoveRecord, &arg, 1, rsp, BT_DEFAULT_TO);
        if (status != ER_OK) {
            QCC_LogError(status, ("DeregisterService(): RemoveRecord method call"));
        }
        adapter->Dec();
    }

    deviceLock.Lock();
    if (defaultAdapterObj && (discoverCount > 0)) {
        CallStopDiscovery();
    }
    deviceLock.Unlock();

    adapterLock.Lock();
    if (defaultAdapterObj) {
        defaultAdapterObj->Dec();
    }
    defaultAdapterObj = NULL;
    if (anyAdapterObj) {
        anyAdapterObj->Dec();
    }
    anyAdapterObj = NULL;
    adapterLock.Unlock();

    return status;
}


void BTTransport::BTAccessor::SetInquiryParameters()
{
    enum InqState {
        IDLE,
        ACTIVE,
        PASSIVE,
        BOTH
    };

#if 0
    InqState inqState = ((discoverable && (discoverCount > 0)) ? BOTH :
                         (discoverable ? PASSIVE :
                          ((discoverCount > 0) ? ACTIVE : IDLE)));
    uint16_t deviceId;
    uint16_t window;
    uint16_t interval;

    switch (inqState) {
    case IDLE:
        break;

    case ACTIVE:
        break;

    case PASSIVE:
        break;

    case BOTH:
        break;

    }

    ConfigureInquiry(deviceId, window, interval, true);
#endif
}

void BTTransport::BTAccessor::DeviceFoundSignalHandler(const InterfaceDescription::Member* member,
                                                       const char* sourcePath,
                                                       Message& msg)
{
    /*
     * Ignore if we are not doing discovery.
     *
     * Note: other Bluetooth applications may have enabled discovery.
     */
    if (!Discovering()) {
        return;
    }

    BDAddress addr(msg->GetArg(0)->v_string.str);
    const AllJoynArray& array(msg->GetArg(1)->v_array);

    QCC_DbgTrace(("BTTransport::BTAccessor::DeviceFoundSignalHandler - signal from \"%s\" - addr = %s", sourcePath, addr.ToString().c_str()));

    /*
     * Note - we only kick off one SDP query per device found signal.
     */
    for (size_t i = 0; i < array.GetNumElements(); ++i) {
        const AllJoynDictEntry* entry = &array.GetElements()[i].v_dictEntry;

        if ((entry->key->typeId == ALLJOYN_STRING) && (entry->val->typeId == ALLJOYN_VARIANT)) {
            qcc::String key(entry->key->v_string.str, entry->key->v_string.len);
            const MsgArg* var = entry->val->v_variant.val;
            /*
             * Four possible cases for this device:
             *
             * 1) No AllJoyn UUID's so device is not a candidate
             * 2) A known AllJoyn UUID so is up to date.
             * 3) Unknown AllJoyn UUID so do an SDP query to get it's names
             * 4) More than one AllJoyn UUID so do an SDP query to refresh names
             */
            if ((var->typeId == ALLJOYN_ARRAY) && (key.compare("UUIDs") == 0)) {
                QCC_DbgPrintf(("BTTransport::BTAccessor::DeviceFoundSignalHandler(): checking %s (%d UUIDs)",
                               addr.ToString().c_str(), var->v_array.GetNumElements()));

                qcc::String uuid;
                size_t count = FindAllJoynUUID(var->v_array, uuid);

                if (count > 0) {
                    uint32_t now = GetTimestamp();
                    /*
                     * Extract revision number from the found UUID
                     */
                    uint32_t uuidRev = StringToU32(uuid.substr(0, ALLJOYN_UUID_REV_SIZE), 16);
                    deviceLock.Lock();
                    FoundInfo* foundInfo = &foundDevices[addr];
                    foundInfo->timestamp = now;
                    if ((count > 1) || (foundInfo->uuidRev != uuidRev)) {
                        deviceLock.Unlock();
                        QCC_DbgPrintf(("SDP query for advertised names"));
                        /*
                         * Do an SDP query to get or update the advertized names list.
                         */
                        AdapterObject* adapter(GetAdapterObject(sourcePath));
                        if (adapter) {
                            /*
                             * Stop discovering devices until we have queried this one.
                             */
                            PauseDiscovery();
                            /*
                             * Note we continue to hold a reference to the adapter
                             */
                            Alarm alarm(0, this, 0, new AlarmContext(AlarmContext::FIND_DEVICE, new pair<BDAddress, AdapterObject*>(addr, adapter)));
                            bzBus.GetInternal().GetDispatcher().AddAlarm(alarm);
                        }
                    } else {
                        QCC_DbgPrintf(("Refresh TTL for advertised names"));
                        /*
                         * The advertised names list has not changed but the name cache will expire
                         * shortly so we need to refresh the TTL.
                         */
                        qcc::String busGuid = foundInfo->guid;
                        AdvertisedNamesList names = foundInfo->advertisedNames;
                        deviceLock.Unlock();
                        if (!names->empty()) {
                            qcc::String busAddr("bluetooth:addr=" + addr.ToString());
                            transport->listener->FoundNames(busAddr, busGuid, &(*names), BUS_NAME_TTL);
                        } else {
                            qcc::String busAddr("bluetooth:addr=" + addr.ToString());
                            transport->listener->FoundNames(busAddr, busGuid, NULL, 0);
                        }
                    }
                }
                return;
            }
        }
    }
}


void BTTransport::BTAccessor::DeviceCreatedSignalHandler(const InterfaceDescription::Member* member,
                                                         const char* sourcePath,
                                                         Message& msg)
{
    if (Discovering()) {
        QCC_DbgTrace(("BTTransport::BTAccessor::DeviceCreatedSignalHandler - signal from \"%s\"", sourcePath));
        const char* devObjPath = msg->GetArg(0)->v_objPath.str;

        bzBus.RegisterSignalHandler(this,
                                    SignalHander(&BTTransport::BTAccessor::DevDisconnectRequestedSignalHandler),
                                    org.bluez.Device.DisconnectRequested, devObjPath);
    }
}


void BTTransport::BTAccessor::RemoveDeviceResponse(Message& msg, void* context)
{
    if (msg->GetType() == MESSAGE_ERROR) {
        qcc::String errMsg;
        const char* errName = msg->GetErrorName(&errMsg);
        QCC_LogError(ER_FAIL, ("AllJoyn Error response: %s - %s", errName, errMsg.c_str()));
    } else {
        BDAddress* address = static_cast<BDAddress*>(context);
        delete address;
    }
}


void BTTransport::BTAccessor::DeviceRemovedSignalHandler(const InterfaceDescription::Member* member,
                                                         const char* sourcePath,
                                                         Message& msg)
{
    const char* devPath = msg->GetArg(0)->v_string.str;

    QCC_DbgTrace(("BTTransport::BTAccessor::DeviceRemovedSignalHandler - signal from \"%s\" - removed \"%s\"", sourcePath, devPath));

    /*
     * Stop any endpoints for this device.
     */
    transport->threadListLock.Lock();
    for (vector<BTEndpoint*>::iterator eit = transport->threadList.begin(); eit != transport->threadList.end(); ++eit) {
        DeviceObject* dev = (*eit)->GetDeviceObject();
        if (dev->GetPath().compare(devPath) == 0) {
            (*eit)->Stop();
        }
        dev->Dec();
    }
    transport->threadListLock.Unlock();

    bzBus.UnRegisterSignalHandler(this,
                                  SignalHander(&BTTransport::BTAccessor::DevDisconnectRequestedSignalHandler),
                                  org.bluez.Device.DisconnectRequested, devPath);

}

void BTTransport::BTAccessor::DevDisconnectRequestedSignalHandler(const InterfaceDescription::Member* member,
                                                                  const char* sourcePath,
                                                                  Message& msg)
{
    QCC_DbgTrace(("BTTransport::BTAccessor::DevDisconnectRequestedSignalHandler - signal from \"%s\"", sourcePath));

    /* Connection is being yanked out from under us in 2 seconds. */
    transport->threadListLock.Lock();
    for (vector<BTEndpoint*>::iterator eit = transport->threadList.begin(); eit != transport->threadList.end(); ++eit) {
        DeviceObject* dev = (*eit)->GetDeviceObject();
        if (dev->GetPath().compare(sourcePath) == 0) {
            (*eit)->Stop();
        }
    }
    transport->threadListLock.Unlock();
}


QStatus BTTransport::BTAccessor::ProcessSDPXML(XmlParseContext& xmlctx, uint32_t& psm, uint32_t& channel, qcc::String& uuidstr, vector<qcc::String>& names, qcc::String& devBusGuid)
{
    QCC_DbgTrace(("BTTransport::BTAccessor::ProcessSDPXML()"));
    QStatus status;
    qcc::String psmStr;
    qcc::String channelStr;

    status = XmlElement::Parse(xmlctx);
    if (status != ER_OK) {
        QCC_LogError(status, ("Parsing SDP XML"));
        goto exit;
    }

    if (xmlctx.root.GetName().compare("record") == 0) {
        const vector<XmlElement*>& recElements(xmlctx.root.GetChildren());
        vector<XmlElement*>::const_iterator recElem;

        for (recElem = recElements.begin(); recElem != recElements.end(); ++recElem) {
            if ((*recElem)->GetName().compare("attribute") == 0) {
                const XmlElement* sequenceTag;
                const XmlElement* uuidTag;
                uint32_t attrId(StringToU32((*recElem)->GetAttribute("id")));
                const vector<XmlElement*>& valElements((*recElem)->GetChildren());
                vector<XmlElement*>::const_iterator valElem(valElements.begin());

                switch (attrId) {
                case 0x0001: {
                    sequenceTag = (*valElem)->GetChild("sequence");

                    if (sequenceTag) {
                        uuidTag = sequenceTag->GetChild("uuid");
                    } else {
                        uuidTag = (*valElem)->GetChild("uuid");
                    }

                    if (uuidTag) {
                        const std::map<qcc::String, qcc::String>& attrs(uuidTag->GetAttributes());
                        const std::map<qcc::String, qcc::String>::const_iterator valueIt(attrs.find("value"));
                        if (valueIt != attrs.end()) {
                            uuidstr = valueIt->second;
                            if (uuidstr.compare(ALLJOYN_UUID_REV_SIZE, ALLJOYN_UUID_BASE_SIZE, alljoynUUIDBase) != 0) {
                                // This is not the AllJoyn record;
                                status = ER_FAIL;
                                goto exit;
                            }
                        }
                    }
                    break;
                }

                case MSGBUS_VERSION_NUM_ATTR:
                    QCC_DbgPrintf(("    Attribute ID: %04x  MSGBUS_VERSION_NUM_ATTR", attrId));
                    break;

                case MSGBUS_PSM_ATTR:
                    while ((valElem != valElements.end()) && ((*valElem)->GetName().compare("uint32") != 0)) {
                        ++valElem;
                    }
                    if (valElem == valElements.end()) {
                        status = ER_FAIL;
                        QCC_LogError(status, ("Missing uint32 value for PSM number"));
                        goto exit;
                    }
                    psmStr = (*valElem)->GetAttributes().find("value")->second;
                    QCC_DbgPrintf(("    Attribute ID: %04x  MSGBUS_PSM_ATTR: %s", attrId, psmStr.c_str()));
                    break;

                case MSGBUS_RFCOMM_CH_ATTR:
                    while ((valElem != valElements.end()) && ((*valElem)->GetName().compare("uint32") != 0)) {
                        ++valElem;
                    }
                    if (valElem == valElements.end()) {
                        status = ER_FAIL;
                        QCC_LogError(status, ("Missing uint32 value for PSM number"));
                        goto exit;
                    }
                    channelStr = (*valElem)->GetAttributes().find("value")->second;
                    QCC_DbgPrintf(("    Attribute ID: %04x  MSGBUS_RFCOMM_CH_ATTR: %s", attrId, channelStr.c_str()));
                    break;

                case MSGBUS_ADVERTISEMENTS_ATTR:
                    if ((*valElem)->GetName().compare("sequence") == 0) {
                        const std::vector<XmlElement*> children = (*valElem)->GetChildren();
                        names.reserve(children.size());  // prevent memcpy's as names are added.
                        for (size_t i = 0; i < children.size(); ++i) {
                            const XmlElement* child = children[i];
                            if (child->GetName().compare("text") == 0) {
                                const qcc::String name = child->GetAttribute("value");
                                // A bug in BlueZ adds a space to the end of our text string.
                                if (!name.empty()) {
                                    if (name[name.size() - 1] == ' ') {
                                        names.push_back(name.substr(0, name.size() - 1));
                                    } else {
                                        names.push_back(name);
                                    }
                                }
                            }
                        }
                    }
                    QCC_DbgPrintf(("    Attribute ID: %04x  MSGBUS_ADVERTISEMENTS_ATTR:", attrId));
#ifndef NDEBUG
                    for (size_t i = 0; i < names.size(); ++i) {
                        QCC_DbgPrintf(("       \"%s\"", names[i].c_str()));
                    }
#endif
                    break;

                case MSGBUS_BUS_UUID_ATTR:
                    while ((valElem != valElements.end()) && ((*valElem)->GetName().compare("text") != 0)) {
                        ++valElem;
                    }
                    if (valElem == valElements.end()) {
                        status = ER_FAIL;
                        QCC_LogError(status, ("Missing text value for Bus GUID"));
                        goto exit;
                    }
                    devBusGuid = (*valElem)->GetAttributes().find("value")->second;
                    QCC_DbgPrintf(("    Attribute ID: %04x  MSGBUS_BUS_UUID_ATTR: %s", attrId, devBusGuid.c_str()));
                    break;


                default:
                    break;
                }
            }
        }

        if (devBusGuid.empty() || (psmStr.empty() && channelStr.empty())) {
            status = ER_FAIL;
        } else {
            if (!psmStr.empty()) {
                psm = StringToU32(psmStr);
                if ((psm < 0x1001) || ((psm & 0x1) != 0x1) || (psm > 0x8fff)) {
                    // PSM is invalid.
                    psm = 0;
                }
            }
            if (!channelStr.empty()) {
                channel = StringToU32(channelStr);
                if ((channel < 1) || (channel > 31)) {
                    // RFCOMM channel is invalid.
                    channel = 0xff;
                }
            }
            if ((channel == 0xff) && (psm == 0)) {
                status = ER_FAIL;
            }
        }
    } else {
        status = ER_FAIL;
        QCC_LogError(status, ("ProcessSDP(): Unexpected root tag parsing SDP XML: \"%s\"", xmlctx.root.GetName().c_str()));
    }

exit:
    return status;
}


void BTTransport::BTAccessor::NullHandler(Message& message, void* context)
{
}


void BTTransport::BTAccessor::FindDevice(void* context)
{
    QCC_DbgTrace(("BTTransport::BTAccessor::FindDevice()"));
    pair<BDAddress, AdapterObject*>* ctx(reinterpret_cast<pair<BDAddress, AdapterObject*>*>(context));
    BDAddress& addr(ctx->first);
    AdapterObject* adapter(ctx->second);
    qcc::String addrStr(addr.ToString());
    QStatus status = ER_OK;
    Message rsp(bzBus);

    MsgArg arg("s", addrStr.c_str());

    status = adapter->MethodCall(*org.bluez.Adapter.FindDevice, &arg, 1, rsp, BT_DEFAULT_TO);
    if (status != ER_OK) {
        if ((rsp->GetType() == MESSAGE_ERROR) && strcmp(rsp->GetErrorName(), "org.bluez.Error.DoesNotExist") == 0) {
            QCC_DbgPrintf(("%s is not yet known to BlueZ, creating it", addrStr.c_str()));
            status = adapter->MethodCall(*org.bluez.Adapter.CreateDevice, &arg, 1, rsp, BT_SDPQUERY_TO);
        }
    }
    if (status == ER_OK) {
        /*
         * We need a temporary device proxy object to make the DiscoverServices method call.
         */
        DeviceObject dev(bzBus, *transport, rsp->GetArg(0)->v_string.str, adapter, addr, false);
        dev.AddInterface(*org.bluez.Device.interface);
        MsgArg arg("s", ""); // Get AllJoyn service record
        QCC_DbgPrintf(("Getting service info for AllJoyn service"));
        status = dev.MethodCall(*org.bluez.Device.DiscoverServices, &arg, 1, rsp, BT_SDPQUERY_TO);
    }
    if (status == ER_OK) {
        const AllJoynArray& array(rsp->GetArg(0)->v_array);
        AdvertisedNamesList advertisements;
        /* Find AllJoyn SDP record */
        for (size_t i = 0; i < array.GetNumElements(); ++i) {
            StringSource rawXml(array.GetElements()[i].v_dictEntry.val->v_string.str);
            XmlParseContext xmlctx(rawXml);
            uint32_t psm = 0;
            uint32_t channel = 0xff;
            qcc::String uuidstr;
            qcc::String devBusGuid;
            status = ProcessSDPXML(xmlctx, psm, channel, uuidstr, *advertisements, devBusGuid);
            if ((status == ER_OK) && ((psm != 0) || (channel != 0)) && !uuidstr.empty()) {
                QCC_DbgPrintf(("Found AllJoyn UUID %s psm %#04x channel %d", uuidstr.c_str(), psm, channel));
                deviceLock.Lock();
                FoundInfo* foundInfo = &foundDevices[addr];
                foundInfo->guid = devBusGuid;
                foundInfo->uuidRev = StringToU32(uuidstr.substr(0, ALLJOYN_UUID_REV_SIZE), 16);
                foundInfo->timestamp = GetTimestamp();
                foundInfo->psm = psm;
                foundInfo->channel = channel;
                foundInfo->advertisedNames = advertisements;
                deviceLock.Unlock();
                /*
                 * Report found names.
                 */
                qcc::String busAddr("bluetooth:addr=" + addr.ToString());
                if (advertisements->empty()) {
                    transport->listener->FoundNames(busAddr, busGuid, NULL, 0);
                } else {
                    transport->listener->FoundNames(busAddr, devBusGuid, &(*advertisements), BUS_NAME_TTL);
                }
                break;
            }
        }
    }
    ContinueDiscovery(2);
    adapter->Dec();
    delete ctx;
}

size_t BTTransport::BTAccessor::FindAllJoynUUID(const AllJoynArray& list, qcc::String& uuidString)
{
    size_t count = 0;
    const MsgArg* uuids = list.GetElements();

    // Search the UUID list for AllJoyn UUIDs.
    for (size_t i = 0; i < list.GetNumElements(); ++i) {
        const MsgArg* uuid = &uuids[i];
        if ((uuid->typeId == ALLJOYN_STRING) &&
            (uuid->v_string.len == ALLJOYN_UUID_BASE_SIZE + ALLJOYN_UUID_REV_SIZE) &&
            (strcasecmp(alljoynUUIDBase, uuid->v_string.str + ALLJOYN_UUID_REV_SIZE) == 0)) {

            QCC_DbgPrintf(("BTTransport::BTAccessor::FindAllJoynUUID(list {size = %u}) UUID at %u", list.GetNumElements(), i));

            uuidString = uuid->v_string.str;
            ++count;
        }
    }
    return count;
}



/*****************************************************************************/


BTTransport::BTTransport(BusAttachment& bus) : Thread("BTTransport"), bus(bus), transportIsStopping(false)

{
    btAccessor = new BTAccessor(this, bus.GetGlobalGUIDString());
}


BTTransport::~BTTransport()
{
    /* Stop the thread */
    Stop();
    Join();

    delete btAccessor;
}

QStatus BTTransport::NormalizeTransportSpec(const char* inSpec, qcc::String& outSpec, map<qcc::String, qcc::String>& argMap)
{
    QStatus status = ParseArguments("bluetooth", inSpec, argMap);
    if (status == ER_OK) {
        outSpec = "bluetooth:";
        bool isFirst = true;
        map<qcc::String, qcc::String>::iterator addrIt = argMap.find("addr");
        if (addrIt != argMap.end()) {
            outSpec.append("addr=");
            outSpec += addrIt->second;
            isFirst = false;
        }
        map<qcc::String, qcc::String>::iterator devObjPathIt = argMap.find("devObjPath");
        if (devObjPathIt != argMap.end()) {
            outSpec.append(isFirst ? "devObjPath=" : ",devObjPath=");
            outSpec += devObjPathIt->second;
            isFirst = false;
        }
    }
    return status;
}

void* BTTransport::Run(void* arg)
{
    QStatus status = ER_OK;
    bool discovering = false;
    vector<Event*> signaledEvents;
    vector<Event*> checkEvents;

    while (!IsStopping()) {
        /*
         * Check if discoverability has changed
         */
        if (btAccessor->IsDiscoverable() != discovering) {
            if (!discovering) {
                status = btAccessor->ListenBlueZ();
                if (status == ER_OK) {
                    discovering = true;
                } else {
                    QCC_LogError(status, ("Failed to enable incoming connections"));
                }
            } else {
                btAccessor->CancelListenBlueZ();
                discovering = false;
            }
        }

        Event l2capEvent(btAccessor->l2capLFd, Event::IO_READ, false);
        Event rfcommEvent(btAccessor->rfcommLFd, Event::IO_READ, false);
        checkEvents.push_back(&stopEvent);

        /* wait for something to happen */
        if (discovering) {
            QCC_DbgTrace(("waiting for incoming connection ..."));
            checkEvents.push_back(&l2capEvent);
            checkEvents.push_back(&rfcommEvent);
        } else {
            QCC_DbgTrace(("waiting for alert or stop ..."));
        }

        status = Event::Wait(checkEvents, signaledEvents);
        if (ER_OK != status) {
            QCC_LogError(status, ("Event::Wait failed"));
            break;
        }

        /* Iterate over signaled events */
        for (vector<Event*>::iterator it = signaledEvents.begin(); it != signaledEvents.end(); ++it) {
            if (*it == &stopEvent) {
                stopEvent.ResetEvent();
            } else {
                /* Accept a new connection */
                qcc::String authName;
                bool isBusToBus = false;
                bool allowRemote = false;
                BTEndpoint* conn(btAccessor->Accept(bus, (*it)->GetFD(), *it == &rfcommEvent));
                if (!conn) {
                    continue;
                }

                threadListLock.Lock();
                threadList.push_back(conn);
                threadListLock.Unlock();
                QCC_DbgPrintf(("BTTransport::Run: Calling conn->Establish() [for accepted connection]"));
                status = conn->Establish("ANONYMOUS", authName, isBusToBus, allowRemote);
                if (ER_OK == status) {
                    QCC_DbgPrintf(("Starting endpoint [for accepted connection]"));
                    conn->SetListener(this);
                    status = conn->Start(isBusToBus, allowRemote);
                }

                if (ER_OK != status) {
                    QCC_LogError(status, ("Error starting RemoteEndpoint"));
                    EndpointExit(conn);
                    conn = NULL;
                }
            }
        }
        signaledEvents.clear();
        checkEvents.clear();
    }
    if (discovering) {
        btAccessor->CancelListenBlueZ();
    }
    return (void*) status;
}


QStatus BTTransport::Start()
{
    QCC_DbgTrace(("BTTransport::Start()"));

    QStatus status = Thread::Start();
    if (status == ER_OK) {
        status = btAccessor->StartControlBus();
    }
    return status;
}


QStatus BTTransport::Stop(void)
{
    transportIsStopping = true;

    vector<BTEndpoint*>::iterator eit;

    btAccessor->StopDiscoverability();
    bool isStopping = IsStopping();
    Thread::Stop();

    if (!isStopping) {
        btAccessor->DisconnectBlueZ();
        btAccessor->StopControlBus();
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
    QStatus status = Thread::Join();

    /* Wait for the thread list to empty out */
    threadListLock.Lock();
    while (threadList.size() > 0) {
        threadListLock.Unlock();
        qcc::Sleep(50);
        threadListLock.Lock();
    }
    threadListLock.Unlock();
    Thread::Join();

    return status;
}


void BTTransport::EnableDiscovery(const char* namePrefix)
{
    QCC_DbgTrace(("BTTransport::EnableDiscovery()"));

    QStatus status(ER_OK);

    if (!listener) {
        status = ER_BUS_NO_LISTENER;
        goto exit;
    }

    /*
     * Start discovery even though there may not be an adapter yet so that discovery will commence
     * when the adapter becomes available.
     */
    btAccessor->StartDiscovery(namePrefix == NULL);

    if (!btAccessor->IsBluetoothAvailable()) {
        status = ER_BUS_TRANSPORT_NOT_AVAILABLE;
        goto exit;
    }

exit:
    if (status != ER_OK) {
        QCC_LogError(status, ("BTTransport::EnableDiscovery"));
    }
}


void BTTransport::DisableDiscovery(const char* namePrefix)
{
    QCC_DbgTrace(("BTTransport::DisableDiscovery()"));

    QStatus status(ER_OK);

    if (!listener) {
        status = ER_BUS_NO_LISTENER;
        goto exit;
    }

    btAccessor->StopDiscovery(namePrefix == NULL);

    if (!btAccessor->IsBluetoothAvailable()) {
        status = ER_BUS_TRANSPORT_NOT_AVAILABLE;
        goto exit;
    }

exit:
    if (status != ER_OK) {
        QCC_LogError(status, ("BTTransport::DisableDiscovery"));
    }
}


void BTTransport::EnableAdvertisement(const qcc::String& advertiseName)
{
    QCC_DbgTrace(("BTTransport::EnableAdvertisement(advertiseName = %s)", advertiseName.c_str()));

    btAccessor->AddAdvertiseName(advertiseName);

    if (btAccessor->IsBluetoothAvailable()) {
        btAccessor->UpdateUUID();
    }

    btAccessor->StartDiscoverability();
}


void BTTransport::DisableAdvertisement(const qcc::String& advertiseName, bool nameListEmpty)
{
    QCC_DbgTrace(("BTTransport::DisableAdvertisement()"));

    btAccessor->RemoveAdvertiseName(advertiseName);

    if (btAccessor->IsBluetoothAvailable()) {
        btAccessor->UpdateUUID();
    }

    if (nameListEmpty) {
        btAccessor->DelayedStopDiscoverability();
    }
}


QStatus BTTransport::Connect(const char* connectSpec, RemoteEndpoint** newep)
{
    QCC_DbgTrace(("BTTransport::Connect(connectSpec = \"%s\")", connectSpec));

    if (!btAccessor->IsBluetoothAvailable()) {
        return ER_BUS_TRANSPORT_NOT_AVAILABLE;
    }

    QStatus status;
    BTEndpoint* conn(NULL);
    qcc::String authName;
    qcc::String devObjPath;
    qcc::String normSpec;
    bool isDaemon = bus.GetInternal().GetRouter().IsDaemon();
    bool allowRemote = bus.GetInternal().AllowRemoteMessages();

    conn = btAccessor->Connect(bus, connectSpec);
    if (!conn) {
        status = ER_FAIL;
        goto errorExit;
    }

    threadListLock.Lock();
    threadList.push_back(conn);
    threadListLock.Unlock();
    QCC_DbgPrintf(("BTTransport::Connect: Calling conn->Establish() [connectSpec = %s]", connectSpec));
    status = conn->Establish("ANONYMOUS", authName, isDaemon, allowRemote);
    if (ER_OK != status) {
        QCC_LogError(status, ("BTEndpoint::Establish failed"));
        goto errorExit;
    }

    QCC_DbgPrintf(("Starting endpoint [connectSpec = %s]", connectSpec));
    /* Start the endpoint */
    conn->SetListener(this);
    status = conn->Start(isDaemon, allowRemote);
    if (ER_OK != status) {
        QCC_LogError(status, ("BTEndpoint::Start failed"));
        goto errorExit;
    }

    /* If transport is closing, then don't allow any new endpoints */
    if (transportIsStopping) {
        status = ER_BUS_TRANSPORT_NOT_STARTED;
    }

errorExit:

    /* Cleanup if failed */
    if (status != ER_OK) {
        if (conn) {
            EndpointExit(conn);
            conn = NULL;
        }
    }

    if (newep) {
        *newep = conn;
    }
    return status;
}

QStatus BTTransport::Disconnect(const char* connectSpec)
{
    QCC_DbgTrace(("BTTransport::Disconnect(connectSpec = \"%s\")", connectSpec));

    if (!btAccessor->IsBluetoothAvailable()) {
        return ER_BUS_TRANSPORT_NOT_AVAILABLE;
    }

    /* Normalize and parse the connect spec */
    qcc::String spec;
    map<qcc::String, qcc::String> argMap;
    QStatus status = NormalizeTransportSpec(connectSpec, spec, argMap);

    if (ER_OK == status) {
        if (argMap.find("addr") != argMap.end()) {
            BDAddress addr(argMap["addr"]);

            status = btAccessor->Disconnect(addr);
        }
    }
    return status;
}

// @@ TODO
QStatus BTTransport::StartListen(const char* listenSpec)
{
    QCC_DbgTrace(("BTTransport::StartListen(listenSpec = \"%s\")", listenSpec));
    if (!btAccessor->IsBluetoothAvailable()) {
        return ER_BUS_TRANSPORT_NOT_AVAILABLE;
    }
    /*
     * Bluetooth listens are implicit
     */
    return ER_OK;
}

// @@ TODO
QStatus BTTransport::StopListen(const char* listenSpec)
{
    QCC_DbgTrace(("BTTransport::StopListen(listenSpec = \"%s\")", listenSpec));
    if (!btAccessor->IsBluetoothAvailable()) {
        return ER_BUS_TRANSPORT_NOT_AVAILABLE;
    }

    return ER_OK;
}

void BTTransport::EndpointExit(RemoteEndpoint* endpoint)
{
    BTEndpoint* btEndpoint = static_cast<BTEndpoint*>(endpoint);

    QCC_DbgTrace(("BTTransport::EndpointExit(endpoint => \"%s\" - \"%s\")",
                  btEndpoint->GetRemoteGUID().ToShortString().c_str(),
                  btEndpoint->GetConnectSpec().c_str()));

    /* Remove thread from thread list */
    threadListLock.Lock();
    vector<BTEndpoint*>::iterator eit = find(threadList.begin(), threadList.end(), btEndpoint);
    if (eit != threadList.end()) {
        threadList.erase(eit);
    }
    threadListLock.Unlock();

    SocketFd sockFd = btEndpoint->GetSocketFd();
    if (sockFd != -1) {
        QCC_DbgPrintf(("Closing FD: %d", sockFd));
        shutdown(sockFd, SHUT_RDWR);
        close(sockFd);
    }

    DeviceObject* devObj = btEndpoint->GetDeviceObject();

    QCC_DbgPrintf(("Calling btAccessor->DisconnectComplete(\"%s\", %s, %s)",
                   devObj->GetPath().c_str(),
                   btEndpoint->IsIncomingConnection() ? "incoming" : "outgoing",
                   btEndpoint->SurpriseDisconnect() ? "surprise" : "expected"));

    btAccessor->DisconnectComplete(devObj, btEndpoint->IsIncomingConnection(), btEndpoint->SurpriseDisconnect());
    devObj->Dec();

    delete btEndpoint;
}

}

