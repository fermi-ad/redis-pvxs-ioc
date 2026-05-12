
#include <osiSock.h>

#include <errlog.h>
#include <epicsTime.h>
#include <epicsStdio.h> // redefines printf() and friends for use in report()
#include <callback.h>
#include <drvSup.h>
#include <initHooks.h>
#include <epicsExit.h>
#include <cantProceed.h>

#include "tcast.h"

#include <map>
#include <stdexcept>

#include <string.h>

#include <epicsExport.h>

#ifndef MSG_DONTWAIT
#  define MSG_DONTWAIT 0
#endif

int tcastDebug;
#define LOG(LVL, MSG, ...) do{ if(tcastDebug>=(LVL)) { \
    if(LVL<=1) \
        errCnt++; \
    errlogPrintf("%s TCAST %s " MSG, logtime(), name.c_str(), ##__VA_ARGS__); \
} }while(0)

double tcastTimeout = 0.5;

namespace tcast {

namespace {
ssize_t recvfromx(int sockfd, void *buf, size_t len,
                  struct sockaddr *src_addr, socklen_t *addrlen,
                  uint32_t* ndrop)
{
#ifdef __linux__
    iovec io = {buf, len};
    union {
        cmsghdr _calign;
        char cbuf[CMSG_SPACE(4)];
    };
    msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_name = src_addr;
    msg.msg_namelen = addrlen ? *addrlen : 0u;
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof(cbuf);
    msg.msg_iov = &io;
    msg.msg_iovlen = 1u;
    msg.msg_flags = 0u;

    ssize_t ret = recvmsg(sockfd, &msg, 0);

    if(ndrop && ret>=0) {
        for(cmsghdr *hdr = CMSG_FIRSTHDR(&msg); hdr; hdr = CMSG_NXTHDR(&msg, hdr)) {
            if(hdr->cmsg_level==SOL_SOCKET && hdr->cmsg_type==SO_RXQ_OVFL && hdr->cmsg_len >= CMSG_LEN(4u)) {
                // Linux only injects the drop count when it is non-zero
                // https://github.com/torvalds/linux/blob/13391c60da3308ed9980de0168f74cce6c62ac1d/net/socket.c#L866
                memcpy(ndrop, CMSG_DATA(hdr), 4u);
            }
        }
    }
    if(msg.msg_flags & MSG_CTRUNC) {
        if(tcastDebug>0)
            errlogPrintf("TCAST MSG_CTRUNC\n");
    }
    if(ret>=0 && addrlen)
        *addrlen = msg.msg_namelen;
    return ret;

#else
    return recvfrom(sockfd, buf, len, flags, src_addr, addrlen);
#endif
}
}

TEvent::TEvent(uint8_t event)
    :event(event)
    ,lastseq(0u)
{
    // scans are forever
    scanIoInit(&scan);
}

void TEvent::notify(Guard &G, const epicsTimeStamp &ts)
{
    lasttime = ts;
    UnGuard U(G); // unlocking TReceiver::lock
    // scanIoImmediate calls dbProcess() for any records on the scan list
    scanIoImmediate(scan, priorityHigh);
    scanIoImmediate(scan, priorityMedium);
    scanIoImmediate(scan, priorityLow);
}

TReceiver::TReceiver(const std::string &name, const osiSockAddr& addr, const osiSockAddr& iface)
    :name(name)
    ,lastSeq(0u)
    ,cycleCnt(0u)
    ,intimeout(true)
    ,running(false)
    ,rxCnt(0u)
    ,errCnt(0u)
    ,tmoCnt(0u)
    ,skipCnt(0u)
    ,pbCnt(0u)
{
    lastrx.sa.sa_family = AF_UNSPEC;
    lastRxTime.secPastEpoch = 0u;
    lastRxTime.nsec = 0u;

    epicsSocketEnableAddressUseForDatagramFanout(sock.sock);

    {
#ifdef _WIN32
#  error Use of SO_RCVTIMEO for repeated timeout as we do is not valid with winsock
#endif
        timeval tmo;
        tmo.tv_sec = time_t(tcastTimeout);
        tmo.tv_usec = time_t((tcastTimeout-double(tmo.tv_sec))*1e6);

        if(setsockopt(sock.sock, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof(tmo)))
            throw std::runtime_error(SB()<<name<<" : can't set timeout\n");
    }

#ifdef __linux__
    {
        int flag = 1;
        if(setsockopt(sock.sock, SOL_SOCKET, SO_RXQ_OVFL, &flag, sizeof(flag)) && tcastDebug>0)
            fprintf(stderr, "TCAST %s: Warning: unable to set SO_RXQ_OVFL\n", name.c_str());
    }
#endif

    // we bind to the mcast address in order to receive only multi-casts.
    // Note: winsock doesn't support this.  Would have to bind to 0.0.0.0
    // and receive mcast as well as ucast/bcast traffic from any interface
    osiSockAddr bindaddr(addr);

    if(bindaddr.ia.sin_family!=AF_INET) {
        memset(&bindaddr, 0, sizeof(bindaddr));
        bindaddr.ia.sin_family = AF_INET;
    }

    if(bind(sock.sock, &bindaddr.sa, sizeof(bindaddr.ia)))
        throw std::runtime_error(SB()<<name<<" : can't bind : "<<strerror(SOCKERRNO));

    {
        osiSocklen_t len = sizeof(myaddr);
        if(getsockname(sock.sock, &myaddr.sa, &len))
            throw std::runtime_error(SB()<<name<<" : can't getsockname()");
    }

    // join group on the requested interface.
    // this has the effect normally accomplished by bind()ing to the interface address
    ip_mreq joiner;
    memset(&joiner, 0, sizeof(joiner));
    joiner.imr_multiaddr = addr.ia.sin_addr;
    joiner.imr_interface = iface.ia.sin_addr;

    if(setsockopt(sock.sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &joiner, sizeof(joiner)))
        throw std::runtime_error(SB()<<name<<" : can't join : "<<strerror(SOCKERRNO));

    worker.reset(new epicsThread(*this, name.c_str(),
                                 epicsThreadGetStackSize(epicsThreadStackBig),
                                 epicsThreadPriorityHigh));

    if(tcastDebug>0)
        printf("TCAST %s %s bound=%08x joined=group=%08x/%08x\n",
               name.c_str(), __PRETTY_FUNCTION__,
               unsigned(ntohl(myaddr.ia.sin_addr.s_addr)),
               unsigned(ntohl(joiner.imr_multiaddr.s_addr)),
               unsigned(ntohl(joiner.imr_interface.s_addr)));
}

TReceiver::~TReceiver()
{
    stop();
}

void TReceiver::stop()
{
    {
        Guard G(lock);
        if(!running)
            return;
        running = false;
    }
    if(tcastDebug>0)
        errlogPrintf("%s %s\n", name.c_str(), __PRETTY_FUNCTION__);

    // send a zero length packet to myself to wake up worker
    char buf = 0;
    (void)sendto(sock.sock, &buf, 0, MSG_DONTWAIT, &myaddr.sa, sizeof(myaddr.ia));
    worker->exitWait();
}

void TReceiver::waitCycle()
{
    Guard G(lock);
    uint32_t cnt = cycleCnt;
    do {
        UnGuard U(G);
        if(!cycled.wait(5.0))
            throw std::runtime_error("waitCycle timeout");
    }while(cnt==cycleCnt);
}

// handles clearing of the 'active' flag when no more device supports are interested
namespace {
struct TRCleanup {
    shared_ptr<TEvent> internal;
    TRCleanup(const shared_ptr<TEvent>& internal) : internal(internal) {}
    void operator()(TEvent*) {
        shared_ptr<TEvent> trash;
        std::swap(trash, internal);

        trash->active = false;
    }
};
} // namespace

shared_ptr<TEvent> TReceiver::event(uint8_t event)
{
    shared_ptr<TEvent> ret;

    events_t::iterator it(events.find(event));

    if(it==events.end()) {
        // initial lazy allocation

        shared_ptr<TEvent> internal(new TEvent(event));

        std::pair<events_t::iterator, bool> pair = events.insert(std::make_pair(event, internal));
        assert(pair.second);
        it = pair.first;
    }

    ret = it->second->external_self.lock();

    if(!ret) {
        // (re)activation
        // wrapper shared_ptr which will clear the active flag

        ret.reset(it->second.get(), TRCleanup(it->second));
        ret->external_self = ret;
        ret->active = true;
    }

    return ret;
}

static
struct TRGbl_t {
    epicsMutex lock;

    typedef std::map<std::string, shared_ptr<TReceiver> > rxs_t;
    rxs_t rxs;

    enum lifecycle_t {
        Init, // before iocInit()
        Run,  // normal operation
        Exit, // after epicsExit()
    } lifecycle;

    TRGbl_t() :lifecycle(Init) {}
} *TRGbl;

static
epicsThreadOnceId TRGblOnce = EPICS_THREAD_ONCE_INIT;

static
void TRGblInit(void*)
{
    try {
        TRGbl = new TRGbl_t;
    } catch(std::exception& e) {
        cantProceed("tcast: error in %s : %s", __PRETTY_FUNCTION__, e.what());
    }
}

TEvent::~TEvent()
{
    assert(TRGbl); // we are only reachable after TReceiver::lookup()

    // we should only actually be delete'd on IOC exit, when leaking 'scan' is acceptable.
    // during normal runtime then 'active' flag should be cleared instead.
    Guard G(TRGbl->lock);
    assert(TRGbl->lifecycle==TRGbl_t::Exit);
}

shared_ptr<TReceiver> TReceiver::lookup(const osiSockAddr& addr, const osiSockAddr& iface, bool create)
{
    epicsThreadOnce(&TRGblOnce, &TRGblInit, 0);

    std::string name;
    {
        char buf[24];
        ipAddrToDottedIP(&addr.ia, buf, sizeof(buf));
        name = buf;
    }
    if(iface.ia.sin_addr.s_addr != htonl(INADDR_ANY)) {
        char buf[24];
        ipAddrToDottedIP(&iface.ia, buf, sizeof(buf));
        name += "/";
        name += buf;
    }

    shared_ptr<TReceiver> ret;

    Guard G(TRGbl->lock);

    if(TRGbl->lifecycle==TRGbl_t::Exit)
        throw std::runtime_error("Refuse to create new TCAST receiver during shutdown");

    TRGbl_t::rxs_t::iterator it = TRGbl->rxs.find(name);
    if(it != TRGbl->rxs.end()) {
        // use existing
        ret = it->second;

    } else if(create) {
        ret.reset(new TReceiver(name, addr, iface));
        if(TRGbl->lifecycle==TRGbl_t::Run)
            ret->worker->start();
        TRGbl->rxs[name] = ret;
    }

    return ret;
}

static
uint32_t extract32(const uint8_t *ptr)
{
    union {
        uint32_t val;
        char buf[4];
    } pun;
    memcpy(pun.buf, ptr, 4);
    return ntohl(pun.val);
}

static
uint16_t extract16(const uint8_t *ptr)
{
    union {
        uint16_t val;
        char buf[2];
    } pun;
    memcpy(pun.buf, ptr, 2);
    return ntohs(pun.val);
}

void TReceiver::run()
{
    uint32_t prevndrop = 0;
    Guard G(lock); // mutex locked unless UnGuard'd

    std::vector<uint8_t> buf(0x2e); // always start too short to exercise resize logic

    while(running) {
        osiSockAddr src;
        osiSocklen_t srclen = sizeof(src);

        // used to sync. test code
        cycleCnt++;
        cycled.trigger();

        int nrx;
        uint32_t ndrop = 0;
        {
            UnGuard U(G);
            nrx = recvfromx(sock.sock, &buf[0], buf.size(), &src.sa, &srclen, &ndrop);
        }
        epicsTimeGetCurrent(&lastRxTime);

        LOG(5, "recvfrom() -> %d (%u)\n", nrx, unsigned(ndrop));

        if(nrx>=0 && ndrop && prevndrop!=ndrop) {
            LOG(2, "Warning: socket RX buffer overflow %u -> %u\n",
                unsigned(prevndrop), unsigned(ndrop));
            prevndrop = ndrop;
        }

        if(nrx < 0 && SOCKERRNO==SOCK_EWOULDBLOCK) {
            // timeout from SO_RCVTIMEO
            tmoCnt++;
            if(!intimeout) {
                LOG(1, "recvfrom() timeout\n");

                intimeout = true;

                // push alarm status to all interested records
                for(events_t::iterator it(events.begin()), end(events.end()); it!=end; ++it) {
                    it->second->notify(G, lastRxTime);
                }
            }
            continue;

        } else if(nrx < 0) {
            int err = SOCKERRNO;
            LOG(1, "recvfrom() Error: (%d) %s\n", int(err), strerror(err));

            // throttle error spam
            UnGuard U(G);
            epicsThreadSleep(2.0);
            continue;

        } else if(src.sa.sa_family!=AF_INET) {
            LOG(1, "recvfrom() Error: ignore !ipv4\n");
            continue;

        } else if(src.ia.sin_addr.s_addr == myaddr.ia.sin_addr.s_addr &&
                src.ia.sin_port == myaddr.ia.sin_port)
        {
            // talking to myself.  Maybe time to exit
            LOG(5, "recvfrom() wakeup\n");
            continue;

        } else if(nrx < 0x2e || memcmp((char*)&buf[7], "\x06""ACCEVENT", 9)!=0) { // check proto version and ID fields
            LOG(1, "Warning: ignore packet with invalid header size=%d\n", nrx);
            continue;
        }

        uint8_t nTCLK = buf[0x20],
                nMIBS = buf[0x21],
                nRRBS = buf[0x22],
                nTVBS = buf[0x23],
                nPTCLK = buf[0x24];

        const size_t msgsize = 0x2e + (nTCLK + nMIBS + nRRBS + nTVBS + nPTCLK)*8u;
        if(buf.size() < msgsize) {
            LOG(1, "ignore truncated packet %zu < %zu \n",
                buf.size(), msgsize);

            if(msgsize <= 0xffff) {
                buf.resize(msgsize);
            }
            continue;

        } else if(size_t(nrx) < msgsize) {
            LOG(1, "Warning: ignore packet with invalid header sizes\n");
            continue;
        }

        rxCnt++;

        uint16_t ptc = extract16(&buf[0x10]);
        uint32_t seq = extract32(&buf[0x18]);
        epicsTimeStamp stamp;
        stamp.secPastEpoch = extract32(&buf[0x26]) - POSIX_TIME_AT_EPICS_EPOCH;
        stamp.nsec = extract32(&buf[0x2a]);

        uint32_t seqdelta = seq-lastSeq;

        if(intimeout) {
            LOG(4, "recvfrom() leaving timeout\n");
            intimeout = false;

        } else if(seqdelta!=1u) {
            LOG(1, "Warning: sequence skip 0x%08x -> 0x%08x\n",
                unsigned(lastSeq), unsigned(seq));
            skipCnt++;

        } else {
            LOG(4, "sequence 0x%08x -> 0x%08x\n",
                unsigned(lastSeq), unsigned(seq));
        }
        lastSeq = seq;

        if(nPTCLK && seqdelta>1u && seqdelta<32u) {
            LOG(2, "Playback %u PTCLK after loss of %u packet(s)\n", nPTCLK, unsigned(seqdelta));
            pbCnt++;

            // can't reconstruct time for PTCLK entries
            procTCLK(G, ptc, 0, &buf[0x2e + (nTCLK + nMIBS + nRRBS + nTVBS)*8u], nPTCLK);
        }

        if(nTCLK)
            procTCLK(G, ptc, &stamp, &buf[0x2e], nTCLK);
    }
}

void TReceiver::procTCLK(Guard &G, uint16_t ptc, const epicsTimeStamp* stamp, const uint8_t* pbase, uint8_t nEvt)
{
    // PTC==CMTF(2) is a special case
    const uint8_t flushEvt = ptc==2 ? 0xa6 : 0x0f;
    int32_t flushStamp = -1;

    // iterate once to find (first) stamp of flushing event
    for(uint8_t i=0; i<nEvt; i++) {
        size_t offset = 8*i;

        if(pbase[offset+3]==flushEvt) {
            if(flushStamp!=-1) {
                LOG(1, "TCLK array contains duplicate evt %u\n", flushEvt);
            }
            if(pbase[offset+0]==0xff) {
                LOG(1, "Warning: flush event %u appears as rapid\n", unsigned(flushEvt));
                flushStamp=-2;

            } else {
                flushStamp = extract32(&pbase[offset])>>8;
            }
        }
    }

    if(!stamp) {
        flushStamp=-1;

    } else if(flushStamp==-1) {
        LOG(1, "Error: valid flush event %u not found in body\n", unsigned(flushEvt));
    }

    // iterate again for actual processing
    for(uint8_t i=0; i<nEvt; i++) {
        size_t offset = 8*i;

        uint32_t fld = extract32(&pbase[offset+0]);
        uint32_t iseq = extract32(&pbase[offset+4]);
        uint8_t evt = fld&0xff;
        fld >>= 8u;
        bool israpid = (fld>>16u)==0xff;

        events_t::iterator it(events.find(evt));
        if(it==events.end()) {
            LOG(3, "ignore unused%s %u\n",
                 israpid?" rapid":"", unsigned(evt));
            continue;
        }

        TEvent& event = *it->second;
        event.lastseq = iseq;

        if(!event.active) {
            LOG(4, "ignore inactive %u\n", unsigned(evt));
            continue;
        }

        if(israpid) {
            // Rapid event
            uint16_t cnt = fld;

            LOG(3, "scan rapid %u cnt=%u\n", unsigned(evt), cnt);

            for(uint16_t t = 0; t < cnt; t++) {
                epicsTimeStamp ts;
                if(flushStamp>=0) {
                    ts = *stamp;

                } else {
                    ts = lastRxTime;
                }
                // ensure a unique (and hopefully monotonic) time for each scan
                epicsTimeAddSeconds(&ts, (int32_t(cnt + t) - flushStamp)*1e-6);
                event.notify(G, ts);
            }

        } else {
            // normal event

            LOG(3, "scan %u\n", unsigned(evt));

            epicsTimeStamp ts;
            if(flushStamp!=-1) {
                ts = *stamp;
                epicsTimeAddSeconds(&ts, (int32_t(fld) - flushStamp)*1e-6);

            } else {
                ts = lastRxTime;
            }
            event.notify(G, ts);
        }
    }
}

void TReceiver::startall()
{
    epicsThreadOnce(&TRGblOnce, &TRGblInit, 0);

    Guard G(TRGbl->lock);

    TRGbl->lifecycle = TRGbl_t::Run;

    for(TRGbl_t::rxs_t::const_iterator it(TRGbl->rxs.begin()), end(TRGbl->rxs.end())
        ; it!=end ; ++it)
    {
        {
            Guard G(it->second->lock);
            it->second->running = true;
        }
        it->second->worker->start();
    }
}

void TReceiver::stopall()
{
    epicsThreadOnce(&TRGblOnce, &TRGblInit, 0);

    TRGbl_t::rxs_t rxs;
    {
        Guard G(TRGbl->lock);

        TRGbl->lifecycle = TRGbl_t::Exit;

        rxs.swap(TRGbl->rxs);
    }

    for(TRGbl_t::rxs_t::const_iterator it(rxs.begin()), end(rxs.end())
        ; it!=end ; ++it)
    {
        it->second->stop();
    }
}

} // namespace tcast

namespace {
using namespace tcast;

long tcast_report(int lvl)
{
    try {
        epicsThreadOnce(&TRGblOnce, &TRGblInit, 0);

        std::vector<shared_ptr<TReceiver> > rxs;
        {
            Guard G(TRGbl->lock);
            rxs.reserve(TRGbl->rxs.size());
            for(TRGbl_t::rxs_t::const_iterator it(TRGbl->rxs.begin()), end(TRGbl->rxs.end())
                ; it!=end ; ++it)
            {
                rxs.push_back(it->second);
            }
        }

        for(size_t i=0; i<rxs.size(); i++) {
            if(!rxs[i])
                continue;
            const TReceiver& rx = *rxs[i];

            Guard G(rx.lock);

            printf("tcast %s #rx=%u #err=%u #tmo=%u #skip=%u #pb=%u\n",
                   rx.name.c_str(), rx.rxCnt, rx.errCnt, rx.tmoCnt, rx.skipCnt, rx.pbCnt);
        }

    }catch(std::exception& e){
        fprintf(stderr, "Error: %s\n", e.what());
    }
    return 0;

}

void tcast_init(initHookState state)
{
    if(state==initHookAfterIocRunning) {
        try {
            TReceiver::startall();
        }catch(std::exception& e){
            errlogPrintf("%s Error: %s\n", __PRETTY_FUNCTION__, e.what());
        }
    }
}

void tcast_exit(void*)
{
    try {
        TReceiver::stopall();
    }catch(std::exception& e){
        errlogPrintf("%s Error: %s\n", __PRETTY_FUNCTION__, e.what());
    }
}

drvet tcastdrv = {
    2,
    (DRVSUPFUN)&tcast_report,
    0,
};

void tcast_registrar()
{
    initHookRegister(&tcast_init);
    epicsAtExit(&tcast_exit, 0);
}

} // namespace

extern "C" {
epicsExportAddress(int, tcastDebug);
epicsExportAddress(double, tcastTimeout);
epicsExportAddress(drvet, tcastdrv);
epicsExportRegistrar(tcast_registrar);
}
