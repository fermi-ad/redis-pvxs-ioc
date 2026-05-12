
#include <stdexcept>

#include <string.h>

#include <dbAccess.h>
#include <epicsStdlib.h>

#include "tcast.h"

namespace tcast {

std::vector<std::string> split(const std::string& inp)
{
    std::vector<std::string> ret;
    std::istringstream strm(inp);
    std::string line;
    while(std::getline(strm, line, ' ')) {
        if(!line.empty())
            ret.push_back(line);
    }
    return ret;
}

uint8_t parseU8(const std::string& val)
{
    epicsUInt8 ret;
    if(epicsParseUInt8(val.c_str(), &ret, 0, 0))
        throw std::runtime_error(SB()<<"Can't parse as uint8 : "<<val);
    return ret;
}

osiSockAddr parseAddr(const std::string& addr, unsigned short defport)
{
    osiSockAddr ret;
    if(aToIPAddr(addr.c_str(), defport, &ret.ia))
        throw std::runtime_error(SB()<<"Not ip:port or host:port : "<<addr);
    return ret;
}

const char* logtime()
{
    static __thread char buf[64];

    epicsTimeStamp now;
    if(epicsTimeGetCurrent(&now)) {
        strcpy(buf, "<no time>");

    } else {
        epicsTimeToStrftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.%9f", &now);
        buf[sizeof(buf)-1] = '\0'; // paranoia
    }
    return buf;
}

Record::Record(dbCommon *prec)
{
    dbInitEntryFromRecord(prec, &ent);
}

Record::~Record()
{
    dbFinishEntry(&ent);
}

const char* Record::info(const char *key, const char *defval)
{
    if(dbFindInfo(&ent, key)==0) {
        return dbGetInfoString(&ent);
    }
    return defval;
}

UDPSocket::UDPSocket()
    :sock(epicsSocketCreate(AF_INET, SOCK_DGRAM, 0))
{
    if(sock==INVALID_SOCKET)
        throw std::bad_alloc();
}

UDPSocket::~UDPSocket()
{
    epicsSocketDestroy(sock);
}

} // namespace tcast
