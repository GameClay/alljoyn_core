/**
 * @file
 * LaunchdTransport is an implementation of Transport that listens
 * on an AF_UNIX socket.
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
#include "LaunchdTransport.h"
#include <qcc/StringUtil.h>

#define QCC_MODULE "ALLJOYN"

using namespace std;
using namespace qcc;

namespace ajn {

QStatus LaunchdTransport::NormalizeTransportSpec(const char* inSpec, qcc::String& outSpec, map<qcc::String, qcc::String>& argMap) const
{
    /*
     * Take the string in inSpec, which must start with "launchd:" and parse it,
     * looking for comma-separated "key=value" pairs and initialize the
     * argMap with those pairs.
     */
    QStatus status = ParseArguments("launchd", inSpec, argMap);
    if (status != ER_OK) {
        return status;
    }

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

QStatus LaunchdTransport::GetUnixTransportSpec(const char* launchdConnectSpec, qcc::String& unixConnectSpec)
{
#if defined(QCC_OS_DARWIN)
    QStatus status;
    qcc::String normSpec;
    map<qcc::String, qcc::String> argMap;
    status = NormalizeTransportSpec(launchdConnectSpec, normSpec, argMap);
    if (ER_OK != status) {
        QCC_LogError(status, ("LaunchdTransport::Connect(): Invalid launchd connect spec \"%s\"", launchdConnectSpec));
        return status;
    }

    /*
     * 'launchctl getenv DBUS_LAUNCHD_SESSION_BUS_SOCKET' will return a blank line, or a line containing
     * the Unix domain socket path.
     */
    qcc::String command = "launchctl getenv " + argMap["_spec"];
    FILE* launchctl = popen(command.c_str(), "r");
    if (!launchctl) {
        status = ER_OS_ERROR;
        QCC_LogError(status, ("launchctl failed"));
        return status;
    }
    unixConnectSpec = "unix:path=";
    char buf[256];
    while (!feof(launchctl) && !ferror(launchctl)) {
        size_t n = fread(buf, 1, 255, launchctl);
        buf[n] = 0;
        unixConnectSpec += buf;
    }
    unixConnectSpec = Trim(unixConnectSpec);
    if (ferror(launchctl)) {
        status = ER_OS_ERROR;
        QCC_LogError(status, ("launchctl failed"));
    }
    pclose(launchctl);
    return status;
#else
    return ER_NOT_IMPLEMENTED;
#endif
}

QStatus LaunchdTransport::Connect(const char* connectArgs, const SessionOpts& opts, RemoteEndpoint** newep)
{
    qcc::String unixConnectArgs;
    QStatus status = GetUnixTransportSpec(connectArgs, unixConnectArgs);
    if (ER_OK == status) {
        status = UnixTransport::Connect(unixConnectArgs.c_str(), opts, newep);
    }
    return status;
}

QStatus LaunchdTransport::Disconnect(const char* connectArgs)
{
    qcc::String unixConnectArgs;
    QStatus status = GetUnixTransportSpec(connectArgs, unixConnectArgs);
    if (ER_OK == status) {
        status = UnixTransport::Disconnect(unixConnectArgs.c_str());
    }
    return status;
}


} // namespace ajn
