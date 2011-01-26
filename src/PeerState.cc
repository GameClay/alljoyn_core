/**
 * @file
 * This class maintains information about peers connected to the bus.
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

#include <algorithm>

#include <qcc/Debug.h>
#include <qcc/Crypto.h>
#include <qcc/time.h>

#include "PeerState.h"
#include "AllJoynCrypto.h"

#include <Status.h>

#define QCC_MODULE "ALLJOYN"

using namespace std;
using namespace qcc;

namespace ajn {

uint32_t _PeerState::EstimateTimestamp(uint32_t remote)
{
    uint32_t local = qcc::GetTimestamp();
    int32_t delta = static_cast<int32_t>(local - remote);
    int32_t oldOffset = clockOffset;

    /*
     * Clock drift adjustment. Make remote re-confirm minimum occasionally.
     * This will adjust for clock drift that is less than 100 ppm.
     */
    if ((local - lastDriftAdjustTime) > 10000) {
        lastDriftAdjustTime = local;
        ++clockOffset;
    }

    if (((oldOffset - delta) > 0) || firstClockAdjust) {
        QCC_DbgHLPrintf(("Updated clock offset old %d, new %d", clockOffset, delta));
        clockOffset = delta;
        firstClockAdjust = false;
    }

    return remote + static_cast<uint32_t>(clockOffset);
}

bool _PeerState::IsValidSerial(uint32_t serial, bool secure, bool unreliable)
{
    /*
     * Serial 0 is always invalid.
     */
    if (serial == 0) {
        return false;
    }

#if 0
    /*
     * Check for first time through.
     */
    if (expectedSerial == 0) {
        expectedSerial = serial;
    }

    /*
     * If the gap is +ve advance expectedSerial otherwise check serial is within the window.
     */
    int32_t gap = 1 + (int32_t)serial - (int32_t)expectedSerial;
    if (gap > 0) {
        /*
         * Allow gaps that much larger than the window size but not so large that a clever
         * attacker can do a replay attack by causing the window to wrap around.
         */
        if (secure && gap >= 50000) {
            QCC_DbgHLPrintf(("Invalid serial %u: advanced too far", serial));
            return false;
        }
        /*
         * Serial 0 is invalid so we never expect it.
         */
        expectedSerial = (serial + 1) ? (serial + 1) : 1;
        /*
         * Unset bits for serials that are now allowed.
         */
        if (gap >= (int32_t)window.size()) {
            window.reset();
        } else {
            uint32_t i = serial;
            while (gap--) {
                window[++i % window.size()] = 0;
            }
            gap = 0;
        }
    } else if (gap < -((int32_t)window.size())) {
        /*
         * We hold secure messages to a higher standard
         */
        if (secure) {
            QCC_DbgHLPrintf(("Invalid serial %u: outside window %d", serial, gap));
            return false;
        }
    } else if (window[serial % window.size()]) {
        QCC_DbgHLPrintf(("Invalid serial %u: in window %s", serial, window.to_string<char, char_traits<char>, allocator<char> >().c_str()));
        return false;
    }
    window[serial % window.size()] = 1;
    // QCC_DbgPrintf(("Window %s", window.to_string< char,char_traits<char>,allocator<char> >().c_str()));
    /*
     * Unreliable messages are only valid when received in order.
     */
    if (unreliable && (gap < 0)) {
        return false;
    } else {
        return true;
    }
#else
    bool ret = false;
    const size_t winSize = sizeof(window) / sizeof(window[0]);
    uint32_t* entry = window + (serial % winSize);
    if (*entry != serial) {
        *entry = serial;
        ret = true;
    }
    return ret;
#endif

}

PeerState PeerStateTable::GetPeerState(const qcc::String& busName)
{
    lock.Lock();
    QCC_DbgHLPrintf(("PeerStateTable::GetPeerState() %s state for %s", peerMap.count(busName) ? "got" : "no", busName.c_str()));
    PeerState result = peerMap[busName];
    lock.Unlock();

    return result;
}

PeerState PeerStateTable::GetPeerState(const qcc::String& uniqueName, const qcc::String& aliasName)
{
    assert(uniqueName[0] == ':');
    PeerState result;
    lock.Lock();
    std::map<const qcc::String, PeerState>::iterator iter = peerMap.find(uniqueName);
    if (iter == peerMap.end()) {
        QCC_DbgHLPrintf(("PeerStateTable::GetPeerState() no state stored for %s aka %s", uniqueName.c_str(), aliasName.c_str()));
        result = peerMap[aliasName];
        peerMap[uniqueName] = result;
    } else {
        QCC_DbgHLPrintf(("PeerStateTable::GetPeerState() got state for %s aka %s", uniqueName.c_str(), aliasName.c_str()));
        result = iter->second;
        peerMap[aliasName] = result;
    }
    lock.Unlock();
    return result;
}

void PeerStateTable::DelPeerState(const qcc::String& busName)
{
    lock.Lock();
    QCC_DbgHLPrintf(("PeerStateTable::DelPeerState() %s for %s", peerMap.count(busName) ? "remove state" : "no state to remove", busName.c_str()));
    peerMap.erase(busName);
    if (groupKey.IsValid()) {
        /*
         * If none of the remaining peers are secure clear the group key.
         */
        std::map<const qcc::String, PeerState>::iterator iter = peerMap.begin();
        for (iter = peerMap.begin(); iter != peerMap.end(); ++iter) {
            if (iter->second->IsSecure() && !iter->second->IsLocalPeer()) {
                break;
            }
        }
        if (iter == peerMap.end()) {
            QCC_DbgHLPrintf(("Deleting stale group key"));
            groupKey.Erase();
        }
    }
    lock.Unlock();
}

void PeerStateTable::GetGroupKeyAndNonce(qcc::KeyBlob& key, qcc::KeyBlob& nonce)
{
    /*
     * If we don't have a group key generate it now.
     */
    if (!groupKey.IsValid()) {
        QCC_DbgHLPrintf(("Allocating fresh group key"));
        groupKey.Rand(Crypto_AES::AES128_SIZE, KeyBlob::AES);
        groupNonce.Rand(Crypto::NonceBytes, KeyBlob::GENERIC);
    }
    key = groupKey;
    nonce = groupNonce;
}

}
