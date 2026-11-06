#include "Common.h"
#include "TTY.h"
#include "ps2_log.h"

namespace ps2_stubs
{
    void scePrintf(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t format_addr = getRegU32(ctx, 4);
        const std::string formatOwned = readPs2CStringBounded(rdram, runtime, format_addr, 1024);
        if (format_addr == 0)
            return;
        std::string rendered = formatPs2StringWithArgs(rdram, ctx, runtime, formatOwned.c_str(), 1);
        if (rendered.size() > 2048)
            rendered.resize(2048);
        // PS2X_PRINTF=1: show the game's own debug prints (with the caller) in any build; the first 2000.
        static const bool s_show = [](){ const char *v = std::getenv("PS2X_PRINTF"); return v && v[0] && v[0] != '0'; }();
        if (s_show)
        {
            static std::atomic<uint32_t> s_n{0};
            if (s_n.fetch_add(1u) < 2000u)
                std::fprintf(stderr, "[printf ra=0x%x] %s%s", getRegU32(ctx, 31), sanitizeForLog(rendered).c_str(),
                             (!rendered.empty() && rendered.back() == '\n') ? "" : "\n");
        }
        PS2_IF_AGRESSIVE_LOGS({
            const std::string logLine = sanitizeForLog(rendered);
            uint32_t count = 0;
            {
                std::lock_guard<std::mutex> lock(g_printfLogMutex);
                count = ++g_printfLogCount;
            }
            if (count <= kMaxPrintfLogs)
            {
                RUNTIME_LOG("PS2 scePrintf: " << logLine);
                RUNTIME_LOG(std::flush);
            }
            else if (count == kMaxPrintfLogs + 1)
            {
                std::cerr << "PS2 printf logging suppressed after " << kMaxPrintfLogs << " lines" << std::endl;
            }
        });
    }

    void sceResetttyinit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceResetttyinit", rdram, ctx, runtime);
    }

    void sceTtyHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceTtyHandler", rdram, ctx, runtime);
    }

    void sceTtyInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceTtyInit", rdram, ctx, runtime);
    }

    void sceTtyRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceTtyRead", rdram, ctx, runtime);
    }

    void sceTtyWrite(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceTtyWrite", rdram, ctx, runtime);
    }
}
