#pragma once

// Runs an IOP module (IRX) as it is, instead of a host replacement: its R3000 code is interpreted in the shared
// IOP RAM, its imports (IOP kernel threads, semaphores, timers, SIF RPC, sysmem, sysclib, stdio, libsd) are served
// here, and EE calls to the RPC servers it registers run its own handlers. libsd drives Iop/Spu2.cpp.
//
// Used for Crash Tag Team Racing's AUDIO.IRX (Radical's sound engine: RPC servers 0x11 and 0x12).

#include <cstdint>
#include <string>
#include <vector>

class PS2Runtime;

namespace ps2_iopvm
{
    // Loads and starts the module. `image` is the IRX file. Returns false when it cannot be loaded.
    bool loadModule(PS2Runtime *runtime, const std::string &name, const std::vector<uint8_t> &image);

    // An EE SifCallRpc to `sid`: runs the module's server function and copies its reply. Returns false when no
    // module registered `sid`.
    bool callRpc(PS2Runtime *runtime, uint8_t *rdram, uint32_t sid, uint32_t rpcNum,
                 uint32_t sendBuf, uint32_t sendSize, uint32_t recvBuf, uint32_t recvSize, uint32_t &resultPtr);
}
