/**
 * @file
 * LaunchdTransport is an implementation of Transport that listens
 * on a launchd created AF_UNIX socket.
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

#include "DaemonLaunchdTransport.h"

#include <errno.h>
#if defined(QCC_OS_DARWIN)
#include <launch.h>
#endif
#include <qcc/StringUtil.h>

#define QCC_MODULE "ALLJOYN"

using namespace std;
using namespace qcc;

namespace ajn {

QStatus DaemonLaunchdTransport::NormalizeTransportSpec(const char* inSpec, qcc::String& outSpec, map<qcc::String, qcc::String>& argMap) const
{
    QStatus status = ParseArguments("launchd", inSpec, argMap);
    qcc::String env = Trim(argMap["env"]);
    if (ER_OK == status) {
        outSpec = "launchd:";
        if (env.empty()) {
            env = "DBUS_LAUNCHD_SESSION_BUS_SOCKET";
        }
        outSpec.append("env=");
        outSpec.append(env);
        argMap["_spec"] = env;
    }

    return status;
}

QStatus DaemonLaunchdTransport::ListenFd(std::map<qcc::String, qcc::String>& serverArgs, qcc::SocketFd& listenFd)
{
#if defined(QCC_OS_DARWIN)
    launch_data_t request, response = NULL;
    launch_data_t sockets, fdArray;
    QStatus status;

    request = launch_data_new_string(LAUNCH_KEY_CHECKIN);
    if (request == NULL) {
        status = ER_OS_ERROR;
        QCC_LogError(status, ("Unable to create checkin request"));
        goto exit;
    }
    response = launch_msg(request);
    if (response == NULL) {
        status = ER_OS_ERROR;
        QCC_LogError(status, ("Checkin request failed"));
        goto exit;
    }
    if (launch_data_get_type(response) == LAUNCH_DATA_ERRNO) {
        errno = launch_data_get_errno(response);
        status = ER_OS_ERROR;
        QCC_LogError(status, ("Checkin request failed %s", strerror(errno)));
        goto exit;
    }

    sockets = launch_data_dict_lookup(response, LAUNCH_JOBKEY_SOCKETS);
    if (sockets == NULL) {
        status = ER_OS_ERROR;
        QCC_LogError(status, ("Lookup sockets failed"));
        goto exit;
    }
    if (launch_data_dict_get_count(sockets) > 1) {
        QCC_DbgHLPrintf(("Ignoring additional sockets in launchd plist"));
    }
    fdArray = launch_data_dict_lookup(sockets, "unix_domain_listener");
    if (fdArray == NULL) {
        status = ER_OS_ERROR;
        QCC_LogError(status, ("No listen sockets found"));
        goto exit;
    }
    if (launch_data_array_get_count(fdArray) != 1) {
        status = ER_FAIL;
        QCC_LogError(status, ("Socket 'unix_domain_listener' must have exactly one FD"));
        goto exit;
    }
    listenFd = launch_data_get_fd(launch_data_array_get_index(fdArray, 0));
    status = ER_OK;

exit:
    if (response) {
        launch_data_free(response);
    }
    return status;
#else
    return ER_NOT_IMPLEMENTED;
#endif
}

} // namespace ajn
