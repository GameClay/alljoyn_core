
/**
 * @file
 * This file contains an enumerated list values that QStatus can return
 *
 * Note: This file is generated during the make process.
 *
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
 */
#ifndef _BTLITECONTROLLER_H
#define _BTLITECONTROLLER_H

#ifndef __cplusplus
#error Only include BTLiteController.h in C++ code.
#endif

#include <Status.h>
#include <qcc/String.h>

#include <vector>
#include <list>

#include "BTLiteTransport.h"

using namespace qcc;
namespace ajn {
class BTLiteTransport;

class BTLiteController {
  public:
    BTLiteController();
    virtual ~BTLiteController();

    virtual void EnsureDiscoverable();
    virtual void EnableAdvertisement(String name);
    virtual void DisableAdvertisement(String name);
    virtual void EnableDiscovery(String namePrefix);
    virtual void DisableDiscovery(String namePrefix);
    virtual void FoundName(String wkn, String guid, String addr, String port);
    virtual void StartListen();
    virtual void StopListen();
    virtual void EndpointExit(String uniqueID);

    virtual String Connect(String spec);
    virtual int DisConnect(String spec);

    void SetTransport(BTLiteTransport* trans);
    String GetGlobalGUID();
    void Accepted(qcc::String uniqueID);

  private:
    BTLiteTransport* m_trans;
};

}
#endif
