/**
 * @file
 * BusObject responsible for implementing the AllJoyn methods (org.alljoyn.Debug)
 * for messages controlling debug output.
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
#ifndef _ALLJOYN_ALLJOYNDEBUGOBJ_H
#define _ALLJOYN_ALLJOYNDEBUGOBJ_H

#include <qcc/platform.h>

#include <qcc/Log.h>
#include <qcc/String.h>

#include <alljoyn/AllJoynStd.h>
#include <alljoyn/BusObject.h>

#include "Bus.h"

namespace ajn {

/**
 * BusObject responsible for implementing the AllJoyn methods at org.alljoyn.Debug
 * for messages controlling debug output.
 *
 * @cond ALLJOYN_DEV
 *
 * This is implemented entirely in the header file for the following reasons:
 *
 * - It is fairly small.
 * - It is only instantiated by the BusController in debug builds.
 * - It is easily excluded from release builds by conditionally including it.
 *
 * @endcond
 */
class AllJoynDebugObj : public BusObject {

  public:
    /**
     * Constructor
     *
     * @param bus        Bus to associate with org.freedesktop.DBus message handler.
     */
    AllJoynDebugObj(Bus& bus) : BusObject(bus, org::alljoyn::Daemon::Debug::ObjectPath) { }

    /**
     * Destructor
     */
    ~AllJoynDebugObj() { }

    /**
     * Initialize and register this DBusObj instance.
     *
     * @return ER_OK if successful.
     */
    QStatus Init()
    {
        QStatus status;

        /* Make this object implement org.alljoyn.Bus */
        const InterfaceDescription* alljoynDbgIntf = bus.GetInterface(org::alljoyn::Daemon::Debug::InterfaceName);
        if (!alljoynDbgIntf) {
            status = ER_BUS_NO_SUCH_INTERFACE;
            return status;
        }

        status = AddInterface(*alljoynDbgIntf);
        if (status == ER_OK) {
            /* Hook up the methods to their handlers */
            const MethodEntry methodEntries[] = {
                { alljoynDbgIntf->GetMember("SetDebugLevel"), static_cast<MessageReceiver::MethodHandler>(&AllJoynDebugObj::SetDebugLevel) },
            };

            status = AddMethodHandlers(methodEntries, ArraySize(methodEntries));

            if (status == ER_OK) {
                status = bus.RegisterBusObject(*this);
            }
        }
        return status;
    }

    /**
     * Handles the SetDebugLevel method call.
     *
     * @param member    Member
     * @param msg       The incoming message
     */
    void SetDebugLevel(const InterfaceDescription::Member* member, Message& msg)
    {
        const qcc::String guid(bus.GetInternal().GetGlobalGUID().ToShortString());
        qcc::String sender(msg->GetSender());
        if (sender.substr(1, guid.size()) == guid) {
            const char* module;
            uint32_t level;
            QStatus status = msg->GetArgs("su", &module, &level);
            if (status == ER_OK) {
                QCC_SetDebugLevel(module, level);
                MethodReply(msg, (MsgArg*)NULL, 0);
            } else {
                MethodReply(msg, "org.alljoyn.Debug.InternalError", QCC_StatusText(status));
            }
        } // else someone off-device is trying to set our debug output, punish them by not responding.
    }
};

}

#endif
