/*
 * Copyright (c) 2015, Christian Menard
 * Copyright (c) 2015, Nils Asmussen
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

#ifndef __MEM_TCU_TCU_HH__
#define __MEM_TCU_TCU_HH__

#include "mem/tcu/connector/base.hh"
#include "mem/tcu/base.hh"
#include "mem/tcu/regfile.hh"
#include "mem/tcu/noc_addr.hh"
#include "mem/tcu/xfer_unit.hh"
#include "mem/tcu/core_reqs.hh"
#include "mem/tcu/error.hh"
#include "params/Tcu.hh"

class MessageUnit;
class MemoryUnit;
class XferUnit;

class Tcu : public BaseTcu
{
  public:

    static const uint16_t INVALID_VPE_ID    = 0xFFFF;
    static const size_t CREDITS_UNLIM       = 0x3F;
    static const uint16_t INVALID_EP_ID     = 0xFFFF;

    enum MemoryFlags : uint8_t
    {
        READ                = (1 << 0),
        WRITE               = (1 << 1),
    };

    enum MessageFlags : uint8_t
    {
        REPLY_FLAG          = (1 << 0),
    };

    enum class NocPacketType
    {
        MESSAGE,
        READ_REQ,
        WRITE_REQ,
        CACHE_MEM_REQ_FUNC,
        CACHE_MEM_REQ,
    };

    struct MemSenderState : public Packet::SenderState
    {
        Addr data;
        MasterID mid;
    };

    enum NocFlags
    {
        NONE    = 0,
        NOPF    = 1,
    };

    struct NocSenderState : public Packet::SenderState
    {
        TcuError result;
        NocPacketType packetType;
        uint64_t cmdId;
        uint flags;
    };

    struct InitSenderState : public Packet::SenderState
    {
    };

    struct Command
    {
        enum Opcode
        {
            IDLE            = 0,
            SEND            = 1,
            REPLY           = 2,
            READ            = 3,
            WRITE           = 4,
            FETCH_MSG       = 5,
            FETCH_EVENTS    = 6,
            ACK_MSG         = 7,
            SLEEP           = 8,
            PRINT           = 9,
        };

        enum Flags
        {
            NONE            = 0,
            NOPF            = 1,
        };

        BitUnion64(Bits)
            Bitfield<56, 25> arg;
            Bitfield<24, 21> error;
            Bitfield<20> flags;
            Bitfield<19, 4> epid;
            Bitfield<3, 0> opcode;
        EndBitUnion(Bits)

        Bits value;
    };

    struct PrivCommand
    {
        enum Opcode
        {
            IDLE            = 0,
            INV_PAGE        = 1,
            INV_TLB         = 2,
            XCHG_VPE        = 3,
        };

        Opcode opcode;
        uint64_t arg;
    };

    struct ExtCommand
    {
        enum Opcode
        {
            IDLE            = 0,
            INV_EP          = 1,
            INV_REPLY       = 2,
            RESET           = 3,
        };

        Opcode opcode;
        TcuError error;
        uint64_t arg;
    };

  public:

    Tcu(TcuParams* p);

    ~Tcu();

    void regStats() override;

    RegFile &regs() { return regFile; }

    TcuTlb *tlb() { return tlBuf; }

    bool isMemPE(unsigned pe) const;

    PacketPtr generateRequest(Addr addr, Addr size, MemCmd cmd);
    void freeRequest(PacketPtr pkt);

    void printLine(Addr len);

    bool startSleep(uint64_t cycles, int wakeupEp, bool ack);

    void stopSleep();

    void wakeupCore(bool force);

    Cycles reset(bool flushInval);

    Cycles flushInvalCaches(bool invalidate);

    void setIrq();

    void clearIrq();

    void forwardRequestToRegFile(PacketPtr pkt, bool isCpuRequest);

    void sendFunctionalMemRequest(PacketPtr pkt)
    {
        // set our master id (it might be from a different PE)
        pkt->req->setMasterId(masterId);

        dcacheMasterPort.sendFunctional(pkt);
    }

    void scheduleFinishOp(Cycles delay, TcuError error = TcuError::NONE);

    void sendMemRequest(PacketPtr pkt,
                        Addr virt,
                        Addr data,
                        Cycles delay);

    void sendNocRequest(NocPacketType type,
                        PacketPtr pkt,
                        uint flags,
                        Cycles delay,
                        bool functional = false);

    void sendNocResponse(PacketPtr pkt);

    void setCommandSent() { cmdSent = true; }

    static Addr physToNoc(Addr phys);
    static Addr nocToPhys(Addr noc);

    void startTransfer(void *event, Cycles delay);

    void startTranslate(size_t id,
                        unsigned vpeId,
                        Addr virt,
                        uint access,
                        XferUnit::Translation *trans);

    void startForeignReceive(size_t id,
                             unsigned epId,
                             unsigned vpeId,
                             XferUnit::TransferEvent *event);

    void abortTranslate(size_t id);

    void printPacket(PacketPtr pkt) const;

  private:

    Command::Bits getCommand()
    {
        return regFile.get(CmdReg::COMMAND);
    }

    void executeCommand(PacketPtr pkt);

    void abortCommand();

    PrivCommand getPrivCommand();

    void executePrivCommand(PacketPtr pkt);

    ExtCommand getExtCommand();

    void executeExtCommand(PacketPtr pkt);

    void finishCommand(TcuError error);

    bool has_message(int ep);

    void completeNocRequest(PacketPtr pkt) override;

    void completeMemRequest(PacketPtr pkt) override;

    void handleNocRequest(PacketPtr pkt) override;

    bool handleCoreMemRequest(PacketPtr pkt,
                              TcuSlavePort &sport,
                              TcuMasterPort &mport,
                              bool icache,
                              bool functional) override;

    bool handleCacheMemRequest(PacketPtr pkt, bool functional) override;

  private:

    const MasterID masterId;

    System *system;

    RegFile regFile;

    BaseConnector *connector;

    TcuTlb *tlBuf;

    MessageUnit *msgUnit;

    MemoryUnit *memUnit;

    XferUnit *xferUnit;

    CoreRequests coreReqs;

    EventWrapper<Tcu, &Tcu::abortCommand> abortCommandEvent;

    EventWrapper<CoreRequests, &CoreRequests::completeReqs> completeCoreReqEvent;

    struct TcuEvent : public Event
    {
        Tcu& tcu;

        TcuEvent(Tcu& _tcu)
            : tcu(_tcu)
        {}

        const std::string name() const override { return tcu.name(); }
    };

    struct ExecCmdEvent : public TcuEvent
    {
        PacketPtr pkt;

        ExecCmdEvent(Tcu& _tcu, PacketPtr _pkt)
            : TcuEvent(_tcu), pkt(_pkt)
        {}

        void process() override
        {
            tcu.executeCommand(pkt);
            setFlags(AutoDelete);
        }

        const char* description() const override { return "ExecCmdEvent"; }
    };

    struct ExecPrivCmdEvent : public TcuEvent
    {
        PacketPtr pkt;

        ExecPrivCmdEvent(Tcu& _tcu, PacketPtr _pkt)
            : TcuEvent(_tcu), pkt(_pkt)
        {}

        void process() override
        {
            tcu.executePrivCommand(pkt);
            setFlags(AutoDelete);
        }

        const char* description() const override { return "ExecPrivCmdEvent"; }
    };

    struct ExecExtCmdEvent : public TcuEvent
    {
        PacketPtr pkt;

        ExecExtCmdEvent(Tcu& _tcu, PacketPtr _pkt)
            : TcuEvent(_tcu), pkt(_pkt)
        {}

        void process() override
        {
            tcu.executeExtCommand(pkt);
            setFlags(AutoDelete);
        }

        const char* description() const override { return "ExecExtCmdEvent"; }
    };

    struct FinishCommandEvent : public TcuEvent
    {
        TcuError error;

        FinishCommandEvent(Tcu& _tcu, TcuError _error = TcuError::NONE)
            : TcuEvent(_tcu), error(_error)
        {}

        void process() override
        {
            tcu.finishCommand(error);
            setFlags(AutoDelete);
        }

        const char* description() const override { return "FinishCommandEvent"; }
    };

    PacketPtr cmdPkt;
    FinishCommandEvent *cmdFinish;
    uint64_t cmdId;
    uint abortCmd;
    size_t cmdXferBuf;
    bool cmdSent;
    int wakeupEp;

  public:

    unsigned memPe;
    Addr memOffset;
    Addr memSize;

    const bool atomicMode;

    const unsigned numEndpoints;

    const Addr maxNocPacketSize;

    const size_t blockSize;

    const size_t bufCount;
    const size_t bufSize;
    const size_t reqCount;

    const unsigned cacheBlocksPerCycle;

    const Cycles registerAccessLatency;

    const Cycles cpuToCacheLatency;

    const Cycles commandToNocRequestLatency;
    const Cycles startMsgTransferDelay;

    const Cycles transferToMemRequestLatency;
    const Cycles transferToNocLatency;
    const Cycles nocToTransferLatency;

    // NoC receives
    Stats::Scalar nocMsgRecvs;
    Stats::Scalar nocReadRecvs;
    Stats::Scalar nocWriteRecvs;

    // other
    Stats::Scalar regFileReqs;
    Stats::Scalar intMemReqs;
    Stats::Scalar extMemReqs;
    Stats::Scalar irqInjects;
    Stats::Scalar resets;

    // commands
    Stats::Vector commands;
    Stats::Vector privCommands;
    Stats::Vector extCommands;

    static uint64_t nextCmdId;

};

#endif // __MEM_TCU_TCU_HH__
