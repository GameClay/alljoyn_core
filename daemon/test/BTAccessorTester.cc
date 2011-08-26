/**
 * @file
 * AllJoyn service that implements interfaces and members affected test.conf.
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

#include <assert.h>

#include <list>

#include <qcc/GUID.h>
#include <qcc/Mutex.h>
#include <qcc/String.h>
#include <qcc/Thread.h>

#include "BDAddress.h"
#include "RemoteEndpoint.h"
#include "Transport.h"


using namespace std;
using namespace qcc;


namespace ajn {

#define _ALLJOYNBTTRANSPORT_H
class BTTransport {

  public:
    class BTAccessor;

    std::set<RemoteEndpoint*> threadList;
    Mutex threadListLock;

    BTTransport() { }
    virtual ~BTTransport() { }

    void BTDeviceAvailable(bool avail);
    bool CheckIncomingAddress(const BDAddress& addr) const;
    void DeviceChange(const BDAddress& bdAddr, uint32_t uuidRev, bool eirCapable);
    void DisconnectAll();

    virtual void TestBTDeviceAvailable(bool avail) = 0;
    virtual bool TestCheckIncomingAddress(const BDAddress& addr) const = 0;
    virtual void TestDeviceChange(const BDAddress& bdAddr, uint32_t uuidRev, bool eirCapable) = 0;
};


void BTTransport::BTDeviceAvailable(bool avail)
{
    TestBTDeviceAvailable(avail);
}
bool BTTransport::CheckIncomingAddress(const BDAddress& addr) const
{
    return TestCheckIncomingAddress(addr);
}
void BTTransport::DeviceChange(const BDAddress& bdAddr, uint32_t uuidRev, bool eirCapable)
{
    TestDeviceChange(bdAddr, uuidRev, eirCapable);
}
void BTTransport::DisconnectAll()
{
}

}


#include "BTNodeDB.h"
#include "BTNodeInfo.h"

#if defined QCC_OS_GROUP_POSIX
#if defined(QCC_OS_DARWIN)
#error Darwin support for bluetooth to be implemented
#else
#include "../bt_bluez/BTAccessor.h"
#endif
#elif defined QCC_OS_GROUP_WINDOWS
#include "../bt_windows/BTAccessor.h"
#endif


using namespace ajn;

class TestDriver : public BTTransport {
  public:
    typedef bool (TestDriver::*TestCase)();

    struct TestCaseInfo {
        TestDriver::TestCase tc;
        String description;
        bool reqPrevTC;
        TestCaseInfo(TestDriver::TestCase tc, const String& description, bool reqPrevTC) :
            tc(tc), description(description), reqPrevTC(reqPrevTC)
        {
        }
    };


    TestDriver(const String& basename, bool allowInteractive, bool reportDetails);
    virtual ~TestDriver();

    void AddTestCase(TestDriver::TestCase tc, const String description, bool reqPrevTC = false);
    int RunTests();
    void ReportTestDetail(const String detail) const;

    void TestBTDeviceAvailable(bool available);
    virtual bool TestCheckIncomingAddress(const BDAddress& addr) const = 0;
    virtual void TestDeviceChange(const BDAddress& bdAddr, uint32_t uuidRev, bool eirCapable) = 0;

    bool TC_CreateBTAccessor();
    bool TC_DestroyBTAccessor();
    bool TC_StartBTAccessor();
    bool TC_StopBTAccessor();
    bool TC_IsMaster();
    bool TC_RequestBTRole();
    bool TC_IsEIRCapable();

  protected:
    BTAccessor* btAccessor;
    String basename;
    const bool allowInteractive;

    deque<bool> btDevAvailQueue;
    Event btDevAvailEvent;

    deque<BDAddress> bdAddrQueue;
    Event bdAddrEvent;

    set<BDAddress> connectedDevices;

    bool eirCapable;

  private:
    const bool reportDetails;
    list<TestCaseInfo> tcList;
    uint32_t testcase;
    mutable list<String> detailList;
    bool success;

    void ReportTest(bool tcSuccess, const String desciption);
};


class ClientTestDriver : public TestDriver {
  public:
    ClientTestDriver(const String& basename, bool allowInteractive, bool reportDetails);
    virtual ~ClientTestDriver() { }

    bool TestCheckIncomingAddress(const BDAddress& addr) const;
    void TestDeviceChange(const BDAddress& bdAddr, uint32_t uuidRev, bool eirCapable);

    bool TC_StartDiscovery();
    bool TC_StopDiscovery();
    bool TC_Connect();
    bool TC_GetDeviceInfo();
};


class ServerTestDriver : public TestDriver {
  public:
    ServerTestDriver(const String& basename, bool allowInteractive, bool reportDetails);
    virtual ~ServerTestDriver() { }

    bool TestCheckIncomingAddress(const BDAddress& addr) const;
    void TestDeviceChange(const BDAddress& bdAddr, uint32_t uuidRev, bool eirCapable);

    bool TC_StartDiscoverability();
    bool TC_StopDiscoverability();
    bool TC_SetSDPInfo();
    bool TC_StartConnectable();
    bool TC_StopConnectable();
    bool TC_Accept();
    bool TC_GetL2CAPConnectEvent();

  private:
    bool allowIncomingAddress;
};


TestDriver::TestDriver(const String& basename, bool allowInteractive, bool reportDetails) :
    btAccessor(NULL),
    basename(basename),
    allowInteractive(allowInteractive),
    reportDetails(reportDetails),
    testcase(0),
    success(true)
{
}

TestDriver::~TestDriver()
{
    if (btAccessor) {
        delete btAccessor;
    }
}

void TestDriver::AddTestCase(TestDriver::TestCase tc, const String description, bool reqPrevTC)
{
    tcList.push_back(TestCaseInfo(tc, description, reqPrevTC));
}

int TestDriver::RunTests()
{
    list<TestCaseInfo>::iterator it;
    bool tcSuccess;

    tcSuccess = TC_CreateBTAccessor();
    ReportTest(tcSuccess, "Create BTAccessor");
    if (!tcSuccess) {
        goto exit;
    }

    tcSuccess = TC_IsEIRCapable();
    ReportTest(tcSuccess, "Check EIR capability");

    tcSuccess = true;
    for (it = tcList.begin(); (it != tcList.end()); ++it) {
        TestCaseInfo& test = *it;
        if (!test.reqPrevTC && tcSuccess) {
            tcSuccess = (this->*(test.tc))();
            ReportTest(tcSuccess, test.description);
        }
    }

    tcSuccess = TC_DestroyBTAccessor();
    ReportTest(tcSuccess, "Destroy BTAccessor");

exit:
    return success ? 0 : 1;
}

void TestDriver::ReportTest(bool tcSuccess, const String description)
{
    static const size_t maxWidth = 80;
    static const size_t tcWidth = 2;
    static const size_t tcNumWidth = 1 + ((detailList.size() > 100) ? 3 :
                                          ((detailList.size() > 10) ? 2 : 1));
    static const size_t tcColonWidth = 1;
    static const size_t pfWidth = 5;
    static const size_t dashWidth = 2;
    static const size_t descWidth = maxWidth - (tcWidth + tcNumWidth + tcColonWidth + pfWidth + dashWidth + 1);
    static const size_t detailIndent = (4 + (maxWidth - descWidth));
    static const size_t detailWidth = maxWidth - (detailIndent + dashWidth + 1);

    size_t i;
    String line;
    String tc;
    String desc = description;
    tc.reserve(maxWidth);

    tc.append("TC");
    tc.append(U32ToString(++testcase, 10, tcNumWidth, ' '));
    tc.append(":");
    tc.append(tcSuccess ? " PASS" : " FAIL");

    if (!desc.empty()) {
        tc.append(" - ");
    }

    while (!desc.empty()) {
        if (desc.size() > descWidth) {
            line = desc.substr(0, desc.find_last_of(' ', descWidth));
            desc = desc.substr(line.size());
        } else {
            line = desc;
            desc.clear();
        }
        tc.append(line);
        printf("%s\n", tc.c_str());
        tc.clear();
        for (i = 0; i < (maxWidth - descWidth); ++i) {
            tc.push_back(' ');
        }
    }

    for (; i < detailIndent; ++i) {
        tc.push_back(' ');
    }

    while (detailList.begin() != detailList.end()) {
        String detail = detailList.front();
        if (!detail.empty()) {
            while (!detail.empty()) {
                if (detail.size() > detailWidth) {
                    line = detail.substr(0, detail.find_last_of(' ', detailWidth));
                    desc = detail.substr(line.size());
                } else {
                    line = detail;
                    detail.clear();
                }
                tc.append(" - ");
                tc.append(line);
                printf("%s\n", tc.c_str());
                tc.clear();
                for (i = 0; i < detailIndent; ++i) {
                    tc.push_back(' ');
                }
            }
        }
        detailList.erase(detailList.begin());
    }
    success = success && tcSuccess;
}

void TestDriver::ReportTestDetail(const String detail) const
{
    if (reportDetails) {
        detailList.push_back(detail);
    }
}

void TestDriver::TestBTDeviceAvailable(bool available)
{
    String detail = "Received device ";
    detail += available ? "available" : "unavailable";
    detail += " indication from BTAccessor";
    ReportTestDetail(detail);
    btDevAvailQueue.push_back(available);
    btDevAvailEvent.SetEvent();
}

bool ClientTestDriver::TestCheckIncomingAddress(const BDAddress& addr) const
{
    String detail = "BTAccessor needs BD Address ";
    detail += addr.ToString().c_str();
    detail += " checked";
    ReportTestDetail(detail);
    detail = "Responding with reject since this is the Client Test Driver";
    ReportTestDetail(detail);

    return false;
}

bool ServerTestDriver::TestCheckIncomingAddress(const BDAddress& addr) const
{
    String detail = "BTAccessor needs BD Address ";
    detail += addr.ToString().c_str();
    detail += " checked";
    ReportTestDetail(detail);
    detail = "Responding with ";
    detail += allowIncomingAddress ? "allow" : "reject";
    ReportTestDetail(detail);

    return allowIncomingAddress;
}

void ClientTestDriver::TestDeviceChange(const BDAddress& bdAddr,
                                        uint32_t uuidRev,
                                        bool eirCapable)
{
    String detail = "BTAccessor reported a found device to us: ";
    detail += bdAddr.ToString().c_str();
    ReportTestDetail(detail);
    if (eirCapable) {
        detail = "It is EIR capable with a UUID Revision of 0x";
        detail += U32ToString(uuidRev, 16);
        detail += ".";
    } else {
        detail = "It is not EIR capable.";
    }
    ReportTestDetail(detail);

    bdAddrQueue.push_back(bdAddr);
    bdAddrEvent.SetEvent();
}

void ServerTestDriver::TestDeviceChange(const BDAddress& bdAddr,
                                        uint32_t uuidRev,
                                        bool eirCapable)
{
    ReportTestDetail("BTAccessor reported a found device to us.");
    ReportTestDetail("Ignoring since this is the Server Test Driver.");
}




/****************************************/

bool TestDriver::TC_CreateBTAccessor()
{
    GUID busGuid;
    btAccessor = new BTAccessor(this, busGuid.ToString());

    return true;
}

bool TestDriver::TC_DestroyBTAccessor()
{
    delete btAccessor;
    btAccessor = NULL;
    return true;
}

bool TestDriver::TC_StartBTAccessor()
{
    bool available = false;

    btDevAvailQueue.clear();
    btDevAvailEvent.ResetEvent();

    QStatus status = btAccessor->Start();
    if (status != ER_OK) {
        goto exit;
    }

    do {
        status = Event::Wait(btDevAvailEvent, 30000);
        if (status != ER_OK) {
            String detail = "Waiting for BT device available notification failed: ";
            detail += QCC_StatusText(status);
            ReportTestDetail(detail);
            goto exit;
        }

        btDevAvailEvent.ResetEvent();

        while (!btDevAvailQueue.empty()) {
            available = btDevAvailQueue.front();
            btDevAvailQueue.pop_front();
        }

        if (!available) {
            fprintf(stderr, "Please enable system's Bluetooth.\n");
        }
    } while (!available);

exit:
    return (status == ER_OK);
}

bool TestDriver::TC_StopBTAccessor()
{
    bool available = true;
    QStatus status = ER_OK;

    btAccessor->Stop();

    do {
        status = Event::Wait(btDevAvailEvent, 30000);
        if (status != ER_OK) {
            String detail = "Waiting for BT device available notification failed: ";
            detail += QCC_StatusText(status);
            ReportTestDetail(detail);
            goto exit;
        }

        btDevAvailEvent.ResetEvent();

        while (!btDevAvailQueue.empty()) {
            available = btDevAvailQueue.front();
            btDevAvailQueue.pop_front();
        }
    } while (available);

exit:
    return (status == ER_OK);
}

bool TestDriver::TC_IsMaster()
{
    bool tcSuccess = true;
    set<BDAddress>::const_iterator it;
    for (it = connectedDevices.begin(); it != connectedDevices.end(); ++it) {
        bool master;
        String detail;
        QStatus status = btAccessor->IsMaster(*it, master);
        if (status == ER_OK) {
            detail = "Got the ";
            detail += master ? "master" : "slave";
        } else {
            detail = "Failed to get master/slave";
            tcSuccess = false;
        }
        detail += " role for connection with ";
        detail += it->ToString().c_str();
        ReportTestDetail(detail);
    }
    return true;
}

bool TestDriver::TC_RequestBTRole()
{
    return true;
}

bool TestDriver::TC_IsEIRCapable()
{
    eirCapable = btAccessor->IsEIRCapable();
    String detail = "The local device is ";
    detail += eirCapable ? "EIR capable" : "not EIR capable";
    ReportTestDetail(detail);
    return true;
}


bool ClientTestDriver::TC_StartDiscovery()
{
    return true;
}

bool ClientTestDriver::TC_StopDiscovery()
{
    return true;
}

bool ClientTestDriver::TC_Connect()
{
    return true;
}

bool ClientTestDriver::TC_GetDeviceInfo()
{
    return true;
}


bool ServerTestDriver::TC_StartDiscoverability()
{
    return true;
}

bool ServerTestDriver::TC_StopDiscoverability()
{
    return true;
}

bool ServerTestDriver::TC_SetSDPInfo()
{
    return true;
}

bool ServerTestDriver::TC_StartConnectable()
{
    return true;
}

bool ServerTestDriver::TC_StopConnectable()
{
    return true;
}

bool ServerTestDriver::TC_Accept()
{
    return true;
}

bool ServerTestDriver::TC_GetL2CAPConnectEvent()
{
    return true;
}

/****************************************/

ClientTestDriver::ClientTestDriver(const String& basename, bool allowInteractive, bool reportDetails) :
    TestDriver(basename, allowInteractive, reportDetails)
{
    AddTestCase((TestDriver::TestCase)&ServerTestDriver::TC_StartBTAccessor, "Start BTAccessor");
    AddTestCase((TestDriver::TestCase)&ServerTestDriver::TC_StopBTAccessor, "Stop BTAccessor");
}


ServerTestDriver::ServerTestDriver(const String& basename, bool allowInteractive, bool reportDetails) :
    TestDriver(basename, allowInteractive, reportDetails),
    allowIncomingAddress(true)
{
    AddTestCase((TestDriver::TestCase)&ServerTestDriver::TC_StartBTAccessor, "Start BTAccessor");
    AddTestCase((TestDriver::TestCase)&ServerTestDriver::TC_StartConnectable, "Start Connectable", true);
    AddTestCase((TestDriver::TestCase)&ServerTestDriver::TC_SetSDPInfo, "Set SDP Info", true);
    AddTestCase((TestDriver::TestCase)&ServerTestDriver::TC_StopBTAccessor, "Stop BTAccessor");
}


/****************************************/

static void Usage(void)
{
    printf("Usage: BTAccessorTester [-h] [-c | -s] [-n <basename>] [-a] [-d]\n"
           "\n"
           "    -h              Print this help message\n"
           "    -c              Run in client mode\n"
           "    -s              Run in server mode\n"
           "    -n <basename>   Set the base name for advertised/find names\n"
           "    -a              Automatic tests only (disable interactive tests)\n"
           "    -d              Output test details\n");
}


static void ParseCmdLine(int argc, char** argv, bool& client, String& basename, bool& allowInteractive, bool& reportDetails)
{
    int i;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0) {
            Usage();
            exit(0);
        } else if (strcmp(argv[i], "-c") == 0) {
            client = true;
        } else if (strcmp(argv[i], "-s") == 0) {
            client = false;
        } else if (strcmp(argv[i], "-n") == 0) {
            ++i;
            if (i == argc) {
                printf("option %s requires a parameter\n", argv[i - 1]);
                Usage();
                exit(-1);
            } else {
                basename = argv[i];
            }
        } else if (strcmp(argv[i], "-a") == 0) {
            allowInteractive = false;
        } else if (strcmp(argv[i], "-d") == 0) {
            reportDetails = true;
        }
    }
}

int main(int argc, char** argv)
{
    bool client = false;
    String basename = "org.alljoyn.BTAccessorTester";
    TestDriver* driver;
    bool allowInteractive = true;
    bool reportDetails = false;

    ParseCmdLine(argc, argv, client, basename, allowInteractive, reportDetails);

    if (client) {
        driver = new ClientTestDriver(basename, allowInteractive, reportDetails);
    } else {
        driver = new ServerTestDriver(basename, allowInteractive, reportDetails);
    }

    int ret = driver->RunTests();
    delete driver;

    return ret;
}
