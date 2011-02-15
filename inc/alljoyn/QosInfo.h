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

/** DBus signature of QosInfo */
#define QOSINFO_SIG "(yyq)"

namespace ajn {

/**
 * QosInfo describes a Quality of Service preference or requirement.
 */
struct QosInfo {

    /** Traffic type */
    // {@
    typedef uint8_t TrafficType;
    static const TrafficType TRAFFIC_MESSAGES          = 0x01;
    static const TrafficType TRAFFIC_STREAM_UNRELIABLE = 0x02;
    static const TrafficType TRAFFIC_STREAM_RELIABLE   = 0x04;
    TrafficType traffic;
    // @}

    /** Proximity */
    // {@
    typedef uint8_t Proximity;
    static const Proximity PROXIMITY_ANY      = 0xFF;
    static const Proximity PROXIMITY_PHYSICAL = 0x01;
    static const Proximity PROXIMITY_NETWORK  = 0x02;
    Proximity proximity;
    // @}

    /** Transport  */
    // @{
    typedef uint16_t Transport;
    static const Transport TRANSPORT_ANY       = 0xFFFF;
    static const Transport TRANSPORT_BLUETOOTH = 0x0001;
    static const Transport TRANSPORT_WLAN      = 0x0002;
    static const Transport TRANSPORT_WWAN      = 0x0004;
    Transport transports;
    // @}

    /**
     * Construct a QosInfo with specific parameters.
     *
     * @param traffic       Type of traffic.
     * @param proximity     Proximity constraint bitmask.
     * @param transports    Allowed transport types bitmask.
     */
    QosInfo(QosInfo::TrafficType traffic, QosInfo::Proximity proximity, QosInfo::Transport transports) :
        traffic(traffic),
        proximity(proximity),
        transports(transports) 
    { }

    /**
     * Construct a default QosInfo.
     */
    QosInfo() : traffic(TRAFFIC_MESSAGES), proximity(PROXIMITY_ANY), transports(TRANSPORT_ANY) { }

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
