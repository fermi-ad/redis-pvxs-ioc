#ifndef TCAST_H
#define TCAST_H

#include <osiSock.h>

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <sstream>

#include <epicsThread.h>
#include <epicsMutex.h>
#include <epicsGuard.h>
#include <epicsEvent.h>
#include <epicsTime.h>

#include <dbStaticLib.h>
#include <dbScan.h>

#if __cplusplus<201103L
#  define override
#  define final

#  include <tr1/memory> // for std::tr1::shared_ptr
#endif

#if defined(_WIN32) || defined(__CYGWIN__)

#  if defined(TCAST_API_BUILDING) && defined(EPICS_BUILD_DLL)
/* building library as dll */
#    define TCAST_API __declspec(dllexport)
#  elif !defined(TCAST_API_BUILDING) && defined(EPICS_CALL_DLL)
/* calling library in dll form */
#    define TCAST_API __declspec(dllimport)
#  endif

#elif __GNUC__ >= 4
#  define TCAST_API __attribute__ ((visibility("default")))
#endif

#ifndef TCAST_API
#  define TCAST_API
#endif

extern "C" {
TCAST_API extern int tcastDebug;
TCAST_API extern double tcastTimeout;
}

namespace tcast {

typedef epicsGuard<epicsMutex> Guard;
typedef epicsGuardRelease<epicsMutex> UnGuard;

#if __cplusplus>=201103L
template<typename T>
using auto_ptr = std::unique_ptr<T>;
using std::shared_ptr;
using std::weak_ptr;
#else
using std::auto_ptr;
using std::tr1::shared_ptr;
using std::tr1::weak_ptr;
#endif

//! in-line string builder (eg. for exception messages)
//! eg. @code throw std::runtime_error(SB()<<"Some message"<<42); @endcode
struct SB {
    std::ostringstream strm;
    SB() {}
    operator std::string() const { return strm.str(); }
    template<typename T>
    SB& operator<<(const T& i) { strm<<i; return *this; }
};

std::vector<std::string> split(const std::string& inp);

const char* logtime();

uint8_t parseU8(const std::string& val);
osiSockAddr parseAddr(const std::string& addr, unsigned short defport=0);

struct Record {
    DBENTRY ent;
    Record(dbCommon *prec);
    ~Record();

    const char* info(const char *key, const char *defval=0);
};

struct TCAST_API UDPSocket
{
    SOCKET sock;
    UDPSocket();
    ~UDPSocket();
private:
    UDPSocket(const UDPSocket&);
    UDPSocket& operator=(const UDPSocket&);
};

struct TEvent
{
    const uint8_t event;
    bool active; // records associated

    weak_ptr<TEvent> external_self;

    epicsTimeStamp lasttime;
    IOSCANPVT scan;
    uint32_t lastseq;

    ~TEvent();
private:
    explicit TEvent(uint8_t event);

    void notify(Guard& G, const epicsTimeStamp& ts);

    friend struct TReceiver;
};

struct TCAST_API TReceiver : epicsThreadRunable
{
    static
    void startall();
    static
    void stopall();
    static
    shared_ptr<TReceiver> lookup(const osiSockAddr& addr, const osiSockAddr& iface, bool create=true);

    shared_ptr<TEvent> event(uint8_t event);

    ~TReceiver();
private:
    TReceiver(const std::string& name, const osiSockAddr& addr, const osiSockAddr& iface);

    void procTCLK(Guard& G, uint16_t ptc, const epicsTimeStamp *stamp, const uint8_t* pbase, uint8_t nEvt);
    virtual void run() override final;
    void stop();
public:

    const osiSockAddr& listening() const { return myaddr; }
    bool connected() const { return !intimeout; }

    void waitCycle();

    const std::string name;

    mutable epicsMutex lock;

private:
    UDPSocket sock;
    osiSockAddr myaddr;
    osiSockAddr lastrx;
    auto_ptr<epicsThread> worker;

    typedef std::map<uint8_t, shared_ptr<TEvent> > events_t;
    events_t events;

    epicsTimeStamp lastRxTime;
    uint32_t lastSeq;

    // used to sync test code
    uint32_t cycleCnt;
    epicsEvent cycled;

    bool intimeout;
    bool running;
public:
    unsigned rxCnt,
             errCnt,
             tmoCnt,
             skipCnt,
             pbCnt;
};

} // namespace tcast

#endif // TCAST_H
