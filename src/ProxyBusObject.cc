/**
 * @file
 *
 * This file implements the ProxyBusObject class.
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

#include <assert.h>
#include <vector>
#include <map>

#include <qcc/Debug.h>
#include <qcc/String.h>
#include <qcc/StringSource.h>
#include <qcc/XmlElement.h>
#include <qcc/Util.h>
#include <qcc/Event.h>

#include <alljoyn/BusAttachment.h>
#include <alljoyn/DBusStd.h>
#include <alljoyn/Message.h>
#include <alljoyn/ProxyBusObject.h>
#include <alljoyn/InterfaceDescription.h>

#include "Router.h"
#include "LocalTransport.h"
#include "BusUtil.h"
#include "AllJoynPeerObj.h"
#include "BusInternal.h"

#include <Status.h>

#define QCC_MODULE "ALLJOYN"

#define SYNC_METHOD_ALERTCODE_OK     0
#define SYNC_METHOD_ALERTCODE_ABORT  1

using namespace qcc;
using namespace std;

namespace ajn {

struct ProxyBusObject::Components {

    /** The interfaces this object implements */
    map<qcc::StringMapKey, const InterfaceDescription*> ifaces;

    /** Names of child objects of this object */
    vector<ProxyBusObject> children;

    /** List of threads that are waiting in sync method calls */
    vector<Thread*> waitingThreads;

};

QStatus ProxyBusObject::GetAllProperties(const char* iface, MsgArg& value) const
{
    QStatus status;
    const InterfaceDescription* valueIface = bus->GetInterface(iface);
    if (!valueIface) {
        status = ER_BUS_OBJECT_NO_SUCH_INTERFACE;
    } else {
        uint8_t flags = 0;
        if (valueIface->IsSecure()) {
            flags |= ALLJOYN_FLAG_ENCRYPTED;
        }
        Message reply(*bus);
        MsgArg arg = MsgArg("s", iface);
        const InterfaceDescription* propIface = bus->GetInterface(org::freedesktop::DBus::Properties::InterfaceName);
        status = MethodCall(*(propIface->GetMember("GetAll")),
                            &arg,
                            1,
                            reply,
                            DefaultCallTimeout,
                            flags);
        if (ER_OK == status) {
            value = *(reply->GetArg(0));
        }
    }
    return status;
}

QStatus ProxyBusObject::GetProperty(const char* iface, const char* property, MsgArg& value) const
{
    QStatus status;
    const InterfaceDescription* valueIface = bus->GetInterface(iface);
    if (!valueIface) {
        status = ER_BUS_OBJECT_NO_SUCH_INTERFACE;
    } else {
        uint8_t flags = 0;
        if (valueIface->IsSecure()) {
            flags |= ALLJOYN_FLAG_ENCRYPTED;
        }
        Message reply(*bus);
        MsgArg inArgs[2];
        size_t numArgs = ArraySize(inArgs);
        MsgArg::Set(inArgs, numArgs, "ss", iface, property);
        const InterfaceDescription* propIface = bus->GetInterface(org::freedesktop::DBus::Properties::InterfaceName);
        status = MethodCall(*(propIface->GetMember("Get")),
                            inArgs,
                            numArgs,
                            reply,
                            DefaultCallTimeout,
                            flags);
        if (ER_OK == status) {
            value = *(reply->GetArg(0));
        }
    }
    return status;
}

QStatus ProxyBusObject::SetProperty(const char* iface, const char* property, MsgArg& value) const
{
    QStatus status;
    const InterfaceDescription* valueIface = bus->GetInterface(iface);
    if (!valueIface) {
        status = ER_BUS_OBJECT_NO_SUCH_INTERFACE;
    } else {
        uint8_t flags = 0;
        if (valueIface->IsSecure()) {
            flags |= ALLJOYN_FLAG_ENCRYPTED;
        }
        Message reply(*bus);
        MsgArg inArgs[3];
        size_t numArgs = ArraySize(inArgs);
        MsgArg::Set(inArgs, numArgs, "ssv", iface, property, &value);
        const InterfaceDescription* propIface = bus->GetInterface(org::freedesktop::DBus::Properties::InterfaceName);
        status = MethodCall(*(propIface->GetMember("Set")),
                            inArgs,
                            numArgs,
                            reply,
                            DefaultCallTimeout,
                            flags);
    }
    return status;
}

size_t ProxyBusObject::GetInterfaces(const InterfaceDescription** ifaces, size_t numIfaces) const
{
    size_t count = components->ifaces.size();
    if (ifaces) {
        count = min(count, numIfaces);
        map<qcc::StringMapKey, const InterfaceDescription*>::const_iterator it = components->ifaces.begin();
        for (size_t i = 0; i < count; i++, it++) {
            ifaces[i] = it->second;
        }
    }
    return count;
}

const InterfaceDescription* ProxyBusObject::GetInterface(const char* ifaceName) const
{
    StringMapKey key = ifaceName;
    map<StringMapKey, const InterfaceDescription*>::const_iterator it = components->ifaces.find(key);
    return (it == components->ifaces.end()) ? NULL : it->second;
}


QStatus ProxyBusObject::AddInterface(const InterfaceDescription& iface) {
    StringMapKey key = iface.GetName();
    pair<StringMapKey, const InterfaceDescription*> item(key, &iface);
    pair<map<StringMapKey, const InterfaceDescription*>::const_iterator, bool> ret = components->ifaces.insert(item);
    return ret.second ? ER_OK : ER_BUS_IFACE_ALREADY_EXISTS;
}


QStatus ProxyBusObject::AddInterface(const char* ifaceName)
{
    const InterfaceDescription* iface = bus->GetInterface(ifaceName);
    if (!iface) {
        return ER_BUS_NO_SUCH_INTERFACE;
    } else {
        return AddInterface(*iface);
    }
}

size_t ProxyBusObject::GetChildren(ProxyBusObject** children, size_t numChildren)
{
    size_t count = components->children.size();
    if (children) {
        count = min(count, numChildren);
        for (size_t i = 0; i < count; i++) {
            children[i] = &components->children[i];
        }
    }
    return count;
}

ProxyBusObject* ProxyBusObject::GetChild(const char* inPath)
{
    /* Create absolute version of inPath */
    qcc::String inPathStr = ('/' == inPath[0]) ? inPath : path + '/' + inPath;


    /* Sanity check to make sure path is possible */
    if ((0 != inPathStr.find(path + '/')) || (inPathStr[inPathStr.length() - 1] == '/')) {
        return NULL;
    }

    /* Find each path element as a child within the parent's vector of children */
    size_t idx = path.size() + 1;
    ProxyBusObject* cur = this;
    while (idx != qcc::String::npos) {
        size_t end = inPathStr.find_first_of('/', idx);
        qcc::String item = inPathStr.substr(0, (qcc::String::npos == end) ? end : end - 1);
        vector<ProxyBusObject>& ch = cur->components->children;
        vector<ProxyBusObject>::iterator it = ch.begin();
        while (it != ch.end()) {
            if (it->GetPath() == item) {
                cur = &(*it);
                break;
            }
            ++it;
        }
        if (it == ch.end()) {
            return NULL;
        }
        idx = ((qcc::String::npos == end) || ((end + 1) == inPathStr.size())) ? qcc::String::npos : end + 1;
    }
    return cur;
}

QStatus ProxyBusObject::AddChild(const ProxyBusObject& child)
{
    qcc::String childPath = child.GetPath();

    /* Sanity check to make sure path is possible */
    if (((path.size() > 1) && (0 != childPath.find(path + '/'))) ||
        ((path.size() == 1) && (childPath[0] != '/')) ||
        (childPath[childPath.length() - 1] == '/')) {
        return ER_BUS_BAD_CHILD_PATH;
    }

    /* Find each path element as a child within the parent's vector of children */
    /* Add new children as necessary */
    size_t idx = path.size() + 1;
    ProxyBusObject* cur = this;
    while (idx != qcc::String::npos) {
        size_t end = childPath.find_first_of('/', idx);
        qcc::String item = childPath.substr(0, (qcc::String::npos == end) ? end : end - 1);
        vector<ProxyBusObject>& ch = cur->components->children;
        vector<ProxyBusObject>::iterator it = ch.begin();
        while (it != ch.end()) {
            if (it->GetPath() == item) {
                cur = &(*it);
                break;
            }
            ++it;
        }
        if (it == ch.end()) {
            if (childPath == item) {
                ch.push_back(child);
                return ER_OK;
            } else {
                ProxyBusObject ro(*bus, serviceName.c_str(), item.c_str());
                ch.push_back(ro);
                cur = &ch.back();
            }
        }
        idx = ((qcc::String::npos == end) || ((end + 1) == childPath.size())) ? qcc::String::npos : end + 1;
    }
    return ER_BUS_OBJ_ALREADY_EXISTS;
}

QStatus ProxyBusObject::RemoveChild(const char* inPath)
{
    QStatus status;
    qcc::String childPath = inPath;

    /* Sanity check to make sure path is possible */
    if ((0 != childPath.find(path + '/')) || (childPath[childPath.length() - 1] == '/')) {
        return ER_BUS_BAD_CHILD_PATH;
    }

    /* Navigate to child and remove it */
    size_t idx = path.size() + 1;
    ProxyBusObject* cur = this;
    while (idx != qcc::String::npos) {
        size_t end = childPath.find_first_of('/', idx);
        qcc::String item = childPath.substr(0, (qcc::String::npos == end) ? end : end - 1);
        vector<ProxyBusObject>& ch = cur->components->children;
        vector<ProxyBusObject>::iterator it = ch.begin();
        while (it != ch.end()) {
            if (it->GetPath() == item) {
                if (end == qcc::String::npos) {
                    ch.erase(it);
                    return ER_OK;
                } else {
                    cur = &(*it);
                    break;
                }
            }
            ++it;
        }
        if (it == ch.end()) {
            status = ER_BUS_OBJ_NOT_FOUND;
            QCC_LogError(status, ("Cannot find object path %s", item.c_str()));
            return status;
        }
        idx = ((qcc::String::npos == end) || ((end + 1) == childPath.size())) ? qcc::String::npos : end + 1;
    }
    /* Shouldn't get here */
    return ER_FAIL;
}



QStatus ProxyBusObject::MethodCallAsync(const InterfaceDescription::Member& method,
                                        MessageReceiver* receiver,
                                        MessageReceiver::ReplyHandler replyHandler,
                                        const MsgArg* args,
                                        size_t numArgs,
                                        void* context,
                                        uint32_t timeout,
                                        uint8_t flags) const
{

    QStatus status;
    uint32_t serial;
    Message msg(*bus);
    LocalEndpoint& localEndpoint = bus->GetInternal().GetLocalEndpoint();

    if (!replyHandler) {
        flags |= ALLJOYN_FLAG_NO_REPLY_EXPECTED;
    }
    /*
     * If the interface is secure the method call must be encrypted.
     */
    if (method.iface->IsSecure()) {
        status = localEndpoint.GetPeerObj()->SecurePeerConnection(serviceName);
        /*
         * Not recoverable if the connection could not be secured
         */
        if (status != ER_OK) {
            return status;
        }
        flags |= ALLJOYN_FLAG_ENCRYPTED;
    }
    status = msg->CallMsg(method.signature,
                          serviceName,
                          path,
                          method.iface->GetName(),
                          method.name,
                          serial,
                          args,
                          numArgs,
                          flags);
    if (status == ER_OK) {
        if (!(flags & ALLJOYN_FLAG_NO_REPLY_EXPECTED)) {
            status = localEndpoint.RegisterReplyHandler(receiver,
                                                        replyHandler,
                                                        method,
                                                        serial,
                                                        (flags & ALLJOYN_FLAG_ENCRYPTED) != 0,
                                                        context,
                                                        timeout);
        }
        if (status == ER_OK) {
            status = bus->GetInternal().GetRouter().PushMessage(msg, localEndpoint);
        }
    }
    return status;
}

QStatus ProxyBusObject::MethodCallAsync(const char* ifaceName,
                                        const char* methodName,
                                        MessageReceiver* receiver,
                                        MessageReceiver::ReplyHandler replyHandler,
                                        const MsgArg* args,
                                        size_t numArgs,
                                        void* context,
                                        uint32_t timeout,
                                        uint8_t flags) const
{
    map<StringMapKey, const InterfaceDescription*>::const_iterator it = components->ifaces.find(StringMapKey(ifaceName));
    if (it == components->ifaces.end()) {
        return ER_BUS_NO_SUCH_INTERFACE;
    }
    const InterfaceDescription::Member* member = it->second->GetMember(methodName);
    if (NULL == member) {
        return ER_BUS_INTERFACE_NO_SUCH_MEMBER;
    }
    return MethodCallAsync(*member, receiver, replyHandler, args, numArgs, context, timeout, flags);
}

/**
 * Internal context structure used between synchronous method_call and method_return
 */
class SyncReplyContext {
  public:
    SyncReplyContext(BusAttachment& bus) : replyMsg(bus) { }
    Message replyMsg;
    Event event;
};


QStatus ProxyBusObject::MethodCall(const InterfaceDescription::Member& method,
                                   const MsgArg* args,
                                   size_t numArgs,
                                   Message& replyMsg,
                                   uint32_t timeout,
                                   uint8_t flags) const
{
    QStatus status;
    uint32_t serial;
    Message msg(*bus);
    LocalEndpoint& localEndpoint = bus->GetInternal().GetLocalEndpoint();

    /*
     * Check if the current thread allows blocking on the current bus.
     */
    if (!(flags & ALLJOYN_FLAG_NO_REPLY_EXPECTED) && !Thread::GetThread()->CanBlock(&bus)) {
        status = ER_BUS_BLOCKING_CALL_NOT_ALLOWED;
        QCC_LogError(status, ("A sychronous method call from inside a handler is not allowed"));
        goto MethodCallExit;
    }
    /*
     * If the interface is secure the method call must be encrypted.
     */
    if (method.iface->IsSecure()) {
        flags |= ALLJOYN_FLAG_ENCRYPTED;
    }
    if (flags & ALLJOYN_FLAG_ENCRYPTED) {
        status = localEndpoint.GetPeerObj()->SecurePeerConnection(serviceName);
        /*
         * Not recoverable if the connection could not be secured
         */
        if (status != ER_OK) {
            goto MethodCallExit;
        }
    }
    status = msg->CallMsg(method.signature,
                          serviceName,
                          path,
                          method.iface->GetName(),
                          method.name,
                          serial,
                          args,
                          numArgs,
                          flags);
    if (status != ER_OK) {
        goto MethodCallExit;
    }
    if (flags & ALLJOYN_FLAG_NO_REPLY_EXPECTED) {
        /*
         * Push the message to the router and we are done
         */
        status = bus->GetInternal().GetRouter().PushMessage(msg, localEndpoint);
    } else {
        SyncReplyContext ctxt(*bus);
        /*
         * Synchronous calls are really asynchronous calls that block waiting for a builtin
         * reply handler to be called.
         */
        status = localEndpoint.RegisterReplyHandler(const_cast<MessageReceiver*>(static_cast<const MessageReceiver* const>(this)),
                                                    static_cast<MessageReceiver::ReplyHandler>(&ProxyBusObject::SyncReplyHandler),
                                                    method,
                                                    serial,
                                                    (flags & ALLJOYN_FLAG_ENCRYPTED) != 0,
                                                    &ctxt,
                                                    timeout);
        if (ER_OK == status) {
            status = bus->GetInternal().GetRouter().PushMessage(msg, localEndpoint);
            Thread* thisThread = Thread::GetThread();
            if (ER_OK == status) {
                components->waitingThreads.push_back(thisThread);
                status = Event::Wait(ctxt.event);
                vector<Thread*>::iterator it = components->waitingThreads.begin();
                while (it != components->waitingThreads.end()) {
                    if (*it == thisThread) {
                        components->waitingThreads.erase(it);
                        break;
                    }
                    ++it;
                }
            }
            if ((ER_OK == status) && (SYNC_METHOD_ALERTCODE_OK == thisThread->GetAlertCode())) {
                replyMsg = ctxt.replyMsg;
            } else if (SYNC_METHOD_ALERTCODE_ABORT == thisThread->GetAlertCode()) {
                /*
                 * We can't touch anything in this case since the external thread that was waiting
                 * can't know whether this object still exists.
                 */
                status = ER_BUS_METHOD_CALL_ABORTED;
                goto MethodCallExit;
            } else {
                localEndpoint.UnRegisterReplyHandler(serial);
            }
        }
    }

MethodCallExit:
    /*
     * Let caller know that the method call reply was an error message
     */
    if (status == ER_OK) {
        if (replyMsg->GetType() == MESSAGE_ERROR) {
            status = ER_BUS_REPLY_IS_ERROR_MESSAGE;
        }
    } else {
        replyMsg->ErrorMsg(status, 0);
    }
    return status;
}

QStatus ProxyBusObject::MethodCall(const char* ifaceName,
                                   const char* methodName,
                                   const MsgArg* args,
                                   size_t numArgs,
                                   Message& replyMsg,
                                   uint32_t timeout,
                                   uint8_t flags) const
{
    map<StringMapKey, const InterfaceDescription*>::const_iterator it = components->ifaces.find(StringMapKey(ifaceName));
    if (it == components->ifaces.end()) {
        return ER_BUS_NO_SUCH_INTERFACE;
    }
    const InterfaceDescription::Member* member = it->second->GetMember(methodName);
    if (NULL == member) {
        return ER_BUS_INTERFACE_NO_SUCH_MEMBER;
    }
    return MethodCall(*member, args, numArgs, replyMsg, timeout, flags);
}

void ProxyBusObject::SyncReplyHandler(Message& msg, void* context)
{
    SyncReplyContext* ctx = reinterpret_cast<SyncReplyContext*>(context);

    /* Set the reply message */
    ctx->replyMsg = msg;

    /* Wake up sync method_call thread */
    QStatus status = ctx->event.SetEvent();
    if (ER_OK != status) {
        QCC_LogError(status, ("SetEvent failed"));
    }
}

QStatus ProxyBusObject::SecureConnection(bool forceAuth)
{
    return bus->GetInternal().GetLocalEndpoint().GetPeerObj()->SecurePeerConnection(serviceName, forceAuth);
}

QStatus ProxyBusObject::IntrospectRemoteObject()
{
    /* Need to have introspectable interface in order to call Introspect */
    const InterfaceDescription* introIntf = GetInterface(org::freedesktop::DBus::Introspectable::InterfaceName);
    if (!introIntf) {
        introIntf = bus->GetInterface(org::freedesktop::DBus::Introspectable::InterfaceName);
        assert(introIntf);
        AddInterface(*introIntf);
    }

    /* Attempt to retrieve introspection from the remote object using sync call */
    Message reply(*bus);
    const InterfaceDescription::Member* introMember = introIntf->GetMember("Introspect");
    assert(introMember);
    QStatus status = MethodCall(*introMember, NULL, 0, reply, 5000);

    /* Parse the XML reply */
    if (ER_OK == status) {
        QCC_DbgPrintf(("Introspection XML: %s\n", reply->GetArg(0)->v_string.str));
        qcc::String ident = reply->GetSender();
        ident += " : ";
        ident += reply->GetObjectPath();
        status = ParseIntrospection(reply->GetArg(0)->v_string.str, ident.c_str());
    }
    return status;
}

struct _IntrospectMethodCBContext {
    ProxyBusObject* obj;
    ProxyBusObject::Listener* listener;
    ProxyBusObject::Listener::IntrospectCB callback;
    void* context;
    _IntrospectMethodCBContext(ProxyBusObject* obj, ProxyBusObject::Listener* listener, ProxyBusObject::Listener::IntrospectCB callback, void* context)
        : obj(obj), listener(listener), callback(callback), context(context) { }
};

QStatus ProxyBusObject::IntrospectRemoteObjectAsync(ProxyBusObject::Listener* listener,
                                                    ProxyBusObject::Listener::IntrospectCB callback,
                                                    void* context)
{
    /* Need to have introspectable interface in order to call Introspect */
    const InterfaceDescription* introIntf = GetInterface(org::freedesktop::DBus::Introspectable::InterfaceName);
    if (!introIntf) {
        introIntf = bus->GetInterface(org::freedesktop::DBus::Introspectable::InterfaceName);
        assert(introIntf);
        AddInterface(*introIntf);
    }

    /* Attempt to retrieve introspection from the remote object using async call */
    const InterfaceDescription::Member* introMember = introIntf->GetMember("Introspect");
    assert(introMember);
    QStatus status = MethodCallAsync(*introMember,
                                     this,
                                     static_cast<MessageReceiver::ReplyHandler>(&ProxyBusObject::IntrospectMethodCB),
                                     NULL,
                                     0,
                                     reinterpret_cast<void*>(new _IntrospectMethodCBContext(this, listener, callback, context)),
                                     5000);

    return status;
}

void ProxyBusObject::IntrospectMethodCB(Message& msg, void* context)
{
    QCC_DbgPrintf(("Introspection XML: %s", msg->GetArg(0)->v_string.str));

    _IntrospectMethodCBContext* ctx = reinterpret_cast<_IntrospectMethodCBContext*>(context);

    /* Parse the XML reply to update this ProxyBusObject instance (plus any new interfaces) */
    qcc::String ident = msg->GetSender();
    ident += " : ";
    ident += msg->GetObjectPath();
    QStatus status = ParseIntrospection(msg->GetArg(0)->v_string.str, ident.c_str());

    /* Call the callback */
    (ctx->listener->*ctx->callback)(status, ctx->obj, ctx->context);
    delete ctx;
}

struct ProxyBusObject::ParseRoot {
    const XmlElement* root;
};

QStatus ProxyBusObject::ParseIntrospection(const char* xml, const char* ident)
{
    StringSource source(xml);

    /* Parse the XML reply to update this ProxyBusObject instance (plus any new interfaces) */
    XmlParseContext pc(source);
    QStatus status = XmlElement::Parse(pc);

    if (ER_OK == status) {
        ParseRoot root = { &pc.root };
        status = ParseNode(root, ident);
    }
    return status;
}

QStatus ProxyBusObject::ParseNode(const ParseRoot& parseRoot, const char* ident)
{
    const XmlElement* root = parseRoot.root;
    QStatus status = ER_OK;

    /* Sanity check. Root element must be a node */
    if (root->GetName() != "node") {
        status = ER_BUS_BAD_XML;
        QCC_LogError(status, ("Introspection root element must be <node>"));
        return status;
    }

    /* Iterate over <interface> elements */
    const vector<XmlElement*>& rootChildren = root->GetChildren();
    vector<XmlElement*>::const_iterator it = rootChildren.begin();
    while ((ER_OK == status) && (it != rootChildren.end())) {
        const XmlElement* elem = *it++;
        qcc::String elemName = elem->GetName();
        if (elemName == "interface") {
            qcc::String ifName = elem->GetAttribute("name");
            if (IsLegalInterfaceName(ifName.c_str())) {

                // TODO @@ get "secure" annotation

                /* Create a new inteface */
                InterfaceDescription intf(ifName.c_str(), false);

                /* Iterate over <method>, <signal> and <property> elements */
                const vector<XmlElement*>& ifChildren = elem->GetChildren();
                vector<XmlElement*>::const_iterator ifIt = ifChildren.begin();
                while ((ER_OK == status) && (ifIt != ifChildren.end())) {
                    const XmlElement* ifChildElem = *ifIt++;
                    qcc::String ifChildName = ifChildElem->GetName();
                    qcc::String memberName = ifChildElem->GetAttribute("name");
                    if ((ifChildName == "method") || (ifChildName == "signal")) {
                        if (IsLegalMemberName(memberName.c_str())) {

                            bool isMethod = (ifChildName == "method");
                            bool isSignal = (ifChildName == "signal");
                            bool isFirstArg = true;
                            qcc::String inSig;
                            qcc::String outSig;
                            qcc::String argList;

                            /* Iterate over args */
                            const vector<XmlElement*>& argChildren = ifChildElem->GetChildren();
                            vector<XmlElement*>::const_iterator argIt = argChildren.begin();
                            while ((ER_OK == status) && (argIt != argChildren.end())) {
                                const XmlElement* argElem = *argIt++;
                                if (argElem->GetName() == "arg") {
                                    if (!isFirstArg) {
                                        argList += ',';
                                    }
                                    isFirstArg = false;
                                    qcc::String nameAtt = argElem->GetAttribute("name");
                                    qcc::String directionAtt = argElem->GetAttribute("direction");
                                    qcc::String typeAtt = argElem->GetAttribute("type");

                                    if (typeAtt.empty() || (isMethod && directionAtt.empty())) {
                                        status = ER_FAIL;
                                        QCC_LogError(status, ("Malformed <arg> tag (bad attributes)"));
                                        break;
                                    }

                                    argList += argElem->GetAttribute("name");
                                    if (isSignal || (argElem->GetAttribute("direction") == "in")) {
                                        inSig += argElem->GetAttribute("type");
                                    } else {
                                        outSig += argElem->GetAttribute("type");
                                    }
                                }
                            }

                            /* Add the member */
                            // TODO @@ annotations
                            if ((ER_OK == status) && (isMethod || isSignal)) {
                                status = intf.AddMember(isMethod ? MESSAGE_METHOD_CALL : MESSAGE_SIGNAL,
                                                        memberName.c_str(),
                                                        inSig.empty() ? NULL : inSig.c_str(),
                                                        outSig.empty() ? NULL : outSig.c_str(),
                                                        argList.empty() ? NULL : argList.c_str(),
                                                        0);
                            }
                        } else {
                            status = ER_FAIL;
                            QCC_LogError(status, ("Illegal member name \"%s\" introspection data for %s",
                                                  memberName.c_str(),
                                                  ident));
                        }
                    } else if (ifChildName == "property") {
                        qcc::String sig = ifChildElem->GetAttribute("type");
                        qcc::String accessStr = ifChildElem->GetAttribute("access");
                        /* TODO @@ Improve signature checking */
                        if (sig.empty() || memberName.empty()) {
                            status = ER_FAIL;
                            QCC_LogError(status, ("Unspecified type or name attriute for property %s in introspection data from %s",
                                                  memberName.c_str(), ident));
                        } else {
                            uint8_t access = 0;
                            if (accessStr == "read") access = PROP_ACCESS_READ;
                            if (accessStr == "write") access = PROP_ACCESS_WRITE;
                            if (accessStr == "readwrite") access = PROP_ACCESS_RW;
                            status = intf.AddProperty(memberName.c_str(), sig.c_str(), access);
                        }
                    } else if (ifChildName != "annotation") {
                        status = ER_FAIL;
                        QCC_LogError(status, ("Unknown element \"%s\" found in introspection data from %s", ifChildName.c_str(), ident));
                        break;
                    }
                }
                /* Add the interface with all its methods, signals and properties */
                if (ER_OK == status) {
                    InterfaceDescription* newIntf = NULL;
                    status = bus->CreateInterface(intf.GetName(), newIntf);
                    if (ER_OK == status) {
                        /* Assign new interface */
                        *newIntf = intf;
                        newIntf->Activate();
                        AddInterface(*newIntf);
                    } else if (ER_BUS_IFACE_ALREADY_EXISTS == status) {
                        /* Make sure definition matches existing one */
                        const InterfaceDescription* existingIntf = bus->GetInterface(intf.GetName());
                        if (existingIntf) {
                            if (*existingIntf == intf) {
                                AddInterface(*existingIntf);
                                status = ER_OK;
                            } else {
                                status = ER_BUS_INTERFACE_MISMATCH;
                                QCC_LogError(status, ("XML interface description does not match existing definition for \"%s\"",
                                                      intf.GetName()));
                            }
                        } else {
                            status = ER_FAIL;
                            QCC_LogError(status, ("Failed to retrieve existing interface \"%s\"", intf.GetName()));
                        }
                    } else {
                        QCC_LogError(status, ("Failed to create new inteface \"%s\"", intf.GetName()));
                    }
                }
            } else {
                status = ER_FAIL;
                QCC_LogError(status, ("Invalid interface name \"%s\" in XML introspection data for %s", ifName.c_str(), ident));
            }
        } else if (elemName == "node") {
            qcc::String relativePath = elem->GetAttribute("name");
            qcc::String childObjPath = path;
            if (0 || childObjPath.size() > 1) {
                childObjPath += '/';
            }
            childObjPath += relativePath;
            if (!relativePath.empty() & IsLegalObjectPath(childObjPath.c_str())) {
                /* Check for existing child with the same name. Use this child if found, otherwise create a new one */
                ParseRoot childRoot = { elem };
                ProxyBusObject* childObj = GetChild(relativePath.c_str());
                if (NULL != childObj) {
                    status = childObj->ParseNode(childRoot, ident);
                } else {
                    ProxyBusObject childObj(*bus, serviceName.c_str(), childObjPath.c_str());
                    status = childObj.ParseNode(childRoot, ident);
                    if (ER_OK == status) {
                        AddChild(childObj);
                    }
                }
                if (ER_OK != status) {
                    QCC_LogError(status, ("Failed to parse child object %s in introspection data for %s", childObjPath.c_str(), ident));
                }
            } else {
                status = ER_FAIL;
                QCC_LogError(status, ("Illegal child object name \"%s\" specified in introspection for %s", relativePath.c_str(), ident));
            }
        }
    }
    return status;
}

ProxyBusObject::~ProxyBusObject()
{
    vector<Thread*>::iterator it = components->waitingThreads.begin();
    while (it != components->waitingThreads.end()) {
        (*it++)->Alert(SYNC_METHOD_ALERTCODE_ABORT);
    }
    delete components;
}

ProxyBusObject::ProxyBusObject(BusAttachment& bus, const char* service, const char* path) :
    bus(&bus),
    components(new Components),
    path(path),
    serviceName(service)
{
    /* The Peer interface is implicitly defined for all objects */
    AddInterface(org::freedesktop::DBus::Peer::InterfaceName);
}

ProxyBusObject::ProxyBusObject() : bus(NULL), components(NULL)
{
}

ProxyBusObject::ProxyBusObject(const ProxyBusObject& other)
{
    components = new Components;
    bus = other.bus;
    path = other.path;
    serviceName = other.serviceName;
    if (other.components) {
        *components = *other.components;
    }
}

ProxyBusObject& ProxyBusObject::operator=(const ProxyBusObject& other)
{
    components = new Components;
    bus = other.bus;
    path = other.path;
    serviceName = other.serviceName;
    if (other.components) {
        *components = *other.components;
    }
    return *this;
}

}
