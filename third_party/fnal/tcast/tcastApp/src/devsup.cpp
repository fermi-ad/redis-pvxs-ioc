
#include <osiSock.h>

#include <sstream>
#include <stdexcept>

#include <stdlib.h>
#include <string.h>

#include <errlog.h>

#include <devSup.h>
#include <dbAccess.h>
#include <recGbl.h>
#include <alarm.h>
#include <dbCommon.h>
#include <longinRecord.h>

#include "tcast.h"

#include <epicsExport.h>

namespace {
using namespace tcast;

struct priv_t {
    shared_ptr<TReceiver> rx;
    shared_ptr<TEvent> evt;

    enum counter_t {
        rxCnt,
        errCnt,
        tmoCnt,
        skipCnt,
        pbCnt,
    } counter;

    priv_t(dbCommon *prec, const std::string& lstr)
        :counter(rxCnt)
    {
        osiSockAddr group, iface;
        int event = -1;

        memset(&group, 0, sizeof(group));
        memset(&iface, 0, sizeof(iface));

        // default interface is wildcard (0.0.0.0)
        iface.ia.sin_family = AF_INET;

        typedef std::vector<std::string> parts_t;
        parts_t parts(split(lstr));

        for(parts_t::const_iterator it(parts.begin()), end(parts.end()); it!=end; ++it)
        {
            size_t sep = (*it).find_first_of('=');
            std::string cmd, val;

            if(sep>=(*it).size()) {// no command implies event code
                cmd = "event";
                val = (*it);

            } else {
                cmd = (*it).substr(0, sep);
                val = (*it).substr(sep+1);
            }

            if(cmd=="group") {
                group = parseAddr(val, 50090);

            } else if(cmd=="iface") {
                iface = parseAddr(val);

            } else if(cmd=="event") {
                event = parseU8(val);

            } else if(cmd=="counter") {
                if(val=="rx") {
                    counter = rxCnt;
                } else if (val=="err") {
                    counter = errCnt;
                } else if (val=="tmo") {
                    counter = tmoCnt;
                } else if (val=="skip") {
                    counter = skipCnt;
                } else if (val=="playback") {
                    counter = pbCnt;
                } else {
                    errlogPrintf("%s : warning : link references counter: counter=%s\n",
                                 prec->name, val.c_str());
                }

            } else {
                errlogPrintf("%s : warning : link contains unknown parameter: %s=%s\n",
                             prec->name, cmd.c_str(), val.c_str());
            }
        }

        if(group.ia.sin_family!=AF_INET)
            throw std::runtime_error("Link missing required parameter: group=");

        // IPv4 mcast groups don't have a port,
        // but for user sanity use port to be specified along with the group
        // to allow iface= to be optional
        iface.ia.sin_port = group.ia.sin_port;

        if(prec->tpro>1) {
            char buf[30];
            ipAddrToDottedIP(&group.ia, buf, sizeof(buf));
            buf[sizeof(buf)-1] = '\0';
            errlogPrintf("%s : group=%s event=%d\n",
                         prec->name, buf, event);
        }

        rx = TReceiver::lookup(group, iface);

        if(event!=-1) {
            Guard G(rx->lock);
            evt = rx->event(event);
        }
    }
};

long add_record_tcast(dbCommon *pcommon)
{
    struct link* plink = dbGetDevLink(pcommon);

    if(pcommon->tpro>1)
        errlogPrintf("%s : %s()\n", pcommon->name, __PRETTY_FUNCTION__);

    if(!plink || pcommon->dpvt)
        return -1;

    try {
        std::ostringstream fulllink;

        // initialize with global config (defaults)
        if(const char* env = getenv("TCAST_DEFAULT"))
            fulllink<<env<<' ';

        Record ent(pcommon);
        // prefix with info() tag for this record
        if(const char* conf = ent.info("tclk:conf"))
            fulllink<<conf<<' ';

        // finish with INP/OUT
        fulllink<<plink->value.instio.string;

        if(pcommon->tpro>1)
            errlogPrintf("%s : full link \"%s\"\n", pcommon->name, fulllink.str().c_str());

        auto_ptr<priv_t> priv(new priv_t(pcommon, fulllink.str()));

        pcommon->dpvt = priv.release();

        return 0;

    }catch(std::exception& e){
        errlogPrintf("%s : %s : %s\n", pcommon->name, __PRETTY_FUNCTION__, e.what());
        return -1;
    }
}

long del_record_tcast(dbCommon *pcommon)
{
    if(pcommon->tpro>1)
        errlogPrintf("%s : %s()\n", pcommon->name, __PRETTY_FUNCTION__);
    if(pcommon->dpvt) {
        auto_ptr<priv_t> priv((priv_t*)pcommon->dpvt);
        pcommon->dpvt = 0;
    }
    return 0;
}

dsxt dsxt_tcast = {
    add_record_tcast,
    del_record_tcast,
};

long init_tcast(int after)
{
    if(!after)
        devExtend(&dsxt_tcast);
    return 0;
}

#define TRY \
    priv_t* priv = (priv_t*)prec->dpvt; \
    if(!priv) { \
        (void)recGblSetSevr(prec, COMM_ALARM, INVALID_ALARM); \
        return -1; \
    } \
    try

#define CATCH() \
    catch(std::exception& e) { \
        errlogPrintf("%s : __PRETTY_FUNCTION__ : %s\n", prec->name, e.what()); \
        (void)recGblSetSevr(prec, COMM_ALARM, INVALID_ALARM); \
    }

long init_record_tcast(dbCommon *pcommon)
{
    if(pcommon->tpro>1)
        errlogPrintf("%s : %s\n", pcommon->name, __PRETTY_FUNCTION__);
    return 0;
}

long get_ioint_info_tcast(int detach, struct dbCommon *prec, IOSCANPVT* pscan)
{
    TRY {
        if(priv->evt)
            *pscan = priv->evt->scan;
    }CATCH()
            if(prec->tpro>1)
                errlogPrintf("%s : %s(%d, %p)\n", prec->name, __PRETTY_FUNCTION__, detach, *pscan);
    return 0;
}

long read_tcast(longinRecord *prec)
{
    TRY {
        if(!priv->evt) {
            (void)recGblSetSevr(prec, READ_ALARM, INVALID_ALARM);
            return -1;
        }

        Guard G(priv->rx->lock);

        prec->val = priv->evt->lastseq;

        if(!priv->rx->connected()) {
            (void)recGblSetSevr(prec, COMM_ALARM, INVALID_ALARM);
        }

        if(prec->tse==epicsTimeEventDeviceTime) {
            prec->time = priv->evt->lasttime;
#ifdef HAS_ALARM_MESSAGE
            prec->utag = priv->evt->event;
#endif
        }

        if(prec->tpro>1)
            errlogPrintf("%s : cnt=%u time=%u:%u\n", prec->name, unsigned(prec->val),
                         unsigned(priv->evt->lasttime.secPastEpoch),
                         unsigned(priv->evt->lasttime.nsec));

        return 0;
    }CATCH()
    return -1;
}

long read_tcast_counter(longinRecord *prec)
{
    if(prec->tpro>1)
        errlogPrintf("%s : %s()\n", prec->name, __PRETTY_FUNCTION__);
    TRY {
        Guard G(priv->rx->lock);

        switch(priv->counter) {
        case priv_t::rxCnt: prec->val = priv->rx->rxCnt; break;
        case priv_t::errCnt: prec->val = priv->rx->errCnt; break;
        case priv_t::tmoCnt: prec->val = priv->rx->tmoCnt; break;
        case priv_t::skipCnt: prec->val = priv->rx->skipCnt; break;
        case priv_t::pbCnt: prec->val = priv->rx->pbCnt; break;
        default:
            prec->val = 0;
            (void)recGblSetSevr(prec, READ_ALARM, INVALID_ALARM);
            break;
        }

        return 0;
    }CATCH()
    return -1;
}

longindset devTcastLIEvt = {
    {
        5,
        0,
        init_tcast,
        init_record_tcast,
        get_ioint_info_tcast,
    },
    read_tcast,
};

longindset devTcastLICnt = {
    {
        5,
        0,
        init_tcast,
        init_record_tcast,
        0,
    },
    read_tcast_counter,
};

} // namespace

extern "C" {
epicsExportAddress(dset, devTcastLIEvt);
epicsExportAddress(dset, devTcastLICnt);
}
