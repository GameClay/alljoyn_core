/**
 * @file
 * SessionPortListener is an abstract base class (interface) implemented by users of the
 * AllJoyn API in order to receive session port related event information.
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
#ifndef _ALLJOYN_SESSIONPORTLISTENER_H
#define _ALLJOYN_SESSIONPORTLISTENER_H

#include <alljoyn/Session.h>
#include <alljoyn/SessionListener.h>

#ifdef __cplusplus
namespace ajn {

/**
 * Abstract base class implemented by AllJoyn users and called by AllJoyn to inform
 * users of session related events.
 */
class SessionPortListener {
  public:
    /**
     * Virtual destructor for derivable class.
     */
    virtual ~SessionPortListener() { }

    /**
     * Accept or reject an incoming JoinSession request. The session does not exist until this
     * after this function returns.
     *
     * This callback is only used by session creators. Therefore it is only called on listeners
     * passed to BusAttachment::BindSessionPort.
     *
     * @param sessionPort    Session port that was joined.
     * @param joiner         Unique name of potential joiner.
     * @param opts           Session options requested by the joiner.
     * @return   Return true if JoinSession request is accepted. false if rejected.
     */
    virtual bool AcceptSessionJoiner(SessionPort sessionPort, const char* joiner, const SessionOpts& opts) { return false; }

    /**
     * Called by the bus when a session has been successfully joined. The session is now fully up.
     *
     * This callback is only used by session creators. Therefore it is only called on listeners
     * passed to BusAttachment::BindSessionPort.
     *
     * @param sessionPort    Session port that was joined.
     * @param id             Id of session.
     * @param joiner         Unique name of the joiner.
     */
    virtual void SessionJoined(SessionPort sessionPort, SessionId id, const char* joiner) {  }
};

}

extern "C" {
#endif /* #ifdef __cplusplus */

/*
 * Type for the AcceptSessionJoiner callback.
 */
typedef QC_BOOL (*alljoyn_sessionportlistener_acceptsessionjoiner_ptr)(const void* context, alljoyn_sessionport sessionPort,
                                                                       const char* joiner,  alljoyn_sessionopts_const opts);

/*
 * Type for the SessionJoined callback.
 */
typedef void (*alljoyn_sessionportlistener_sessionjoined_ptr)(const void* context, alljoyn_sessionport sessionPort,
                                                              alljoyn_sessionid id, const char* joiner);

typedef struct {
    alljoyn_sessionportlistener_acceptsessionjoiner_ptr accept_session_joiner;
    alljoyn_sessionportlistener_sessionjoined_ptr session_joined;
} alljoyn_sessionportlistener_callbacks;

/**
 * Create a SessionPortListener which will trigger the provided callbacks, passing along the provided context.
 *
 * @param callbacks Callbacks to trigger for associated events.
 * @param context   Context to pass to callback functions
 *
 * @return Handle to newly allocated SessionPortListener.
 */
alljoyn_sessionportlistener alljoyn_sessionportlistener_create(const alljoyn_sessionportlistener_callbacks* callbacks, const void* context);

/**
 * Destroy a SessionPortListener.
 *
 * @param listener SessionPortListener to destroy.
 */
void alljoyn_sessionportlistener_destroy(alljoyn_sessionportlistener* listener);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
