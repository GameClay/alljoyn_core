/**
 * @file
 * BusObject responsible for controlling/handling Bluetooth delegations.
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
#ifndef _ALLJOYN_BTNODEDB_H
#define _ALLJOYN_BTNODEDB_H

#include <qcc/platform.h>

#include <limits>
#include <set>
#include <vector>

#include <qcc/ManagedObj.h>
#include <qcc/Mutex.h>
#include <qcc/String.h>
#include <qcc/StringUtil.h>
#include <qcc/time.h>

#include "BDAddress.h"
#include "BTBusAddress.h"
#include "BTNodeInfo.h"


namespace ajn {

/** Bluetooth Node Database */
class BTNodeDB {
  public:
    /** Convenience iterator typedef. */
    typedef std::set<BTNodeInfo>::iterator iterator;

    /** Convenience const_iterator typedef. */
    typedef std::set<BTNodeInfo>::const_iterator const_iterator;


    BTNodeDB(bool useExpirations = false) : useExpirations(useExpirations) { }

    /**
     * Find a node given a Bluetooth device address and a PSM.
     *
     * @param addr  Bluetooth device address
     * @param psm   L2CAP PSM for the AllJoyn service
     *
     * @return  BTNodeInfo of the found node (BTNodeInfo::IsValid() will return false if not found)
     */
    const BTNodeInfo FindNode(const BDAddress& addr, uint16_t psm) const { BTBusAddress busAddr(addr, psm); return FindNode(busAddr); }

    /**
     * Find a node given a bus address.
     *
     * @param addr  bus address
     *
     * @return  BTNodeInfo of the found node (BTNodeInfo::IsValid() will return false if not found)
     */
    const BTNodeInfo FindNode(const BTBusAddress& addr) const;

    /**
     * Find a node given a unique name of the daemon running on a node.
     *
     * @param uniqueName    unique name of the daemon running on a node
     *
     * @return  BTNodeInfo of the found node (BTNodeInfo::IsValid() will return false if not found)
     */
    const BTNodeInfo FindNode(const qcc::String& uniqueName) const;

    /**
     * Find the first node with given a Bluetooth device address.  (Generally,
     * the bluetooth device address should be unique, but it is not completely
     * impossible for 2 instances for AllJoyn to be running on one physical
     * device with the same Bluetooth device address but with different PSMs.)
     *
     * @param addr  Bluetooth device address
     *
     * @return  BTNodeInfo of the found node (BTNodeInfo::IsValid() will return false if not found)
     */
    const BTNodeInfo FindNode(const BDAddress& addr) const;

    void FindNodes(const BDAddress& addr, const_iterator& begin, const_iterator& end)
    {
        BTBusAddress lower(addr, 0x0000);
        BTBusAddress upper(addr, 0xffff);
        Lock();
        begin = nodes.lower_bound(lower);
        end = nodes.upper_bound(upper);
        Unlock();
    }


    /**
     * Find a minion starting with the specified start node in the set of
     * nodes, and skipping over the skip node.  If any nodes (beside start and
     * skip) are EIR capable, those nodes will be selected.  Non-EIR capable
     * nodes will only be considered if eirCapable is false and there are no
     * EIR capable nodes beyond start and skip.
     *
     * @param start         Node in the DB to use as a starting point for the search
     * @param skip          Node in the DB to skip over if next in line
     * @param eirCapable    Flag to indicate if only EIR capable nodes should be considered
     *
     * @return  BTNodeInfo of the next delegate minion.  Will be the same as start if none found.
     */
    BTNodeInfo FindDelegateMinion(const BTNodeInfo& start, const BTNodeInfo& skip, bool eirCapable) const;

    /**
     * Add a node to the DB with no expiration time.
     *
     * @param node  Node to be added to the DB.
     */
    void AddNode(const BTNodeInfo& node);

    /**
     * Remove a node from the DB.
     *
     * @param node  Node to be removed from the DB.
     */
    void RemoveNode(const BTNodeInfo& node);

    /**
     * Determine the difference between this DB and another DB.  Nodes that
     * appear in only one or the other DB will be copied (i.e., share the same
     * referenced data) to the added/removed DBs as appropriate.  Nodes that
     * appear in both this and the other DB but have differences in their set
     * of advertised names will result in fully independed copies of the node
     * information with only the appropriate name changes being put in the
     * added/removed DBs as appropriate.  It is possible for a node to appear
     * in both the added DB and removed DB if that node had advertised names
     * both added and removed.
     *
     * @param other         Other DB for comparision
     * @param added[out]    If non-null, the set of nodes (and names) found in
     *                      other but not in us
     * @param removed[out]  If non-null, the set of nodes (and names) found in
     *                      us but not in other
     */
    void Diff(const BTNodeDB& other, BTNodeDB* added, BTNodeDB* removed) const;

    /**
     * Determine the difference between this DB and another DB in terms of
     * nodes only.  In other words, nodes in this DB that do not appear in the
     * other DB will be copied into the removed DB while nodes in the other DB
     * that do not appear in this DB will be copied to the added DB.
     * Differences in names will be ignored.
     *
     * @param other         Other DB for comparision
     * @param added[out]    If non-null, the set of nodes found in other but not in us
     * @param removed[out]  If non-null, the set of nodes found in us but not in other
     */
    void NodeDiff(const BTNodeDB& other, BTNodeDB* added, BTNodeDB* removed) const;

    /**
     * Applies the differences found in BTNodeDB::Diff to us.
     *
     * @param added         If non-null, nodes (and names) to add
     * @param removed       If non-null, name to remove from nodes
     * @param removeNodes   Optional parameter that defaults to true
     *                      - true: remove nodes that become empty due to all
     *                              names being removed
     *                      - false: keep empty nodes
     */
    void UpdateDB(const BTNodeDB* added, const BTNodeDB* removed, bool removeNodes = true);

    /**
     * Removes the expiration time of all nodes (sets expiration to end-of-time).
     */
    void RemoveExpiration();

    /**
     * Updates the expiration time of all nodes.
     *
     * @param expireDelta   Number of milliseconds from now to set the
     *                      expiration time.
     */
    void RefreshExpiration(uint32_t expireDelta);

    /**
     * Updates the expiration time of all nodes that may be connected to via
     * connAddr.
     *
     * @param connNode      Node accepting connections on behalf of other nodes.
     * @param expireDelta   Number of milliseconds from now to set the
     *                      expiration time.
     */
    void RefreshExpiration(const BTNodeInfo& connNode, uint32_t expireDelta);

    /**
     * Fills a BTNodeDB with the set of nodes that are connectable via
     * connNode.
     *
     * @param connNode  BTNodeInfo of the device accepting connections on
     *                  behalf of other nodes.
     * @param subDB     Sub-set BTNodeDB to store the found nodes in.
     */
    void GetNodesFromConnectNode(const BTNodeInfo& connNode, BTNodeDB& subDB) const
    {
        Lock();
        ConnAddrMap::const_iterator cmit = connMap.lower_bound(connNode);
        ConnAddrMap::const_iterator end = connMap.upper_bound(connNode);

        while (cmit != end) {
            subDB.AddNode(cmit->second);
            ++cmit;
        }
        Unlock();
    }

    void PopExpiredNodes(BTNodeDB& expiredDB)
    {
        Lock();
        qcc::Timespec now;
        qcc::GetTimeNow(&now);
        while (!expireSet.empty() && ((*expireSet.begin())->GetExpireTime() <= now.GetAbsoluteMillis())) {
            BTNodeInfo node = *expireSet.begin();
            RemoveNode(node);
            expiredDB.AddNode(node);
        }
        Unlock();
    }

    uint64_t NextNodeExpiration()
    {
        if (!expireSet.empty()) {
            return (*expireSet.begin())->GetExpireTime();
        }
        return std::numeric_limits<uint64_t>::max();
    }


    void NodeSessionLost(SessionId sessionID);
    void UpdateNodeSessionID(SessionId sessionID, const BTNodeInfo& node);

    /**
     * Lock the mutex that protects the database from unsafe access.
     */
    void Lock() const { lock.Lock(MUTEX_CONTEXT); }

    /**
     * Release the the mutex that protects the database from unsafe access.
     */
    void Unlock() const { lock.Unlock(MUTEX_CONTEXT); }

    /**
     * Get the begin iterator for the set of nodes.
     *
     * @return  const_iterator pointing to the first node
     */
    const_iterator Begin() const { return nodes.begin(); }

    /**
     * Get the end iterator for the set of nodes.
     *
     * @return  const_iterator pointing to one past the last node
     */
    const_iterator End() const { return nodes.end(); }

    /**
     * Get the number of entries in the node DB.
     *
     * @return  the number of entries in the node DB
     */
    size_t Size() const
    {
        Lock();
        size_t size = nodes.size();
        Unlock();
        return size;
    }

    /**
     * Clear out the DB.
     */
    void Clear() { nodes.clear(); addrMap.clear(); nameMap.clear(); connMap.clear(); expireSet.clear(); }

#ifndef NDEBUG
    void DumpTable(const char* info) const;
#else
    void DumpTable(const char* info) const { }
#endif


  private:

    class ExpireNodeComp {
      public:
        bool operator()(const BTNodeInfo& lhs, const BTNodeInfo& rhs) const
        {
            return ((lhs->GetExpireTime() < rhs->GetExpireTime()) ||
                    ((lhs->GetExpireTime() == rhs->GetExpireTime()) && (lhs < rhs)));
        }
    };

    /** Convenience typedef for the lookup table keyed off the bus address. */
    typedef std::map<BTBusAddress, BTNodeInfo> NodeAddrMap;

    /** Convenience typedef for the lookup table keyed off the bus address. */
    typedef std::multimap<BTNodeInfo, BTNodeInfo> ConnAddrMap;

    /** Convenience typedef for the lookup table keyed off the unique bus name. */
    typedef std::map<qcc::String, BTNodeInfo> NodeNameMap;

    /** Convenience typedef for the lookup table sorted by expiration time. */
    typedef std::set<BTNodeInfo, ExpireNodeComp> NodeExpireSet;

    /** Convenience typedef for the lookup table sorted by Session IDs. */
    typedef std::map<SessionId, BTNodeInfo> SessionIDMap;

    std::set<BTNodeInfo> nodes;     /**< The node DB storage. */
    NodeAddrMap addrMap;            /**< Lookup table keyed off the bus address. */
    NodeNameMap nameMap;            /**< Lookup table keyed off the unique bus name. */
    NodeExpireSet expireSet;        /**< Lookup table sorted by the expiration time. */
    ConnAddrMap connMap;            /**< Lookup table keyed off the connect node. */
    SessionIDMap sessionIDMap;

    mutable qcc::Mutex lock;        /**< Mutext to protect the DB. */

    const bool useExpirations;
};

} // namespace ajn

#endif
