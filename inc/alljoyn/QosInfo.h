#ifndef _ALLJOYN_QOSINFO_H
#define _ALLJOYN_QOSINFO_H
/**
 * @file
 * QosInfo describes a Quality of Service preference or requirement.
 */

/******************************************************************************
 * Copyright 2009-2010, Qualcomm Innovation Center, Inc.
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

namespace ajn {

/**
 * QosInfo describes a Quality of Service preference or requirement.
 */
struct QosInfo {

    /** Proximity constraint */
    static const uint16_t PROXIMITY_ANY = 0xFFFF;
    static const uint16_t PROXIMITY_PHYSICAL = 0x0001;
    static const uint16_t PROXIMITY_NETWORK = 0x0002;
    uint16_t proximity;

    /** Traffic type */
    static const uint16_t TRAFFIC_ANY = 0xFFFF;
    static const uint16_t TRAFFIC_RELIABLE = 0x0001;
    static const uint16_t TRAFFIC_UNRELIABLE = 0x0002;
    uint16_t traffic;

    /** Transport types */
    // @{
    static const uint16_t TRANSPORT_ANY = 0xFFFF;
    static const uint16_t TRANSPORT_BLUETOOTH = 0x0001;
    static const uint16_t TRANSPORT_WLAN = 0x0002;
    static const uint16_t TRANSPORT_WWAN = 0x0004;
    uint16_t transports;
    // @}

    /**
     * Determine whether this QoS is compatible with the QoS offered by otherQos
     *
     * @param otherQos  QoS to be compared against this one.
     * @return true iff this QoS can use the QoS offered by otherQos.
     */
    bool IsCompatible(const QosInfo& otherQos) const;
};

}

#endif
