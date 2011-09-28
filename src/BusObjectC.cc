/**
 * @file
 *
 * This file implements a BusObject subclass for use by the C API
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

#include <alljoyn/BusObject.h>

using namespace qcc;
using namespace std;

namespace ajn {

class BusObjectC : public BusObject {
  public:
    BusObjectC(alljoyn_busattachment bus, const char* path, QC_BOOL isPlaceholder, \
               const alljoyn_busobject_callbacks* callbacks_in, const void* context_in) :
        BusObject(*((BusAttachment*)bus), path, isPlaceholder == QC_TRUE ? true : false)
    {
        context = context_in;
        memcpy(&callbacks, callbacks_in, sizeof(alljoyn_busobject_callbacks));
    }

    QStatus MethodReplyC(alljoyn_message msg, alljoyn_msgargs_const args, size_t numArgs)
    {
        return MethodReply(*((Message*)msg), (const MsgArg*)args, numArgs);
    }

    QStatus MethodReplyC(alljoyn_message msg, const char* error, const char* errorMessage)
    {
        return MethodReply(*((Message*)msg), error, errorMessage);
    }

    QStatus MethodReplyC(alljoyn_message msg, QStatus status)
    {
        return MethodReply(*((Message*)msg), status);
    }
#if 0
    QStatus SignalC(const char* destination,
                    alljoyn_sessionid sessionId,
                    const InterfaceDescription::Member& signal,
                    alljoyn_msgargs_const args,
                    size_t numArgs,
                    uint16_t timeToLive,
                    uint8_t flags)
    {

    }
#endif
    QStatus AddInterfaceC(alljoyn_interfacedescription_const iface)
    {
        return AddInterface(*(const InterfaceDescription*)iface);
    }

    QStatus AddMethodHandlersC(const alljoyn_busobject_methodentry* entries, size_t numEntries)
    {
        return AddMethodHandlers((const MethodEntry*)entries, numEntries);
    }


  protected:
    virtual QStatus Get(const char* ifcName, const char* propName, MsgArg& val)
    {
        QStatus ret = ER_BUS_NO_SUCH_PROPERTY;
        if (callbacks.property_get != NULL) {
            ret = callbacks.property_get(context, ifcName, propName, (alljoyn_msgargs)(&val));
        }
        return ret;
    }

    virtual QStatus Set(const char* ifcName, const char* propName, MsgArg& val)
    {
        QStatus ret = ER_BUS_NO_SUCH_PROPERTY;
        if (callbacks.property_set != NULL) {
            ret = callbacks.property_set(context, ifcName, propName, (alljoyn_msgargs)(&val));
        }
        return ret;
    }

    // TODO: Need to do GenerateIntrospection?

    virtual void ObjectRegistered(void)
    {
        if (callbacks.object_registered != NULL) {
            callbacks.object_registered(context);
        }
    }

    virtual void ObjectUnregistered(void)
    {
        // Call parent as per docs
        BusObject::ObjectUnregistered();

        if (callbacks.object_unregistered != NULL) {
            callbacks.object_unregistered(context);
        }
    }

    // TODO: Need to do GetProp, SetProp, GetAllProps, Introspect?

  private:
    alljoyn_busobject_callbacks callbacks;
    const void* context;
};
}

alljoyn_busobject alljoyn_busobject_create(alljoyn_busattachment bus, const char* path, QC_BOOL isPlaceholder,
                                           const alljoyn_busobject_callbacks* callbacks_in, const void* context_in)
{
    return new ajn::BusObjectC(bus, path, isPlaceholder, callbacks_in, context_in);
}

void alljoyn_busobject_destroy(alljoyn_busattachment bus)
{
    delete (ajn::BusObjectC*)bus;
}

QStatus alljoyn_busobject_addinterface(alljoyn_busattachment bus, alljoyn_interfacedescription_const iface)
{
    return ((ajn::BusObjectC*)bus)->AddInterfaceC(iface);
}

QStatus alljoyn_busobject_addmethodhandlers(alljoyn_busattachment bus, const alljoyn_busobject_methodentry* entries, size_t numEntries)
{
    return ((ajn::BusObjectC*)bus)->AddMethodHandlersC(entries, numEntries);
}
