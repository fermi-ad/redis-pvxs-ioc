
#include <string.h>

#include <envDefs.h>
#include <dbAccess.h>
#include <iocsh.h>
#include <generalTimeSup.h>

#include <dbUnitTest.h>
#include <testMain.h>

#include <tcast.h>
#include <sim.h>

static
void timeAddUS(epicsTimeStamp *ts, int32_t us)
{
    // use integer math to avoid FP complications of epicsTimeAddSeconds()
    uint64_t curns = uint64_t(ts->secPastEpoch)*1000000000llu + uint64_t(ts->nsec);
    curns += us*1000;
    ts->secPastEpoch = curns/1000000000llu;
    ts->nsec = curns%1000000000llu;
}

static
int testRecTime(const char *pvname, const epicsTimeStamp& expected, int32_t offsetus=0)
{
    epicsTimeStamp actual, test(expected);
    timeAddUS(&test, offsetus);

    dbCommon* prec = testdbRecordPtr(pvname);
    dbScanLock(prec);
    actual = prec->time;
    dbScanUnlock(prec);

    return testOk(actual.secPastEpoch==test.secPastEpoch && actual.nsec==test.nsec,
                  "\"%s.TIME\" %u:%u == %u:%u",
                  pvname,
                  unsigned(actual.secPastEpoch), unsigned(actual.nsec),
                  unsigned(test.secPastEpoch), unsigned(test.nsec)
                  );
}

extern "C" {
long testioc_registerRecordDeviceDriver(dbBase*);
}

namespace {
using namespace tcast;

SOCKET sender;
shared_ptr<TReceiver> rx;
osiSockAddr rxaddr;
epicsMutex *simTimeLock;
// start non-zero to keep iocInit() from complaining about our fake times
epicsTimeStamp simTime = {2*24*60*60, 0};

int getSimTime(epicsTimeStamp* ts)
{
    Guard G(*simTimeLock);
    *ts = simTime;
    return 0;
}

epicsTimeStamp incSimTimeUS(int32_t us)
{
    Guard G(*simTimeLock);
    timeAddUS(&simTime, us);
    return simTime;
}

void testBasic()
{
    testDiag("%s", __PRETTY_FUNCTION__);

    epicsTimeStamp expectts;

    SimMsg msg;
    expectts = msg.stamp;
    msg.TCLK.add(20,  999, 111);
    msg.TCLK.add(15, 1000, 123);

    // first time may resize RX buffer
    incSimTimeUS(1);
    msg.sendto(sender, rxaddr);
    rx->waitCycle();

    incSimTimeUS(1);
    msg.sendto(sender, rxaddr);
    rx->waitCycle();

    testdbGetFieldEqual("TST:E15:Cnt-I", DBF_LONG, 123);

    // flush event (15) takes the reference timestamp
    testRecTime("TST:E15:Cnt-I", expectts);

    testdbGetFieldEqual("TST:E20:Cnt-I", DBF_LONG, 111);

    // event 20 is 1 microsecond earlier
    testRecTime("TST:E20:Cnt-I", expectts, -1);
}

void testPrev()
{
    testDiag("%s", __PRETTY_FUNCTION__);

    SimMsg msg1;
    msg1.seq = 0xffffffff; // let's test roll-over as well
    msg1.stamp.nsec+=1;
    msg1.TCLK.add(20,  999, 211);
    msg1.TCLK.add(15, 1000, 222);

    SimMsg msg2; // we will "lose" this message
    msg2.seq = 0;
    msg2.stamp.nsec+=2;
    msg2.PTCLK = msg1.TCLK;
    msg2.TCLK.add(20,  999, 212);
    msg2.TCLK.add(15, 1000, 223);

    SimMsg msg3;
    msg3.seq = 1;
    msg3.stamp.nsec+=3;
    msg3.PTCLK = msg2.TCLK;
    // omit #20
    msg3.TCLK.add(15, 1000, 224);

    epicsTimeStamp ts1, ts3;

    ts1 = incSimTimeUS(1);
    msg1.sendto(sender, rxaddr);
    rx->waitCycle();

    // don't send msg2

    // first time may resize RX buffer
    incSimTimeUS(1);
    msg3.sendto(sender, rxaddr);
    rx->waitCycle();

    ts3 = incSimTimeUS(1);
    msg3.sendto(sender, rxaddr);
    rx->waitCycle();

    testdbGetFieldEqual("TST:E15:Cnt-I", DBF_LONG, 224);
    testRecTime("TST:E15:Cnt-I", msg3.stamp);
    testdbGetFieldEqual("TST:E20:Cnt-I", DBF_LONG, 212);
    // playback from PTCLK uses RX time
    testRecTime("TST:E20:Cnt-I", ts3);
}

} // namespace

MAIN(testioc)
{
    // must hook in early to avoid monotonic check
    simTimeLock = new epicsMutex;
    generalTimeRegisterCurrentProvider("SIM", 0, &getSimTime);

    // CI runners can be sloooow
    tcastTimeout = 2.0;

    testPlan(14);
    // we use an address from the MCAST-TEST-NET range, on loopback only
    // choose a random port for further isolation
    epicsEnvSet("TCAST_DEFAULT", "group=233.252.111.222:0 iface=127.0.0.1");

    testdbPrepare();

    {
        epicsTimeStamp current, simtime;
        epicsTimeGetCurrent(&current);
        getSimTime(&simtime);
        testOk1(current.secPastEpoch==simtime.secPastEpoch && current.nsec==simtime.nsec);
    }

    testdbReadDatabase("testioc.dbd", 0, 0);
    testOk1(testioc_registerRecordDeviceDriver(pdbbase)==0);
    iocshCmd("var(tcastDebug, 5)");

    testdbReadDatabase("tcastHealth.db", "../../db", "P=TST:");
    testdbReadDatabase("tcastEvent.db", "../../db", "P=TST:E15:,TPRO=2,TCAST=15");
    testdbReadDatabase("tcastEvent.db", "../../db", "P=TST:E20:,TPRO=2,TCAST=event=20");

    testIocInitOk();
    {
        osiSockAddr group, iface;
        memset(&group, 0, sizeof(group));
        memset(&iface, 0, sizeof(iface));

        group.ia.sin_family = iface.ia.sin_family = AF_INET;
        group.ia.sin_addr.s_addr = htonl(0xe9fc6fde); // 233.252.111.222
        iface.ia.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        rx = TReceiver::lookup(group, iface, false);
    }
    if(!rx)
        testFail("Unable to lookup TReceiver instance");

    rxaddr = rx->listening();
    testOk(rxaddr.ia.sin_family==AF_INET && rxaddr.ia.sin_addr.s_addr==htonl(0xe9fc6fde) && rxaddr.ia.sin_port!=0,
           "Listening on %d", ntohs(rxaddr.ia.sin_port));

    UDPSocket tester;
    sender = tester.sock;

    // override routing to mcast over the loopback interface
    {
        in_addr_t iface = htonl(INADDR_LOOPBACK);
        testOk(setsockopt(sender, IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof(iface))==0, "Set IP_MULTICAST_IF=loopback");
    }
    // limit mcast to local net (loopback shouldn't route anyway)
    {
        osiSockOptMcastTTL_t arg = 1;
        testOk(setsockopt(sender, IPPROTO_IP, IP_MULTICAST_TTL, &arg, sizeof(arg))==0, "Set IP_MULTICAST_TTL=1");
    }
    // loopback isn't useful unless we also copy to other sockets on this host
    {
        osiSockOptMcastLoop_t arg = 1;
        testOk(setsockopt(sender, IPPROTO_IP, IP_MULTICAST_LOOP, &arg, sizeof(arg))==0, "Set IP_MULTICAST_LOOP=1");
    }

    testBasic();
    testPrev();

    testIocShutdownOk();

    testdbCleanup();
    return testDone();
}
