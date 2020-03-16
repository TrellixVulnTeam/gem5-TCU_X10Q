/*
 * Copyright (c) 2017, Georg Kotheimer
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation are
 * those of the authors and should not be interpreted as representing official
 * policies, either expressed or implied, of the FreeBSD Project.
 */

#include "dev/tcu/pci_proxy.hh"

#include "base/trace.hh"
#include "debug/TcuPciProxy.hh"
#include "debug/TcuPciProxyCmd.hh"
#include "debug/TcuPciProxyDevMem.hh"
#include "debug/TcuPciProxyDma.hh"
#include "debug/TcuPciProxyInt.hh"
#include "dev/pci/pcireg.h"
#include "sim/system.hh"

const unsigned TcuPciProxy::EP_INT = 16;
const unsigned TcuPciProxy::EP_DMA = 17;
const Addr TcuPciProxy::REG_ADDR = 0x4000;
const Addr TcuPciProxy::INT_ADDR = 0x10000000;
const Addr TcuPciProxy::DMA_ADDR = 0x20000000;

PacketPtr
TcuPciProxy::createPacket(
    Addr paddr, size_t size, MemCmd cmd = MemCmd::WriteReq)
{
    return createPacket(paddr, new uint8_t[size], size, cmd);
}

PacketPtr
TcuPciProxy::createPacket(
    Addr paddr, const void* data, size_t size, MemCmd cmd = MemCmd::WriteReq)
{
    Request::Flags flags;

    auto req = std::make_shared<Request>(paddr, size, flags, masterId);
    req->setContext(id);

    auto pkt = new Packet(req, cmd);
    pkt->dataDynamic(data);

    return pkt;
}

void
TcuPciProxy::freePacket(PacketPtr pkt)
{
    // the packet will delete the data
    delete pkt;
}

Addr
TcuPciProxy::getRegAddr(TcuReg reg)
{
    return static_cast<Addr>(reg) * sizeof(RegFile::reg_t);
}

PacketPtr
TcuPciProxy::createTcuRegPkt(
    Addr reg, RegFile::reg_t value, MemCmd cmd = MemCmd::WriteReq)
{
    auto pkt = createPacket(tcuRegBase + reg, sizeof(RegFile::reg_t), cmd);
    *pkt->getPtr<RegFile::reg_t>() = value;
    return pkt;
}

Addr
TcuPciProxy::getRegAddr(CmdReg reg)
{
    Addr result = sizeof(RegFile::reg_t) * numTcuRegs;

    result += static_cast<Addr>(reg) * sizeof(RegFile::reg_t);

    return result;
}

PacketPtr
TcuPciProxy::createTcuCmdPkt(Tcu::Command::Opcode cmd, unsigned epid,
    uint64_t data, uint64_t size, uint64_t arg0, uint64_t arg1)
{
    static_assert(static_cast<int>(CmdReg::COMMAND) == 0, "");
    static_assert(static_cast<int>(CmdReg::ABORT) == 1, "");
    static_assert(static_cast<int>(CmdReg::DATA) == 2, "");
    static_assert(static_cast<int>(CmdReg::ARG1) == 3, "");

    auto pkt = createPacket(tcuRegBase + getRegAddr(CmdReg::COMMAND),
        sizeof(RegFile::reg_t) * 4, MemCmd::WriteReq);

    Tcu::Command::Bits cmdreg = 0;
    cmdreg.opcode = static_cast<RegFile::reg_t>(cmd);
    cmdreg.epid = epid;
    cmdreg.arg = arg0;

    RegFile::reg_t* regs = pkt->getPtr<RegFile::reg_t>();
    regs[0] = cmdreg;
    regs[1] = 0;
    regs[2] = DataReg(data, size).value();
    regs[3] = arg1;
    return pkt;
}

Addr
TcuPciProxy::encodePciAddress(PciBusAddr const& busAddr, Addr offset)
{
    Addr addr = insertBits(0, 15, 8, busAddr.bus);
    addr = insertBits(addr, 7, 3, busAddr.dev);
    addr = insertBits(addr, 2, 0, busAddr.func);
    return (addr << 8) | (offset & mask(8));
}

PacketPtr
TcuPciProxy::createPciConfigPacket(
    PciBusAddr busAddr, Addr offset, const void* data, size_t size, MemCmd cmd)
{
    Request::Flags flags;

    Addr addr = encodePciAddress(busAddr, offset);
    auto req = std::make_shared<Request>(addr, size, flags, masterId);

    auto pkt = new Packet(req, cmd);
    pkt->dataStatic(data);

    return pkt;
}

TcuPciProxy::TcuPciProxy(const TcuPciProxyParams* p)
    : ClockedObject(p),
      tcuMasterPort(name() + ".tcu_master_port", this),
      tcuSlavePort(name() + ".tcu_slave_port", this),
      pioPort(name() + ".pio_port", this),
      dmaPort(name() + ".dma_port", this),
      masterId(p->system->getMasterId(this, name())),
      id(p->id),
      tcuRegBase(p->tcu_regfile_base_addr),
      deviceBusAddr(0, 0, 0),
      tickEvent(this),
      cmdSM(this),
      cmdRunning(false),
      interruptPending(false),
      pendingDmaReq(nullptr),
      dmaRetry(false)
{
}

Port&
TcuPciProxy::getPort(const std::string& if_name, PortID idx)
{
    if (if_name == "tcu_master_port")
        return tcuMasterPort;
    else if (if_name == "pio_port")
        return pioPort;
    else if (if_name == "tcu_slave_port")
        return tcuSlavePort;
    else if (if_name == "dma_port")
        return dmaPort;
    else
        return SimObject::getPort(if_name, idx);
}

void
TcuPciProxy::init()
{
    fatal_if(!findDevice(), "Failed to find a device to proxy.");

    tcuSlavePort.sendRangeChange();
    dmaPort.sendRangeChange();
}

bool
TcuPciProxy::findDevice()
{
    DPRINTF(TcuPciProxy, "Enumerating devices...\n");

    for (size_t bus = 0; bus < 256; bus++) {
        for (size_t dev = 0; dev < 32; dev++) {
            uint16_t vendor = 0xFFFF;
            pciHost->read(createPciConfigPacket(PciBusAddr(bus, dev, 0),
                PCI_VENDOR_ID, &vendor, 2, MemCmd::ReadReq));

            if (vendor != 0xFFFF) {
                DPRINTF(TcuPciProxy, "Found device with vendor id: %04x\n",
                    vendor);
                deviceBusAddr = PciBusAddr(bus, dev, 0);
                return true;
            }
        }
    }

    return false;
}

void
TcuPciProxy::executeCommand(PacketPtr cmdPkt)
{
    assert(!cmdRunning);

    DPRINTF(TcuPciProxyCmd, "Execute TCU command.\n");
    cmdRunning = true;
    cmdSM.executeCommand(cmdPkt);
}

void
TcuPciProxy::commandExecutionFinished()
{
    cmdRunning = false;
    DPRINTF(TcuPciProxyCmd, "Finished TCU command execution.\n");

    if (pendingDmaReq && pendingDmaReq->isResponse()) {
        DPRINTF(TcuPciProxyDma,
            "Send response for DMA write request to device.\n");
        dmaPort.schedTimingResp(pendingDmaReq, clockEdge(Cycles(1)));
        pendingDmaReq = nullptr;
    }

    if (interruptPending) {
        sendInterruptCmd();
    } else if (pendingDmaReq) {
        sendDmaCmd();
    } else if (dmaRetry) {
        DPRINTF(TcuPciProxyDma, "Send DMA retry to device.\n");
        dmaRetry = false;
        dmaPort.sendRetryReq();
    }
}

void
TcuPciProxy::signalInterrupt()
{
    DPRINTF(TcuPciProxyInt,
        "Device signaled interrupt (pending: %s, cmdRunning: %s)\n",
        interruptPending ? "true" : "false", cmdRunning ? "true" : "false");

    interruptPending = true;
    if (!cmdRunning)
        sendInterruptCmd();
}

void
TcuPciProxy::sendInterruptCmd()
{
    DPRINTF(
        TcuPciProxyInt, "Send interrupt message using endpoint %u\n", EP_INT);

    PacketPtr cmdPkt = createTcuCmdPkt(
        Tcu::Command::SEND, EP_INT, INT_ADDR, 0x4, Tcu::INVALID_EP_ID, 0
    );
    interruptPending = false;
    executeCommand(cmdPkt);
}

void
TcuPciProxy::handleInterruptMessageContent(PacketPtr pkt)
{
    assert(pkt->needsResponse());
    assert(pkt->isRead());

    pkt->makeResponse();
    memset(pkt->getPtr<uint8_t>(), 0, pkt->getSize());
    tcuSlavePort.schedTimingResp(pkt, clockEdge(Cycles(1)));
}

void
TcuPciProxy::forwardAccessToDeviceMem(PacketPtr pkt)
{
    assert(pkt->getAddr() >= REG_ADDR);
    Addr offset = pkt->getAddr() - REG_ADDR;

    DPRINTF(TcuPciProxyDevMem,
        "Forward %s access at %llx (%llu) to device memory at %llx\n",
        pkt->isWrite() ? "write" : "read", pkt->getAddr(), pkt->getSize(),
        offset);
    pkt->setAddr(pciHost->memAddr(deviceBusAddr, offset));

    pioPort.schedTimingReq(pkt, clockEdge(Cycles(1)));
}

void
TcuPciProxy::completeAccessToDeviceMem(PacketPtr pkt)
{
    DPRINTF(TcuPciProxyDevMem,
        "Send response for device memory %s access at %llx.\n",
        pkt->isWrite() ? "write" : "read", pkt->getAddr());

    // TCU always accepts responses
    tcuSlavePort.schedTimingResp(pkt, clockEdge(Cycles(1)));
}

bool
TcuPciProxy::handleDmaRequest(PacketPtr pkt)
{
    assert(!dmaRetry);

    DPRINTF(TcuPciProxyDma,
        "Received DMA request from device (pending: %s, cmdRunning: %s)\n",
        pendingDmaReq ? "true" : "false", cmdRunning ? "true" : "false");

    if (pendingDmaReq || cmdRunning) {
        dmaRetry = true;
        DPRINTF(TcuPciProxyDma, "Defer DMA request.\n");
        return false;
    }

    pendingDmaReq = pkt;
    sendDmaCmd();

    return true;
}

void
TcuPciProxy::sendDmaCmd()
{
    assert(pendingDmaReq);

    DPRINTF(TcuPciProxyDma,
        "Execute DMA request using endpoint %u: %s @ %llx with %llu bytes\n",
        EP_DMA, pendingDmaReq->cmdString(), pendingDmaReq->getAddr(),
        pendingDmaReq->getSize());

    // TODO: Validate offset lies within the memory endpoint's boundaries
    // Translate to TCU read/write command
    auto cmd
        = pendingDmaReq->isRead() ? Tcu::Command::READ : Tcu::Command::WRITE;
    PacketPtr cmdPkt = createTcuCmdPkt(cmd, EP_DMA, DMA_ADDR,
        pendingDmaReq->getSize(), 0, pendingDmaReq->getAddr());
    executeCommand(cmdPkt);
}

void
TcuPciProxy::handleDmaContent(PacketPtr pkt)
{
    assert(pendingDmaReq);

    // Provide data for dma write
    if (pkt->isRead()) {
        DPRINTF(TcuPciProxyDma, "Send data for DMA write request to TCU.\n");

        pkt->makeResponse();
        pkt->setData(pendingDmaReq->getPtr<uint8_t>());
        DDUMP(TcuPciProxyDma, pkt->getPtr<uint8_t>(), pkt->getSize());

        tcuSlavePort.schedTimingResp(pkt, clockEdge(Cycles(1)));

        pendingDmaReq->makeResponse();
    } else {
        DPRINTF(
            TcuPciProxyDma, "Receive data for DMA read request from TCU.\n");

        pendingDmaReq->makeResponse();
        pendingDmaReq->setData(pkt->getPtr<uint8_t>());
        DDUMP(TcuPciProxyDma, pendingDmaReq->getPtr<uint8_t>(),
            pendingDmaReq->getSize());

        DPRINTF(
            TcuPciProxyDma, "Send response for DMA read request to device.\n");

        dmaPort.schedTimingResp(pendingDmaReq, clockEdge(Cycles(1)));
        pendingDmaReq = nullptr;

        if (pkt->needsResponse()) {
            pkt->makeResponse();
            tcuSlavePort.schedTimingResp(pkt, clockEdge(Cycles(1)));
        }
    }
}

void
TcuPciProxy::tick()
{
    cmdSM.tick();
}

std::string
TcuPciProxy::CommandSM::stateName() const
{
    const char* names[] = { "IDLE", "SEND", "WAIT" };
    return names[static_cast<size_t>(state)];
}

void
TcuPciProxy::CommandSM::executeCommand(PacketPtr cmdPkt)
{
    assert(isIdle());
    assert(!cmd);
    state = CMD_SEND;
    cmd = cmdPkt;
    tick();
}

void
TcuPciProxy::CommandSM::tick()
{
    PacketPtr pkt = nullptr;

    switch (state) {
        case State::CMD_IDLE: {
            pciProxy->commandExecutionFinished();
            break;
        }
        case State::CMD_SEND: {
            pkt = cmd;
            break;
        }
        case State::CMD_WAIT: {
            Addr regAddr = getRegAddr(CmdReg::COMMAND);
            pkt = pciProxy->createTcuRegPkt(regAddr, 0, MemCmd::ReadReq);
            break;
        }
    }

    if (pkt != nullptr) {
        pciProxy->tcuMasterPort.schedTimingReq(
            pkt, pciProxy->clockEdge(Cycles(1)));
    }
}

void
TcuPciProxy::CommandSM::handleMemResp(PacketPtr pkt)
{
    RequestPtr req = pkt->req;

    Cycles delay(1);
    if (pkt->isError()) {
        warn("%s access failed at %#x\n", pkt->isWrite() ? "Write" : "Read",
            req->getPaddr());
    } else {
        switch (state) {
            case State::CMD_IDLE: {
                assert(false);
                break;
            }
            case State::CMD_SEND: {
                cmd = nullptr;
                state = State::CMD_WAIT;
                break;
            }
            case State::CMD_WAIT: {
                RegFile::reg_t reg = *pkt->getConstPtr<RegFile::reg_t>();
                if ((reg & 0xF) == 0)
                    state = State::CMD_IDLE;
                break;
            }
        }
    }

    freePacket(pkt);

    // kick things into action again
    pciProxy->schedule(pciProxy->tickEvent, pciProxy->clockEdge(delay));
}

bool
TcuPciProxy::TcuMasterPort::recvTimingResp(PacketPtr pkt)
{
    assert(pkt->isResponse());

    pciProxy.cmdSM.handleMemResp(pkt);
    return true;
}

bool
TcuPciProxy::TcuSlavePort::recvTimingReq(PacketPtr pkt)
{
    if (pkt->getAddr() >= DMA_ADDR) {
        pciProxy.handleDmaContent(pkt);
    } else if (pkt->getAddr() >= INT_ADDR) {
        pciProxy.handleInterruptMessageContent(pkt);
    } else if (pkt->getAddr() >= REG_ADDR) {
        pciProxy.forwardAccessToDeviceMem(pkt);
    } else {
        warn("Received unexpected request at %llx\n", pkt->getAddr());
    }

    return true;
}

void
TcuPciProxy::TcuSlavePort::recvFunctional(PacketPtr pkt)
{
    panic("not implemented");
}

Tick
TcuPciProxy::TcuSlavePort::recvAtomic(PacketPtr pkt)
{
    panic("not implemented");

    return 0;
}

AddrRangeList
TcuPciProxy::TcuSlavePort::getAddrRanges() const
{
    AddrRangeList ranges;
    // MEMCAP_END = 0x3fc00000
    ranges.push_back(AddrRange(0, 0x3fc00000));
    return ranges;
}

bool
TcuPciProxy::PioPort::recvTimingResp(PacketPtr pkt)
{
    pciProxy.completeAccessToDeviceMem(pkt);
    return true;
}

bool
TcuPciProxy::DmaPort::recvTimingReq(PacketPtr pkt)
{
    return pciProxy.handleDmaRequest(pkt);
}

void
TcuPciProxy::DmaPort::recvFunctional(PacketPtr pkt)
{
    panic("not implemented");
}

Tick
TcuPciProxy::DmaPort::recvAtomic(PacketPtr pkt)
{
    panic("not implemented");

    return 0;
}

AddrRangeList
TcuPciProxy::DmaPort::getAddrRanges() const
{
    AddrRangeList ranges;
    ranges.push_back(AddrRange(0, 0xffffffffffffffff));
    return ranges;
}

TcuPciProxy*
TcuPciProxyParams::create()
{
    return new TcuPciProxy(this);
}