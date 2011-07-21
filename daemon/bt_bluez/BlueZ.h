/**
 * @file
 * BTAccessor declaration for BlueZ
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
#ifndef _ALLJOYN_BLUEZ_H
#define _ALLJOYN_BLUEZ_H

#include <qcc/platform.h>

#include <sys/ioctl.h>


#define SOL_BLUETOOTH  274
#define SOL_HCI          0
#define SOL_L2CAP        6
#define SOL_RFCOMM      18
#define BT_SECURITY      4
#define BT_SECURITY_LOW  1

#define RFCOMM_PROTOCOL_ID 3
#define RFCOMM_CONNINFO  2

#define L2CAP_PROTOCOL_ID  0

#define L2CAP_OPTIONS    1
#define L2CAP_CONNINFO   2
#define L2CAP_LM         3

#define L2CAP_LM_MASTER 0x1

#define HCI_FILTER 2

#define HCI_LM_MASTER 0x1

#define HCI_SCO_LINK  0x00
#define HCI_ACL_LINK  0x01
#define HCI_ESCO_LINK 0x02


namespace ajn {
namespace bluez {
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
    uint8_t fcs;
    uint8_t max_tx;
    uint16_t txwin_size;
};

struct sockaddr_hci {
    sa_family_t family;
    uint16_t dev;
};

struct hci_conn_info {
    uint16_t handle;
    BDADDR bdaddr;
    uint8_t type;
    uint8_t out;
    uint16_t state;
    uint32_t link_mode;
};

struct hci_conn_info_req {
    BDADDR bdaddr;
    uint8_t type;
    struct hci_conn_info conn_info;
};

struct hci_filter {
    uint32_t type_mask;
    uint32_t event_mask[2];
    uint16_t opcode;
};


#define HCIGETCONNINFO _IOR('H', 213, int)


} // namespace bluez
} // namespace ajn


#endif
