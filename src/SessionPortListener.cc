/**
 * @file
 * This implements the C accessable version of the SessionPortListener class using
 * function pointers, and a pass-through implementation of SessionPortListener.
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
#include <alljoyn/SessionPortListener.h>
#include <string.h>
#include <assert.h>

namespace ajn {

/**
 * Abstract base class implemented by AllJoyn users and called by AllJoyn to inform
 * users of session port related events.
 */
class SessionPortListenerCallbackC : SessionPortListener {
  public:
    SessionPortListenerCallbackC(const alljoyn_sessionportlistener_callbacks* in_callbacks, const void* in_context)
    {
        memcpy(&callbacks, in_callbacks, sizeof(alljoyn_sessionportlistener_callbacks));
        context = in_context;
    }

    bool AcceptSessionJoiner(SessionPort sessionPort, const char* joiner, const SessionOpts& opts)
    {
        QC_BOOL ret = QC_FALSE;
        if (callbacks.accept_session_joiner != NULL) {
            ret = callbacks.accept_session_joiner(context, sessionPort, joiner, &opts);
        }
        return (ret == QC_FALSE ? false : true);
    }

    void SessionJoined(SessionPort sessionPort, SessionId id, const char* joiner)
    {
        if (callbacks.session_joined != NULL) {
            callbacks.session_joined(context, sessionPort, id, joiner);
        }
    }
  protected:
    alljoyn_sessionportlistener_callbacks callbacks;
    const void* context;
};

}

alljoyn_sessionportlistener alljoyn_sessionportlistener_create(const alljoyn_sessionportlistener_callbacks* callbacks, const void* context)
{
    return new ajn::SessionPortListenerCallbackC(callbacks, context);
}

void alljoyn_sessionportlistener_destroy(alljoyn_sessionportlistener* listener)
{
    assert(listener != NULL && *listener != NULL && "listener parameter must not be NULL");
    delete ((ajn::SessionPortListenerCallbackC*)*listener);
    *listener = NULL;
}
