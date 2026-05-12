#ifndef SIM_H
#define SIM_H

#include <osiSock.h>

#include <stdint.h>

#include <vector>
#include <utility>

#include <epicsTime.h>

namespace tcast {

struct SimTable
{
    std::vector<uint64_t> tbl;

    void add(uint8_t evt, uint32_t offset, uint32_t seq);
    void add_rapid(uint8_t evt, uint16_t n, uint32_t seq);
};

struct SimMsg
{
    enum {
        ptcTEST=0,
        ptcTCLK,
        ptcCMTF,
        ptcNML,
    } PTC;
    uint32_t seq;
    epicsTimeStamp stamp;
    SimTable TCLK;
    SimTable PTCLK;

    SimMsg();

    void encode(std::vector<uint8_t>& buffer) const;

    void sendto(SOCKET sock, const osiSockAddr& dest) const;
};

} // namespace tcast

#endif // SIM_H
