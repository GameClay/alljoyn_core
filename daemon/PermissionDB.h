/**
 * @file
 * AllJoyn Permission database class
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
#ifndef _ALLJOYN_PERMISSION_DB_H
#define _ALLJOYN_PERMISSION_DB_H

#include "BusEndpoint.h"

using namespace std;
using namespace qcc;

namespace ajn {

class PermissionDB {
  public:
    /**
     * Check whether the endpoint is allowed to use Bluetooth
     * @Param endponit The endpoint to be checked
     */
    bool IsBluetoothAllowed(BusEndpoint& endpoint);

    /**
     * Check whether the endpoint is allowed to use WIFI
     * @Param endpoint The endpoint to be checked
     */
    bool IsWifiAllowed(BusEndpoint& endpoint);

    /**
     * Remove the permission information cache of an enpoint before it exits.
     * @Param endponit The endpoint that will exits
     */
    QStatus RemovePermissionCache(BusEndpoint& endpoint);

  private:
    qcc::Mutex permissionDbLock;
    std::map<uint32_t, std::set<qcc::String> > uidPermsMap;          /**< records the permissions owned by endpoint specified by user id */
};

}
#endif
