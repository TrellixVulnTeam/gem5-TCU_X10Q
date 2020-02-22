/*
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

#include "arch/riscv/faults.hh"
#include "arch/riscv/m3/system.hh"
#include "params/M3RiscvSystem.hh"

#include <libgen.h>

M3RiscvSystem::NoCMasterPort::NoCMasterPort(M3RiscvSystem &_sys)
  : QueuedMasterPort("noc_master_port", &_sys, reqQueue, snoopRespQueue),
    reqQueue(_sys, *this),
    snoopRespQueue(_sys, *this)
{}

M3RiscvSystem::M3RiscvSystem(Params *p)
    : RiscvSystem(p),
      PEMemory(this, p->memory_pe, p->memory_offset, p->memory_size,
                physProxy),
      nocPort(*this),
      loader(p->pes, p->mods, p->boot_osflags, p->core_id,
             p->mod_offset, p->mod_size, p->pe_size)
{
    _resetVect = getKernelEntry();
}

uint32_t M3RiscvSystem::pedesc(unsigned pe) const
{
    return loader.pe_attr()[pe];
}

Port&
M3RiscvSystem::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "noc_master_port")
        return nocPort;
    return System::getPort(if_name, idx);
}

void
M3RiscvSystem::initState()
{
    RiscvSystem::initState();

    loader.initState(*this, *this, nocPort);

    for (auto *tc: threadContexts) {
        RiscvISA::Reset().invoke(tc);
        tc->activate();
    }
}

M3RiscvSystem *
M3RiscvSystemParams::create()
{
    return new M3RiscvSystem(this);
}