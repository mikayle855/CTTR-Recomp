// [mods] File-replacement mods for BT4's headerless archives (DATA/PZS4US0-3.AFS + BIN/DBZ4.BIN).
// See ps2_mods.cpp for the folder layout.
#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace ps2x_mods
{
    struct ModAfs;   // one archive's virtual layout: original entries + mod replacements

    // Non-null when an enabled mod replaces entries of this archive (host path of PZS4US<k>.AFS).
    std::shared_ptr<const ModAfs> archiveFor(const std::filesystem::path &hostAfs);
    uint64_t virtualSize(const ModAfs &a);
    // Reads [offset, offset+n) of the virtual archive; past the end reads zeros.
    bool read(const ModAfs &a, uint64_t offset, uint8_t *dst, size_t n);

    // fioOpen hook: BIN/DBZ4.BIN -> the patched copy in mods\.active\ when mods change any archive; else unchanged.
    std::string redirectHostFile(const std::string &hostPath);
}
