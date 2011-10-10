#ifndef _ALLJOYN_SESSION_H
#define _ALLJOYN_SESSION_H
/**
 * @file
 * AllJoyn session related data types.
 */

/******************************************************************************
 * Copyright 2011, Qualcomm Innovation Center, Inc.
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
#include <alljoyn/TransportMask.h>
#include <alljoyn/AllJoynCTypes.h>

#ifdef __cplusplus

namespace ajn {

/**
 * SessionPort identifies a per-BusAttachment receiver for incoming JoinSession requests.
 * SessionPort values are bound to a BusAttachment when the attachement calls
 * BindSessionPort.
 *
 * NOTE: Valid SessionPort values range from 1 to 0xFFFF.
 */
typedef uint16_t SessionPort;

/** Invalid SessionPort value used to indicate that BindSessionPort should choose any available port */
const SessionPort SESSION_PORT_ANY = 0;

/** SessionId uniquely identifies an AllJoyn session instance */
typedef uint32_t SessionId;

/**
 * SessionOpts contains a set of parameters that define a Session's characteristics.
 */
class SessionOpts {
  public:
    /** Traffic type */
    typedef enum {
        TRAFFIC_MESSAGES       = 0x01,   /**< Session carries message traffic */
        TRAFFIC_RAW_UNRELIABLE = 0x02,   /**< Session carries an unreliable (lossy) byte stream */
        TRAFFIC_RAW_RELIABLE   = 0x04    /**< Session carries a reliable byte stream */
    } TrafficType;
    TrafficType traffic; /**< holds the Traffic type for this SessionOpt*/

    /**
     * Multi-point session capable.
     * A session is multi-point if it can be joined multiple times to form a single
     * session with multi (greater than 2) endpoints. When false, each join attempt
     * creates a new point-to-point session.
     */
    bool isMultipoint;

    /**@name Proximity */
    // {@
    typedef uint8_t Proximity;
    static const Proximity PROXIMITY_ANY      = 0xFF;
    static const Proximity PROXIMITY_PHYSICAL = 0x01;
    static const Proximity PROXIMITY_NETWORK  = 0x02;
    Proximity proximity;
    // @}

    /** Allowed Transports  */
    TransportMask transports;

    /**
     * Construct a SessionOpts with specific parameters.
     *
     * @param traffic       Type of traffic.
     * @param isMultipoint  true iff session supports multipoint (greater than two endpoints).
     * @param proximity     Proximity constraint bitmask.
     * @param transports    Allowed transport types bitmask.
     *
     */
    SessionOpts(SessionOpts::TrafficType traffic, bool isMultipoint, SessionOpts::Proximity proximity, TransportMask transports) :
        traffic(traffic),
        isMultipoint(isMultipoint),
        proximity(proximity),
        transports(transports)
    { }

    /**
     * Construct a default SessionOpts
     */
    SessionOpts() : traffic(TRAFFIC_MESSAGES), isMultipoint(false), proximity(PROXIMITY_ANY), transports(TRANSPORT_ANY) { }

    /**
     * Determine whether this SessionOpts is compatible with the SessionOpts offered by other
     *
     * @param other  Options to be compared against this one.
     * @return true iff this SessionOpts can use the option set offered by other.
     */
    bool IsCompatible(const SessionOpts& other) const;

    /**
     * Compare SessionOpts
     *
     * @param other the SessionOpts being compared against
     * @return true if all of the SessionOpts parameters are the same
     *
     */
    bool operator==(const SessionOpts& other) const
    {
        return (traffic == other.traffic) && (isMultipoint == other.isMultipoint) && (proximity == other.proximity) && (transports == other.transports);
    }

    /**
     * Rather arbitrary less-than operator to allow containers holding SessionOpts
     * to be sorted.
     * Traffic takes precedence when sorting SessionOpts.
     *
     * #TRAFFIC_MESSAGES \< #TRAFFIC_RAW_UNRELIABLE \< #TRAFFIC_RAW_RELIABLE
     *
     * If traffic is equal then Proximity takes next level of precedence.
     *
     * PROXIMITY_PHYSICAL \< PROXIMITY_NETWORK \< PROXIMITY_ANY
     *
     * last transports.
     *
     * #TRANSPORT_LOCAL \< #TRANSPORT_BLUETOOTH \< #TRANSPORT_WLAN \< #TRANSPORT_WWAN \< #TRANSPORT_ANY
     *
     *
     * @param other the SessionOpts being compared against
     * @return true if this SessionOpts is designated as less than the SessionOpts
     *         being compared against.
     */
    bool operator<(const SessionOpts& other) const
    {
        if ((traffic < other.traffic) ||
            ((traffic == other.traffic) && !isMultipoint && other.isMultipoint) ||
            ((traffic == other.traffic) && (isMultipoint == other.isMultipoint) && (proximity < other.proximity)) ||
            ((traffic == other.traffic) && (isMultipoint == other.isMultipoint) && (proximity == other.proximity) && (transports < other.transports))) {
            return true;
        }
        return false;
    }
};


}

extern "C" {
#endif /* #ifdef __cplusplus */

typedef uint16_t alljoyn_sessionport;

/** Invalid SessionPort value used to indicate that BindSessionPort should choose any available port */
const alljoyn_sessionport ALLJOYN_SESSION_PORT_ANY = 0;

/** SessionId uniquely identifies an AllJoyn session instance */
typedef uint32_t alljoyn_sessionid;

#define ALLJOYN_TRAFFIC_TYPE_MESSAGES        0x01   /**< Session carries message traffic */
#define ALLJOYN_TRAFFIC_TYPE_RAW_UNRELIABLE  0x02   /**< Session carries an unreliable (lossy) byte stream */
#define ALLJOYN_TRAFFIC_TYPE_RAW_RELIABLE    0x04   /**< Session carries a reliable byte stream */

#define ALLJOYN_PROXIMITY_ANY       0xFF
#define ALLJOYN_PROXIMITY_PHYSICAL  0x01
#define ALLJOYN_PROXIMITY_NETWORK   0x02

/**
 * Construct a SessionOpts with specific parameters.
 *
 * @param traffic       Type of traffic.
 * @param isMultipoint  true iff session supports multipoint (greater than two endpoints).
 * @param proximity     Proximity constraint bitmask.
 * @param transports    Allowed transport types bitmask.
 *
 */
alljoyn_sessionopts alljoyn_sessionopts_create(uint8_t traffic, QC_BOOL isMultipoint, uint8_t proximity, alljoyn_transportmask transports);

/**
 * Destroy a SessionOpts created with alljoyn_sessionopts_create.
 *
 * @param opts SessionOpts to destroy
 */
void alljoyn_sessionopts_destroy(alljoyn_sessionopts opts);

/**
 * Accessor for the traffic member of SessionOpts.
 *
 * @param opts SessionOpts
 *
 * @return Traffic type specified by the specified SessionOpts.
 */
uint8_t alljoyn_sessionopts_traffic(const alljoyn_sessionopts opts);

/**
 * Accessor for the isMultipoint member of SessionOpts.
 *
 * @param opts SessionOpts
 *
 * @return Multipoint value specified by the specified SessionOpts.
 */
QC_BOOL alljoyn_sessionopts_multipoint(const alljoyn_sessionopts opts);

/**
 * Accessor for the proximity member of SessionOpts.
 *
 * @param opts SessionOpts
 *
 * @return Proximity specified by the specified SessionOpts.
 */
uint8_t alljoyn_sessionopts_proximity(const alljoyn_sessionopts opts);

/**
 * Accessor for the transports member of SessionOpts.
 *
 * @param opts SessionOpts
 *
 * @return Transports allowed by the specified SessionOpts.
 */
alljoyn_transportmask alljoyn_sessionopts_transports(const alljoyn_sessionopts opts);

/**
 * Determine whether one SessionOpts is compatible with the SessionOpts offered by other
 *
 * @param one    Options to be compared against other.
 * @param other  Options to be compared against one.
 * @return QC_TRUE iff this SessionOpts can use the option set offered by other.
 */
QC_BOOL alljoyn_sessionopts_iscompatible(const alljoyn_sessionopts one, const alljoyn_sessionopts other);

/**
 * Compare two SessionOpts.
 *
 * @param one    Options to be compared against other.
 * @param other  Options to be compared against one.
 * @return 0 if the SessionOpts are equal, 1 if one > other, -1 if one < other.
 * @see ajn::SessionOpts::operator<
 * @see ajn::SessionOpts::operator==
 */
int32_t alljoyn_sessionopts_cmp(const alljoyn_sessionopts one, const alljoyn_sessionopts other);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
