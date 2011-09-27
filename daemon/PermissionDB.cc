/**
 * @file
 * AllJoyn permission database class
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

#include <set>
#include <map>

#include <qcc/Debug.h>
#include <qcc/Logger.h>
#include <qcc/XmlElement.h>
#include <qcc/Log.h>
#include <qcc/FileStream.h>

#include "PermissionDB.h"
#include "BusEndpoint.h"

#define QCC_MODULE "ALLJOYN_PERMISSION"

using namespace std;
using namespace qcc;

namespace ajn {
/**
 * Get the assigned permissions of the installed Android package with specific user id
 */
static bool GetPermissionsAssignedByAndriod(uint32_t uid, std::set<qcc::String>& permissions);

bool PermissionDB::IsBluetoothAllowed(BusEndpoint& endpoint)
{
    QCC_DbgTrace(("PermissionDB::IsBluetoothAllowed(endpoint =%s)", endpoint.GetUniqueName().c_str()));
    bool allowed = true;

#if defined(QCC_OS_ANDROID)
    // For Android app, permissions "android.permission.BLUETOOTH" and "android.permission.BLUETOOTH_ADMIN" are required for usage of bluetooth
#ifndef BLUETOOTH_UID
#define BLUETOOTH_UID 1002
#endif
    /* The bluetooth capable daemon runs as Android user Id 1002. It is started as service instead of an app. So
     * there is no permission information in packages.xml for it. However, obviously it is allowed to use bluetooth.
     */
    uint32_t userId = endpoint.GetUserId();
    if (userId == BLUETOOTH_UID) {
        return true;
    }
    permissionDbLock.Lock();
    std::map<uint32_t, std::set<qcc::String> >::const_iterator uidPermsIt = uidPermsMap.find(userId);
    std::set<qcc::String> permOwned;

    if (uidPermsIt == uidPermsMap.end()) {
        /* If no permission info is found because of failure to read the "/data/system/packages.xml" file, then ignore the permission check */
        if (GetPermissionsAssignedByAndriod(userId, permOwned) == false) {
            return true;
        }
        uidPermsMap[userId] = permOwned;
    } else {
        permOwned = uidPermsIt->second;
    }

    set<qcc::String>::iterator permOwnedIt = permOwned.find("android.permission.BLUETOOTH");
    if (permOwnedIt != permOwned.end()) {
        permOwnedIt = permOwned.find("android.permission.BLUETOOTH_ADMIN");
        if (permOwnedIt != permOwned.end()) {
            allowed = true;
            QCC_DbgHLPrintf(("PermissionDB::IsBluetoothAllowed() true for user %d", userId));
        } else {
            allowed = false;
            QCC_DbgHLPrintf(("PermissionDB::IsBluetoothAllowed() false because android.permission.BLUETOOTH_ADMIN is not granted for user %d", userId));
        }
    } else {
        allowed = false;
        QCC_DbgHLPrintf(("PermissionDB::IsBluetoothAllowed() false because android.permission.BLUETOOTH is not granted for user %d", userId));
    }
    permissionDbLock.Unlock();
#endif

    return allowed;
}

bool PermissionDB::IsWifiAllowed(BusEndpoint& endpoint)
{
    QCC_DbgTrace(("PermissionDB::IsWifiAllowed(endpoint =%s)", endpoint.GetUniqueName().c_str()));
    bool allowed = true;

#if defined(QCC_OS_ANDROID)
    // For Android app, permissions "android.permission.INTERNET" and "android.permission.CHANGE_WIFI_MULTICAST_STATE" are required for usage of wifi.
    uint32_t userId = endpoint.GetUserId();
    permissionDbLock.Lock();
    std::map<uint32_t, std::set<qcc::String> >::const_iterator uidPermsIt = uidPermsMap.find(userId);
    std::set<qcc::String> permOwned;
    if (uidPermsIt == uidPermsMap.end()) {
        /* If no permission info is found because of failure to read the "/data/system/packages.xml" file, then ignore the permission check */
        if (GetPermissionsAssignedByAndriod(userId, permOwned) == false) {
            return true;
        }
        uidPermsMap[userId] = permOwned;
    } else {
        permOwned = uidPermsIt->second;
    }

    set<qcc::String>::iterator permOwnedIt = permOwned.find("android.permission.INTERNET");
    if (permOwnedIt != permOwned.end()) {
        permOwnedIt = permOwned.find("android.permission.CHANGE_WIFI_MULTICAST_STATE");
        if (permOwnedIt != permOwned.end()) {
            allowed = true;
            QCC_DbgHLPrintf(("PermissionDB::IsWifiAllowed() true for user %d", userId));
        } else {
            allowed = false;
            QCC_DbgHLPrintf(("PermissionDB::IsWifiAllowed() false because android.permission.CHANGE_WIFI_MULTICAST_STATE is not granted for user %d", userId));
        }
    } else {
        allowed = false;
        QCC_DbgHLPrintf(("PermissionDB::IsWifiAllowed() false because permission android.permission.INTERNET is not granted for user %d", userId));
    }
    permissionDbLock.Unlock();
#endif

    return allowed;
}

QStatus PermissionDB::RemovePermissionCache(BusEndpoint& endpoint)
{
    QCC_DbgTrace(("PermissionDB::RemovePermissionCache(endpoint =%s)", endpoint.GetUniqueName().c_str()));
    uint32_t userId = endpoint.GetUserId();
    if (userId > 0) {
        permissionDbLock.Lock();
        uidPermsMap.erase(userId);
        permissionDbLock.Unlock();
    }
    return ER_OK;
}

static bool GetPermissionsAssignedByAndriod(uint32_t uid, std::set<qcc::String>& permissions)
{
    QCC_DbgTrace(("PermissionDB::GetPermissionsAssignedByAndriod(uid =%d)", uid));
    const char* xml = "/data/system/packages.xml"; // The file contains information about all installed Android packages including Permissions

    const uint32_t MAX_ID_SIZE = 32;
    char userId [MAX_ID_SIZE];
    snprintf(userId, MAX_ID_SIZE, "%d", uid);
    FileSource source(xml);
    if (!source.IsValid()) {
        QCC_LogError(ER_FAIL, ("Failed to open %", "/data/system/packages.xml"));
        return false;
    }

    XmlParseContext xmlParseCtx(source);
    XmlElement& root = xmlParseCtx.root;
    bool success = XmlElement::Parse(xmlParseCtx) == ER_OK;
    bool found  = false;

    if (success) {
        if (root.GetName().compare("packages") == 0) {
            QCC_DbgPrintf(("Xml Tag %s", "packages"));
            const vector<XmlElement*>& elements = root.GetChildren();
            vector<XmlElement*>::const_iterator it;

            for (it = elements.begin(); it != elements.end() && !found; ++it) {
                if ((*it)->GetName().compare("package") == 0) {
                    QCC_DbgPrintf(("Xml Tag %s", "package"));
                    const map<qcc::String, qcc::String>& attrs((*it)->GetAttributes());

                    map<qcc::String, qcc::String>::const_iterator attrIt;
                    for (attrIt = attrs.begin(); attrIt != attrs.end(); ++attrIt) {
                        if (attrIt->first.compare("userId") == 0) {
                            QCC_DbgPrintf(("Xml Tag %s = %s", "userId", attrIt->second.c_str()));
                            if (attrIt->second.compare(userId) == 0) {
                                QCC_DbgHLPrintf(("PermissionDB::GetPermissionsAssignedByAndriod() entry for userId %d is found", uid));
                                found = true;
                            }
                        } else if (attrIt->first.compare("shareUserId") == 0) {
                            if (attrIt->second.compare(userId) == 0) {
                                QCC_DbgHLPrintf(("PermissionDB::GetPermissionsAssignedByAndriod() entry for userId %d is found", uid));
                                found = true;
                            }
                        }
                    }
                    if (found) {
                        const vector<XmlElement*>& childElements = (*it)->GetChildren();
                        for (vector<XmlElement*>::const_iterator childIt = childElements.begin(); childIt != childElements.end(); ++childIt) {
                            if ((*childIt)->GetName().compare("perms") == 0) {
                                QCC_DbgPrintf(("Xml Tag %s", "perms"));
                                const vector<XmlElement*>& permElements = (*childIt)->GetChildren();
                                for (vector<XmlElement*>::const_iterator permIt = permElements.begin(); permIt != permElements.end(); ++permIt) {
                                    if ((*permIt)->GetName().compare("item") == 0) {
                                        QCC_DbgPrintf(("Xml Tag %s", "item"));
                                        const map<qcc::String, qcc::String>& itemAttrs((*permIt)->GetAttributes());
                                        map<qcc::String, qcc::String>::const_iterator itemAttrIt;
                                        for (itemAttrIt = itemAttrs.begin(); itemAttrIt != itemAttrs.end(); ++itemAttrIt) {
                                            if (itemAttrIt->first.compare("name") == 0) {
                                                QCC_DbgPrintf(("Xml Tag %s", "name"));
                                                permissions.insert(itemAttrIt->second);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    if (!found) {
        QCC_LogError(ER_FAIL, ("Failed to find permission info for userId %d in File %s", uid, "/data/system/packages.xml"));
    }
    return found;
}

}