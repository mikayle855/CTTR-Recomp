#include "IopModuleVm.h"
#include "Spu2.h"

#include "../Syscalls/Common.h"

#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

uint32_t ps2xIopHeapAlloc(uint32_t size);   // Stubs/SIF.cpp: the IOP heap sceSifAllocIopHeap also uses

namespace ps2_iopvm
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr uint32_t kIopRamSize = 0x200000u;
        constexpr uint32_t kModuleBase = 0x40000u;          // below the IOP heap (0x80000): "kernel + modules"
        constexpr uint32_t kThreadExit = 0x1fc00100u;       // return address of a thread entry / module start
        constexpr uint32_t kRpcReturn = 0x1fc00200u;        // return address of an RPC server function
        constexpr uint64_t kSysClockHz = 36864000ull;       // IOP system clock (GetSystemTime)

        bool logOn()
        {
            static const bool s_on = [](){ const char *v = std::getenv("PS2X_IOPVMLOG"); return v && v[0] && v[0] != '0'; }();
            return s_on;
        }

        struct Cpu
        {
            uint32_t r[32] = {};
            uint32_t pc = 0, npc = 4, hi = 0, lo = 0;
        };

        enum class St { Ready, WaitSema, Sleep, Delay, RpcWait, Dormant, Halted };

        struct Thread
        {
            int id = 0;
            Cpu cpu;
            int prio = 64;
            St st = St::Dormant;
            uint32_t entry = 0, gp = 0, stackTop = 0;
            int waitSema = 0;
            Clock::time_point wakeAt{};
            int wakeups = 0;
            uint32_t rpcQueue = 0;                 // the queue (qd) this thread serves in sceSifRpcLoop
            struct Request *rpc = nullptr;         // the request being served
            uint64_t lastRun = 0;
        };

        struct Sema { int count = 0, max = 1; std::deque<int> waiters; };

        struct Server { uint32_t sid = 0, func = 0, buf = 0, queue = 0; };

        struct Request
        {
            uint32_t sid = 0, rpcNum = 0, sendSize = 0, recvSize = 0;
            std::vector<uint8_t> send, recv;
            uint32_t resultPtr = 0;
            bool assigned = false, done = false;
        };

        struct Vm
        {
            std::mutex m;
            std::condition_variable cv;        // the IOP thread waits on this
            std::condition_variable doneCv;    // EE callers wait on this
            PS2Runtime *runtime = nullptr;
            uint8_t *ram = nullptr;
            uint8_t scratch[0x400] = {};
            std::map<int, std::unique_ptr<Thread>> threads;
            std::map<int, Sema> semas;
            std::map<uint32_t, Server> servers;     // by sid
            std::map<uint32_t, std::pair<std::string, uint32_t>> stubs;   // import stub address -> (library, export)
            std::deque<Request *> requests;
            int nextThread = 1, nextSema = 1;
            uint64_t runCounter = 0;
            uint64_t instructions = 0;                 // [iopvm] stats, every 10 s
            std::map<uint64_t, uint32_t> rpcCounts;    // (sid << 32 | rpcNum) -> calls
            Clock::time_point statAt = Clock::now();
            uint32_t nextModuleBase = kModuleBase;
            bool hostStarted = false;
            Clock::time_point t0 = Clock::now();
        };
        Vm g_vm;

        // ---- memory -------------------------------------------------------------------------------------------
        uint8_t *mem(uint32_t addr, uint32_t size)
        {
            const uint32_t p = addr & 0x1fffffffu;
            if (p < 0x800000u)
            {
                const uint32_t o = p & (kIopRamSize - 1);
                return (o + size <= kIopRamSize) ? g_vm.ram + o : nullptr;
            }
            if (p >= 0x1f800000u && p + size <= 0x1f800400u)
                return g_vm.scratch + (p - 0x1f800000u);
            static std::atomic<int> s_n{0};
            if (s_n.fetch_add(1) < 16)
                std::fprintf(stderr, "[iopvm] access to unmapped IOP address 0x%08x\n", addr);
            return nullptr;
        }
        uint32_t rd32(uint32_t a) { uint32_t v = 0; if (uint8_t *p = mem(a, 4)) std::memcpy(&v, p, 4); return v; }
        uint16_t rd16(uint32_t a) { uint16_t v = 0; if (uint8_t *p = mem(a, 2)) std::memcpy(&v, p, 2); return v; }
        uint8_t rd8(uint32_t a) { uint8_t *p = mem(a, 1); return p ? *p : 0; }
        void wr32(uint32_t a, uint32_t v) { if (uint8_t *p = mem(a, 4)) std::memcpy(p, &v, 4); }
        void wr16(uint32_t a, uint16_t v) { if (uint8_t *p = mem(a, 2)) std::memcpy(p, &v, 2); }
        void wr8(uint32_t a, uint8_t v) { if (uint8_t *p = mem(a, 1)) *p = v; }

        std::string cstr(uint32_t a)
        {
            std::string s;
            for (int i = 0; i < 512; ++i) { const char c = char(rd8(a + i)); if (!c) break; s.push_back(c); }
            return s;
        }

        uint64_t sysClockNow() { return uint64_t(std::chrono::duration<double>(Clock::now() - g_vm.t0).count() * double(kSysClockHz)); }

        // ---- IOP printf ---------------------------------------------------------------------------------------
        std::string iopFormat(Cpu &c)
        {
            const std::string fmt = cstr(c.r[4]);
            std::string out;
            int arg = 5;
            auto nextArg = [&]() -> uint32_t {
                if (arg <= 7) return c.r[arg++];
                const uint32_t v = rd32(c.r[29] + 0x10 + (arg - 8) * 4); ++arg; return v; };
            for (size_t i = 0; i < fmt.size(); ++i)
            {
                if (fmt[i] != '%') { out.push_back(fmt[i]); continue; }
                std::string spec = "%";
                while (++i < fmt.size() && std::strchr("-+ #0123456789.lh", fmt[i])) if (fmt[i] != 'l' && fmt[i] != 'h') spec.push_back(fmt[i]);
                if (i >= fmt.size()) break;
                const char conv = fmt[i];
                char buf[256];
                if (conv == 's') { spec.push_back('s'); std::snprintf(buf, sizeof buf, spec.c_str(), cstr(nextArg()).c_str()); }
                else if (conv == 'c') { spec.push_back('c'); std::snprintf(buf, sizeof buf, spec.c_str(), int(nextArg())); }
                else if (conv == '%') { std::snprintf(buf, sizeof buf, "%%"); }
                else if (std::strchr("diuxXop", conv)) { spec.push_back(conv == 'p' ? 'x' : conv); std::snprintf(buf, sizeof buf, spec.c_str(), nextArg()); }
                else { std::snprintf(buf, sizeof buf, "%%%c", conv); }
                out += buf;
            }
            return out;
        }

        // ---- scheduling helpers (g_vm.m held) -----------------------------------------------------------------
        void wake(Thread &t, uint32_t v0)
        {
            t.cpu.r[2] = v0;
            t.st = St::Ready;
        }

        void signalSema(int id)
        {
            auto it = g_vm.semas.find(id);
            if (it == g_vm.semas.end()) return;
            Sema &s = it->second;
            while (!s.waiters.empty())
            {
                const int tid = s.waiters.front();
                s.waiters.pop_front();
                auto t = g_vm.threads.find(tid);
                if (t != g_vm.threads.end() && t->second->st == St::WaitSema && t->second->waitSema == id)
                {
                    wake(*t->second, 0);
                    return;
                }
            }
            if (s.count < s.max) ++s.count;
        }

        // ---- imports ------------------------------------------------------------------------------------------
        // Returns false when the calling thread blocked (its return value is set when it wakes).
        bool callImport(Thread &t, const std::string &lib, uint32_t num)
        {
            Cpu &c = t.cpu;
            uint32_t &v0 = c.r[2];
            const uint32_t a0 = c.r[4], a1 = c.r[5], a2 = c.r[6], a3 = c.r[7];
            auto stackArg = [&](int i) { return rd32(c.r[29] + 0x10 + i * 4); };   // 5th argument and up
            v0 = 0;
            if (lib == "thbase")
            {
                switch (num)
                {
                case 4:   // CreateThread(iop_thread_t *): attr, option, entry, stacksize, priority
                {
                    auto nt = std::make_unique<Thread>();
                    nt->id = g_vm.nextThread++;
                    nt->entry = rd32(a0 + 8);
                    const uint32_t stackSize = std::max<uint32_t>(rd32(a0 + 12), 0x400u);
                    nt->prio = int(rd32(a0 + 16));
                    nt->gp = c.r[28];
                    const uint32_t stack = ps2xIopHeapAlloc(stackSize);
                    nt->stackTop = stack ? stack + stackSize - 0x10u : 0;
                    v0 = stack ? uint32_t(nt->id) : uint32_t(-400);
                    if (logOn()) std::fprintf(stderr, "[iopvm] CreateThread %d entry 0x%x prio %d stack 0x%x+0x%x\n", nt->id, nt->entry, nt->prio, stack, stackSize);
                    if (stack) g_vm.threads[nt->id] = std::move(nt);
                    return true;
                }
                case 5: g_vm.threads.erase(int(a0)); return true;   // DeleteThread
                case 6: case 7:   // StartThread(id, arg)
                {
                    auto it = g_vm.threads.find(int(a0));
                    if (it == g_vm.threads.end()) { v0 = uint32_t(-407); return true; }
                    Thread &n = *it->second;
                    n.cpu = Cpu{};
                    n.cpu.pc = n.entry; n.cpu.npc = n.entry + 4;
                    n.cpu.r[4] = a1; n.cpu.r[28] = n.gp; n.cpu.r[29] = n.stackTop; n.cpu.r[31] = kThreadExit;
                    n.st = St::Ready;
                    return true;
                }
                case 16: case 17: return true;   // RotateThreadReadyQueue: the scheduler rotates after every call slice
                case 20: v0 = uint32_t(t.id); return true;   // GetThreadId
                case 24:   // SleepThread
                    if (t.wakeups > 0) { --t.wakeups; return true; }
                    t.st = St::Sleep;
                    return false;
                case 25: case 26:   // WakeupThread
                {
                    auto it = g_vm.threads.find(int(a0));
                    if (it == g_vm.threads.end()) { v0 = uint32_t(-407); return true; }
                    if (it->second->st == St::Sleep) wake(*it->second, 0); else ++it->second->wakeups;
                    return true;
                }
                case 33:   // DelayThread(usec)
                    t.st = St::Delay;
                    t.wakeAt = Clock::now() + std::chrono::microseconds(a0);
                    return false;
                case 34:   // GetSystemTime(iop_sys_clock_t *)
                {
                    const uint64_t now = sysClockNow();
                    wr32(a0, uint32_t(now)); wr32(a0 + 4, uint32_t(now >> 32));
                    return true;
                }
                case 39:   // USec2SysClock(usec, clock *)
                {
                    const uint64_t clk = uint64_t(a0) * kSysClockHz / 1000000ull;
                    wr32(a1, uint32_t(clk)); wr32(a1 + 4, uint32_t(clk >> 32));
                    return true;
                }
                case 40:   // SysClock2USec(clock *, sec *, usec *)
                {
                    const uint64_t clk = uint64_t(rd32(a0)) | (uint64_t(rd32(a0 + 4)) << 32);
                    const uint64_t us = clk * 1000000ull / kSysClockHz;
                    wr32(a1, uint32_t(us / 1000000ull)); wr32(a2, uint32_t(us % 1000000ull));
                    return true;
                }
                default: break;
                }
            }
            else if (lib == "thsemap")
            {
                switch (num)
                {
                case 4:   // CreateSema(iop_sema_t *): attr, option, initial, max
                {
                    Sema s; s.count = int(rd32(a0 + 8)); s.max = std::max(1, int(rd32(a0 + 12)));
                    const int id = g_vm.nextSema++;
                    g_vm.semas[id] = s;
                    v0 = uint32_t(id);
                    return true;
                }
                case 5: g_vm.semas.erase(int(a0)); return true;
                case 6: case 7: signalSema(int(a0)); return true;
                case 8:   // WaitSema
                {
                    auto it = g_vm.semas.find(int(a0));
                    if (it == g_vm.semas.end()) { v0 = uint32_t(-408); return true; }
                    if (it->second.count > 0) { --it->second.count; return true; }
                    it->second.waiters.push_back(t.id);
                    t.st = St::WaitSema; t.waitSema = int(a0);
                    return false;
                }
                case 9: case 10:   // PollSema
                {
                    auto it = g_vm.semas.find(int(a0));
                    if (it == g_vm.semas.end()) { v0 = uint32_t(-408); return true; }
                    if (it->second.count > 0) { --it->second.count; return true; }
                    v0 = uint32_t(-419);
                    return true;
                }
                default: break;
                }
            }
            else if (lib == "sifcmd")
            {
                switch (num)
                {
                case 14: return true;   // sceSifInitRpc
                case 17:   // sceSifRegisterRpc(sd, sid, func, buf, cfunc, cbuf, qd)
                {
                    Server s; s.sid = a1; s.func = a2; s.buf = a3; s.queue = stackArg(2);
                    g_vm.servers[a1] = s;
                    if (logOn()) std::fprintf(stderr, "[iopvm] RPC server 0x%x func 0x%x buf 0x%x queue 0x%x\n", a1, a2, a3, s.queue);
                    return true;
                }
                case 19: return true;   // sceSifSetRpcQueue(qd, thread)
                case 22:   // sceSifRpcLoop(qd): serve requests forever
                    t.st = St::RpcWait; t.rpcQueue = a0;
                    return false;
                default: break;
                }
            }
            else if (lib == "sysclib")
            {
                switch (num)
                {
                case 12: case 13:   // memcpy / memmove
                {
                    uint8_t *d = mem(a0, a2), *s = mem(a1, a2);
                    if (d && s) std::memmove(d, s, a2);
                    v0 = a0;
                    return true;
                }
                case 14: { if (uint8_t *d = mem(a0, a2)) std::memset(d, int(a1 & 0xff), a2); v0 = a0; return true; }   // memset
                default: break;
                }
            }
            else if (lib == "sysmem")
            {
                switch (num)
                {
                case 4: v0 = ps2xIopHeapAlloc(a1); return true;   // AllocSysMemory(mode, size, addr)
                case 5: return true;                               // FreeSysMemory: the bump heap keeps it
                default: break;
                }
            }
            else if (lib == "loadcore" || lib == "intrman")
            {
                if (lib == "intrman" && num == 17 && a0) wr32(a0, 0);   // CpuSuspendIntr(&state)
                return true;
            }
            else if (lib == "stdio")
            {
                if (num == 4)
                {
                    const std::string s = iopFormat(c);
                    if (logOn()) std::fprintf(stderr, "[iop] %s", s.c_str());
                    v0 = uint32_t(s.size());
                    return true;
                }
            }
            else if (lib == "libsd")
            {
                switch (num)
                {
                case 5: ps2_spu2::setParam(uint16_t(a0), uint16_t(a1)); return true;
                case 6: v0 = ps2_spu2::getParam(uint16_t(a0)); return true;
                case 7: ps2_spu2::setSwitch(uint16_t(a0), a1); return true;
                case 8: v0 = ps2_spu2::getSwitch(uint16_t(a0)); return true;
                case 9: ps2_spu2::setAddr(uint16_t(a0), a1); return true;
                case 10: v0 = ps2_spu2::getAddr(uint16_t(a0)); return true;
                case 17:   // sceSdVoiceTrans(channel, mode, iopaddr, spuaddr, size): done at once
                    v0 = ps2_spu2::voiceTrans(g_vm.ram, kIopRamSize, uint16_t(a1), a2, a3, stackArg(0));
                    return true;
                case 19: v0 = 1; return true;   // sceSdVoiceTransStatus: complete
                default: break;
                }
            }
            static std::set<std::pair<std::string, uint32_t>> s_warned;
            if (s_warned.insert({lib, num}).second)
                std::fprintf(stderr, "[iopvm] unimplemented import %s #%u (returns 0)\n", lib.c_str(), num);
            return true;
        }

        // ---- R3000 --------------------------------------------------------------------------------------------
        // Runs up to `budget` instructions. Returns when the thread blocks, exits or the budget runs out.
        void run(Thread &t, int budget)
        {
            Cpu &c = t.cpu;
            for (int n = 0; n < budget && t.st == St::Ready; ++n)
            {
                c.r[0] = 0;
                ++g_vm.instructions;
                if (c.pc == kThreadExit)
                {
                    t.st = St::Dormant;
                    return;
                }
                if (c.pc == kRpcReturn)
                {
                    Request *rq = t.rpc;
                    t.rpc = nullptr;
                    if (rq)
                    {
                        rq->resultPtr = c.r[2];
                        rq->recv.assign(rq->recvSize, 0);
                        if (rq->recvSize)
                            if (uint8_t *p = mem(c.r[2], rq->recvSize)) std::memcpy(rq->recv.data(), p, rq->recvSize);
                        rq->done = true;
                        g_vm.doneCv.notify_all();
                    }
                    t.st = St::RpcWait;
                    return;
                }
                if (auto st = g_vm.stubs.find(c.pc); st != g_vm.stubs.end())
                {   // an import: the stub's caller resumes at $ra
                    const uint32_t ra = c.r[31];
                    c.pc = ra; c.npc = ra + 4;
                    if (!callImport(t, st->second.first, st->second.second))
                        return;
                    continue;
                }
                const uint32_t in = rd32(c.pc);
                uint32_t nextPc = c.npc, nextNpc = c.npc + 4;
                const uint32_t op = in >> 26, rs = (in >> 21) & 31, rt = (in >> 16) & 31, rd = (in >> 11) & 31, sa = (in >> 6) & 31;
                const int32_t simm = int16_t(in & 0xffff);
                const uint32_t uimm = in & 0xffff;
                uint32_t *R = c.r;
                auto branch = [&](bool cond) { if (cond) nextNpc = c.npc + (uint32_t(simm) << 2); };
                switch (op)
                {
                case 0x00:
                    switch (in & 0x3f)
                    {
                    case 0x00: R[rd] = R[rt] << sa; break;
                    case 0x02: R[rd] = R[rt] >> sa; break;
                    case 0x03: R[rd] = uint32_t(int32_t(R[rt]) >> sa); break;
                    case 0x04: R[rd] = R[rt] << (R[rs] & 31); break;
                    case 0x06: R[rd] = R[rt] >> (R[rs] & 31); break;
                    case 0x07: R[rd] = uint32_t(int32_t(R[rt]) >> (R[rs] & 31)); break;
                    case 0x08: nextNpc = R[rs]; break;
                    case 0x09: { const uint32_t target = R[rs]; R[rd] = c.npc + 4; nextNpc = target; break; }
                    case 0x0c: break;   // SYSCALL: the IOP kernel is not called this way by modules
                    case 0x0d:
                        std::fprintf(stderr, "[iopvm] thread %d hit BREAK at 0x%x: halted\n", t.id, c.pc);
                        t.st = St::Halted;
                        return;
                    case 0x10: R[rd] = c.hi; break;
                    case 0x11: c.hi = R[rs]; break;
                    case 0x12: R[rd] = c.lo; break;
                    case 0x13: c.lo = R[rs]; break;
                    case 0x18: { const int64_t p = int64_t(int32_t(R[rs])) * int32_t(R[rt]); c.lo = uint32_t(p); c.hi = uint32_t(uint64_t(p) >> 32); break; }
                    case 0x19: { const uint64_t p = uint64_t(R[rs]) * R[rt]; c.lo = uint32_t(p); c.hi = uint32_t(p >> 32); break; }
                    case 0x1a:
                    {
                        const int32_t n = int32_t(R[rs]), d = int32_t(R[rt]);
                        if (d == 0) { c.lo = n >= 0 ? 0xffffffffu : 1u; c.hi = uint32_t(n); }
                        else if (n == INT32_MIN && d == -1) { c.lo = uint32_t(n); c.hi = 0; }
                        else { c.lo = uint32_t(n / d); c.hi = uint32_t(n % d); }
                        break;
                    }
                    case 0x1b:
                        if (R[rt] == 0) { c.lo = 0xffffffffu; c.hi = R[rs]; }
                        else { c.lo = R[rs] / R[rt]; c.hi = R[rs] % R[rt]; }
                        break;
                    case 0x20: case 0x21: R[rd] = R[rs] + R[rt]; break;
                    case 0x22: case 0x23: R[rd] = R[rs] - R[rt]; break;
                    case 0x24: R[rd] = R[rs] & R[rt]; break;
                    case 0x25: R[rd] = R[rs] | R[rt]; break;
                    case 0x26: R[rd] = R[rs] ^ R[rt]; break;
                    case 0x27: R[rd] = ~(R[rs] | R[rt]); break;
                    case 0x2a: R[rd] = int32_t(R[rs]) < int32_t(R[rt]) ? 1 : 0; break;
                    case 0x2b: R[rd] = R[rs] < R[rt] ? 1 : 0; break;
                    default:
                        std::fprintf(stderr, "[iopvm] thread %d: unknown SPECIAL %08x at 0x%x: halted\n", t.id, in, c.pc);
                        t.st = St::Halted;
                        return;
                    }
                    break;
                case 0x01:
                {
                    const bool link = (rt & 0x1e) == 0x10;
                    const bool cond = (rt & 1) ? int32_t(R[rs]) >= 0 : int32_t(R[rs]) < 0;
                    if (link) R[31] = c.npc + 4;
                    branch(cond);
                    break;
                }
                case 0x02: nextNpc = (c.npc & 0xf0000000u) | ((in & 0x3ffffff) << 2); break;
                case 0x03: R[31] = c.npc + 4; nextNpc = (c.npc & 0xf0000000u) | ((in & 0x3ffffff) << 2); break;
                case 0x04: branch(R[rs] == R[rt]); break;
                case 0x05: branch(R[rs] != R[rt]); break;
                case 0x06: branch(int32_t(R[rs]) <= 0); break;
                case 0x07: branch(int32_t(R[rs]) > 0); break;
                case 0x08: case 0x09: R[rt] = R[rs] + uint32_t(simm); break;
                case 0x0a: R[rt] = int32_t(R[rs]) < simm ? 1 : 0; break;
                case 0x0b: R[rt] = R[rs] < uint32_t(simm) ? 1 : 0; break;
                case 0x0c: R[rt] = R[rs] & uimm; break;
                case 0x0d: R[rt] = R[rs] | uimm; break;
                case 0x0e: R[rt] = R[rs] ^ uimm; break;
                case 0x0f: R[rt] = uimm << 16; break;
                case 0x10:   // COP0: status/cause are not modelled; mfc0 reads 0
                    if (rs == 0) R[rt] = 0;
                    break;
                case 0x20: R[rt] = uint32_t(int8_t(rd8(R[rs] + simm))); break;
                case 0x21: R[rt] = uint32_t(int16_t(rd16(R[rs] + simm))); break;
                case 0x22: { const uint32_t a = R[rs] + simm, w = rd32(a & ~3u), sh = (a & 3) * 8; R[rt] = (R[rt] & (0x00ffffffu >> sh)) | (w << (24 - sh)); break; }   // LWL
                case 0x23: R[rt] = rd32(R[rs] + simm); break;
                case 0x24: R[rt] = rd8(R[rs] + simm); break;
                case 0x25: R[rt] = rd16(R[rs] + simm); break;
                case 0x26: { const uint32_t a = R[rs] + simm, w = rd32(a & ~3u), sh = (a & 3) * 8; R[rt] = (R[rt] & ~(0xffffffffu >> sh)) | (w >> sh); break; }        // LWR
                case 0x28: wr8(R[rs] + simm, uint8_t(R[rt])); break;
                case 0x29: wr16(R[rs] + simm, uint16_t(R[rt])); break;
                case 0x2a: { const uint32_t a = R[rs] + simm, sh = (a & 3) * 8, w = rd32(a & ~3u); wr32(a & ~3u, (w & ~(0xffffffffu >> (24 - sh))) | (R[rt] >> (24 - sh))); break; }   // SWL
                case 0x2b: wr32(R[rs] + simm, R[rt]); break;
                case 0x2e: { const uint32_t a = R[rs] + simm, sh = (a & 3) * 8, w = rd32(a & ~3u); wr32(a & ~3u, (w & ~(0xffffffffu << sh)) | (R[rt] << sh)); break; }               // SWR
                default:
                    std::fprintf(stderr, "[iopvm] thread %d: unknown opcode %08x at 0x%x: halted\n", t.id, in, c.pc);
                    t.st = St::Halted;
                    return;
                }
                c.pc = nextPc;
                c.npc = nextNpc;
            }
        }

        // ---- the IOP host thread ------------------------------------------------------------------------------
        void hostLoop()
        {
            std::unique_lock<std::mutex> lock(g_vm.m);
            for (;;)
            {
                const auto now = Clock::now();
                Clock::time_point nextWake = now + std::chrono::milliseconds(50);
                for (auto &[id, tp] : g_vm.threads)
                {
                    Thread &t = *tp;
                    if (t.st == St::Delay)
                    {
                        if (t.wakeAt <= now) wake(t, 0);
                        else nextWake = std::min(nextWake, t.wakeAt);
                    }
                }
                for (Request *rq : g_vm.requests)
                {   // hand each request to the thread looping on its server's queue
                    if (rq->assigned) continue;
                    const auto sv = g_vm.servers.find(rq->sid);
                    if (sv == g_vm.servers.end()) continue;
                    for (auto &[id, tp] : g_vm.threads)
                    {
                        Thread &t = *tp;
                        if (t.st != St::RpcWait || t.rpcQueue != sv->second.queue) continue;
                        if (sv->second.buf && rq->sendSize)
                            if (uint8_t *p = mem(sv->second.buf, rq->sendSize)) std::memcpy(p, rq->send.data(), rq->sendSize);
                        t.cpu.pc = sv->second.func; t.cpu.npc = t.cpu.pc + 4;
                        t.cpu.r[4] = rq->rpcNum; t.cpu.r[5] = sv->second.buf; t.cpu.r[6] = rq->sendSize;
                        t.cpu.r[31] = kRpcReturn;
                        t.rpc = rq;
                        t.st = St::Ready;
                        rq->assigned = true;
                        break;
                    }
                }
                g_vm.requests.erase(std::remove_if(g_vm.requests.begin(), g_vm.requests.end(), [](Request *r) { return r->assigned; }),
                                    g_vm.requests.end());
                // the highest-priority ready thread (lowest number); least recently run among equals
                Thread *pick = nullptr;
                for (auto &[id, tp] : g_vm.threads)
                {
                    Thread &t = *tp;
                    if (t.st != St::Ready) continue;
                    if (!pick || t.prio < pick->prio || (t.prio == pick->prio && t.lastRun < pick->lastRun))
                        pick = &t;
                }
                if (!pick)
                {
                    g_vm.cv.wait_until(lock, nextWake);
                    continue;
                }
                pick->lastRun = ++g_vm.runCounter;
                run(*pick, 20000);
                if (Clock::now() - g_vm.statAt >= std::chrono::seconds(10))
                {
                    g_vm.statAt = Clock::now();
                    std::string line;
                    char b[96];
                    for (auto &[k, n] : g_vm.rpcCounts) { std::snprintf(b, sizeof b, " 0x%x#%u:%u", uint32_t(k >> 32), uint32_t(k), n); line += b; }
                    int ready = 0, blocked = 0;
                    for (auto &[id, tp] : g_vm.threads) (tp->st == St::Ready ? ready : blocked)++;
                    std::fprintf(stderr, "[iopvm] 10 s: %.1fM instructions, rpc%s | threads ready %d blocked %d | %s\n", g_vm.instructions / 1e6, line.c_str(), ready, blocked,
                                 ps2_spu2::statusLine().c_str());
                    g_vm.instructions = 0;
                    g_vm.rpcCounts.clear();
                }
            }
        }

        void ensureHost()
        {
            if (g_vm.hostStarted) return;
            g_vm.hostStarted = true;
            std::thread(hostLoop).detach();
        }

        // ---- IRX loading --------------------------------------------------------------------------------------
        bool relocateAndLoad(const std::vector<uint8_t> &e, uint32_t base, uint32_t &entry, uint32_t &gp, uint32_t &memSize)
        {
            auto u16 = [&](size_t o) { uint16_t v; std::memcpy(&v, &e[o], 2); return v; };
            auto u32 = [&](size_t o) { uint32_t v; std::memcpy(&v, &e[o], 4); return v; };
            if (e.size() < 0x34 || std::memcmp(e.data(), "\x7f" "ELF", 4) != 0 || u16(16) != 0xff80) return false;
            const uint32_t phoff = u32(0x1c), shoff = u32(0x20);
            const uint16_t phnum = u16(0x2c), shentsize = u16(0x2e), shnum = u16(0x30);
            bool haveMod = false, haveLoad = false;
            for (uint16_t i = 0; i < shnum; ++i)
            {
                const size_t sh = shoff + size_t(i) * shentsize;
                if (u32(sh + 4) == 0x70000080u)
                {   // .iopmod: module info, entry, gp, text, data, bss
                    const uint32_t o = u32(sh + 16);
                    entry = u32(o + 4); gp = u32(o + 8);
                    haveMod = true;
                }
            }
            for (uint16_t i = 0; i < phnum; ++i)
            {
                const size_t ph = phoff + size_t(i) * 32;
                if (u32(ph) != 1) continue;
                const uint32_t off = u32(ph + 4), va = u32(ph + 8), fsz = u32(ph + 16), msz = u32(ph + 20);
                if (base + va + msz > kIopRamSize || off + fsz > e.size()) return false;
                std::memset(g_vm.ram + base + va, 0, msz);
                std::memcpy(g_vm.ram + base + va, &e[off], fsz);
                memSize = va + msz;
                haveLoad = true;
            }
            if (!haveMod || !haveLoad) return false;
            for (uint16_t i = 0; i < shnum; ++i)
            {
                const size_t sh = shoff + size_t(i) * shentsize;
                if (u32(sh + 4) != 9) continue;   // SHT_REL
                const uint32_t off = u32(sh + 16), size = u32(sh + 20);
                std::vector<uint32_t> pendingHi;
                for (uint32_t k = 0; k + 8 <= size; k += 8)
                {
                    const uint32_t where = base + u32(off + k), type = u32(off + k + 4) & 0xff;
                    uint32_t w = rd32(where);
                    switch (type)
                    {
                    case 2: wr32(where, w + base); break;                                                       // R_MIPS_32
                    case 4: wr32(where, (w & 0xfc000000u) | (((w & 0x3ffffffu) + (base >> 2)) & 0x3ffffffu)); break;   // R_MIPS_26
                    case 5: pendingHi.push_back(where); break;                                                  // R_MIPS_HI16
                    case 6:                                                                                     // R_MIPS_LO16
                    {
                        const int32_t lo = int16_t(w & 0xffff);
                        for (uint32_t hiAt : pendingHi)
                        {
                            const uint32_t hw = rd32(hiAt);
                            const uint32_t value = ((hw & 0xffff) << 16) + uint32_t(lo) + base;
                            wr32(hiAt, (hw & 0xffff0000u) | (((value + 0x8000u) >> 16) & 0xffff));
                        }
                        pendingHi.clear();
                        wr32(where, (w & 0xffff0000u) | ((uint32_t(lo) + base) & 0xffff));
                        break;
                    }
                    default:
                        std::fprintf(stderr, "[iopvm] unsupported relocation type %u\n", type);
                        return false;
                    }
                }
            }
            // import tables: magic 0x41e00000, next, version, flags, name[8], then (jr $ra; addiu $0,$0,n) stubs
            for (uint32_t a = base; a + 28 <= base + memSize; a += 4)
            {
                if (rd32(a) != 0x41e00000u) continue;
                char name[9] = {};
                std::memcpy(name, mem(a + 12, 8), 8);
                uint32_t p = a + 20;
                while (rd32(p) == 0x03e00008u)
                {
                    g_vm.stubs[p] = {name, rd32(p + 4) & 0xffff};
                    p += 8;
                }
                a = p;
            }
            return true;
        }
    }

    bool loadModule(PS2Runtime *runtime, const std::string &name, const std::vector<uint8_t> &image)
    {
        std::lock_guard<std::mutex> lock(g_vm.m);
        if (!runtime) return false;
        g_vm.runtime = runtime;
        g_vm.ram = runtime->memory().getIOPRAM();
        if (!g_vm.ram) return false;
        uint32_t entry = 0, gp = 0, memSize = 0;
        const uint32_t base = g_vm.nextModuleBase;
        if (!relocateAndLoad(image, base, entry, gp, memSize))
        {
            std::fprintf(stderr, "[iopvm] cannot load %s\n", name.c_str());
            return false;
        }
        g_vm.nextModuleBase = (base + memSize + 0xfffu) & ~0xfffu;
        // the module's start function runs in a thread of its own at the loader's priority
        auto t = std::make_unique<Thread>();
        t->id = g_vm.nextThread++;
        t->prio = 8;
        const uint32_t stack = ps2xIopHeapAlloc(0x1000);
        t->stackTop = stack + 0x1000 - 0x10;
        t->gp = base + gp;
        t->cpu.pc = base + entry; t->cpu.npc = t->cpu.pc + 4;
        t->cpu.r[28] = t->gp; t->cpu.r[29] = t->stackTop; t->cpu.r[31] = kThreadExit;
        t->st = St::Ready;
        std::fprintf(stderr, "[iopvm] %s loaded at 0x%x (0x%x bytes, entry 0x%x, %zu imports); starting it\n", name.c_str(), base, memSize,
                     base + entry, g_vm.stubs.size());
        g_vm.threads[t->id] = std::move(t);
        ensureHost();
        g_vm.cv.notify_all();
        return true;
    }

    bool callRpc(PS2Runtime *runtime, uint8_t *rdram, uint32_t sid, uint32_t rpcNum,
                 uint32_t sendBuf, uint32_t sendSize, uint32_t recvBuf, uint32_t recvSize, uint32_t &resultPtr)
    {
        (void)runtime;
        Request rq;
        {
            std::unique_lock<std::mutex> lock(g_vm.m);
            if (!g_vm.servers.count(sid))
            {   // the module may still be starting: give its server threads a moment to register
                const auto deadline = Clock::now() + std::chrono::milliseconds(500);
                bool known = false;
                for (auto &[s, srv] : g_vm.servers) known = known || s == sid;
                if (!g_vm.hostStarted) return false;
                while (!g_vm.servers.count(sid) && Clock::now() < deadline)
                {
                    lock.unlock();
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    lock.lock();
                }
                if (!g_vm.servers.count(sid)) return false;
            }
            {   // [rpclog] PS2X_RPCLOG=<sid hex>: time, number and sizes of each call to that server (first 3000)
                static const uint32_t s_sid = [](){ const char *v = std::getenv("PS2X_RPCLOG"); return v ? uint32_t(std::strtoul(v, nullptr, 16)) : 0u; }();
                static int s_n = 0;
                if (s_sid && sid == s_sid && s_n < 3000)
                {
                    ++s_n;
                    uint32_t w[8] = {0, 0, 0, 0, 0, 0, 0, 0};
                    for (int k = 0; k < 8 && uint32_t(k * 4 + 4) <= sendSize; ++k)
                        for (int b = 0; b < 4; ++b)
                            if (const uint8_t *p = getConstMemPtr(rdram, sendBuf + k * 4 + b)) w[k] |= uint32_t(*p) << (b * 8);
                    std::fprintf(stderr, "[rpclog] t=%.3f sid 0x%x #%u send %u recv %u | %08x %08x %08x %08x %08x %08x %08x %08x\n",
                                 std::chrono::duration<double>(Clock::now() - g_vm.t0).count(), sid, rpcNum, sendSize, recvSize, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
                }
            }
            rq.sid = sid; rq.rpcNum = rpcNum; rq.sendSize = sendSize; rq.recvSize = recvSize;
            ++g_vm.rpcCounts[(uint64_t(sid) << 32) | rpcNum];
            rq.send.resize(sendSize);
            for (uint32_t i = 0; i < sendSize; ++i)
                if (const uint8_t *p = getConstMemPtr(rdram, sendBuf + i)) rq.send[i] = *p;
            g_vm.requests.push_back(&rq);
            g_vm.cv.notify_all();
            if (!g_vm.doneCv.wait_for(lock, std::chrono::seconds(2), [&] { return rq.done; }))
            {
                std::fprintf(stderr, "[iopvm] RPC 0x%x #%u timed out\n", sid, rpcNum);
                g_vm.requests.erase(std::remove(g_vm.requests.begin(), g_vm.requests.end(), &rq), g_vm.requests.end());
                for (auto &[id, tp] : g_vm.threads)
                    if (tp->rpc == &rq) tp->rpc = nullptr;
                return true;
            }
        }
        for (uint32_t i = 0; i < recvSize && i < rq.recv.size(); ++i)
            if (uint8_t *p = getMemPtr(rdram, recvBuf + i)) *p = rq.recv[i];
        {   // [sndcmd] PS2X_SNDCMD=1: the per-tick sound command RPC (0x12 #16): sizes, then send/recv words whenever they change
            static const bool s_on = [](){ const char *v = std::getenv("PS2X_SNDCMD"); return v && v[0] && v[0] != '0'; }();
            if (s_on && sid == 0x12u)
            {
                static std::vector<uint8_t> s_lastSend, s_lastRecv;
                static int s_n = 0;
                int nzS = 0, nzR = 0;
                for (size_t i = 4; i + 4 <= rq.send.size(); i += 4) { uint32_t w; std::memcpy(&w, &rq.send[i], 4); nzS += w != 0; }
                for (size_t i = 4; i + 4 <= rq.recv.size(); i += 4) { uint32_t w; std::memcpy(&w, &rq.recv[i], 4); nzR += w != 0; }
                static uint64_t s_calls = 0, s_withCmds = 0;
                ++s_calls; if (nzS) ++s_withCmds;
                if ((nzS > 2 || nzR) && s_n < 300)   // an idle tick carries just the two 0xffffffff words at 0x3f4/0x3f8
                {   // non-zero words past the counter only, as index:value
                    ++s_n;
                    std::fprintf(stderr, "[sndcmd] #%u call %llu (%llu with commands) send nz %d:", rpcNum, (unsigned long long)s_calls, (unsigned long long)s_withCmds, nzS);
                    int shown = 0;
                    for (size_t i = 4; i + 4 <= rq.send.size() && shown < 40; i += 4) { uint32_t w; std::memcpy(&w, &rq.send[i], 4); if (w) { std::fprintf(stderr, " %zx:%08x", i, w); ++shown; } }
                    std::fprintf(stderr, " | recv nz %d:", nzR);
                    shown = 0;
                    for (size_t i = 4; i + 4 <= rq.recv.size() && shown < 24; i += 4) { uint32_t w; std::memcpy(&w, &rq.recv[i], 4); if (w) { std::fprintf(stderr, " %zx:%08x", i, w); ++shown; } }
                    std::fprintf(stderr, "\n");
                }
                (void)s_lastSend; (void)s_lastRecv;
            }
        }
        resultPtr = recvBuf;
        return true;
    }
}
