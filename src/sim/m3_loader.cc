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

#include "sim/m3_loader.hh"
#include "arch/vtophys.hh"
#include "base/trace.hh"
#include "base/loader/object_file.hh"
#include "cpu/thread_context.hh"
#include "debug/TcuTlb.hh"
#include "mem/port_proxy.hh"
#include "mem/tcu/tlb.hh"
#include "mem/tcu/tcu.hh"
#include "sim/byteswap.hh"

#include <libgen.h>
#include <sstream>

M3Loader::M3Loader(const std::vector<Addr> &pes,
                   const std::vector<std::string> &mods,
                   const std::string &cmdline,
                   unsigned coreId,
                   Addr modOffset,
                   Addr modSize,
                   Addr peSize)
    : pes(pes),
      mods(mods),
      commandLine(cmdline),
      coreId(coreId),
      modOffset(modOffset),
      modSize(modSize),
      peSize(peSize)
{
}

size_t
M3Loader::getArgc() const
{
    const char *cmd = commandLine.c_str();
    size_t argc = 0;
    size_t len = 0;
    while (*cmd)
    {
        if (isspace(*cmd))
        {
            if(len > 0)
                argc++;
            len = 0;
        }
        else
            len++;
        cmd++;
    }
    if(len > 0)
        argc++;

    return argc;
}

void
M3Loader::writeArg(System &sys, Addr &args, size_t &i, Addr argv,
                   const char *cmd, const char *begin)
{
    const char zero[] = {0};
    // write argument pointer
    uint64_t argvPtr = args;
    sys.physProxy.writeBlob(argv + i * sizeof(uint64_t),
                            (uint8_t*)&argvPtr, sizeof(argvPtr));
    // write argument
    sys.physProxy.writeBlob(args, (uint8_t*)begin, cmd - begin);
    args += cmd - begin;
    sys.physProxy.writeBlob(args, (uint8_t*)zero, 1);
    args++;
    i++;
}

void
M3Loader::writeRemote(MasterPort &noc, Addr dest,
                      const uint8_t *data, size_t size)
{
    auto req = std::make_shared<Request>(dest, size, 0, Request::funcMasterId);
    Packet pkt(req, MemCmd::WriteReq);
    pkt.dataStaticConst(data);

    auto senderState = new Tcu::NocSenderState();
    senderState->packetType = Tcu::NocPacketType::CACHE_MEM_REQ_FUNC;
    senderState->result = TcuError::NONE;

    pkt.pushSenderState(senderState);

    noc.sendFunctional(&pkt);

    delete senderState;
}

Addr
M3Loader::loadModule(MasterPort &noc, const std::string &filename, Addr addr)
{
    FILE *f = fopen(filename.c_str(), "r");
    if(!f)
        panic("Unable to open '%s' for reading", filename.c_str());

    fseek(f, 0L, SEEK_END);
    size_t sz = ftell(f);
    fseek(f, 0L, SEEK_SET);

    auto data = new uint8_t[sz];
    if(fread(data, 1, sz, f) != sz)
        panic("Unable to read '%s'", filename.c_str());
    writeRemote(noc, addr, data, sz);
    delete[] data;
    fclose(f);

    return sz;
}

void
M3Loader::initState(System &sys, PEMemory &mem, MasterPort &noc)
{
    BootEnv env;
    memset(&env, 0, sizeof(env));
    env.pe_id = coreId;
    env.pe_desc = pes[coreId];
    env.argc = getArgc();
    Addr argv = ENV_START + sizeof(env);
    // the kernel gets the kernel env behind the normal env
    if (modOffset)
        argv += sizeof(KernelEnv);
    Addr args = argv + sizeof(uint64_t) * env.argc;
    env.argv = argv;

    // pass the PE memory address and size to PEMux/kernel
    env.pe_mem_base = 0;
    env.pe_mem_size = mem.memSize;

    // with paging, the kernel gets an initial heap mapped
    if ((pes[coreId] & 0x7) == 1 || (pes[coreId] & 0x7) == 2)
        env.heap_size = HEAP_SIZE;
    // otherwise, he should use all internal memory
    else
        env.heap_size = 0;

    // check if there is enough space
    if (commandLine.length() + 1 > ENV_START + ENV_SIZE - args)
    {
        panic("Command line \"%s\" is longer than %d characters.\n",
                commandLine, ENV_START + ENV_SIZE - args - 1);
    }

    // write arguments to state area
    const char *cmd = commandLine.c_str();
    const char *begin = cmd;
    size_t i = 0;
    while (*cmd)
    {
        if (isspace(*cmd))
        {
            if (cmd > begin)
                writeArg(sys, args, i, argv, cmd, begin);
            begin = cmd + 1;
        }
        cmd++;
    }

    if (cmd > begin)
        writeArg(sys, args, i, argv, cmd, begin);

    // modules for the kernel
    if (modOffset)
    {
        uint8_t *modarray = nullptr;
        size_t modarraysize = 0;

        i = 0;
        Addr addr = NocAddr(mem.memPe, modOffset).getAddr();
        for (const std::string &mod : mods)
        {
            Addr size = loadModule(noc, mod, addr);

            // determine module name
            char *tmp = new char[mod.length() + 1];
            strcpy(tmp, mod.c_str());
            std::string mod_name(basename(tmp));
            delete[] tmp;

            // extend module array
            size_t cmdlen = mod_name.length() + 1;
            modarraysize += cmdlen + sizeof(BootModule);
            modarray = reinterpret_cast<uint8_t*>(
                realloc(modarray, modarraysize));

            // construct module info
            BootModule *bmod = reinterpret_cast<BootModule*>(
                modarray + modarraysize - (cmdlen + sizeof(BootModule)));
            bmod->namelen = cmdlen;
            bmod->addr = addr;
            bmod->size = size;
            strcpy(bmod->name, mod_name.c_str());

            inform("Loaded '%s' to %p .. %p",
                bmod->name, bmod->addr, bmod->addr + bmod->size);

            // to next
            addr += size + TcuTlb::PAGE_SIZE - 1;
            addr &= ~static_cast<Addr>(TcuTlb::PAGE_SIZE - 1);
            i++;
        }

        // write kenv
        env.kenv = addr;
        KernelEnv kenv;
        kenv.mod_count = mods.size();
        kenv.mod_size = modarraysize;
        kenv.pe_count  = pes.size();
        auto avail_mem_start = modOffset + modSize + pes.size() * peSize;
        kenv.mems[0].size = pes[mem.memPe] & ~static_cast<Addr>(0xFFF);
        if (kenv.mems[0].size < avail_mem_start)
            panic("Not enough DRAM for modules and PEs");
        kenv.mems[0].addr = avail_mem_start;
        kenv.mems[0].size -= avail_mem_start;

        size_t j = 1;
        for (size_t i = 0; i < pes.size(); ++i) {
            if (i != mem.memPe && (pes[i] & 0x7) == 2) {
                if (j >= MAX_MEMS)
                    panic("Too many memory PEs");
                kenv.mems[j].addr = 0;
                kenv.mems[j].size = pes[i] & ~static_cast<Addr>(0xFFF);
                j++;
            }
        }
        for (; j < MAX_MEMS; ++j)
            kenv.mems[j] = {0, 0};
        writeRemote(noc, addr, reinterpret_cast<uint8_t*>(&kenv),
                    sizeof(kenv));
        addr += sizeof(kenv);

        // write modules to memory
        writeRemote(noc, addr, reinterpret_cast<uint8_t*>(modarray),
                    modarraysize);
        free(modarray);
        addr += modarraysize;

        // write PEs to memory
        uint32_t *kpes = new uint32_t[pes.size()]();
        for (size_t i = 0; i < pes.size(); ++i)
            kpes[i] = pes[i];
        writeRemote(noc, addr, reinterpret_cast<uint8_t*>(kpes),
                    pes.size() * sizeof(uint32_t));
        delete[] kpes;
        addr += pes.size() * sizeof(uint32_t);

        // check size
        Addr end = NocAddr(mem.memPe, modOffset + modSize).getAddr();
        if (addr > end)
        {
            panic("Modules are too large (have: %lu, need: %lu)",
                modSize, addr - NocAddr(mem.memPe, modOffset).getAddr());
        }
    }

    // write env
    sys.physProxy.writeBlob(
        ENV_START, reinterpret_cast<uint8_t*>(&env), sizeof(env));
}
