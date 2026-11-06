#pragma once
// [fileread] positioned reads from host files kept open: the disc reader and the mod archives used to open, seek
// and close the archive on every read (~46% of the game thread during menu loads went to CreateFile/Close).
#include <cstddef>
#include <cstdint>
#include <filesystem>

// Reads up to n bytes at off into dst; returns the bytes read, or -1 when the file cannot be opened.
long long ps2xReadFileAt(const std::filesystem::path &p, uint64_t off, void *dst, size_t n);
