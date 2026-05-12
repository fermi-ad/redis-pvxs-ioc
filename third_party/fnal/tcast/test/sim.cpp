
#include <osiSock.h>

#include <string.h>

#include <epicsAssert.h>

#include "sim.h"

namespace tcast {

void SimTable::add(uint8_t evt, uint32_t offset, uint32_t seq)
{
    uint64_t ent = offset;
    ent<<=8u;
    ent |= evt;
    ent<<=32u;
    ent |= seq;
    tbl.push_back(ent);
}

void SimTable::add_rapid(uint8_t evt, uint16_t n, uint32_t seq)
{
    uint64_t ent = 0xff0000 | n;;
    ent<<=8u;
    ent |= evt;
    ent<<=32u;
    ent |= seq;
    tbl.push_back(ent);
}

SimMsg::SimMsg()
    :PTC(ptcTEST)
    ,seq(0xdeadbeefu)
{
    // Tue Dec 24 21:01:21 2041 PST
    stamp.secPastEpoch = 0x87654321 - POSIX_TIME_AT_EPICS_EPOCH;
    stamp.nsec = 0u;
}

static
void encode_table(const SimTable& tbl, uint8_t*& dest)
{
    for(size_t i=0; i<tbl.tbl.size(); i++) {
        uint64_t ent = tbl.tbl[i];
        *dest++ = ent>>56u;
        *dest++ = ent>>48u;
        *dest++ = ent>>40u;
        *dest++ = ent>>32u;
        *dest++ = ent>>24u;
        *dest++ = ent>>16u;
        *dest++ = ent>>8u;
        *dest++ = ent>>0u;
    }
}

void SimMsg::encode(std::vector<uint8_t> &buffer) const
{
    buffer.clear();
    buffer.resize(0x2e + 8*(TCLK.tbl.size() + PTCLK.tbl.size()), 0u);

    buffer[0x00] = 1;
    buffer[0x07] = 6; // proto version
    strcpy((char*)&buffer[0x08], "ACCEVENT");
    buffer[0x11] = int(PTC);
    buffer[0x14] = 1;
    buffer[0x18] = seq>>24u;
    buffer[0x19] = seq>>16u;
    buffer[0x1a] = seq>>8u;
    buffer[0x1b] = seq>>0u;
    buffer[0x20] = TCLK.tbl.size();
    buffer[0x24] = PTCLK.tbl.size();
    uint32_t sec = stamp.secPastEpoch + POSIX_TIME_AT_EPICS_EPOCH;
    buffer[0x26] = sec>>24u;
    buffer[0x27] = sec>>16u;
    buffer[0x28] = sec>>8u;
    buffer[0x29] = sec>>0u;
    buffer[0x2a] = stamp.nsec>>24u;
    buffer[0x2b] = stamp.nsec>>16u;
    buffer[0x2c] = stamp.nsec>>8u;
    buffer[0x2d] = stamp.nsec>>0u;

    uint8_t *ptr = &buffer[0x2e];
    encode_table(TCLK, ptr);
    encode_table(PTCLK, ptr);
    assert(ptr==buffer.size() + &buffer[0]);
}

void SimMsg::sendto(SOCKET sock, const osiSockAddr& dest) const
{
    std::vector<uint8_t> buf;
    encode(buf);

    int ret = ::sendto(sock, &buf[0], buf.size(), 0, &dest.sa, sizeof(dest.ia));
    assert(ret >=0 && size_t(ret)==buf.size());
}

} // namespace tcast
