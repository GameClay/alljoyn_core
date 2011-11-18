/**
 * @file
 *
 * Implements the BT node database.
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

#include <list>
#include <set>
#include <vector>

#include <qcc/String.h>

#include <alljoyn/MsgArg.h>

#include "BDAddress.h"
#include "BTNodeDB.h"

#define QCC_MODULE "ALLJOYN_BTC"


using namespace std;
using namespace qcc;


namespace ajn {


const BTNodeInfo BTNodeDB::FindNode(const BTBusAddress& addr) const
{
    BTNodeInfo node;
    Lock();
    NodeAddrMap::const_iterator it = addrMap.find(addr);
    if (it != addrMap.end()) {
        node = it->second;
    }
    Unlock();
    return node;
}


const BTNodeInfo BTNodeDB::FindNode(const BDAddress& addr) const
{
    BTNodeInfo node;
    BTBusAddress busAddr(addr, bt::INVALID_PSM);
    Lock();
    NodeAddrMap::const_iterator it = addrMap.lower_bound(busAddr);
    if (it != addrMap.end() && (it->second)->GetBusAddress().addr == addr) {
        node = it->second;
    }
    Unlock();
    return node;
}


const BTNodeInfo BTNodeDB::FindNode(const String& uniqueName) const
{
    BTNodeInfo node;
    Lock();
    NodeNameMap::const_iterator it = nameMap.find(uniqueName);
    if (it != nameMap.end()) {
        node = it->second;
    }
    Unlock();
    return node;
}


BTNodeInfo BTNodeDB::FindDelegateMinion(const BTNodeInfo& start, const BTNodeInfo& skip, bool eirCapable) const
{
    Lock();
    const_iterator next = nodes.find(start);
    const_iterator traditional = nodes.end();
#ifndef NDEBUG
    if (next == End()) {
        String s("Failed to find: " + start->GetBusAddress().addr.ToString());
        DumpTable(s.c_str());
    }
#endif
    assert(next != End());
    do {
        ++next;
        if (next == End()) {
            next = Begin();
        }

        if (!(*next)->IsEIRCapable() && (traditional == nodes.end()) && (*next != skip)) {
            traditional = next;
        }



    } while ((*next != start) && (!(*next)->IsMinion() || (*next == skip) || !(*next)->IsEIRCapable()));
    Unlock();

    if (!eirCapable) {
        next = (*next == start) ? traditional : next;
    }

    return *next;
}


void BTNodeDB::AddNode(const BTNodeInfo& node)
{
    Lock();
    assert(node->IsValid());
    RemoveNode(node);  // remove the old one (if it exists) before adding the new one with updated info

    // Add to the master set
    nodes.insert(node);

    // Add to the address map
    addrMap[node->GetBusAddress()] = node;
    if (!node->GetUniqueName().empty()) {
        nameMap[node->GetUniqueName()] = node;
    }

    if (useExpirations) {
        // Add to the expiration set
        expireSet.insert(node);
    }

    // Add to the connect address multimap
    connMap.insert(std::pair<BTNodeInfo, BTNodeInfo>(node->GetConnectNode(), node));

    // Add to the session ID map
    if (node->GetSessionID() != 0) {
        sessionIDMap[node->GetSessionID()] = node;
    }

    assert(connMap.size() == nodes.size());
    assert(!useExpirations || (expireSet.size() == nodes.size()));
    Unlock();
}


void BTNodeDB::RemoveNode(const BTNodeInfo& node)
{
    Lock();
    NodeAddrMap::iterator it = addrMap.find(node->GetBusAddress());
    if (it != addrMap.end()) {
        BTNodeInfo lnode = it->second;

        // Remove from the master set
        nodes.erase(lnode);

        // Remove from the address map
        addrMap.erase(it);

        // Remove from the session ID map
        if (lnode->GetSessionID() != 0) {
            sessionIDMap.erase(lnode->GetSessionID());
        }

        // Remove from the connect address multimap
        ConnAddrMap::iterator cmit = connMap.lower_bound(lnode->GetConnectNode());
        ConnAddrMap::iterator end = connMap.upper_bound(lnode->GetConnectNode());
        while ((cmit != end) && (cmit->second != lnode)) {
            ++cmit;
        }
        assert(cmit != end);
        if (cmit != end) {
            connMap.erase(cmit);
        }

        // Remove from the name map (if it has a name)
        if (!lnode->GetUniqueName().empty()) {
            nameMap.erase(lnode->GetUniqueName());
        }

        if (useExpirations) {
            // Remove from the exipiration set
            NodeExpireSet::iterator expit = expireSet.find(lnode);
            if (expit == expireSet.end()) {
                // expireSet is out of order
                expireSet.clear();
                expireSet.insert(nodes.begin(), nodes.end());
            } else {
                expireSet.erase(expit);
            }
        }
    }

    assert(connMap.size() == nodes.size());
    assert(!useExpirations || (expireSet.size() == nodes.size()));
    Unlock();
}


void BTNodeDB::Diff(const BTNodeDB& other, BTNodeDB* added, BTNodeDB* removed) const
{
    Lock();
    other.Lock();
    if (added) {
        added->Lock();
    }
    if (removed) {
        removed->Lock();
    }

    const_iterator nodeit;
    NodeAddrMap::const_iterator addrit;

    // Find removed names/nodes
    if (removed) {
        for (nodeit = Begin(); nodeit != End(); ++nodeit) {
            const BTNodeInfo& node = *nodeit;
            addrit = other.addrMap.find(node->GetBusAddress());
            if (addrit == other.addrMap.end()) {
                removed->AddNode(node);
            } else {
                BTNodeInfo diffNode(node->GetBusAddress(), node->GetUniqueName(), node->GetGUID());
                bool include = false;
                const BTNodeInfo& onode = addrit->second;
                NameSet::const_iterator nameit;
                NameSet::const_iterator onameit;
                for (nameit = node->GetAdvertiseNamesBegin(); nameit != node->GetAdvertiseNamesEnd(); ++nameit) {
                    const String& name = *nameit;
                    onameit = onode->FindAdvertiseName(name);
                    if (onameit == onode->GetAdvertiseNamesEnd()) {
                        diffNode->AddAdvertiseName(name);
                        include = true;
                    }
                }
                if (include) {
                    removed->AddNode(diffNode);
                }
            }
        }
    }

    // Find added names/nodes
    if (added) {
        for (nodeit = other.Begin(); nodeit != other.End(); ++nodeit) {
            const BTNodeInfo& onode = *nodeit;
            addrit = addrMap.find(onode->GetBusAddress());
            if (addrit == addrMap.end()) {
                added->AddNode(onode);
            } else {
                BTNodeInfo diffNode(onode->GetBusAddress(), onode->GetUniqueName(), onode->GetGUID());
                bool include = false;
                const BTNodeInfo& node = addrit->second;
                NameSet::const_iterator nameit;
                NameSet::const_iterator onameit;
                for (onameit = onode->GetAdvertiseNamesBegin(); onameit != onode->GetAdvertiseNamesEnd(); ++onameit) {
                    const String& oname = *onameit;
                    nameit = node->FindAdvertiseName(oname);
                    if (nameit == node->GetAdvertiseNamesEnd()) {
                        diffNode->AddAdvertiseName(oname);
                        include = true;
                    }
                }
                if (include) {
                    added->AddNode(diffNode);
                }
            }
        }
    }

    if (removed) {
        removed->Unlock();
    }
    if (added) {
        added->Unlock();
    }
    other.Unlock();
    Unlock();
}


void BTNodeDB::NodeDiff(const BTNodeDB& other, BTNodeDB* added, BTNodeDB* removed) const
{
    Lock();
    other.Lock();
    if (added) {
        added->Lock();
    }
    if (removed) {
        removed->Lock();
    }

    const_iterator nodeit;
    NodeAddrMap::const_iterator addrit;

    // Find removed names/nodes
    if (removed) {
        for (nodeit = Begin(); nodeit != End(); ++nodeit) {
            const BTNodeInfo& node = *nodeit;
            addrit = other.addrMap.find(node->GetBusAddress());
            if (addrit == other.addrMap.end()) {
                removed->AddNode(node);
            }
        }
    }

    // Find added names/nodes
    if (added) {
        for (nodeit = other.Begin(); nodeit != other.End(); ++nodeit) {
            const BTNodeInfo& onode = *nodeit;
            addrit = addrMap.find(onode->GetBusAddress());
            if (addrit == addrMap.end()) {
                added->AddNode(onode);
            }
        }
    }

    if (removed) {
        removed->Unlock();
    }
    if (added) {
        added->Unlock();
    }
    other.Unlock();
    Unlock();
}


void BTNodeDB::UpdateDB(const BTNodeDB* added, const BTNodeDB* removed, bool removeNodes)
{
    // Remove names/nodes
    Lock();
    if (removed) {
        const_iterator rit;
        for (rit = removed->Begin(); rit != removed->End(); ++rit) {
            BTNodeInfo rnode = *rit;
            NodeAddrMap::iterator it = addrMap.find(rnode->GetBusAddress());
            if (it != addrMap.end()) {
                // Remove names from node
                BTNodeInfo node = it->second;
                if (&(*node) == &(*rnode)) {
                    // The exact same instance of node is in the removed DB so
                    // just remove the node so that the names don't get
                    // corrupted in the removed DB.
                    RemoveNode(node);

                } else {
                    // node and rnode are different instances so there is no
                    // chance of corrupting the list of names in rnode.
                    NameSet::const_iterator rnameit;
                    for (rnameit = rnode->GetAdvertiseNamesBegin(); rnameit != rnode->GetAdvertiseNamesEnd(); ++rnameit) {
                        const String& rname = *rnameit;
                        node->RemoveAdvertiseName(rname);
                    }
                    if (removeNodes && (node->AdvertiseNamesEmpty())) {
                        // Remove node with no advertise names
                        RemoveNode(node);
                    }
                }
            } // else not in DB so ignore it.
        }
    }

    if (added) {
        // Add names/nodes
        const_iterator ait;
        for (ait = added->Begin(); ait != added->End(); ++ait) {
            BTNodeInfo anode = *ait;
            NodeAddrMap::iterator it = addrMap.find(anode->GetBusAddress());
            if (it == addrMap.end()) {
                // New node
                BTNodeInfo connNode = FindNode(anode->GetConnectNode()->GetBusAddress());
                if (connNode->IsValid()) {
                    anode->SetConnectNode(connNode);
                }
                assert(anode->GetConnectNode()->IsValid());
                AddNode(anode);
            } else {
                // Add names to existing node
                BTNodeInfo node = it->second;
                NameSet::const_iterator anameit;
                for (anameit = anode->GetAdvertiseNamesBegin(); anameit != anode->GetAdvertiseNamesEnd(); ++anameit) {
                    const String& aname = *anameit;
                    node->AddAdvertiseName(aname);
                }
                // Update the connect node map
                ConnAddrMap::iterator cmit = connMap.lower_bound(node->GetConnectNode());
                ConnAddrMap::iterator end = connMap.upper_bound(node->GetConnectNode());
                while ((cmit != end) && (cmit->second != node)) {
                    ++cmit;
                }
                if (cmit != end) {
                    connMap.erase(cmit);
                }
                BTNodeInfo connNode = FindNode(anode->GetConnectNode()->GetBusAddress());
                if (!connNode->IsValid()) {
                    connNode = added->FindNode(anode->GetConnectNode()->GetBusAddress());
                }
                assert(connNode->IsValid());
                node->SetConnectNode(connNode);
                connMap.insert(pair<BTNodeInfo, BTNodeInfo>(node->GetConnectNode(), node));
                // Update the UUIDRev
                node->SetUUIDRev(anode->GetUUIDRev());
                if (useExpirations) {
                    // Update the expire time
                    expireSet.erase(node);
                    node->SetExpireTime(anode->GetExpireTime());
                    expireSet.insert(node);
                }
                if ((node->GetUniqueName() != anode->GetUniqueName()) && !anode->GetUniqueName().empty()) {
                    if (!node->GetUniqueName().empty()) {
                        nameMap.erase(node->GetUniqueName());
                    }
                    node->SetUniqueName(anode->GetUniqueName());
                    nameMap[node->GetUniqueName()] = node;
                }
            }
        }
    }

    assert(connMap.size() == nodes.size());
    assert(!useExpirations || (expireSet.size() == nodes.size()));
    Unlock();
}


void BTNodeDB::RemoveExpiration()
{
    if (useExpirations) {
        Lock();
        uint64_t expireTime = numeric_limits<uint64_t>::max();
        expireSet.clear();
        iterator it = nodes.begin();
        while (it != nodes.end()) {
            BTNodeInfo node = *it;
            node->SetExpireTime(expireTime);
            expireSet.insert(node);
            ++it;
        }
        assert(expireSet.size() == nodes.size());
        Unlock();
    } else {
        QCC_LogError(ER_FAIL, ("Called RemoveExpiration on BTNodeDB instance initialized without expiration support."));
        assert(false);
    }
}


void BTNodeDB::RefreshExpiration(uint32_t expireDelta)
{
    if (useExpirations) {
        Lock();
        Timespec now;
        GetTimeNow(&now);
        uint64_t expireTime = now.GetAbsoluteMillis() + expireDelta;
        expireSet.clear();
        iterator it = nodes.begin();
        while (it != nodes.end()) {
            BTNodeInfo node = *it;
            node->SetExpireTime(expireTime);
            expireSet.insert(node);
            ++it;
        }
        assert(expireSet.size() == nodes.size());
        Unlock();
    } else {
        QCC_LogError(ER_FAIL, ("Called RefreshExpiration on BTNodeDB instance initialized without expiration support."));
        assert(false);
    }
}


void BTNodeDB::RefreshExpiration(const BTNodeInfo& connNode, uint32_t expireDelta)
{
    if (useExpirations) {
        Lock();
        ConnAddrMap::iterator cmit = connMap.lower_bound(connNode);
        ConnAddrMap::iterator end = connMap.upper_bound(connNode);

        Timespec now;
        GetTimeNow(&now);
        uint64_t expireTime = now.GetAbsoluteMillis() + expireDelta;

        while (cmit != end) {
            assert(cmit->first == cmit->second->GetConnectNode());
            expireSet.erase(cmit->second);
            cmit->second->SetExpireTime(expireTime);
            cmit->second->SetUUIDRev(connNode->GetUUIDRev());
            expireSet.insert(cmit->second);
            ++cmit;
        }

        assert((end == connMap.end()) || (connNode < (end->second->GetConnectNode())));
        assert(connMap.size() == nodes.size());
        assert(expireSet.size() == nodes.size());
        Unlock();
    } else {
        QCC_LogError(ER_FAIL, ("Called RefreshExpiration on BTNodeDB instance initialized without expiration support."));
        assert(false);
    }
}


void BTNodeDB::NodeSessionLost(SessionId sessionID)
{
    Lock();
    SessionIDMap::iterator it = sessionIDMap.find(sessionID);
    if (it != sessionIDMap.end()) {
        BTNodeInfo lnode = it->second;

        sessionIDMap.erase(sessionID);
        lnode->SetSessionID(0);
        lnode->SetSessionState(_BTNodeInfo::NO_SESSION);
    }
    Unlock();
}


void BTNodeDB::UpdateNodeSessionID(SessionId sessionID, const BTNodeInfo& node)
{
    Lock();
    NodeAddrMap::iterator it = addrMap.find(node->GetBusAddress());
    if (it != addrMap.end()) {
        BTNodeInfo lnode = it->second;

        SessionIDMap::iterator sit = sessionIDMap.find(lnode->GetSessionID());
        if (sit != sessionIDMap.end()) {
            sessionIDMap.erase(sit);
        }

        lnode->SetSessionID(sessionID);
        lnode->SetSessionState(_BTNodeInfo::SESSION_UP);

        sessionIDMap[sessionID] = lnode;
    }
    Unlock();
}


#ifndef NDEBUG
void BTNodeDB::DumpTable(const char* info) const
{
    Lock();
    const_iterator nodeit;
    QCC_DbgPrintf(("Node DB (%s):", info));
    for (nodeit = Begin(); nodeit != End(); ++nodeit) {
        const BTNodeInfo& node = *nodeit;
        NameSet::const_iterator nameit;
        String expireTime;
        if (node->GetExpireTime() == numeric_limits<uint64_t>::max()) {
            expireTime = "<infinite>";
        } else {
            Timespec now;
            GetTimeNow(&now);
            int64_t delta = node->GetExpireTime() - now.GetAbsoluteMillis();
            expireTime = I64ToString(delta, 10, (delta < 0) ? 5 : 4, '0');
            expireTime = expireTime.substr(0, expireTime.size() - 3) + '.' + expireTime.substr(expireTime.size() - 3);
        }
        QCC_DbgPrintf(("    %s (connect addr: %s  unique name: \"%s\"  uuidRev: %08x  direct: %s  expire time: %s):",
                       node->ToString().c_str(),
                       node->GetConnectNode()->ToString().c_str(),
                       node->GetUniqueName().c_str(),
                       node->GetUUIDRev(),
                       node->IsDirectMinion() ? "true" : "false",
                       expireTime.c_str()));
        QCC_DbgPrintf(("         Advertise names:"));
        for (nameit = node->GetAdvertiseNamesBegin(); nameit != node->GetAdvertiseNamesEnd(); ++nameit) {
            QCC_DbgPrintf(("            %s", nameit->c_str()));
        }
        QCC_DbgPrintf(("         Find names:"));
        for (nameit = node->GetFindNamesBegin(); nameit != node->GetFindNamesEnd(); ++nameit) {
            QCC_DbgPrintf(("            %s", nameit->c_str()));
        }
    }
    Unlock();
}
#endif

}
