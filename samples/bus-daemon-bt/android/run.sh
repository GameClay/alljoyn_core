# Copyright 2011, Qualcomm Innovation Center, Inc.
#
#    Licensed under the Apache License, Version 2.0 (the "License");
#    you may not use this file except in compliance with the License.
#    You may obtain a copy of the License at
#
#        http://www.apache.org/licenses/LICENSE-2.0
#
#    Unless required by applicable law or agreed to in writing, software
#    distributed under the License is distributed on an "AS IS" BASIS,
#    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#    See the License for the specific language governing permissions and
#    limitations under the License.
#


adb -s 41242cbf uninstall org.alljoyn.bus.daemonservice
adb -s 41242cbf install  bin/org_alljoyn_bus_daemonservice_bt.apk 
adb -s 1234567890ABCDEF  uninstall org.alljoyn.bus.daemonservice
adb -s 1234567890ABCDEF  install  bin/org_alljoyn_bus_daemonservice_bt.apk 
adb -s 510ef0dc uninstall org.alljoyn.bus.daemonservice
adb -s 510ef0dc install  bin/org_alljoyn_bus_daemonservice_bt.apk
