-- Wireshark Lua script plugin
-- packet disector for FNAL TCLK protocol
--
--   wireshark -X lua_script:tclk.lua
--
-- Revision $Id$

local tclk = Proto("tclk", "FNAL TCLK")

local ptcs = {
    [0] = "TEST",
    [1] = "TCLK",
    [2] = "CMTF",
    [3] = "NML",
}

local fver = ProtoField.uint8("tclk.ver", "Version")
local fid  = ProtoField.string("tclk.id", "ID")
local fseq = ProtoField.uint32("tclk.seq", "Seq #")
local fptc = ProtoField.uint16("tclk.ptc", "PTC", base.HEX, ptcs)
local ftime= ProtoField.absolute_time("tclk.time", "Flush Time")

local ftevt = ProtoField.uint8("tclk.e", "TCLK")
local ftcnt = ProtoField.uint16("tclk.e.cnt", "Count")
local fttime= ProtoField.uint24("tclk.e.stamp", "Time")
local ftseq = ProtoField.uint32("tclk.e.seq", "Seq")

local fpevt = ProtoField.uint8("tclk.p", "PTCLK")
local fpcnt = ProtoField.uint16("tclk.p.cnt", "Count")
local fptime= ProtoField.uint24("tclk.p.stamp", "Time")
local fpseq = ProtoField.uint32("tclk.p.seq", "Seq")

tclk.fields = {
    fver, fid, fseq, fptc, ftime,
    ftevt, ftcnt, fttime, ftseq,
    fpevt, fpcnt, fptime, fpseq,
}

local fcur = {
    evt = ftevt,
    cnt = ftcnt,
    time = fttime,
    seq = ftseq,
}
local fprev = {
    evt = fpevt,
    cnt = fpcnt,
    time = fptime,
    seq = fpseq,
}

function dissectarr(arr, pkt, root, n, fs)
    for i=0,n-1
    do
        local ent = arr(i*8, 8)
        local t = root:add(fs.evt, ent(3,1))

        t:add(fs.seq, ent(4,4))

        if ent(0,1):uint()==0xff
        then
            -- "Rapid" event
            t:add(fs.cnt, ent(1,2))
        else
            -- regular event
            t:add(fs.time, ent(0,3))
        end

        if i<4
        then
            pkt.cols.info:append(ent(3,1):uint()..", ")
        elseif i==4
        then
            pkt.cols.info:append(" ...")
        end
    end
end

function tclk.dissector (buf, pkt, root)

    pkt.cols.protocol = tclk.name
    pkt.cols.info:clear()
    pkt.cols.info:append(tostring(pkt.dst)..":"..pkt.dst_port)

    root = root:add(tclk, buf(0, buf:len()))

    if buf:len()<46
    then
        root:add_expert_info(PI_MALFORMED, PI_ERROR, "Truncated")
        return
    end

    root:add(fver, buf(0x07, 1))
    root:add(fid,  buf(0x08, 8))
    root:add(fptc, buf(0x10, 2))
    root:add(fseq, buf(0x18, 4))
    root:add(ftime, buf(0x26, 8), NSTime.new(buf(0x26,4):uint(), buf(0x2a,4):uint()))

    if buf(7, 9):string()~="\6ACCEVENT"
    then
        root:add_expert_info(PI_MALFORMED, PI_ERROR, "Bad header")
        return
    end

    local ptc = ptcs[buf(0x10, 2):uint()]
    if ptc
    then
        pkt.cols.info:append(" "..ptc)
    else
        pkt.cols.info:append(" ptc="..buf(0x10, 2):uint())
        t:add_expert_info(PI_DEBUG, PI_NOTE, "Unknown PTC")
    end
    pkt.cols.info:append(" seq="..buf(0x18, 4):uint())

    local n
    local offset = 0x2e

    -- TCLK array
    n = buf(0x20, 1):uint()
    pkt.cols.info:append(" #tclk="..n.." [")
    dissectarr(buf(offset, n*8), pkt, root, n, fcur)
    offset = offset + n*8

    -- skip MIBS, RRBS, TVBS
    n = buf(0x21, 1):uint() + buf(0x22, 1):uint() + buf(0x23, 1):uint() 
    offset = offset + n*8

    -- PTCLK array
    n = buf(0x24, 1):uint()
    pkt.cols.info:append("] #ptclk="..n.." [")
    dissectarr(buf(offset, n*8), pkt, root, n, fprev)
    pkt.cols.info:append("]")
end

local utbl = DissectorTable.get("udp.port")
utbl:add(50090, tclk)
