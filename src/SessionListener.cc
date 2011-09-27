/**
 * @file
 * This implements the C accessable version of the SessionListener class using
 * function pointers, and a pass-through implementation of SessionListener.
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
#include <alljoyn/SessionListener.h>
#include <string.h>
#include <assert.h>

namespace ajn {

/**
 * Abstract base class implemented by AllJoyn users and called by AllJoyn to inform
 * users of session related events.
 */
class SessionListenerCallbackC : SessionListener {
  public:
    SessionListenerCallbackC(const alljoyn_sessionlistener_callbacks* in_callbacks, const void* in_context)
    {
        memcpy(&callbacks, in_callbacks, sizeof(alljoyn_sessionlistener_callbacks));
        context = in_context;
    }

    void SessionLost(SessionId sessionId)
    {
        if (callbacks.session_lost != NULL) {
            callbacks.session_lost(context, sessionId);
        }
    }
  protected:
    alljoyn_sessionlistener_callbacks callbacks;
    const void* context;
};

}

alljoyn_sessionlistener alljoyn_sessionlistener_create(const alljoyn_sessionlistener_callbacks* callbacks, const void* context)
{
    return new ajn::SessionListenerCallbackC(callbacks, context);
}

void alljoyn_sessionlistener_destroy(alljoyn_sessionlistener* listener)
{
    assert(listener != NULL && *listener != NULL && "listener parameter must not be NULL");
    delete ((ajn::SessionListenerCallbackC*)*listener);
    *listener = NULL;
}
