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


BTNodeInfo BTNodeDB::FindDirectMinion(const BTNodeInfo& start, const BTNodeInfo& skip) const
{
    Lock();
    const_iterator next = nodes.find(start);
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
    } while ((!(*next)->IsDirectMinion() || (*next == skip)) && ((*next) != start));
    BTNodeInfo node = *next;
    Unlock();
    return node;
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

    // Add to the expiration set
    expireSet.insert(node);

    // Add to the connect address multimap
    connMap.insert(std::pair<BTBusAddress, BTNodeInfo>(node->GetConnectAddress(), node));
    assert(connMap.size() == expireSet.size());
    assert(expireSet.size() == nodes.size());
    Unlock();
}


void BTNodeDB::RemoveNode(const BTNodeInfo& node)
{
    Lock();
    NodeAddrMap::iterator it = addrMap.find(node->GetBusAddress());
    if (it != addrMap.end()) {
        BTNodeInfo lnode = it->second;

        // Remove from the address map
        addrMap.erase(it);

        // Remove from the connect address multimap
        ConnAddrMap::iterator cmit = connMap.lower_bound(lnode->GetConnectAddress());
        ConnAddrMap::iterator end = connMap.upper_bound(lnode->GetConnectAddress());
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

        // Remove from the exipiration set
        expireSet.erase(lnode);

        // Remove from the master set
        nodes.erase(lnode);
        assert(connMap.size() == expireSet.size());
    }
    assert(expireSet.size() == nodes.size());
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
                ConnAddrMap::iterator cmit = connMap.lower_bound(node->GetConnectAddress());
                ConnAddrMap::iterator end = connMap.upper_bound(node->GetConnectAddress());
                while ((cmit != end) && (cmit->second != node)) {
                    ++cmit;
                }
                if (cmit != end) {
                    connMap.erase(cmit);
                }
                BTNodeInfo connNode = FindNode(anode->GetConnectAddress());
                if (!connNode->IsValid()) {
                    connNode = added->FindNode(anode->GetConnectAddress());
                }
                node->SetConnectNode(connNode);
                connMap.insert(pair<BTBusAddress, BTNodeInfo>(node->GetConnectAddress(), node));
                // Update the UUIDRev
                node->SetUUIDRev(anode->GetUUIDRev());
                // Update the expire time
                expireSet.erase(node);
                node->SetExpireTime(anode->GetExpireTime());
                expireSet.insert(node);
                assert(connMap.size() == expireSet.size());
            }
        }
    }

    assert(expireSet.size() == nodes.size());
    Unlock();
}


void BTNodeDB::RemoveExpiration()
{
    Lock();
    uint64_t expireTime = numeric_limits<uint64_t>::max();
    iterator it = expireSet.begin();
    while (it != expireSet.end()) {
        BTNodeInfo node = *it;
        expireSet.erase(it++);
        node->SetExpireTime(expireTime);
        expireSet.insert(node);
    }
    assert(expireSet.size() == nodes.size());
    Unlock();
}


void BTNodeDB::RefreshExpiration(uint32_t expireDelta)
{
    Lock();
    Timespec now;
    GetTimeNow(&now);
    uint64_t expireTime = now.GetAbsoluteMillis() + expireDelta;
    iterator it = expireSet.begin();
    while (it != expireSet.end()) {
        BTNodeInfo node = *it;
        expireSet.erase(it++);
        node->SetExpireTime(expireTime);
        expireSet.insert(node);
    }
    assert(expireSet.size() == nodes.size());
    Unlock();
}


void BTNodeDB::RefreshExpiration(const BTBusAddress& connAddr, uint32_t expireDelta)
{
    Lock();
    ConnAddrMap::iterator cmit = connMap.lower_bound(connAddr);
    ConnAddrMap::iterator end = connMap.upper_bound(connAddr);

    Timespec now;
    GetTimeNow(&now);
    uint64_t expireTime = now.GetAbsoluteMillis() + expireDelta;

    while (cmit != end) {
        assert(cmit->first == cmit->second->GetConnectAddress());
        expireSet.erase(cmit->second);
        cmit->second->SetExpireTime(expireTime);
        expireSet.insert(cmit->second);
        ++cmit;
    }

    assert((end == connMap.end()) || (connAddr < (end->second->GetConnectAddress())));
    assert(connMap.size() == expireSet.size());
    assert(expireSet.size() == nodes.size());
    Unlock();
}


void BTNodeDB::RefreshExpiration(const BTNodeInfo& node, uint32_t expireDelta)
{
    Lock();
    NodeAddrMap::iterator it = addrMap.find(node->GetBusAddress());
    if (it != addrMap.end()) {
        BTNodeInfo lnode = it->second;

        Timespec now;
        GetTimeNow(&now);
        uint64_t expireTime = now.GetAbsoluteMillis() + expireDelta;

        expireSet.erase(lnode);
        lnode->SetExpireTime(expireTime);
        expireSet.insert(lnode);
    }
    assert(connMap.size() == expireSet.size());
    assert(expireSet.size() == nodes.size());
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
                       node->GetBusAddress().ToString().c_str(),
                       node->GetConnectAddress().ToString().c_str(),
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
