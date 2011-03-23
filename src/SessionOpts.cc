/**
 * @file
 * Class for encapsulating Session option information.
 */

/******************************************************************************
 * Copyright 2010-2011, Qualcomm Innovation Center, Inc.
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
#include <alljoyn/Session.h>

#define QCC_MODULE "ALLJOYN"

using namespace std;

namespace ajn {

bool SessionOpts::IsCompatible(const SessionOpts& other) const
{
    /* No overlapping transports means opts are not compatible */
    if (0 == (transports & other.transports)) {
        return false;
    }

    /* Not overlapping traffic types means opts are not compatible */
    if (0 == (traffic & other.traffic)) {
        return false;
    }

    /* Not overlapping proximities means opts are not compatible */
    if (0 == (proximity & other.proximity)) {
        return false;
    }

    return true;
}


}

