// Crash Tag Team Racing (PC port) launcher: the same settings as "Play CTTR (test).bat", but no console window.
// Build: games\cttr\launcher\build_launcher.bat (clang-cl + rc, output games\cttr\launcher\CTTR.exe).
//
// Layout ([portable]): used when a "runtime\ps2EntryRunner.exe" sits next to this launcher (games\cttr\make_portable.ps1 builds
//    such a folder): everything lives in the launcher's folder -- runtime\ (the game program), assets\, data\ (the game
//    file, copied from the disc on first start), savedata\ (memory card, settings, controller bindings), crashes\,
//    save_backups\, play.log. The disc image comes from PS2X_CD_IMAGE, else disc.txt, else the first *.iso in the
//    folder, else the player picks it once (saved to disc.txt).
// [savebackup] Each start (real save only) first backs up savedata\BESLES-53439-C1 + settings.toml to save_backups.
// [crashnotify] Then it waits for the game to close. If the game wrote a new crash report (<game folder>\crashes\, see
// ps2xRuntime ps2_crashreport.cpp) and did not close normally, it copies the end of the log into the report as
// run_tail.log and tells the player where it is.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <string>
#include <filesystem>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
namespace fs = std::filesystem;

static const wchar_t *kTitle = L"Crash Tag Team Racing";
static const wchar_t *kElfName = L"SLES_534.39";
static const wchar_t *kSaveFolder = L"BESLES-53439-C1";

struct Paths
{
    bool portable = false;
    fs::path runner, runnerDir, gameDir, elf, iso, log, outLog;
};

static bool fileExists(const fs::path &p) { std::error_code ec; return fs::exists(p, ec); }
static void fail(const std::wstring &msg) { MessageBoxW(nullptr, msg.c_str(), kTitle, MB_OK | MB_ICONERROR); }
static std::wstring envW(const wchar_t *name)
{
    wchar_t b[MAX_PATH * 2] = L"";
    const DWORD n = GetEnvironmentVariableW(name, b, MAX_PATH * 2);
    return (n > 0 && n < MAX_PATH * 2) ? std::wstring(b) : std::wstring();
}

// [savebackup] Before each start: copy the memory-card save folder + settings to save_backups\<date time>\ when they
// changed since the newest backup; keep the newest 10. Backing up before play means a bad session (or a crash
// while saving) never leaves only a damaged copy.
static bool sameFile(const fs::path &a, const fs::path &b)
{
    std::error_code ec;
    if (!fs::exists(a, ec) || !fs::exists(b, ec) || fs::file_size(a, ec) != fs::file_size(b, ec)) return false;
    FILE *fa = _wfopen(a.c_str(), L"rb"), *fb = _wfopen(b.c_str(), L"rb");
    bool same = fa && fb;
    static char ba[65536], bb[65536];
    while (same) { size_t na = fread(ba, 1, sizeof ba, fa), nb = fread(bb, 1, sizeof bb, fb); if (na != nb || memcmp(ba, bb, na)) same = false; if (na == 0) break; }
    if (fa) fclose(fa); if (fb) fclose(fb);
    return same;
}
static void backupSave(const fs::path &gameDir)
{
    std::error_code ec;
    const fs::path saveDir = gameDir / L"savedata", backupDir = gameDir / L"save_backups";
    const fs::path save = saveDir / kSaveFolder, settings = saveDir / L"settings.toml";
    if (!fs::exists(save / kSaveFolder, ec)) return;   // nothing saved yet
    std::vector<fs::path> olds;
    for (auto &e : fs::directory_iterator(backupDir, ec)) if (e.is_directory()) olds.push_back(e.path());
    std::sort(olds.begin(), olds.end());   // names are yyyy-mm-dd_hh-mm-ss
    if (!olds.empty() && sameFile(save / kSaveFolder, olds.back() / kSaveFolder / kSaveFolder) &&
        sameFile(settings, olds.back() / L"settings.toml")) return;   // unchanged
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t name[64]; swprintf(name, 64, L"%04u-%02u-%02u_%02u-%02u-%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    const fs::path dst = backupDir / name;
    fs::create_directories(dst, ec);
    fs::copy(save, dst / kSaveFolder, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    fs::copy_file(settings, dst / L"settings.toml", fs::copy_options::overwrite_existing, ec);
    olds.push_back(dst);
    while (olds.size() > 10) { fs::remove_all(olds.front(), ec); olds.erase(olds.begin()); }
}

// [portable] ISO9660: find a file in the disc's root directory. Returns false when it is not there.
static bool isoFindRoot(FILE *f, const std::string &name, uint32_t &lbn, uint32_t &size)
{
    unsigned char pvd[2048];
    if (_fseeki64(f, 16ll * 2048, SEEK_SET) || fread(pvd, 1, 2048, f) != 2048 || memcmp(pvd + 1, "CD001", 5)) return false;
    uint32_t dirLbn = 0, dirSize = 0;
    memcpy(&dirLbn, pvd + 156 + 2, 4);
    memcpy(&dirSize, pvd + 156 + 10, 4);
    std::vector<unsigned char> dir(dirSize);
    if (_fseeki64(f, (long long)dirLbn * 2048, SEEK_SET) || fread(dir.data(), 1, dirSize, f) != dirSize) return false;
    for (uint32_t p = 0; p < dirSize;)
    {
        const unsigned len = dir[p];
        if (len == 0) { p = (p / 2048 + 1) * 2048; continue; }   // records never cross a sector
        const unsigned nlen = dir[p + 32];
        std::string n(reinterpret_cast<const char *>(&dir[p + 33]), nlen);
        if (const size_t semi = n.find(';'); semi != std::string::npos) n.resize(semi);
        if (_stricmp(n.c_str(), name.c_str()) == 0)
        {
            memcpy(&lbn, &dir[p + 2], 4);
            memcpy(&size, &dir[p + 10], 4);
            return true;
        }
        p += len;
    }
    return false;
}
static bool isoIsCttr(const fs::path &iso)
{
    FILE *f = _wfopen(iso.c_str(), L"rb");
    if (!f) return false;
    uint32_t lbn = 0, size = 0;
    const bool ok = isoFindRoot(f, "SLES_534.39", lbn, size);
    fclose(f);
    return ok;
}
// Copies the game file (SLES_534.39) from the disc into data\ -- the PC port runs the recompiled code, but reads the
// game's data from this file.
static bool extractElf(const fs::path &iso, const fs::path &dst)
{
    FILE *f = _wfopen(iso.c_str(), L"rb");
    if (!f) return false;
    uint32_t lbn = 0, size = 0;
    bool ok = isoFindRoot(f, "SLES_534.39", lbn, size) && size > 0;
    std::vector<char> buf;
    if (ok)
    {
        buf.resize(size);
        ok = !_fseeki64(f, (long long)lbn * 2048, SEEK_SET) && fread(buf.data(), 1, size, f) == size;
    }
    fclose(f);
    if (!ok) return false;
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    const fs::path tmp = dst.wstring() + L".tmp";
    FILE *o = _wfopen(tmp.c_str(), L"wb");
    if (!o) return false;
    ok = fwrite(buf.data(), 1, buf.size(), o) == buf.size();
    fclose(o);
    if (ok) fs::rename(tmp, dst, ec);
    return ok && !ec;
}
static fs::path pickIso(const fs::path &startDir)
{
    wchar_t file[MAX_PATH * 2] = L"";
    OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof ofn;
    ofn.lpstrFilter = L"PS2 disc image (*.iso)\0*.iso\0All files\0*.*\0";
    ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH * 2;
    ofn.lpstrInitialDir = startDir.c_str();
    ofn.lpstrTitle = L"Choose your Crash Tag Team Racing (PAL) disc image";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&ofn) ? fs::path(file) : fs::path();
}
static std::wstring readLine(const fs::path &p)
{
    FILE *f = _wfopen(p.c_str(), L"rb");
    if (!f) return {};
    char b[2048] = {};
    const size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    std::string s(b, n);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF) s = s.substr(3);   // UTF-8 BOM
    const int wn = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(wn, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], wn);
    return w;
}
static void writeLine(const fs::path &p, const std::wstring &w)
{
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    if (FILE *f = _wfopen(p.c_str(), L"wb")) { fwrite(s.data(), 1, s.size(), f); fputs("\r\n", f); fclose(f); }
}

// Works out where everything is; false = a message was shown, do not start.
static bool resolvePaths(Paths &P)
{
    wchar_t self[MAX_PATH * 2] = L"";
    GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
    const fs::path here = fs::path(self).parent_path();
    const std::wstring envExe = envW(L"PS2X_EXEDIR"), envIso = envW(L"PS2X_CD_IMAGE");

    if (fileExists(here / L"runtime" / L"ps2EntryRunner.exe"))
    {   // [portable]
        P.portable = true;
        P.runner = here / L"runtime" / L"ps2EntryRunner.exe";
        P.gameDir = envExe.empty() ? here : fs::path(envExe);
        P.runnerDir = P.gameDir;   // controller bindings are written to <working dir>\savedata
        P.elf = P.gameDir / L"data" / kElfName;
        P.log = P.gameDir / L"play.log";
        P.outLog = P.gameDir / L"play_stdout.log";
        const fs::path discTxt = here / L"disc.txt";
        if (!envIso.empty()) P.iso = envIso;
        if (P.iso.empty()) { const std::wstring t = readLine(discTxt); if (!t.empty() && fileExists(t)) P.iso = t; }
        if (P.iso.empty())
        {
            std::error_code ec;
            for (auto &e : fs::directory_iterator(here, ec))
                if (e.is_regular_file() && _wcsicmp(e.path().extension().c_str(), L".iso") == 0 && isoIsCttr(e.path())) { P.iso = e.path(); break; }
        }
        while (P.iso.empty())
        {
            MessageBoxW(nullptr, L"Please choose your Crash Tag Team Racing disc image (PAL, SLES-53439, .iso).\n\n"
                                 L"You only need to do this once.", kTitle, MB_OK | MB_ICONINFORMATION);
            const fs::path pick = pickIso(here);
            if (pick.empty()) return false;   // cancelled
            if (!isoIsCttr(pick)) { fail(L"That file is not the PAL Crash Tag Team Racing disc (SLES_534.39 not found on it)."); continue; }
            P.iso = pick;
            writeLine(discTxt, P.iso.wstring());
        }
        if (!fileExists(P.elf))
        {
            if (!extractElf(P.iso, P.elf))
            {
                fail(L"Could not copy the game file from the disc image:\n" + P.iso.wstring());
                return false;
            }
        }
    }
    else
    {
        fail(L"The game program was not found next to this launcher (runtime\\ps2EntryRunner.exe).\n\n"
             L"Build the game, then run games\\cttr\\make_portable.ps1: it puts CTTR.exe, runtime\\ and assets\\ "
             L"together in one folder.");
        return false;
    }
    if (!fileExists(P.iso)) { fail(L"The game disc image was not found:\n" + P.iso.wstring()); return false; }
    if (!fileExists(P.elf)) { fail(L"The game file was not found:\n" + P.elf.wstring()); return false; }
    return true;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    Paths P;
    if (!resolvePaths(P)) return 1;

    if (envW(L"PS2X_EXEDIR").empty()) backupSave(P.gameDir);   // [savebackup] (only the real save, never a test folder)

    // the same as the .bat (an env var already set by the user wins)
    const std::wstring gameDir = P.gameDir.wstring(), iso = P.iso.wstring();
    const wchar_t *env[][2] = {
        {L"PS2X_EXEDIR", gameDir.c_str()}, {L"PS2X_NODEFAULTS", L"1"}, {L"PS2X_PRINTF", L"1"}, {L"PS2X_GPU", L"1"},
        {L"PS2X_ASYNC_KICK", L"1"},   // VIF1/VU1 on a worker thread
        {L"PS2X_FPSUNLOCK", L"0"},    // locked to 25 fps like the PS2
        {L"PS2X_CD_IMAGE", iso.c_str()},
    };
    for (auto &e : env)
        if (GetEnvironmentVariableW(e[0], nullptr, 0) == 0) SetEnvironmentVariableW(e[0], e[1]);

    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE err = CreateFileW(P.log.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE out = CreateFileW(P.outLog.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{}; si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in; si.hStdOutput = out; si.hStdError = err;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + P.runner.wstring() + L"\" \"" + P.elf.wstring() + L"\"";
    const BOOL ok = CreateProcessW(P.runner.c_str(), &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, P.runnerDir.c_str(), &si, &pi);
    if (!ok) { fail(L"Could not start the game (error " + std::to_wstring(GetLastError()) + L")."); return 1; }
    CloseHandle(pi.hThread);
    if (err != INVALID_HANDLE_VALUE) CloseHandle(err);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (in != INVALID_HANDLE_VALUE) CloseHandle(in);

    // [crashnotify] the newest report before the game starts, to tell a new one apart
    const fs::path crashes = P.gameDir / L"crashes";
    auto newestReport = [&]() {
        std::error_code ec;
        fs::path best;
        for (auto &e : fs::directory_iterator(crashes, ec))
            if (e.is_directory() && (best.empty() || e.path().filename() > best.filename())) best = e.path();
        return best;
    };
    const fs::path before = newestReport();
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    const fs::path report = newestReport();
    if (report.empty() || report == before) return 0;   // no new report
    if (FILE *f = _wfopen(P.log.c_str(), L"rb"))
    {   // the end of the log (the [wincrash] stack, the last things the game did)
        _fseeki64(f, 0, SEEK_END);
        const long long size = _ftelli64(f);
        const long long keep = size > 262144 ? 262144 : size;
        _fseeki64(f, size - keep, SEEK_SET);
        std::vector<char> buf(size_t(keep) + 1, 0);
        const size_t n = fread(buf.data(), 1, size_t(keep), f);
        fclose(f);
        if (FILE *o = _wfopen((report / L"run_tail.log").c_str(), L"wb")) { fwrite(buf.data(), 1, n, o); fclose(o); }
    }
    if (code == 0) return 0;   // closed normally: a soft report (e.g. a hang that recovered), kept but not announced
    wchar_t hex[16]; swprintf(hex, 16, L"%08lX", code);
    const std::wstring msg = std::wstring(L"The game closed unexpectedly (code 0x") + hex + L").\n\nA crash report was saved in:\n" +
                             report.wstring() + L"\n\nPlease pass this folder on so the problem can be found. Your save was backed up before the game started.";
    MessageBoxW(nullptr, msg.c_str(), kTitle, MB_OK | MB_ICONWARNING);
    return 0;
}
