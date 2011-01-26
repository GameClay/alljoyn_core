#ifndef _ALLJOYN_ALLJOYNSTD_H
#define _ALLJOYN_ALLJOYNSTD_H
/**
 * @file
 * This file provides definitions for AllJoyn interfaces
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

#include <alljoyn/InterfaceDescription.h>

/*!
 * \def QCC_MODULE
 * Internal usage
 */
#define QCC_MODULE  "ALLJOYN"

/** Daemon-to-daemon protocol version number */
#define ALLJOYN_PROTOCOL_VERSION  1

namespace ajn {


namespace org {
namespace alljoyn {
/**
 * Interface definitions for org.alljoyn.Bus
 */
namespace Bus {

extern const char* ErrorName;                     /**< Standard AllJoyn error name */
extern const char* ObjectPath;                    /**< Object path */
extern const char* InterfaceName;                 /**< Interface name */
extern const char* WellKnownName;                 /**< Well known bus name */

/** Interface definitions for org.alljoyn.Bus.Peer.* */
namespace Peer {
extern const char* ObjectPath;                         /**< Object path */
namespace HeaderCompression {
extern const char* InterfaceName;                      /**<Interface name */
}
namespace Authentication {
extern const char* InterfaceName;                      /**<Interface name */
}
}

QStatus CreateInterfaces(BusAttachment& bus);             /**< Create the org.alljoyn.Bus interfaces and sub-interfaces */
}
}
}

/**
 * @name org.alljoyn.Bus.Connect
 *  Interface: org.alljoyn.Bus
 *  Method: UINT32 Connect(String busAddr)
 *
 *  busAddr = Remote bus address to connect to (e.g. bluetooth:addr=00.11.22.33.44.55, or tcp:addr=1.2.3.4,port=1234)
 *
 *  Request the local daemon to connect to a given remote AllJoyn address.
 *
 *  Returns a status code (see below) indicating success or failure.
 */
// @{
/* org.alljoyn.Bus.Connect */
#define ALLJOYN_CONNECT_REPLY_SUCCESS                 1   /**< Connect reply: Success */
#define ALLJOYN_CONNECT_REPLY_INVALID_SPEC            2   /**< Connect reply: Invalid connect specification */
#define ALLJOYN_CONNECT_REPLY_FAILED                  4   /**< Connect reply: Connect failed */
// @}
/**
 * @name org.alljoyn.Bus.Disconnect
 *  Interface: org.alljoyn.Bus
 *  Method: UINT32 Disconnect(String busAddr)
 *
 *  busAddr = Remote bus address to disconnect. Must match busAddress previously passed to Connect().
 *
 *  Request the local daemon to disconnect from a given remote AllJoyn address previously connected
 *  via a call to Connect().
 *
 *  Returns a status code (see below) indicating success or failure.
 */
// @{
/* org.alljoyn.Bus.Disconnect */
#define ALLJOYN_DISCONNECT_REPLY_SUCCESS              1   /**< Disconnect reply: Success */
#define ALLJOYN_DISCONNECT_REPLY_NO_CONN              2   /**< Disconnect reply: No connection matching spec was found */
#define ALLJOYN_DISCONNECT_REPLY_FAILED               3   /**< Disconnect reply: Disconnect failed */
// @}

/**
 * @name org.alljoyn.Bus.AdvertiseName
 *  Interface: org.alljoyn.Bus
 *  Method: UINT32 AdvertiseName(String wellKnownName)
 *
 *  Request the local daemon to advertise the already obtained well-known attachment name to other
 *  AllJoyn instances that might be interested in connecting to the named service.
 *
 *  wellKnownName = Well-known name of the attachment that wishes to be advertised to remote AllJoyn instances.
 *
 *  Returns a status code (see below) indicating success or failure.
 */
// @{
/* org.alljoyn.Bus.AdvertiseName */
#define ALLJOYN_ADVERTISENAME_REPLY_SUCCESS               1   /**< AdvertiseName reply: Success */
#define ALLJOYN_ADVERTISENAME_REPLY_ALREADY_ADVERTISING   2   /**< AdvertiseName reply: This endpoint has already requested advertising this name */
#define ALLJOYN_ADVERTISENAME_REPLY_FAILED                3   /**< AdvertiseName reply: Advertise failed */
// @}

/**
 * @name org.alljoyn.Bus.CancelAdvertise
 *  Interface: org.alljoyn.Bus
 *  Method: CancelAdvertiseName(String wellKnownName)
 *
 *  wellKnownName = Well-known name of the attachment that should end advertising.
 *
 *  Request the local daemon to stop advertising the well-known attachment name to other
 *  AllJoyn instances. The well-known name must have previously been advertised via a call
 *  to org.alljoyn.Bus.Advertise().
 *
 *  Returns a status code (see below) indicating success or failure.
 */
// @{
/* org.alljoyn.Bus.CancelAdvertise */
#define ALLJOYN_CANCELADVERTISENAME_REPLY_SUCCESS         1   /**< CancelAdvertiseName reply: Success */
#define ALLJOYN_CANCELADVERTISENAME_REPLY_FAILED          2   /**< CancelAdvertiseName reply: Advertise failed */
// @}

/**
 * @name org.alljoyn.Bus.FindName
 *  Interface: org.alljoyn.Bus
 *  Method: FindName(String wellKnownNamePrefix)
 *
 *  wellKnownNamePrefix = Well-known name prefix of the attachment that client is interested in.
 *
 *  Register interest in a well-known attachment name being advertised by a remote AllJoyn instance.
 *  When the local AllJoyn daemon receives such an advertisement it will send an org.alljoyn.Bus.FoundName
 *  signal. This attachment can then choose to ignore the advertisement or to connect to the remote Bus by
 *  calling org.alljoyn.Bus.Connect().
 *
 *  Returns a status code (see below) indicating success or failure.
 */
// @{
/* org.alljoyn.Bus.FindName */
#define ALLJOYN_FINDNAME_REPLY_SUCCESS                1   /**< FindName reply: Success */
#define ALLJOYN_FINDNAME_REPLY_ALREADY_DISCOVERING    2   /**< FindName reply: This enpoint has already requested discover for name */
#define ALLJOYN_FINDNAME_REPLY_FAILED                 3   /**< FindName reply: Failed */
// @}

/**
 * @name org.alljoyn.Bus.CancelDiscover
 *  Interface: org.alljoyn.Bus
 *  Method: CancelFindName(String wellKnownName)
 *
 *  wellKnownName = Well-known name of the attachment that client is no longer interested in.
 *
 *  Cancel interest in a well-known attachment name that was previously included in a call
 *  to org.alljoyn.Bus.FindName().
 *
 *  Returns a status code (see below) indicating success or failure.
 */
// @{
/* org.alljoyn.Bus.CancelDiscover */
#define ALLJOYN_CANCELFINDNAME_REPLY_SUCCESS          1   /**< CancelFindName reply: Success */
#define ALLJOYN_CANCELFINDNAME_REPLY_FAILED           2   /**< CancelFindName reply: Failed */
// @}
}

#undef QCC_MODULE

#endif
