/**
 * @file
 * UnixTransport is an implementation of Transport that listens
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

#include <errno.h>

#include <qcc/platform.h>
#include <qcc/Socket.h>
#include <qcc/SocketStream.h>
#include <qcc/String.h>
#include <qcc/StringUtil.h>
#include <qcc/Util.h>
#include <qcc/Logger.h>
#include <vector>

#include "BusInternal.h"
#include "RemoteEndpoint.h"
#include "Router.h"
#include "BTLiteController.h"

#define QCC_MODULE "ALLJOYN"

using namespace qcc;

namespace ajn {

BTLiteController::BTLiteController() {
}

BTLiteController::~BTLiteController() {
}

void BTLiteController::EnsureDiscoverable() {
}

void BTLiteController::EnableAdvertisement(String name) {
}

void BTLiteController::DisableAdvertisement(String name) {
}

void BTLiteController::EnableDiscovery(String namePrefix) {
}

void BTLiteController::DisableDiscovery(String namePrefix) {
}

void BTLiteController::StartListen() {
}

void BTLiteController::StopListen() {
}

String BTLiteController::Connect(String spec) {
    return String("NULL");
}

int BTLiteController::DisConnect(String spec) {
    return 0;
}

void BTLiteController::EndpointExit(String uniqueID) {
}

String BTLiteController::GetGlobalGUID() {
    if (m_trans) {
        return m_trans->GetGlobalGUID();
    } else {
        return String("NULL");
    }
}

void BTLiteController::FoundName(String wkn, String guid, String addr, String port) {
    std::vector<qcc::String> namelist;
    String str(wkn);
    size_t pos;
    if (wkn.size() == 0) return;
    while ((pos = str.find_first_of(";")) != String::npos) {
        namelist.push_back(str.substr(0, pos));
        str.erase(0, pos + 1);
    }

    namelist.push_back(str);

    String busAddr("btlite:addr=" + addr + ",port=" + port);

    qcc::Log(LOG_DEBUG, "busAddr= %s guid = %s", busAddr.c_str(), guid.c_str());
    if (m_trans != NULL) {
        m_trans->FoundName(namelist, guid, busAddr);
    }
}

void BTLiteController::Accepted(qcc::String uniqueID) {
    qcc::Log(LOG_DEBUG, "Accepted uniqueID = %s ", uniqueID.c_str());
    if (m_trans != NULL) {
        m_trans->Accepted(uniqueID);
    }
}

void BTLiteController::SetTransport(BTLiteTransport* trans) {
    m_trans = trans;
}

}

