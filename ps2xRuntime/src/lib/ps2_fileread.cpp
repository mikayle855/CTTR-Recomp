// [fileread] see ps2_fileread.h
#include "runtime/ps2_fileread.h"

#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace
{
    struct OpenFile
    {
        std::FILE *f = nullptr;
        std::mutex m;   // one seek+read at a time per file
        ~OpenFile() { if (f) std::fclose(f); }
    };
    std::mutex g_mapMutex;
    std::unordered_map<std::filesystem::path::string_type, std::unique_ptr<OpenFile>> g_files;

    OpenFile *get(const std::filesystem::path &p)
    {
        std::lock_guard<std::mutex> lk(g_mapMutex);
        auto &slot = g_files[p.native()];
        if (!slot)
        {
            std::FILE *f = nullptr;
#if defined(_WIN32)
            if (_wfopen_s(&f, p.c_str(), L"rb") != 0) f = nullptr;
#else
            f = std::fopen(p.c_str(), "rb");
#endif
            if (!f) { g_files.erase(p.native()); return nullptr; }   // retried next time (the file may appear later)
            std::setvbuf(f, nullptr, _IONBF, 0);   // reads go straight into the caller's buffer
            slot = std::make_unique<OpenFile>();
            slot->f = f;
        }
        return slot.get();
    }
}

long long ps2xReadFileAt(const std::filesystem::path &p, uint64_t off, void *dst, size_t n)
{
    OpenFile *of = get(p);
    if (!of) return -1;
    std::lock_guard<std::mutex> lk(of->m);
#if defined(_WIN32)
    if (_fseeki64(of->f, (long long)off, SEEK_SET) != 0) return 0;
#else
    if (fseeko(of->f, (off_t)off, SEEK_SET) != 0) return 0;
#endif
    return (long long)std::fread(dst, 1, n, of->f);
}
