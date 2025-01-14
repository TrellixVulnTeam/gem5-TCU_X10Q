/*
 * Copyright (C) 2018 Nils Asmussen <nils@os.inf.tu-dresden.de>
 * Copyright (C) 2019-2021 Nils Asmussen, Barkhausen Institut
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

#include "cpu/tcu-accel/ctxswsm.hh"
#include "debug/TcuAccel.hh"

AccelCtxSwSM::AccelCtxSwSM(TcuAccel *_accel)
    : accel(_accel),
      state(FETCH_MSG), stateChanged(), switched(), vpe_id(OUR_VPE)
{
}

std::string
AccelCtxSwSM::stateName() const
{
    const char *names[] =
    {
        "FETCH_MSG",
        "FETCH_MSG_WAIT",
        "READ_MSG_ADDR",
        "READ_MSG",
        "STORE_REPLY",
        "SEND_REPLY",
        "REPLY_WAIT",
    };
    return names[static_cast<size_t>(state)];
}

PacketPtr
AccelCtxSwSM::tick()
{
    PacketPtr pkt = nullptr;

    switch(state)
    {
        case State::FETCH_MSG:
        {
            pkt = accel->tcuif().createTcuCmdPkt(
                CmdCommand::create(CmdCommand::FETCH_MSG, EP_RECV),
                0
            );
            break;
        }
        case State::READ_MSG_ADDR:
        {
            Addr regAddr = TcuIf::getRegAddr(UnprivReg::ARG1);
            pkt = accel->tcuif().createTcuRegPkt(regAddr, 0, MemCmd::ReadReq);
            break;
        }
        case State::READ_MSG:
        {
            pkt = accel->tcuif().createPacket(msgAddr, MSG_SIZE,
                                              MemCmd::ReadReq);
            break;
        }

        case State::STORE_REPLY:
        {
            reply.res = 0;
            reply.val1 = 0;
            reply.val2 = 0;
            pkt = accel->tcuif().createPacket(msgAddr, sizeof(reply),
                                              MemCmd::WriteReq);
            memcpy(pkt->getPtr<uint8_t>(), (char*)&reply, sizeof(reply));
            break;
        }
        case State::SEND_REPLY:
        {
            pkt = accel->tcuif().createTcuCmdPkt(
                CmdCommand::create(CmdCommand::REPLY, EP_RECV,
                                   msgAddr - (RBUF_ADDR + accel->offset)),
                CmdData::create(msgAddr, sizeof(reply))
            );
            break;
        }

        case State::FETCH_MSG_WAIT:
        case State::REPLY_WAIT:
        {
            Addr regAddr = TcuIf::getRegAddr(UnprivReg::COMMAND);
            pkt = accel->tcuif().createTcuRegPkt(regAddr, 0, MemCmd::ReadReq);
            break;
        }
    }

    return pkt;
}

bool
AccelCtxSwSM::handleMemResp(PacketPtr pkt)
{
    auto lastState = state;

    auto pkt_data = pkt->getConstPtr<uint8_t>();

    switch(state)
    {
        case State::FETCH_MSG:
        {
            state = State::FETCH_MSG_WAIT;
            break;
        }
        case State::FETCH_MSG_WAIT:
        {
            CmdCommand::Bits cmd =
                *reinterpret_cast<const RegFile::reg_t*>(pkt_data);
            if (cmd.opcode == 0)
                state = State::READ_MSG_ADDR;
            break;
        }
        case State::READ_MSG_ADDR:
        {
            const RegFile::reg_t *regs = pkt->getConstPtr<RegFile::reg_t>();
            if(regs[0] != static_cast<RegFile::reg_t>(-1))
            {
                msgAddr = regs[0] + RBUF_ADDR + accel->offset;
                state = State::READ_MSG;
            }
            else
            {
                state = State::FETCH_MSG;
                return true;
            }
            break;
        }
        case State::READ_MSG:
        {
            const uint64_t *args =
                reinterpret_cast<const uint64_t*>(pkt_data + sizeof(MessageHeader));

            DPRINTF(TcuAccel,
                    "Received side call with op=%u, act=%u, ctrl=%u\n",
                    args[0], args[1], args[2]);

            if(args[0] == Operation::VPE_CTRL)
            {
                vpe_id = args[1];
                if (args[2] == VPECtrl::START)
                    switched = true;
                else if (args[2] == VPECtrl::STOP)
                    vpe_id = OUR_VPE;
            }
            state = State::STORE_REPLY;
            break;
        }

        case State::STORE_REPLY:
        {
            state = State::SEND_REPLY;
            break;
        }
        case State::SEND_REPLY:
        {
            state = State::REPLY_WAIT;
            break;
        }
        case State::REPLY_WAIT:
        {
            CmdCommand::Bits cmd =
                *reinterpret_cast<const RegFile::reg_t*>(pkt_data);
            if (cmd.opcode == 0)
            {
                bool res = switched;
                if (switched)
                {
                    accel->setSwitched();
                    switched = false;
                }
                state = State::FETCH_MSG;
                return res;
            }
            break;
        }
    }

    stateChanged = state != lastState;

    return false;
}
