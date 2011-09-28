/**
 * @file
 * Class for encapsulating Session option information.
 */

/******************************************************************************
 * Copyright 2010-2011, Qualcomm Innovation Center, Inc.
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
#include <qcc/Util.h>
#include <alljoyn/MsgArg.h>

#include <assert.h>

#include "SessionInternal.h"

#define QCC_MODULE "ALLJOYN"

using namespace std;

namespace ajn {

/** SessionOpts key values */
#define SESSIONOPTS_TRAFFIC     "traf"
#define SESSIONOPTS_ISMULTICAST "multi"
#define SESSIONOPTS_PROXIMITY   "prox"
#define SESSIONOPTS_TRANSPORTS  "trans"

bool SessionOpts::IsCompatible(const SessionOpts& other) const
{
    /* No overlapping transports means opts are not compatible */
    if (0 == (transports & other.transports)) {
        return false;
    }

    /* Not overlapping traffic types means opts are not compatible */
    if (0 == (traffic & other.traffic)) {
        return false;
    }

    /* Not overlapping proximities means opts are not compatible */
    if (0 == (proximity & other.proximity)) {
        return false;
    }

    /* Note that isMultipoint is not a condition of compatibility */

    return true;
}

QStatus GetSessionOpts(const MsgArg& msgArg, SessionOpts& opts)
{
    const MsgArg* dictArray;
    size_t numDictEntries;
    QStatus status = msgArg.Get("a{sv}", &numDictEntries, &dictArray);
    if (status == ER_OK) {
        for (size_t n = 0; n < numDictEntries; ++n) {
            const char* key = dictArray[n].v_dictEntry.key->v_string.str;
            const MsgArg* val = dictArray[n].v_dictEntry.val->v_variant.val;

            dictArray[n].Get("{sv}", &key, &val);
            if (::strcmp(SESSIONOPTS_TRAFFIC, key) == 0) {
                uint8_t tmp;
                val->Get("y", &tmp);
                opts.traffic = static_cast<SessionOpts::TrafficType>(tmp);
            } else if (::strcmp(SESSIONOPTS_ISMULTICAST, key) == 0) {
                val->Get("b", &opts.isMultipoint);
            } else if (::strcmp(SESSIONOPTS_PROXIMITY, key) == 0) {
                val->Get("y", &opts.proximity);
            } else if (::strcmp(SESSIONOPTS_TRANSPORTS, key) == 0) {
                val->Get("q", &opts.transports);
            }
        }
    }
    return status;
}

void SetSessionOpts(const SessionOpts& opts, MsgArg& msgArg)
{
    MsgArg trafficArg("y", opts.traffic);
    MsgArg isMultiArg("b", opts.isMultipoint);
    MsgArg proximityArg("y", opts.proximity);
    MsgArg transportsArg("q", opts.transports);

    MsgArg entries[4];
    entries[0].Set("{sv}", SESSIONOPTS_TRAFFIC, &trafficArg);
    entries[1].Set("{sv}", SESSIONOPTS_ISMULTICAST, &isMultiArg);
    entries[2].Set("{sv}", SESSIONOPTS_PROXIMITY, &proximityArg);
    entries[3].Set("{sv}", SESSIONOPTS_TRANSPORTS, &transportsArg);
    QStatus status = msgArg.Set("a{sv}", ArraySize(entries), entries);
    if (status == ER_OK) {
        msgArg.Stabilize();
    } else {
        QCC_LogError(status, ("Failed to set SessionOpts message arg"));
    }

}

}

struct _alljoyn_sessionopts_handle {
    /* Empty by design, this is just to allow the type restrictions to save coders from themselves */
};

alljoyn_sessionopts alljoyn_sessionopts_create(uint8_t traffic, QC_BOOL isMultipoint,
                                               uint8_t proximity, alljoyn_transportmask transports)
{
    return (alljoyn_sessionopts) new ajn::SessionOpts((ajn::SessionOpts::TrafficType)traffic, isMultipoint == QC_TRUE ? true : false,
                                                      (ajn::SessionOpts::Proximity)proximity, (ajn::TransportMask)transports);
}

void alljoyn_sessionopts_destroy(alljoyn_sessionopts opts)
{
    delete (ajn::SessionOpts*)opts;
}

uint8_t alljoyn_sessionopts_traffic(const alljoyn_sessionopts opts)
{
    return ((const ajn::SessionOpts*)opts)->traffic;
}

QC_BOOL alljoyn_sessionopts_multipoint(const alljoyn_sessionopts opts)
{
    return (((const ajn::SessionOpts*)opts)->isMultipoint ? QC_TRUE : QC_FALSE);
}

uint8_t alljoyn_sessionopts_proximity(const alljoyn_sessionopts opts)
{
    return ((const ajn::SessionOpts*)opts)->proximity;
}

alljoyn_transportmask alljoyn_sessionopts_transports(const alljoyn_sessionopts opts)
{
    return ((const ajn::SessionOpts*)opts)->transports;
}
