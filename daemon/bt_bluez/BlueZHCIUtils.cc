/**
 * @file
 * Utility functions for tweaking Bluetooth behavior via BlueZ.
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

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#include <qcc/Socket.h>
#include <qcc/time.h>

#include "BlueZ.h"
#include "BlueZHCIUtils.h"
#include "BTTransportConsts.h"

#include <Status.h>


#define QCC_MODULE "ALLJOYN_BT"

using namespace ajn;
using namespace qcc;

namespace ajn {
namespace bluez {


const static uint16_t L2capDefaultMtu = (1 * 1021) + 1011; // 2 x 3DH5


/*
 * Set the L2CAP mtu to something better than the BT 1.0 default value.
 */
void ConfigL2capMTU(SocketFd sockFd)
{
    int ret;
    uint8_t secOpt = BT_SECURITY_LOW;
    socklen_t optLen = sizeof(secOpt);
    uint16_t outMtu = 672; // default BT 1.0 value
    ret = setsockopt(sockFd, SOL_BLUETOOTH, BT_SECURITY, &secOpt, optLen);
    if (ret < 0) {
        QCC_DbgPrintf(("Setting security low: %d: %s", errno, strerror(errno)));
    }

    struct l2cap_options opts;
    optLen = sizeof(opts);
    ret = getsockopt(sockFd, SOL_L2CAP, L2CAP_OPTIONS, &opts, &optLen);
    if (ret != -1) {
        opts.imtu = L2capDefaultMtu;
        opts.omtu = L2capDefaultMtu;
        ret = setsockopt(sockFd, SOL_L2CAP, L2CAP_OPTIONS, &opts, optLen);
        if (ret == -1) {
            QCC_LogError(ER_OS_ERROR, ("Failed to set in/out MTU for L2CAP socket (%d - %s)", errno, strerror(errno)));
        } else {
            outMtu = opts.omtu;
            QCC_DbgPrintf(("Set L2CAP mtu to %d", opts.omtu));
        }
    } else {
        QCC_LogError(ER_OS_ERROR, ("Failed to get in/out MTU for L2CAP socket (%d - %s)", errno, strerror(errno)));
    }

    // Only let the kernel buffer up 2 packets at a time.
    int sndbuf = 2 * outMtu;

    ret = setsockopt(sockFd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    if (ret == -1) {
        QCC_LogError(ER_OS_ERROR, ("Failed to set send buf to %d: %d - %s", sndbuf, errno, strerror(errno)));
    }
}

void ConfigL2capMaster(SocketFd sockFd)
{
    int ret;
    int lmOpt = 0;
    socklen_t optLen = sizeof(lmOpt);
    ret = getsockopt(sockFd, SOL_L2CAP, L2CAP_LM, &lmOpt, &optLen);
    if (ret == -1) {
        QCC_LogError(ER_OS_ERROR, ("Failed to get LM flags (%d - %s)", errno, strerror(errno)));
    } else {
        lmOpt |= L2CAP_LM_MASTER;
        ret = setsockopt(sockFd, SOL_L2CAP, L2CAP_LM, &lmOpt, optLen);
        if (ret == -1) {
            QCC_LogError(ER_OS_ERROR, ("Failed to set LM flags (%d - %s)", errno, strerror(errno)));
        }
    }
}

/*
 * @param deviceId    The Bluetooth device id
 * @param window      The inquiry window in milliseconds (10 .. 2560)
 * @param interval    The inquiry interval in milliseconds (11 .. 2560)
 * @param interlaced  If true use interlaced inquiry.
 */
QStatus ConfigureInquiryScan(uint16_t deviceId, uint16_t window, uint16_t interval, bool interlaced, int8_t txPower)
{
    static const uint8_t hciSetInquiryParams[] = {
        0x01, 0x1E, 0x0C, 0x04, 0x28, 0x00, 0x14, 0x00
    };

    static const uint8_t hciSetInquiryInterlaced[] = {
        0x01, 0x43, 0x0C, 0x01, 0x01
    };

    static const uint8_t hciSetInquiryTxPower[] = {
        0x01, 0x59, 0x0C, 0x01, 0x00
    };

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
    if ((txPower < -70) || (txPower > 20)) {
        status = ER_BAD_ARG_5;
        QCC_LogError(status, ("TX Power must be in range -70 .. 20"));
        return status;
    }

    hciFd = (SocketFd)socket(AF_BLUETOOTH, QCC_SOCK_RAW, 1);
    if (hciFd < 0) {
        status = ER_OS_ERROR;
        QCC_LogError(status, ("Failed to create socket (errno %d)", errno));
        return status;
    }

    addr.family = AF_BLUETOOTH;
    addr.dev = deviceId;
    if (bind(hciFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        status = ER_OS_ERROR;
        QCC_LogError(status, ("Failed to bind to BT device id %d socket (errno %d)", deviceId, errno));
        goto Exit;
    }

    /*
     * Convert window and interval from millseconds to ticks.
     */
    window = window == 10 ? 0x11 : (uint16_t)(((uint32_t)window * 1000 + 313) / 625);
    interval = (uint16_t)(((uint32_t)interval * 1000 + 313) / 625);

    memcpy(cmd, hciSetInquiryParams, 4);
    cmd[4] =  (uint8_t)(interval & 0xFF);
    cmd[5] =  (uint8_t)(interval >> 8);
    cmd[6] =  (uint8_t)(window & 0xFF);
    cmd[7] =  (uint8_t)(window >> 8);

    status = Send(hciFd, cmd, sizeof(hciSetInquiryParams), sent);
    if (status != ER_OK) {
        QCC_LogError(status, ("Failed to send SetInquiryParams HCI command (errno %d)", errno));
        goto Exit;
    }

    memcpy(cmd, hciSetInquiryInterlaced, 4);
    cmd[4] = interlaced ? 1 : 0;

    status = Send(hciFd, cmd, sizeof(hciSetInquiryInterlaced), sent);
    if (status != ER_OK) {
        QCC_LogError(status, ("Failed to send SetInquiryInterlaced HCI command (errno %d)", errno));
        goto Exit;
    }

    memcpy(cmd, hciSetInquiryTxPower, 4);
    cmd[4] = (uint8_t)txPower;

    status = Send(hciFd, cmd, sizeof(hciSetInquiryTxPower), sent);
    if (status != ER_OK) {
        QCC_LogError(status, ("Failed to send SetInquiryTxPower HCI command (errno %d)", errno));
        goto Exit;
    }

Exit:

    close(hciFd);
    return status;
}


/*
 * @param deviceId      The Bluetooth device id
 * @param minPeriod     Value in range 2..0xFFFE expressed as multiple of 1.28 seconds
 * @param maxPeriod     Value in range 3..0xFFFF expressed as multiple of 1.28 seconds
 * @param length        Value in range 1..0x30  (0 will turn off periodic inquiry)
 * @param maxResponses  0 means no limit
 *
 */
QStatus ConfigurePeriodicInquiry(uint16_t deviceId, uint16_t minPeriod, uint16_t maxPeriod, uint8_t length, uint8_t maxResponses)
{
    static const uint8_t hciStartPeriodicInquiry[] = {
        0x01, 0x03, 0x04, 0x09, 0x00, 0x00, 0x00, 0x00, 0x33, 0x8B, 0x9E, 0x00, 0x00
    };

    static const uint8_t hciExitPeriodicInquiry[] = {
        0x01, 0x04, 0x04, 0x00
    };

    QStatus status = ER_OK;
    uint8_t cmd[sizeof(hciStartPeriodicInquiry)];
    sockaddr_hci addr;
    SocketFd hciFd;
    size_t sent;

    if (length > 0) {
        if (minPeriod < 2 || (minPeriod >= maxPeriod)) {
            status = ER_BAD_ARG_2;
            QCC_LogError(status, ("minPeriod %d must be in range 2..0xFFFE and less than maxPeriod", minPeriod));
            return status;
        }
        if (maxPeriod < 3) {
            status = ER_BAD_ARG_3;
            QCC_LogError(status, ("minPeriod %d must be in range 3..0xFFFF", maxPeriod));
            return status;
        }
        if ((length > 0x30) || (length >= minPeriod)) {
            status = ER_BAD_ARG_4;
            QCC_LogError(status, ("length %d must be in range 1..0x30 and less than minPeriod", length));
            return status;
        }
    }

    hciFd = (SocketFd)socket(AF_BLUETOOTH, QCC_SOCK_RAW, 1);
    if (hciFd < 0) {
        status = ER_OS_ERROR;
        QCC_LogError(status, ("Failed to create socket (errno %d)", errno));
        return status;
    }

    addr.family = AF_BLUETOOTH;
    addr.dev = deviceId;
    if (bind(hciFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        status = ER_OS_ERROR;
        QCC_LogError(status, ("Failed to bind to BT device id %d socket (errno %d)", deviceId, errno));
        goto Exit;
    }

    /*
     * First exit periodic inquiry
     */
    memcpy(cmd, hciExitPeriodicInquiry, sizeof(hciExitPeriodicInquiry));
    status = Send(hciFd, cmd, sizeof(hciExitPeriodicInquiry), sent);
    if (status != ER_OK) {
        QCC_LogError(status, ("Failed to send HciExitPeriodicInquiry HCI command (errno %d)", errno));
        goto Exit;
    }

    if (length > 0) {
        /*
         * Now start periodic inquiry with our new parameters
         */
        memcpy(cmd, hciStartPeriodicInquiry, sizeof(hciStartPeriodicInquiry));
        cmd[4] =  (uint8_t)(maxPeriod & 0xFF);
        cmd[5] =  (uint8_t)(maxPeriod >> 8);
        cmd[6] =  (uint8_t)(minPeriod & 0xFF);
        cmd[7] =  (uint8_t)(minPeriod >> 8);
        cmd[11] = length;
        cmd[12] = maxResponses;

        status = Send(hciFd, cmd, sizeof(hciStartPeriodicInquiry), sent);
        if (status != ER_OK) {
            QCC_LogError(status, ("Failed to send HciStartPeriodicInquiry HCI command (errno %d)", errno));
            goto Exit;
        }
    }

Exit:

    close(hciFd);
    return status;
}


QStatus ConfigureSimplePairingDebugMode(uint16_t deviceId, bool enable)
{
    static const uint8_t hciSimplePairingDebugMode[] = {
        0x01, 0x04, 0x18, 0x01, 0x01
    };
    QStatus status = ER_OK;
    uint8_t cmd[sizeof(hciSimplePairingDebugMode)];
    sockaddr_hci addr;
    SocketFd hciFd;
    size_t sent;

    hciFd = (SocketFd)socket(AF_BLUETOOTH, QCC_SOCK_RAW, 1);
    if (hciFd < 0) {
        status = ER_OS_ERROR;
        QCC_LogError(status, ("Failed to create socket (errno %d)", errno));
        return status;
    }

    addr.family = AF_BLUETOOTH;
    addr.dev = deviceId;
    if (bind(hciFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        status = ER_OS_ERROR;
        QCC_LogError(status, ("Failed to bind to BT device id %d socket (errno %d)", deviceId, errno));
        goto Exit;
    }

    memcpy(cmd, hciSimplePairingDebugMode, sizeof(hciSimplePairingDebugMode));
    cmd[4] = enable ? 1 : 0;
    status = Send(hciFd, cmd, sizeof(hciSimplePairingDebugMode), sent);
    if (status != ER_OK) {
        QCC_LogError(status, ("Failed to send HciSimplePairingDebugMode HCI command (errno %d)", errno));
        goto Exit;
    }

Exit:

    close(hciFd);
    return status;
}


QStatus ConfigureClassOfDevice(uint16_t deviceId, uint32_t cod)
{
    static const uint8_t hciWriteCOD[] = {
        0x01, 0x24, 0x0c, 0x03, 0x00, 0x00, 0x00
    };
    QStatus status = ER_OK;
    uint8_t cmd[sizeof(hciWriteCOD)];
    sockaddr_hci addr;
    SocketFd hciFd;
    size_t sent;

    hciFd = (SocketFd)socket(AF_BLUETOOTH, QCC_SOCK_RAW, 1);
    if (hciFd < 0) {
        status = ER_OS_ERROR;
        QCC_LogError(status, ("Failed to create socket (errno %d)", errno));
        return status;
    }

    addr.family = AF_BLUETOOTH;
    addr.dev = deviceId;
    if (bind(hciFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        status = ER_OS_ERROR;
        QCC_LogError(status, ("Failed to bind to BT device id %d socket (errno %d)", deviceId, errno));
        goto Exit;
    }

    memcpy(cmd, hciWriteCOD, sizeof(hciWriteCOD));

    cmd[4] = cod & 0xff;
    cmd[5] = (cod >> 8) & 0xff;
    cmd[6] = (cod >> 16) & 0xff;

    status = Send(hciFd, cmd, sizeof(hciWriteCOD), sent);
    if (status != ER_OK) {
        QCC_LogError(status, ("Failed to send Write CoD HCI command (errno %d)", errno));
        goto Exit;
    }

Exit:

    close(hciFd);
    return status;
}


QStatus IsMaster(uint16_t deviceId, const BDAddress& bdAddr, bool& master)
{
    int ret;
    QStatus status = ER_OK;
    struct hci_conn_info_req connInfoReq;
    sockaddr_hci addr;
    SocketFd hciFd;

    hciFd = (SocketFd)socket(AF_BLUETOOTH, QCC_SOCK_RAW, 1);
    if (hciFd < 0) {
        status = ER_OS_ERROR;
        QCC_LogError(status, ("Failed to create socket (%d - %s)", errno, strerror(errno)));
        return status;
    }

    addr.family = AF_BLUETOOTH;
    addr.dev = deviceId;
    if (bind(hciFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        status = ER_OS_ERROR;
        QCC_LogError(status, ("Failed to bind to BT device id %d socket (errno %d)", deviceId, errno));
        goto exit;
    }

    bdAddr.CopyTo(connInfoReq.bdaddr.b, true);
    connInfoReq.type = HCI_ACL_LINK;

    ret = ioctl(hciFd, HCIGETCONNINFO, &connInfoReq);
    if (ret < 0) {
        status = ER_OS_ERROR;
        QCC_LogError(status, ("Getting connection information (%d - %s)", errno, strerror(errno)));
        goto exit;
    }

    master = static_cast<bool>(connInfoReq.conn_info.link_mode & HCI_LM_MASTER);

exit:
    close(hciFd);
    return status;
}

QStatus RequestBTRole(uint16_t deviceId, const BDAddress& bdAddr, bt::BluetoothRole role)
{
    // Template for the role switch command.
    static const uint8_t hciRoleSwitch[] = {
        0x01, 0x0B, 0x08, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    QStatus status = ER_OK;
    uint8_t cmd[sizeof(hciRoleSwitch)];
    sockaddr_hci addr;
    SocketFd hciFd;
    size_t sent;
    uint8_t rxBuf[260];
    size_t pos;
    size_t recvd;
    bool gotCmdStatus = false;
    bool gotRoleSwitchEvent = false;
    int ret;
    struct hci_filter evtFilter;
    uint64_t timeout;
    int flags;

    // HCI command sent via raw sockets (must have privileges for this)
    hciFd = (SocketFd)socket(AF_BLUETOOTH, QCC_SOCK_RAW, 1);
    if (hciFd < 0) {
        status = ER_OS_ERROR;
        QCC_LogError(status, ("Failed to create socket (errno %d)", errno));
        return status;
    }

    Event hciRxEvent(hciFd);

    // Need to select the adapter we are sending the HCI command to.
    addr.family = AF_BLUETOOTH;
    addr.dev = deviceId;
    if (bind(hciFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        status = ER_OS_ERROR;
        QCC_LogError(status, ("Failed to bind to BT device id %d socket (errno %d)", deviceId, errno));
        goto exit;
    }

    // Initialize the command with the template.
    memcpy(cmd, hciRoleSwitch, sizeof(hciRoleSwitch));

    // Embed the BD address into the command.
    bdAddr.CopyTo(cmd + 4, true);

    // Set which role we want.
    cmd[10] = (role == bt::MASTER) ? 0x00 : 0x01;   // Select the role: master or slave

    // Send the command.
    status = Send(hciFd, cmd, sizeof(hciRoleSwitch), sent);
    if (status != ER_OK) {
        QCC_LogError(status, ("Failed to send HciRoleSwitch HCI command (errno %d)", errno));
        goto exit;
    }

    // Setup HCI event types to receive.
    evtFilter.type_mask = 1 << 0x04;
    evtFilter.event_mask[0] = (1 << 0x0f) | (1 << 0x12);
    evtFilter.event_mask[1] = 0;
    evtFilter.opcode = htole16(0x0b | (0x2 << 10));

    ret = setsockopt(hciFd, SOL_HCI, HCI_FILTER, &evtFilter, sizeof(evtFilter));
    if (ret == -1) {
        status = ER_OS_ERROR;
        QCC_LogError(status, ("Failed to send HciRoleSwitch HCI command (errno %d)", errno));
        goto exit;
    }

    flags = fcntl(hciFd, F_GETFL);
    ret = fcntl(hciFd, F_SETFL, flags | O_NONBLOCK);

    timeout = GetTimestamp64() + 10000;

    pos = 0;
    do {
        status = Event::Wait(hciRxEvent, 5000);  // 5 second timeout
        if (status != ER_OK) {
            QCC_LogError(status, ("Waiting for HCI event"));
            goto exit;
        }

        status = Recv(hciFd, rxBuf + pos, sizeof(rxBuf) - pos, recvd);
        if (status == ER_WOULDBLOCK) {
            continue;
        }
        if (status != ER_OK) {
            QCC_LogError(status, ("Failed to receive HCI event (errno %d)", errno));
            goto exit;
        }

        pos += recvd;

        if ((pos > 2) && (pos > rxBuf[2])) {
            if (!gotCmdStatus) {
                if ((rxBuf[0] == 0x04) &&
                    (rxBuf[1] == 0x0f) &&
                    (rxBuf[2] == 0x04) &&
                    memcmp(rxBuf + 4, hciRoleSwitch, 3) == 0) {
                    if (rxBuf[3] != 0x00) {
                        status = ER_FAIL;
                        QCC_LogError(status, ("HCI role switch command failed with HCI status 0x%02x", rxBuf[3]));
                        goto exit;
                    }
                    gotCmdStatus = true;
                }

            } else if (!gotRoleSwitchEvent) {
                if ((rxBuf[0] == 0x04) &&
                    (rxBuf[1] == 0x12) &&
                    (rxBuf[2] == 0x08) &&
                    memcmp(rxBuf + 4, cmd + 4, sizeof(hciRoleSwitch) - 5) == 0) {
                    if (rxBuf[3] != 0x00) {
                        status = ER_FAIL;
                        QCC_LogError(status, ("HCI role switch event received with HCI fail code 0x%02x", rxBuf[3]));
                        goto exit;
                    }
                    gotRoleSwitchEvent = true;
                    QCC_DbgPrintf(("BT role switched to %s for connection to %s",
                                   (rxBuf[9] == 0x00) ? "MASTER" : "SLAVE",
                                   bdAddr.ToString().c_str()));
                }
            }
            pos = 0;
        }
    } while (!gotRoleSwitchEvent && (timeout > GetTimestamp64()));

    if (!gotRoleSwitchEvent) {
        status = ER_TIMEOUT;
        QCC_LogError(status, ("Timed out waiting for role switch confirmation"));
    }

exit:
    close(hciFd);
    return status;
}



} // namespace bluez
} // namespace ajn
