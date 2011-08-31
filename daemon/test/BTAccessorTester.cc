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
#include <set>

#include <qcc/GUID.h>
#include <qcc/Mutex.h>
#include <qcc/String.h>
#include <qcc/Thread.h>
#include <qcc/time.h>

#include "BDAddress.h"
#include "Transport.h"


using namespace std;
using namespace qcc;


namespace ajn {

/********************
 * TEST STUBS
 ********************/


#define _ALLJOYN_REMOTEENDPOINT_H
class RemoteEndpoint {
  public:
    RemoteEndpoint(BusAttachment& bus,
                   bool incoming,
                   const qcc::String& connectSpec,
                   qcc::Stream& stream,
                   const char* type = "endpoint",
                   bool isSocket = true);
};


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
    void DisconnectAll();

    virtual void TestBTDeviceAvailable(bool avail) = 0;
    virtual bool TestCheckIncomingAddress(const BDAddress& addr) const = 0;
    virtual void TestDeviceChange(const BDAddress& bdAddr, uint32_t uuidRev, bool eirCapable) = 0;

  private:
    void DeviceChange(const BDAddress& bdAddr, uint32_t uuidRev, bool eirCapable);

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


#define TestCaseFunction(_tcf) static_cast<TestDriver::TestCase>(&_tcf)


using namespace ajn;

class TestDriver : public BTTransport {
  public:
    typedef bool (TestDriver::*TestCase)();

    struct TestCaseInfo {
        TestDriver::TestCase tc;
        String description;
        bool success;
        TestCaseInfo(TestDriver::TestCase tc, const String& description) :
            tc(tc), description(description), success(false)
        {
        }
    };

    struct DeviceChange {
        BDAddress addr;
        uint32_t uuidRev;
        bool eirCapable;
        DeviceChange(const BDAddress& addr, uint32_t uuidRev, bool eirCapable) :
            addr(addr), uuidRev(uuidRev), eirCapable(eirCapable)
        {
        }
    };


    TestDriver(const String& basename, bool allowInteractive, bool reportDetails);
    virtual ~TestDriver();

    void AddTestCase(TestDriver::TestCase tc, const String description);
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
    bool TC_StartConnectable();
    bool TC_StopConnectable();

  protected:
    BTAccessor* btAccessor;
    String basename;
    GUID busGuid;
    const bool allowInteractive;

    deque<bool> btDevAvailQueue;
    Event btDevAvailEvent;
    Mutex btDevAvailLock;

    deque<DeviceChange> devChangeQueue;
    Event devChangeEvent;
    Mutex devChangeLock;

    set<BDAddress> connectedDevices;

    bool eirCapable;
    BTNodeInfo self;
    BTNodeDB nodeDB;

  private:
    const bool reportDetails;
    list<TestCaseInfo> tcList;
    uint32_t testcase;
    mutable list<String> detailList;
    bool success;
    list<TestCaseInfo>::iterator insertPos;

    void ReportTest(const TestCaseInfo& test);
};


class ClientTestDriver : public TestDriver {
  public:
    struct Counts {
        uint32_t found;
        uint32_t changed;
        uint32_t uuidRev;
        Counts() : found(0), changed(0), uuidRev(0) { }
        Counts(uint32_t uuidRev) : found(1), changed(0), uuidRev(uuidRev) { }
    };

    ClientTestDriver(const String& basename, bool allowInteractive, bool reportDetails);
    virtual ~ClientTestDriver() { }

    bool TestCheckIncomingAddress(const BDAddress& addr) const;
    void TestDeviceChange(const BDAddress& bdAddr, uint32_t uuidRev, bool eirCapable);

    bool TC_StartDiscovery();
    bool TC_StopDiscovery();
    bool TC_ConnectSingle();
    bool TC_ConnectMultiple();
    bool TC_GetDeviceInfo();
    bool TC_ExchangeSmallData();
    bool TC_ExchangeLargeData();

  private:

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
    bool TC_Accept();
    bool TC_GetL2CAPConnectEvent();

  private:
    bool allowIncomingAddress;

    uint32_t uuidRev;
};


TestDriver::TestDriver(const String& basename, bool allowInteractive, bool reportDetails) :
    btAccessor(NULL),
    basename(basename),
    allowInteractive(allowInteractive),
    reportDetails(reportDetails),
    testcase(0),
    success(true)
{
    String uniqueName = ":";
    uniqueName += busGuid.ToShortString();
    uniqueName += ".1";
    self->SetGUID(busGuid);
    self->SetRelationship(_BTNodeInfo::SELF);
    self->SetUniqueName(uniqueName);

    tcList.push_back(TestCaseInfo(TestCaseFunction(TestDriver::TC_CreateBTAccessor), "Create BT Accessor"));
    tcList.push_back(TestCaseInfo(TestCaseFunction(TestDriver::TC_StartBTAccessor), "Start BTAccessor"));
    tcList.push_back(TestCaseInfo(TestCaseFunction(TestDriver::TC_IsEIRCapable), "Check EIR capability"));
    tcList.push_back(TestCaseInfo(TestCaseFunction(TestDriver::TC_StartConnectable), "Start Connectable"));
    tcList.push_back(TestCaseInfo(TestCaseFunction(TestDriver::TC_StopConnectable), "Stop Connectable"));
    insertPos = tcList.end();
    --insertPos;
    tcList.push_back(TestCaseInfo(TestCaseFunction(TestDriver::TC_StopBTAccessor), "Stop BTAccessor"));
    tcList.push_back(TestCaseInfo(TestCaseFunction(TestDriver::TC_DestroyBTAccessor), "Destroy BTAccessor"));
}

TestDriver::~TestDriver()
{
    if (btAccessor) {
        delete btAccessor;
    }
}

void TestDriver::AddTestCase(TestDriver::TestCase tc, const String description)
{
    tcList.insert(insertPos, TestCaseInfo(tc, description));
}

int TestDriver::RunTests()
{
    list<TestCaseInfo>::iterator it;
    for (it = tcList.begin(); success && (it != tcList.end()); ++it) {
        TestCaseInfo& test = *it;
        test.success = (this->*(test.tc))();
        ReportTest(test);
    }

    return success ? 0 : 1;
}

void TestDriver::ReportTest(const TestCaseInfo& test)
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
    static const size_t detailWidth = maxWidth - (detailIndent + dashWidth);

    size_t i;
    String line;
    String tc;
    String desc = test.description;
    tc.reserve(maxWidth);

    tc.append("TC");
    tc.append(U32ToString(++testcase, 10, tcNumWidth, ' '));
    tc.append(":");
    tc.append(test.success ? " PASS" : " FAIL");

    if (!desc.empty()) {
        tc.append(" - ");
    }

    while (!desc.empty()) {
        if (desc.size() > descWidth) {
            line = desc.substr(0, desc.find_last_of(' ', descWidth));
            desc = desc.substr(line.size() + 1);
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
            bool wrapped = false;
            while (!detail.empty()) {
                if (detail.size() > detailWidth) {
                    line = detail.substr(0, detail.find_last_of(' ', detailWidth));
                    detail = detail.substr(detail.find_first_not_of(" ", line.size()));
                } else {
                    line = detail;
                    detail.clear();
                }
                if (wrapped) {
                    tc.append("  ");
                } else {
                    tc.append("- ");
                }
                tc.append(line);
                printf("%s\n", tc.c_str());
                tc.clear();
                for (i = 0; i < detailIndent; ++i) {
                    tc.push_back(' ');
                }
                wrapped = !detail.empty();
            }
        }
        detailList.erase(detailList.begin());
    }
    success = success && test.success;
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
    detail += " indication from BTAccessor.";
    ReportTestDetail(detail);
    btDevAvailLock.Lock();
    btDevAvailQueue.push_back(available);
    btDevAvailLock.Unlock();
    btDevAvailEvent.SetEvent();
}

bool ClientTestDriver::TestCheckIncomingAddress(const BDAddress& addr) const
{
    String detail = "BTAccessor needs BD Address ";
    detail += addr.ToString().c_str();
    detail += " checked.";
    ReportTestDetail(detail);
    detail = "Responding with reject since this is the Client Test Driver.";
    ReportTestDetail(detail);

    return false;
}

bool ServerTestDriver::TestCheckIncomingAddress(const BDAddress& addr) const
{
    String detail = "BTAccessor needs BD Address ";
    detail += addr.ToString().c_str();
    detail += " checked.";
    ReportTestDetail(detail);
    detail = "Responding with ";
    detail += allowIncomingAddress ? "allow" : "reject";
    detail += ".";
    ReportTestDetail(detail);

    return allowIncomingAddress;
}

void ClientTestDriver::TestDeviceChange(const BDAddress& bdAddr,
                                        uint32_t uuidRev,
                                        bool eirCapable)
{
    String detail = "BTAccessor reported a found device to us: ";
    detail += bdAddr.ToString().c_str();
    if (eirCapable) {
        detail += ".  It is EIR capable with a UUID Revision of 0x";
        detail += U32ToString(uuidRev, 16, 8, '0');
        detail += ".";
    } else {
        detail += ".  It is not EIR capable.";
    }
    ReportTestDetail(detail);

    devChangeLock.Lock();
    devChangeQueue.push_back(DeviceChange(bdAddr, uuidRev, eirCapable));
    devChangeLock.Unlock();
    devChangeEvent.SetEvent();
}

void ServerTestDriver::TestDeviceChange(const BDAddress& bdAddr,
                                        uint32_t uuidRev,
                                        bool eirCapable)
{
    ReportTestDetail("BTAccessor reported a found device to us.  Ignoring since this is the Server Test Driver.");
}




/****************************************/

bool TestDriver::TC_CreateBTAccessor()
{
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

    btDevAvailLock.Lock();
    btDevAvailQueue.clear();
    btDevAvailLock.Unlock();
    btDevAvailEvent.ResetEvent();

    QStatus status = btAccessor->Start();
    if (status != ER_OK) {
        String detail = "Call to start BT device failed: ";
        detail += QCC_StatusText(status);
        detail += ".";
        ReportTestDetail(detail);
        goto exit;
    }

    do {
        status = Event::Wait(btDevAvailEvent, 30000);
        if (status != ER_OK) {
            String detail = "Waiting for BT device available notification failed: ";
            detail += QCC_StatusText(status);
            detail += ".";
            ReportTestDetail(detail);
            goto exit;
        }

        btDevAvailEvent.ResetEvent();

        btDevAvailLock.Lock();
        while (!btDevAvailQueue.empty()) {
            available = btDevAvailQueue.front();
            btDevAvailQueue.pop_front();
        }
        btDevAvailLock.Unlock();

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
            detail += ".";
            ReportTestDetail(detail);
            goto exit;
        }

        btDevAvailEvent.ResetEvent();

        btDevAvailLock.Lock();
        while (!btDevAvailQueue.empty()) {
            available = btDevAvailQueue.front();
            btDevAvailQueue.pop_front();
        }
        btDevAvailLock.Unlock();
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
            goto exit;
        }
        detail += " role for connection with ";
        detail += it->ToString().c_str();
        if (status != ER_OK) {
            detail += ": ";
            detail += QCC_StatusText(status);
        }
        detail += ".";
        ReportTestDetail(detail);
    }

exit:
    return tcSuccess;
}

bool TestDriver::TC_RequestBTRole()
{
    bool tcSuccess = true;
    set<BDAddress>::const_iterator it;
    for (it = connectedDevices.begin(); it != connectedDevices.end(); ++it) {
        bool master;
        String detail;
        QStatus status = btAccessor->IsMaster(*it, master);
        if (status != ER_OK) {
            detail = "Failed to get master/slave role with ";
            detail += it->ToString().c_str();
            detail += ": ";
            detail += QCC_StatusText(status);
            detail += ".";
            ReportTestDetail(detail);
            tcSuccess = false;
            goto exit;
        }

        detail = "Switching role with ";
        detail += it->ToString().c_str();
        detail += master ? " to slave" : " to master";
        detail += ".";
        ReportTestDetail(detail);

        bt::BluetoothRole role = master ? bt::SLAVE : bt::MASTER;
        btAccessor->RequestBTRole(*it, role);

        status = btAccessor->IsMaster(*it, master);
        if (status != ER_OK) {
            detail = "Failed to get master/slave role with ";
            detail += it->ToString().c_str();
            detail += ": ";
            detail += QCC_StatusText(status);
            detail += ".";
            ReportTestDetail(detail);
            tcSuccess = false;
            goto exit;
        }

        if (master != (role == bt::MASTER)) {
            detail = "Failed to switch role with ";
            detail += it->ToString().c_str();
            detail += " (not a test case failure).";
            ReportTestDetail(detail);
        }

        detail = "Switching role with ";
        detail += it->ToString().c_str();
        detail += " back to ";
        detail += (role == bt::SLAVE) ? "master" : "slave";
        detail += ".";
        ReportTestDetail(detail);
        role = (role == bt::SLAVE) ? bt::MASTER : bt::SLAVE;
        btAccessor->RequestBTRole(*it, role);
    }

exit:
    return tcSuccess;
}

bool TestDriver::TC_IsEIRCapable()
{
    eirCapable = btAccessor->IsEIRCapable();
    self->SetEIRCapable(eirCapable);
    String detail = "The local device is ";
    detail += eirCapable ? "EIR capable" : "not EIR capable";
    detail += ".";
    ReportTestDetail(detail);
    return true;
}

bool TestDriver::TC_StartConnectable()
{
    QStatus status;
    BTBusAddress addr;

    status = btAccessor->StartConnectable(addr.addr, addr.psm);
    bool tcSuccess = (status == ER_OK);
    if (tcSuccess) {
        self->SetBusAddress(addr);
        nodeDB.AddNode(self);
    } else {
        String detail = "Call to start connectable returned failure code: ";
        detail += QCC_StatusText(status);
        detail += ".";
        ReportTestDetail(status);
    }

    return tcSuccess;
}

bool TestDriver::TC_StopConnectable()
{
    bool tcSuccess = true;
    btAccessor->StopConnectable();
    Event* l2capEvent = btAccessor->GetL2CAPConnectEvent();
    if (l2capEvent) {
        QStatus status = Event::Wait(*l2capEvent, 500);
        if ((status == ER_OK) ||
            (status == ER_TIMEOUT)) {
            ReportTestDetail("L2CAP connect event object is still valid.");
            tcSuccess = false;
        }
    }

    nodeDB.RemoveNode(self);

    return tcSuccess;
}


bool ClientTestDriver::TC_StartDiscovery()
{
    bool tcSuccess = true;
    QStatus status;
    BDAddressSet ignoreAddrs;
    uint64_t now;
    uint64_t stop;
    Timespec tsNow;
    String detail;
    map<BDAddress, Counts> findCount;
    map<BDAddress, Counts>::iterator fcit;

    set<BDAddress>::const_iterator it;
    for (it = connectedDevices.begin(); it != connectedDevices.end(); ++it) {
        ignoreAddrs->insert(*it);
    }

    GetTimeNow(&tsNow);
    now = tsNow.GetAbsoluteMillis() + 35000;
    stop = now + 30000;

    devChangeLock.Lock();
    devChangeQueue.clear();
    devChangeEvent.ResetEvent();
    devChangeLock.Unlock();

    status = btAccessor->StartDiscovery(ignoreAddrs, 30);
    if (status != ER_OK) {
        detail = "Call to start discovery failed: ";
        detail += QCC_StatusText(status);
        detail += ".";
        ReportTestDetail(detail);
        tcSuccess = false;
        goto exit;
    }

    while (now < stop) {
        status = Event::Wait(devChangeEvent, stop - now);
        if (status == ER_TIMEOUT) {
            break;
        }

        devChangeEvent.ResetEvent();

        devChangeLock.Lock();
        while (!devChangeQueue.empty()) {
            fcit = findCount.find(devChangeQueue.front().addr);
            if (fcit == findCount.end()) {
                findCount[devChangeQueue.front().addr] = Counts(devChangeQueue.front().uuidRev);
            } else {
                ++(fcit->second.found);
                if (fcit->second.uuidRev != devChangeQueue.front().uuidRev) {
                    ++(fcit->second.changed);
                    fcit->second.uuidRev = devChangeQueue.front().uuidRev;
                }
            }
            devChangeQueue.pop_front();
        }
        devChangeLock.Unlock();

        GetTimeNow(&tsNow);
        now = tsNow.GetAbsoluteMillis();
    }

    if (findCount.empty()) {
        ReportTestDetail("No devices found");
    } else {
        for (fcit = findCount.begin(); fcit != findCount.end(); ++fcit) {
            detail = "Found ";
            detail += fcit->first.ToString().c_str();
            detail += " ";
            detail += U32ToString(fcit->second.found).c_str();
            detail += " times";
            if (fcit->second.changed > 0) {
                detail += " - changed ";
                detail += U32ToString(fcit->second.changed).c_str();
                detail += " times";
            }
            detail += " (UUID Rev: 0x";
            detail += U32ToString(fcit->second.uuidRev, 16, 8, '0');
            detail += ")";
            detail += ".";
            ReportTestDetail(detail);
        }
    }

    Sleep(5000);

    devChangeLock.Lock();
    devChangeQueue.clear();
    devChangeEvent.ResetEvent();
    devChangeLock.Unlock();

    status = Event::Wait(devChangeEvent, 30000);
    if (status != ER_TIMEOUT) {
        ReportTestDetail("Received device found notification long after discovery should have stopped.");
        tcSuccess = false;
        devChangeLock.Lock();
        devChangeQueue.clear();
        devChangeEvent.ResetEvent();
        devChangeLock.Unlock();
        goto exit;
    }

    // Start infinite discovery until stopped
    status = btAccessor->StartDiscovery(ignoreAddrs, 0);
    if (status != ER_OK) {
        detail = "Call to start discovery with infinite timeout failed: ";
        detail += QCC_StatusText(status);
        detail += ".";
        ReportTestDetail(detail);
        tcSuccess = false;
    }

exit:
    return tcSuccess;
}

bool ClientTestDriver::TC_StopDiscovery()
{
    QStatus status;
    bool tcSuccess = true;
    size_t count;
    BDAddressSet ignoreAddrs;
    String detail;

    status = btAccessor->StopDiscovery();
    if (status != ER_OK) {
        detail = "Call to stop discovery failed: ";
        detail += QCC_StatusText(status);
        detail += ".";
        ReportTestDetail(detail);
        tcSuccess = false;
        goto exit;
    }

    Sleep(5000);

    devChangeLock.Lock();
    count = devChangeQueue.size();
    devChangeQueue.clear();
    devChangeEvent.ResetEvent();
    devChangeLock.Unlock();

    status = Event::Wait(devChangeEvent, 30000);
    if (status != ER_TIMEOUT) {
        ReportTestDetail("Received device found notification long after discovery should have stopped.");
        tcSuccess = false;
        devChangeLock.Lock();
        devChangeQueue.clear();
        devChangeEvent.ResetEvent();
        devChangeLock.Unlock();
    }

exit:
    return tcSuccess;
}

bool ClientTestDriver::TC_ConnectSingle()
{
    ReportTestDetail("NOT YET IMPLEMENTED");
    return true;
}

bool ClientTestDriver::TC_ConnectMultiple()
{
    ReportTestDetail("NOT YET IMPLEMENTED");
    return true;
}

bool ClientTestDriver::TC_GetDeviceInfo()
{
    ReportTestDetail("NOT YET IMPLEMENTED");
    return true;
}

bool ClientTestDriver::TC_ExchangeSmallData()
{
    ReportTestDetail("NOT YET IMPLEMENTED");
    return true;
}

bool ClientTestDriver::TC_ExchangeLargeData()
{
    ReportTestDetail("NOT YET IMPLEMENTED");
    return true;
}


bool ServerTestDriver::TC_StartDiscoverability()
{
    bool tcSuccess = true;
    QStatus status;

    status = btAccessor->StartDiscoverability();
    if (status != ER_OK) {
        String detail = "Call to start discoverability failed: ";
        detail += QCC_StatusText(status);
        detail += ".";
        ReportTestDetail(detail);
    }

    return tcSuccess;
}

bool ServerTestDriver::TC_StopDiscoverability()
{
    bool tcSuccess = true;
    QStatus status;

    status = btAccessor->StopDiscoverability();
    if (status != ER_OK) {
        String detail = "Call to stop discoverability failed: ";
        detail += QCC_StatusText(status);
        detail += ".";
        ReportTestDetail(detail);
    }

    return tcSuccess;
}

bool ServerTestDriver::TC_SetSDPInfo()
{
    QStatus status;
    String adName = basename + "." + self->GetBusAddress().addr.ToString('_') + ".";
    int i;

    // Advertise 100 names for the local device.
    for (i = 0; i < 100; ++i) {
        self->AddAdvertiseName(adName + RandHexString(4));
    }

    // Advertise names for 100 nodes
    for (i = 0; i < 100; ++i) {
        BDAddress addr(RandHexString(6));
        BTBusAddress busAddr(addr, Rand32() % 0xffff);
        BTNodeInfo fakeNode;
        int j;
        fakeNode = BTNodeInfo(busAddr);
        adName = basename + "." + fakeNode->GetBusAddress().addr.ToString('_') + ".";
        for (j = 0; j < 5; ++j) {
            fakeNode->AddAdvertiseName(adName + RandHexString(4));
        }
        nodeDB.AddNode(fakeNode);
    }

    status = btAccessor->SetSDPInfo(uuidRev,
                                    self->GetBusAddress().addr,
                                    self->GetBusAddress().psm,
                                    nodeDB);
    bool tcSuccess = (status == ER_OK);
    if (!tcSuccess) {
        String detail = "Call to set SDP information returned failure code: ";
        detail += QCC_StatusText(status);
        detail += ".";
        ReportTestDetail(status);
    }
    return tcSuccess;
}

bool ServerTestDriver::TC_Accept()
{
    Sleep(120000);  // Temporary until TC_Accpet() is implemented

    ReportTestDetail("NOT YET IMPLEMENTED");
    return true;
}

bool ServerTestDriver::TC_GetL2CAPConnectEvent()
{
    bool tcSuccess = false;
    Event* l2capEvent = btAccessor->GetL2CAPConnectEvent();
    if (l2capEvent) {
        QStatus status = Event::Wait(*l2capEvent, 500);
        if ((status == ER_OK) ||
            (status == ER_TIMEOUT)) {
            tcSuccess = true;
        } else {
            ReportTestDetail("L2CAP connect event object is invalid.");
        }
    } else {
        ReportTestDetail("L2CAP connect event object does not exist.");
    }
    return tcSuccess;
}

/****************************************/

ClientTestDriver::ClientTestDriver(const String& basename, bool allowInteractive, bool reportDetails) :
    TestDriver(basename, allowInteractive, reportDetails)
{
    AddTestCase(TestCaseFunction(ClientTestDriver::TC_StartDiscovery), "Start Discovery");
    AddTestCase(TestCaseFunction(ClientTestDriver::TC_GetDeviceInfo), "Get Device Information");
    AddTestCase(TestCaseFunction(ClientTestDriver::TC_StopDiscovery), "Stop Discovery");
    AddTestCase(TestCaseFunction(ClientTestDriver::TC_ConnectSingle), "Single Connection to Server");
    AddTestCase(TestCaseFunction(ClientTestDriver::TC_ConnectMultiple), "Multiple Simultaneous Connections to Server");
    AddTestCase(TestCaseFunction(ClientTestDriver::TC_ExchangeSmallData), "Exchange Small Amount of Data");
    AddTestCase(TestCaseFunction(ClientTestDriver::TC_ExchangeLargeData), "Exchange Large Amount of Data");
}


ServerTestDriver::ServerTestDriver(const String& basename, bool allowInteractive, bool reportDetails) :
    TestDriver(basename, allowInteractive, reportDetails),
    allowIncomingAddress(true)
{
    while (uuidRev == bt::INVALID_UUIDREV) {
        uuidRev = Rand32();
    }

    AddTestCase(TestCaseFunction(ServerTestDriver::TC_SetSDPInfo), "Set SDP Information");
    AddTestCase(TestCaseFunction(ServerTestDriver::TC_GetL2CAPConnectEvent), "Check L2CAP Connect Event Object");
    AddTestCase(TestCaseFunction(ServerTestDriver::TC_StartDiscoverability), "Start Discoverability");
    AddTestCase(TestCaseFunction(ServerTestDriver::TC_Accept), "Accept Incoming Connections");
    AddTestCase(TestCaseFunction(ServerTestDriver::TC_StopDiscoverability), "Stop Discoverability");
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
