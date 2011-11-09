/**
 * @file
 *
 * BusController is responsible for responding to standard DBus messages
 * directed at the bus itself.
 */

/******************************************************************************
 *
 *
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

#include "BusController.h"
#include "DaemonRouter.h"
#include "BusInternal.h"

#define QCC_MODULE "ALLJOYN_DAEMON"

using namespace std;
using namespace qcc;

namespace ajn {

BusController::BusController(Bus& alljoynBus, QStatus& status) :
    bus(alljoynBus),
#ifndef NDEBUG
    alljoynDebugObj(bus),
#endif
    dbusObj(bus, this),
    alljoynObj(bus, this)
{
    DaemonRouter& router(reinterpret_cast<DaemonRouter&>(bus.GetInternal().GetRouter()));
    router.SetBusController(this);
    status = dbusObj.Init();
    if (ER_OK != status) {
        QCC_LogError(status, ("DBusObj::Init failed"));
    }
}

BusController::~BusController()
{
    DaemonRouter& router(reinterpret_cast<DaemonRouter&>(bus.GetInternal().GetRouter()));
    router.SetBusController(NULL);
}

#ifndef NDEBUG
debug::AllJoynDebugObj* debug::AllJoynDebugObj::self = NULL;
#endif


void BusController::ObjectRegistered(BusObject* obj)
{
    QStatus status = ER_OK;
    if (obj == &dbusObj) {
        status = alljoynObj.Init();
#ifndef NDEBUG
    } else if (obj == &alljoynObj) {
        status = alljoynDebugObj.Init();
#endif
    }

    if (status != ER_OK) {
        QCC_LogError(status, ("BusController::ObjectRegistered failed"));
    }
}

}
