// [mods] File-replacement mods for BT4's headerless archives.
//
// BT4 stores no index inside DATA/PZS4US0-3.AFS: entry i of archive k starts at (sum of the sector sizes of entries
// 0..i-1) * 2048, and the sizes live in BIN/DBZ4.BIN (one record per archive: 282-byte header -- u32 0, u32 record
// length, u32 count, u32 count, name at +0x10 -- then count u16 sector sizes; records 16-byte aligned).
// A replacement of a different size therefore moves every later entry. Nothing is rebuilt on disk: when an enabled
// mod replaces entries, the CD layer serves a VIRTUAL archive (original bytes, mod files spliced in, each padded to
// whole sectors) and fioOpen serves a patched DBZ4.BIN with the new sizes (written to mods\.active\DBZ4.BIN).
//
// Layout (<exe dir>\mods\, or PS2X_MODSDIR):
//   modlist.txt            load order, one mod folder per line, TOP WINS; "-Name" = disabled; '#' comments.
//                          Missing file -> every mod folder is enabled, in name order.
//   <Mod>\mod.txt          optional metadata for the launcher (name= author= version= description=)
//   <Mod>\PZS4US<k>\<index>[<non-digit>...]   replacement for entry <index> of archive k ("12345.pak",
//                          "12345_goku.pak"). Index = position in the archive, the order AFS tools list entries in.
//   .active\report.txt     what the last run applied, and conflicts (two mods replacing the same entry).
//   <Mod>\...\*<n>).unk   [bt3mod] a BT3 mod file, named by its BT3 PZS3US1.AFS index n (anywhere in the folder).
//                          BT3 keeps 10 entries per character from 1423 (costumes 1-4, damaged 1-4, animations,
//                          special moves); costume PAKs are converted to BT4's split 52-slot layout at start-up
//                          (cached in .active\bt3\) and replace the character's BT4 outfit. Animations, special
//                          moves and per-character move data stay BT4's (BT4 changed the movesets).
//                          BT3 had no SSG/SSB Goku, so BT3 mods put those costumes on another Goku form; a Goku
//                          costume in a folder (or file) named "...God..." (not "God Dragon") or "...Blue..." is
//                          also put on BT4's SSG / SSB Goku, on the outfit most of the mod uses: transforming
//                          keeps the outfit number, so the mod's base form then transforms into its God/Blue.
//   <Mod>\mod.txt keys    place.<folder>=<BT4 char id>/<outfit>[,...] | none
//                          BT3 files in that folder go there instead of their BT3 slot (none = not used).
//                          transform.<BT4 char id>=<t1>,<t2>,<t3>,<t4>  that character's 4 transformation targets
//                          (BT4 char ids, "-" = none, "id:cost" sets the skill-stock cost, default 3); BT4 keeps
//                          them in PZS4US2 750+id, part 1, bytes 152-155 (costs 156-159). By convention the
//                          last one is "back to normal".
//                          hud.<folder>=yes  that folder's HUD face (BT3 costume part 0) replaces the face of each BT4
//                          character its costumes land on -- BT4 has one face per character (750+id part 0), so
//                          every outfit of that character shows it. Off unless set.
//                          lightning.<BT4 char id>=yes (SS2 class) | ss3 | <class>  SS2/SS3-style lightning around that
//                          character (all its outfits): sets its Saiyan class, 750+id part 1 byte 0.
// PS2X_MODS=0 disables everything. Only the extracted-files layout is modded (not a configured ISO image).
#include "ps2_mods.h"
#include "ps2_runtime.h"   // before <windows.h>: its macros clash with runtime names

#include "runtime/ps2_fileread.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <array>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

// [costumebuf] PS2X_COSTUMEBUF=0 keeps the stock 0xE7000-byte outfit buffers; default 1 MB
// [roster] mod.txt roster.<box id>=<char id>[,...]: extra forms appended to a character-select box (game_overrides.cpp
// hooks the roster builder func_25F610). Filled once by the mod scan.
static std::vector<std::pair<uint32_t, uint32_t>> g_rosterAdds;
// [outfitentry] mod.txt outfitentry.<char id>=<entry>,<template char>: where a new character's outfit 1 lives in
// PZS4US2 (normal at entry, damaged at entry + 1) -- ids 231-235 have no outfit entries of their own -- and whose
// outfit 1 is the conversion template. game_overrides.cpp redirects the game's outfit lookup (func_26BDFC) there.
// mod.txt newbox.<char id>=1: turn one of the empty "? ? ?" select boxes into a box for that character.
struct OutfitEntry { uint32_t entry; uint32_t templ; };
static std::map<uint32_t, OutfitEntry> g_outfitEntry;
static std::vector<uint32_t> g_newBoxes;
bool ps2xModOutfitEntry(uint32_t id, uint32_t &entry, uint32_t &templ)
{
    auto it = g_outfitEntry.find(id);
    if (it == g_outfitEntry.end()) return false;
    entry = it->second.entry; templ = it->second.templ; return true;
}
const std::vector<uint32_t> &ps2xModNewBoxes() { return g_newBoxes; }
// [cinematic] mod.txt cinematic.<entry>><new entry>=<char>/<outfit>[/damaged]: the cinematic outfit copy is built into
// <new entry> and the game reads it instead of <entry> while a character with an outfitentry is fighting (the copy
// belongs to the template character, e.g. Goku (End)'s Spirit Bomb #3773, whose own copy stays stock).
static std::map<uint32_t, uint32_t> g_cineRedirect;
uint32_t ps2xModCineRedirect(uint32_t entry) { auto it = g_cineRedirect.find(entry); return it == g_cineRedirect.end() ? 0u : it->second; }
const std::vector<std::pair<uint32_t, uint32_t>> &ps2xModRosterAdds() { return g_rosterAdds; }
// [roster] mod.txt name.<char id>= / form.<char id>= / outfit.<char id>=<text>: the select screen's character, form and
// outfit plates for a roster-added character (ids 161-164 share their text lines with Random / Locked). Top of the load order wins.
// [itemcap] mod.txt stats.<char id>=attack,defense,ki,speed,blast: item points (5 = one item level) a character always
// has on top of its items (game_overrides.cpp adds them where the fight clamps the member's bonus levels).
static std::map<uint32_t, std::array<int, 5>> g_charStats;
const int *ps2xModCharStats(uint32_t id)
{
    auto it = g_charStats.find(id);
    return it == g_charStats.end() ? nullptr : it->second.data();
}
// [voice] mod.txt voice.<char id>=<char id>: whose voice lines (pre-fight talk, win lines, select screen) a new character
// speaks -- the new ids have no line files of their own (game_overrides.cpp swaps the id in Voice_PlayCharaEx).
static std::map<uint32_t, uint32_t> g_voiceDonor;
uint32_t ps2xModVoiceDonor(uint32_t id) { auto it = g_voiceDonor.find(id); return it == g_voiceDonor.end() ? id : it->second; }
// [roster] mod.txt randomportrait=<PZS4US1 entry>: a copy of the stock select portrait #1980, which character 161 shares
// with the Random box; game_overrides.cpp reads it while a cursor is on Random
static uint32_t g_randomPortrait = 0;
uint32_t ps2xModRandomPortrait() { return g_randomPortrait; }
static std::map<std::pair<uint32_t, int>, std::string> g_slotNames;
const std::string *ps2xModSlotName(uint32_t id, int kind)   // 0 name, 1 form, 2 outfit
{
    auto it = g_slotNames.find({id, kind});
    return it == g_slotNames.end() ? nullptr : &it->second;
}
uint32_t ps2xCostumeBufBytes()
{
    static const uint32_t s_b = [](){ const char *v = std::getenv("PS2X_COSTUMEBUF"); return (v && v[0] == '0') ? 0xE7000u : 0x100000u; }();
    return s_b;
}

namespace fs = std::filesystem;

namespace ps2x_mods
{
    struct ModAfs
    {
        fs::path orig;                    // the original archive on disk
        uint64_t origSize = 0;
        std::vector<uint64_t> origOff;    // count+1 byte offsets in the original
        std::vector<uint64_t> newOff;     // count+1 byte offsets in the virtual archive
        std::vector<int32_t> src;         // per entry: -1 = original, else index into files
        std::vector<fs::path> files;
        std::vector<uint64_t> fileSize;
        uint64_t virtSize = 0;
    };

    namespace
    {
        constexpr uint64_t kSector = 2048;
        constexpr size_t kHeader = 282;
        constexpr int kArchives = 4;

        std::once_flag g_once;
        bool g_active = false;
        fs::path g_patchedBin;
        std::shared_ptr<ModAfs> g_arch[kArchives];

        std::string u8(const fs::path &p) { const auto s = p.u8string(); return std::string(s.begin(), s.end()); }
        std::string lower(std::string s) { for (auto &c : s) c = (char)std::tolower((unsigned char)c); return s; }

        fs::path exeDir()
        {
            if (const char *e = std::getenv("PS2X_EXEDIR"); e && e[0]) return fs::path(e);
#if defined(_WIN32)
            wchar_t p[MAX_PATH] = {0};
            GetModuleFileNameW(nullptr, p, MAX_PATH);
            return fs::path(p).parent_path();
#else
            return fs::current_path();
#endif
        }

        fs::path modsDir()
        {
            if (const char *e = std::getenv("PS2X_MODSDIR"); e && e[0]) return fs::path(e);
            return exeDir() / "mods";
        }

        // A child of `dir` whose name matches case-insensitively (the data tree keeps the disc's upper case).
        fs::path childNoCase(const fs::path &dir, const std::string &name)
        {
            std::error_code ec;
            if (fs::exists(dir / name, ec)) return dir / name;
            const std::string want = lower(name);
            for (const auto &d : fs::directory_iterator(dir, ec))
                if (lower(d.path().filename().string()) == want) return d.path();
            return {};
        }

        fs::path dataRoot()
        {
            const PS2Runtime::IoPaths &p = PS2Runtime::getIoPaths();
            for (const fs::path &r : {p.cdRoot, p.elfDirectory})
                if (!r.empty() && !childNoCase(r, "BIN").empty()) return r;
            return {};
        }

        bool readFile(const fs::path &p, uint64_t off, uint8_t *dst, size_t n)
        {
            const long long got = ps2xReadFileAt(p, off, dst, n);   // [fileread] the file stays open between reads
            if (got < 0) return false;
            if ((size_t)got < n) std::memset(dst + got, 0, n - (size_t)got);
            return true;
        }


        // ---- [bt3mod] BT3 -> BT4 costume conversion -------------------------------------------------------------
        // BT4's costume PAK = BT3's slots 0..51 minus what BT4 moved out (no BT4 PAK uses these slots), plus slot 25 =
        // the damaged costume's textures (BT3 slot 11 of the damaged PAK) and 51 = its slot 12. Model/texture slots
        // 1-24 come from the mod only; effect slots 26-50 the mod lacks keep the BT4 outfit's data (SS3 Goku's 28-31
        // hung the fight load when empty). Above BT4's largest costume (943,168 bytes of slots) the damaged copies are
        // dropped -- a 1,031,888-byte SS3 conversion hung the fight load.
        constexpr uint32_t kBt3Base = 1423, kBt4LimitStock = 943168;
        // [costumebuf] the game's per-fighter outfit buffers are 0xE7000 bytes (the largest stock outfit is 942,944);
        // game_overrides.cpp enlarges them to ps2xCostumeBufBytes(), so converted outfits may grow by the same amount
        // (keeps the damaged textures of high-detail BT3 costumes, which transformations use)
        const uint32_t kBt4Limit = kBt4LimitStock + (ps2xCostumeBufBytes() - 0xE7000u);
        bool bt3NeverSlot(int j)
        {
            static const int never[] = {0, 10, 16, 17, 18, 19, 20, 21, 23, 24, 26, 27, 35, 39, 40, 41, 42, 43, 44, 47, 48, 49};
            for (int v : never) if (v == j) return true;
            return false;
        }
        // first BT4 PZS4US2 entry of a character's outfits (outfit k = start + k - 1), verified in game by loading
        // the outfit and logging the read. Characters not listed are skipped (reported).
        int bt4OutfitStart(uint32_t chara)
        {
            if (auto it = g_outfitEntry.find(chara); it != g_outfitEntry.end()) return (int)it->second.entry;   // [outfitentry]
            switch (chara)
            {
            case 0: return 2250;  case 1: return 2262;  case 2: return 2278;  case 3: return 2294;  case 4: return 2306;
            case 5: return 2318;  case 6: return 2330;  case 10: return 2366; case 13: return 2394;
            case 59: return 2792; case 122: return 3168;   // SSG Goku, SSB Goku (BT4 only)
            case 161: return 3382; case 162: return 3384; case 163: return 3386; case 164: return 3388;   // Password slots
            default: return -1;
            }
        }
        // outfits per character: a block is N normal + N damaged entries (boundaries where the skeleton changes,
        // checked against the damaged entries the game reads: Goku (End) o4 -> #2303, SS4 GT o4 -> #2373)
        uint32_t bt4OutfitCount(uint32_t chara)
        {
            if (g_outfitEntry.count(chara)) return 1;   // [outfitentry]
            switch (chara) { case 0: return 6; case 1: case 2: return 8; case 3: case 4: case 5: case 6: return 6;
                             case 10: return 4; case 13: return 5; case 59: case 122: return 6;
                             case 161: case 162: case 163: case 164: return 1; default: return 0; }
        }
        const char *bt4CharName(uint32_t c)
        {
            static const char *n[] = {"Goku (Early)", "Goku (Mid)", "SS1 Goku (Mid)", "Goku (End)", "SS1 Goku (End)", "SS2 Goku (End)",
                                      "SS3 Goku (End)", "Goku (GT)", "SS1 Goku (GT)", "SS3 Goku (GT)", "SS4 Goku (GT)", "Kid Goku", "Great Ape", "Kid Gohan"};
            if (c == 59) return "SSG Goku";
            if (c == 122) return "SSB Goku";
            return c < sizeof(n) / sizeof(n[0]) ? n[c] : "character";
        }
        // BT4-only Goku form a BT3 Goku costume is meant to be, from its folder or file name: 59 SSG, 122 SSB, else -1
        int bt4OnlyGokuForm(const fs::path &file, uint32_t chara)
        {
            if (chara > 10) return -1;
            for (const fs::path &p : {file.parent_path().filename(), file.stem()})
            {
                const std::string n = lower(u8(p));
                if (n.find("blue") != std::string::npos) return 122;
                if (n.find("god") != std::string::npos && n.find("dragon") == std::string::npos) return 59;
            }
            return -1;
        }
        // mod.txt as key -> value (keys lower case)
        std::map<std::string, std::string> modKeys(const fs::path &dir)
        {
            std::map<std::string, std::string> out;
            std::ifstream in(dir / "mod.txt", std::ios::binary);
            std::string line;
            while (std::getline(in, line))
            {
                if (line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF) line.erase(0, 3);
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                const size_t eq = line.find('=');
                if (eq == std::string::npos || line[0] == '#') continue;
                std::string k = lower(line.substr(0, eq)), v = line.substr(eq + 1);
                while (!k.empty() && k.back() == ' ') k.pop_back();
                while (!v.empty() && v.front() == ' ') v.erase(0, 1);
                out[k] = v;
            }
            return out;
        }
        std::vector<std::string> splitList(const std::string &v)
        {
            std::vector<std::string> out; std::string cur;
            for (char c : v + ",")
                if (c == ',') { while (!cur.empty() && cur.back() == ' ') cur.pop_back(); out.push_back(cur); cur.clear(); }
                else if (c != ' ' || !cur.empty()) cur += c;
            return out;
        }
        bool pakParse(const std::vector<uint8_t> &d, std::vector<std::pair<uint32_t, uint32_t>> &out)
        {
            out.clear();
            if (d.size() < 8) return false;
            uint32_t n = 0; std::memcpy(&n, d.data(), 4);
            if (n == 0 || n > 4096 || 4ull + 4ull * (n + 1) > d.size()) return false;
            std::vector<uint32_t> o(n + 1);
            std::memcpy(o.data(), d.data() + 4, 4ull * (n + 1));
            for (uint32_t i = 0; i < n; ++i)
            {   // mod tools often leave the last slot unpadded: offsets past the end are clamped to the file
                const uint32_t a = std::min<uint32_t>(o[i], (uint32_t)d.size()), b = std::min<uint32_t>(o[i + 1], (uint32_t)d.size());
                if (b < a) return false;
                out.push_back({a, b - a});
            }
            return true;
        }
        std::vector<uint8_t> pakBuild(const std::vector<std::vector<uint8_t>> &subs)
        {
            const uint32_t n = (uint32_t)subs.size();
            const uint32_t hdr = (4 + 4 * (n + 1) + 15) & ~15u;
            std::vector<uint8_t> out(hdr, 0);
            std::vector<uint32_t> offs;
            for (const auto &s : subs)
            {
                offs.push_back((uint32_t)out.size());
                out.insert(out.end(), s.begin(), s.end());
                out.resize((out.size() + 15) & ~size_t(15), 0);
            }
            offs.push_back((uint32_t)out.size());
            std::memcpy(out.data(), &n, 4);
            std::memcpy(out.data() + 4, offs.data(), 4ull * offs.size());
            out.resize((out.size() + kSector - 1) / kSector * kSector, 0);
            return out;
        }
        std::vector<uint8_t> slurp(const fs::path &p)
        {
            std::ifstream in(p, std::ios::binary);
            return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
        }
        // empty result = not convertible (reason in why)
        std::vector<uint8_t> bt3Convert(const std::vector<uint8_t> &normal, const std::vector<uint8_t> *damaged,
                                        const std::vector<uint8_t> &orig, std::string &why, bool &droppedDmg)
        {
            std::vector<std::pair<uint32_t, uint32_t>> a, dm, o;
            if (!pakParse(normal, a) || a.size() != 252) { why = "not a BT3 costume PAK (252 slots)"; return {}; }
            if (!pakParse(orig, o) || o.size() != 52) { why = "the BT4 outfit is not a 52-slot costume PAK"; return {}; }
            const bool haveDmg = damaged && pakParse(*damaged, dm) && dm.size() == 252;
            auto slot = [](const std::vector<uint8_t> &d, const std::pair<uint32_t, uint32_t> &e) {
                return std::vector<uint8_t>(d.begin() + e.first, d.begin() + e.first + e.second); };
            std::vector<std::vector<uint8_t>> out(52);
            for (int j = 0; j < 52; ++j)
            {
                if (bt3NeverSlot(j)) continue;
                if (j <= 24 || a[j].second) out[j] = slot(normal, a[j]);
                else out[j] = slot(orig, o[j]);
            }
            const auto &dsrc = haveDmg ? *damaged : normal;
            const auto &dpak = haveDmg ? dm : a;
            out[25] = o[25].second ? slot(dsrc, dpak[11]) : std::vector<uint8_t>();
            out[51] = o[51].second ? slot(dsrc, dpak[12]) : std::vector<uint8_t>();
            size_t total = 0; for (const auto &v : out) total += v.size();
            droppedDmg = false;
            if (total > kBt4Limit && (!out[25].empty() || !out[51].empty())) { out[25].clear(); out[51].clear(); droppedDmg = true; }
            return pakBuild(out);
        }

        // [slot] the Password slots' outfit entries are 2 KB placeholders, not costume PAKs: convert onto SS4 Goku (GT)'s
        // outfit 1 (#2366 normal / #2370 damaged) instead, which has the slot layout BT4 expects
        std::vector<uint8_t> outfitTemplate(const fs::path &afs, const std::vector<uint64_t> &offs, uint32_t e, bool damaged)
        {
            std::vector<uint8_t> d((size_t)(offs[e + 1] - offs[e]));
            readFile(afs, offs[e], d.data(), d.size());
            std::vector<std::pair<uint32_t, uint32_t>> pp;
            if (pakParse(d, pp) && pp.size() == 52) return d;
            uint32_t t = damaged ? 2370u : 2366u;
            for (const auto &[c, oe] : g_outfitEntry)   // [outfitentry] the template character's outfit 1
                if (e == oe.entry || e == oe.entry + 1u)
                    if (const int st = bt4OutfitStart(oe.templ); st >= 0 && !g_outfitEntry.count(oe.templ))
                        t = (uint32_t)st + (damaged ? bt4OutfitCount(oe.templ) : 0u);
            d.assign((size_t)(offs[t + 1] - offs[t]), 0);
            readFile(afs, offs[t], d.data(), d.size());
            return d;
        }

        struct Mod { std::string name; fs::path dir; };

        std::vector<Mod> enabledMods(const fs::path &root, std::ostringstream &rep)
        {
            std::vector<Mod> out;
            std::error_code ec;
            const fs::path list = root / "modlist.txt";
            if (fs::exists(list, ec))
            {
                std::ifstream in(list);
                for (std::string ln; std::getline(in, ln);)
                {
                    while (!ln.empty() && (ln.back() == '\r' || ln.back() == ' ' || ln.back() == '\t')) ln.pop_back();
                    size_t b = ln.find_first_not_of(" \t");
                    if (b == std::string::npos || ln[b] == '#') continue;
                    ln = ln.substr(b);
                    if (ln[0] == '-') { rep << "disabled: " << ln.substr(1) << "\n"; continue; }
                    if (ln[0] == '+') ln = ln.substr(1);
                    const fs::path d = root / fs::path(std::u8string(ln.begin(), ln.end()));
                    if (fs::is_directory(d, ec)) out.push_back({ln, d});
                    else rep << "missing:  " << ln << " (listed in modlist.txt, no such folder)\n";
                }
            }
            else
            {
                for (const auto &d : fs::directory_iterator(root, ec))
                {
                    const std::string n = u8(d.path().filename());
                    if (d.is_directory() && !n.empty() && n[0] != '.') out.push_back({n, d.path()});
                }
                std::sort(out.begin(), out.end(), [](const Mod &a, const Mod &b) { return lower(a.name) < lower(b.name); });
            }
            // mods made for an emulator (mod.txt type=external, e.g. a PCSX2 trainer the Mod Manager keeps with its web
            // page) run outside this port: nothing of theirs is loaded, and their folders are not searched
            // mod.txt requires=<mod folder>: a variant built on another mod (e.g. Battle Damage+ on Shader HQ Auto's
            // colours) loads only while that mod is on
            std::vector<Mod> kept;
            std::vector<std::string> needs(out.size());
            for (size_t i = 0; i < out.size(); ++i)
            {
                Mod &m = out[i];
                bool external = false;
                std::ifstream in(m.dir / "mod.txt");
                for (std::string ln; in && std::getline(in, ln);)
                {
                    while (!ln.empty() && (ln.back() == '\r' || ln.back() == ' ')) ln.pop_back();
                    if (lower(ln) == "type=external") external = true;
                    if (lower(ln.substr(0, 9)) == "requires=") needs[i] = ln.substr(9);
                }
                if (external) { rep << "external: " << m.name << " (made for an emulator: not loaded by this port)\n"; needs[i] = "\x01"; }
            }
            for (size_t i = 0; i < out.size(); ++i)
            {
                if (needs[i] == "\x01") continue;
                if (!needs[i].empty())
                {
                    bool on = false;
                    for (size_t j = 0; j < out.size(); ++j) if (j != i && lower(out[j].name) == lower(needs[i]) && needs[j] != "\x01") on = true;
                    if (!on) { rep << "skipped:  " << out[i].name << " (needs the mod \"" << needs[i] << "\", which is off)\n"; continue; }
                }
                kept.push_back(std::move(out[i]));
            }
            return kept;
        }

        void init()
        {
            if (const char *e = std::getenv("PS2X_MODS"); e && e[0] == '0') return;
            const fs::path root = modsDir();
            std::error_code ec;
            if (!fs::is_directory(root, ec)) return;
            std::ostringstream rep;
            const std::vector<Mod> mods = enabledMods(root, rep);
            if (mods.empty()) return;

            const fs::path data = dataRoot();
            const fs::path binDir = data.empty() ? fs::path() : childNoCase(data, "BIN");
            const fs::path binPath = binDir.empty() ? fs::path() : childNoCase(binDir, "DBZ4.BIN");
            const fs::path dataDir = data.empty() ? fs::path() : childNoCase(data, "DATA");
            if (binPath.empty() || dataDir.empty())
            {
                std::cerr << "[mods] " << mods.size() << " mod(s) enabled, but no BIN/DBZ4.BIN + DATA/ under the game data -- mods need the extracted BT4 files" << std::endl;
                return;
            }
            std::vector<uint8_t> bin;
            {
                std::ifstream in(binPath, std::ios::binary);
                bin.assign(std::istreambuf_iterator<char>(in), {});
            }

            // Parse DBZ4.BIN: record offset and entry count per archive.
            size_t recOff[kArchives] = {}; uint32_t count[kArchives] = {}; std::string recName[kArchives];
            {
                size_t o = 0; int k = 0;
                while (k < kArchives && o + kHeader <= bin.size())
                {
                    uint32_t len = 0, n = 0;
                    std::memcpy(&len, &bin[o + 4], 4); std::memcpy(&n, &bin[o + 8], 4);
                    if (len < kHeader || o + kHeader + 2ull * n > bin.size()) break;
                    recOff[k] = o; count[k] = n;
                    recName[k] = std::string(reinterpret_cast<const char *>(&bin[o + 16]), strnlen(reinterpret_cast<const char *>(&bin[o + 16]), 32));
                    o = (o + len + 15) & ~size_t(15); ++k;
                }
                if (k != kArchives)
                {
                    std::cerr << "[mods] BIN/DBZ4.BIN has an unexpected layout (" << k << " archive records) -- mods disabled" << std::endl;
                    return;
                }
            }

            // Collect replacements: archive -> index -> (mod, file). Top of the list wins.
            struct Pick { size_t mod; fs::path file; };
            std::map<uint32_t, Pick> pick[kArchives];
            uint32_t conflicts = 0, rejected = 0;
            rep << "load order (top wins):\n";
            for (size_t m = 0; m < mods.size(); ++m)
            {
                rep << "  " << (m + 1) << ". " << mods[m].name << "\n";
                for (int k = 0; k < kArchives; ++k)
                {
                    const fs::path sub = childNoCase(mods[m].dir, "PZS4US" + std::to_string(k));
                    if (sub.empty()) continue;
                    for (const auto &f : fs::directory_iterator(sub, ec))
                    {
                        if (!f.is_regular_file()) continue;
                        const std::string n = u8(f.path().filename());
                        size_t d = 0; while (d < n.size() && std::isdigit((unsigned char)n[d])) ++d;
                        if (d == 0 || d > 9) { rep << "  skipped " << mods[m].name << "/" << u8(sub.filename()) << "/" << n << ": name must start with the entry index\n"; ++rejected; continue; }
                        const uint32_t idx = (uint32_t)std::strtoul(n.substr(0, d).c_str(), nullptr, 10);
                        const uint64_t sz = f.file_size(ec);
                        if (idx >= count[k] || sz == 0 || (sz + kSector - 1) / kSector > 0xFFFFu)
                        {
                            rep << "  skipped " << mods[m].name << "/" << u8(sub.filename()) << "/" << n << ": "
                                << (idx >= count[k] ? "index past the archive's " + std::to_string(count[k]) + " entries" : sz == 0 ? std::string("empty file") : std::string("larger than 128 MB")) << "\n";
                            ++rejected; continue;
                        }
                        auto it = pick[k].find(idx);
                        if (it != pick[k].end())
                        {
                            if (it->second.mod != m)
                            {
                                ++conflicts;
                                rep << "  conflict PZS4US" << k << " #" << idx << ": " << mods[it->second.mod].name << " wins over " << mods[m].name << "\n";
                            }
                            continue;
                        }
                        pick[k][idx] = {m, f.path()};
                    }
                }
            }


            // [bt3mod] BT3 mod files: "<anything>(<BT3 index>).unk" anywhere in an enabled mod's folder
            {
                const fs::path afs2 = childNoCase(dataDir, recName[2].empty() ? "PZS4US2.AFS" : recName[2]);
                std::vector<uint64_t> off2(count[2] + 1, 0);
                for (uint32_t i = 0; i < count[2]; ++i)
                {
                    uint16_t sz = 0; std::memcpy(&sz, &bin[recOff[2] + kHeader + 2ull * i], 2);
                    off2[i + 1] = off2[i] + sz * kSector;
                }
                // [outfitentry] [roster] outfit locations and new boxes from every enabled mod, before any conversion
                for (const Mod &md : mods)
                    for (const auto &[k, v] : modKeys(md.dir))
                    {
                        if (k.compare(0, 12, "outfitentry.") == 0)
                        {
                            const uint32_t id = (uint32_t)std::atoi(k.c_str() + 12);
                            const std::vector<std::string> t = splitList(v);
                            if (t.size() >= 1 && id < 256)
                            {
                                const OutfitEntry oe{(uint32_t)std::atoi(t[0].c_str()), t.size() >= 2 ? (uint32_t)std::atoi(t[1].c_str()) : 10u};
                                if (oe.entry + 1u < count[2] && g_outfitEntry.emplace(id, oe).second)
                                    rep << "  outfitentry: character " << id << " outfit 1 at PZS4US2 #" << oe.entry << " (template " << oe.templ << ")\n";
                            }
                        }
                        else if (k.compare(0, 7, "newbox.") == 0)
                        {
                            const uint32_t id = (uint32_t)std::atoi(k.c_str() + 7);
                            if (std::find(g_newBoxes.begin(), g_newBoxes.end(), id) == g_newBoxes.end()) { g_newBoxes.push_back(id); rep << "  newbox: box " << id << "\n"; }
                        }
                    }
                const fs::path cache = root / ".active" / "bt3";
                bool cacheMade = false;
                for (size_t m = 0; m < mods.size() && !afs2.empty(); ++m)
                {
                    std::map<uint32_t, fs::path> byIdx;   // BT3 index -> file
                    for (auto it = fs::recursive_directory_iterator(mods[m].dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
                    {
                        if (!it->is_regular_file()) continue;
                        const std::string n = u8(it->path().filename());
                        const std::string ln = lower(n);
                        if (ln.size() < 8 || ln.compare(ln.size() - 5, 5, ").unk") != 0) continue;
                        // the digits right before ").unk" (the opening bracket is often a Shift-JIS character)
                        size_t b = n.size() - 5;
                        while (b > 0 && std::isdigit((unsigned char)n[b - 1])) --b;
                        const std::string num = n.substr(b, n.size() - 5 - b);
                        if (num.empty() || num.size() > 6) continue;
                        byIdx[(uint32_t)std::strtoul(num.c_str(), nullptr, 10)] = it->path();
                    }
                    ec.clear();
                    // the outfit most of the mod's own costumes use (for the SSG/SSB copies)
                    int perOutfit[4] = {0, 0, 0, 0};
                    for (const auto &[idx, file] : byIdx)
                        if (idx >= kBt3Base && (idx - kBt3Base) % 10 < 4 && bt4OnlyGokuForm(file, (idx - kBt3Base) / 10) < 0)
                            ++perOutfit[(idx - kBt3Base) % 10];
                    const uint32_t mainOutfit = (uint32_t)(std::max_element(perOutfit, perOutfit + 4) - perOutfit);
                    struct Job { uint32_t idx; fs::path file; uint32_t chara, slotj; };
                    std::vector<Job> jobs;
                    const auto keys = modKeys(mods[m].dir);
                    for (const auto &[k, v] : keys)   // [roster]
                    {
                        if (k.compare(0, 6, "stats.") == 0)   // [itemcap]
                        {
                            const uint32_t id = (uint32_t)std::atoi(k.c_str() + 6);
                            std::array<int, 5> st{};
                            size_t i = 0;
                            for (const std::string &t : splitList(v)) if (i < st.size()) st[i++] = std::atoi(t.c_str());
                            if (g_charStats.emplace(id, st).second)
                                rep << "  stats of character " << id << ": attack " << st[0] << ", defense " << st[1] << ", ki " << st[2]
                                    << ", speed " << st[3] << ", blast " << st[4] << "\n";
                            continue;
                        }
                        if (k == "randomportrait")   // [roster]
                        {
                            const uint32_t e = (uint32_t)std::atoi(v.c_str());
                            if (!g_randomPortrait && e) { g_randomPortrait = e; rep << "  Random box portrait: PZS4US1 #" << e << "\n"; }
                            continue;
                        }
                        if (k.compare(0, 6, "voice.") == 0)   // [voice]
                        {
                            const uint32_t id = (uint32_t)std::atoi(k.c_str() + 6), d = (uint32_t)std::atoi(v.c_str());
                            if (id < 256 && d < 256 && g_voiceDonor.emplace(id, d).second) rep << "  voice of character " << id << ": character " << d << "'s lines\n";
                            continue;
                        }
                        const int kind = k.compare(0, 5, "name.") == 0 ? 0 : k.compare(0, 5, "form.") == 0 ? 1 : k.compare(0, 7, "outfit.") == 0 ? 2 : -1;
                        if (kind >= 0)
                        {
                            const size_t dot = k.find('.');
                            const uint32_t id = (uint32_t)std::atoi(k.c_str() + dot + 1);
                            if (g_slotNames.emplace(std::make_pair(id, kind), v).second)
                                rep << "  " << k.substr(0, dot) << " of character " << id << ": " << v << "\n";
                            continue;
                        }
                        if (k.compare(0, 7, "roster.") != 0) continue;
                        const uint32_t box = (uint32_t)std::atoi(k.c_str() + 7);
                        for (const std::string &t : splitList(v))
                            if (!t.empty()) { g_rosterAdds.push_back({box, (uint32_t)std::atoi(t.c_str())}); rep << "  roster: box " << box << " + character " << std::atoi(t.c_str()) << "\n"; }
                    }
                    // PZS4US2 750+id (HUD face + parameters), patched in memory by hud.* and transform.*, written once below
                    std::map<uint32_t, std::vector<uint8_t>> hudPak;
                    auto hudFor = [&](uint32_t c) -> std::vector<uint8_t> * {
                        const uint32_t e = 750 + c;
                        if (e >= count[2]) return nullptr;
                        auto it = hudPak.find(e);
                        if (it == hudPak.end())
                        {
                            std::vector<uint8_t> d((size_t)(off2[e + 1] - off2[e]));
                            readFile(afs2, off2[e], d.data(), d.size());
                            std::vector<std::pair<uint32_t, uint32_t>> parts;
                            if (!pakParse(d, parts) || parts.size() < 2 || parts[1].second < 160) return nullptr;
                            it = hudPak.emplace(e, std::move(d)).first;
                        }
                        return &it->second;
                    };
                    for (const auto &[idx, file] : byIdx)
                    {
                        const std::string shown = mods[m].name + "/" + u8(file.filename());
                        if (idx < kBt3Base) { rep << "  bt3: skipped " << shown << " (index " << idx << " is not a character file)\n"; ++rejected; continue; }
                        const uint32_t chara = (idx - kBt3Base) / 10, slotj = (idx - kBt3Base) % 10;
                        if (slotj >= 4 && slotj <= 7) continue;   // damaged costume: used with its normal costume below
                        if (slotj >= 8) { rep << "  bt3: kept BT4's " << (slotj == 8 ? "animations" : "special moves") << " for " << bt4CharName(chara) << " (" << shown << " not converted)\n"; continue; }
                        // mod.txt place.<folder>= overrides where this folder's costumes go
                        if (auto pl = keys.find("place." + lower(u8(file.parent_path().filename()))); pl != keys.end())
                        {
                            if (lower(pl->second) == "none") { rep << "  bt3: left out " << shown << " (mod.txt " << pl->first << "=none)\n"; continue; }
                            for (const std::string &t : splitList(pl->second))
                            {
                                const size_t sl = t.find('/');
                                const int c = std::atoi(t.c_str()), o = sl == std::string::npos ? (int)slotj + 1 : std::atoi(t.c_str() + sl + 1);
                                if (t.empty() || c < 0 || o < 1 || o > 4) { rep << "  bt3: bad mod.txt value " << pl->first << "=" << pl->second << "\n"; ++rejected; continue; }
                                jobs.push_back({idx, file, (uint32_t)c, (uint32_t)(o - 1)});
                            }
                            continue;
                        }
                        jobs.push_back({idx, file, chara, slotj});
                        const int form = bt4OnlyGokuForm(file, chara);
                        if (form >= 0) jobs.push_back({idx, file, (uint32_t)form, mainOutfit});
                    }
                    for (const Job &job : jobs)
                    {
                        const uint32_t idx = job.idx, chara = job.chara, slotj = job.slotj;
                        const fs::path &file = job.file;
                        const std::string shown = mods[m].name + "/" + u8(file.filename());
                        const int start = bt4OutfitStart(chara);
                        if (start < 0) { rep << "  bt3: skipped " << shown << " (BT3 character " << chara << " has no known BT4 outfit entries yet)\n"; ++rejected; continue; }
                        const uint32_t target = (uint32_t)start + slotj;
                        if (target >= count[2]) { ++rejected; continue; }
                        auto prev = pick[2].find(target);
                        if (prev != pick[2].end())
                        {
                            // plain files were collected first: a plain file of a mod LOWER in the list loses to this one
                            if (prev->second.mod == m) continue;
                            const bool mine = m < prev->second.mod;
                            ++conflicts; rep << "  conflict PZS4US2 #" << target << ": " << mods[mine ? m : prev->second.mod].name << " wins over " << mods[mine ? prev->second.mod : m].name << "\n";
                            if (!mine) continue;
                        }
                        const std::vector<uint8_t> orig = outfitTemplate(afs2, off2, target, false);
                        const std::vector<uint8_t> normal = slurp(file);
                        std::vector<uint8_t> dmgData; const std::vector<uint8_t> *dmg = nullptr;
                        if (auto d = byIdx.find(idx + 4); d != byIdx.end()) { dmgData = slurp(d->second); dmg = &dmgData; }
                        std::string why; bool dropped = false;
                        const std::vector<uint8_t> conv = bt3Convert(normal, dmg, orig, why, dropped);
                        if (conv.empty()) { rep << "  bt3: skipped " << shown << ": " << why << "\n"; ++rejected; continue; }
                        if (!cacheMade) { fs::create_directories(cache, ec); cacheMade = true; }
                        const fs::path outp = cache / (std::to_string(target) + "_" + std::to_string(m) + ".pak");
                        {
                            std::ofstream o(outp, std::ios::binary | std::ios::trunc);
                            o.write(reinterpret_cast<const char *>(conv.data()), (std::streamsize)conv.size());
                        }
                        pick[2][target] = {m, outp};
                        rep << "  bt3: " << shown << " -> " << bt4CharName(chara) << " outfit " << (slotj + 1) << " (PZS4US2 #" << target << ")"
                            << (dmg ? "" : ", no damaged costume in the mod") << (dropped ? ", damaged textures dropped (memory limit)" : "") << "\n";
                        // [bt3dmg] BT4 loads a SEPARATE damaged-outfit entry once the clothes tear: a character's block is its
                        // N normal outfits then N damaged ones (same 52-slot layout, damaged textures in slot 11, slot 25
                        // empty). Without it the mod's look reverts to the stock damaged costume mid-fight.
                        if (dmg)
                        {
                            const uint32_t n = bt4OutfitCount(chara);
                            const uint32_t dt = (uint32_t)start + n + slotj;
                            auto dprev = pick[2].find(dt);
                            const bool dlose = dprev != pick[2].end() && dprev->second.mod != m && dprev->second.mod < m;
                            if (n == 0 || slotj >= n || dt >= count[2] || dlose || (dprev != pick[2].end() && dprev->second.mod == m)) {}
                            else
                            {
                                const std::vector<uint8_t> dorig = outfitTemplate(afs2, off2, dt, true);
                                std::string dwhy; bool ddrop = false;
                                const std::vector<uint8_t> dconv = bt3Convert(dmgData, nullptr, dorig, dwhy, ddrop);
                                if (dconv.empty()) rep << "  bt3: damaged outfit for " << shown << " skipped: " << dwhy << "\n";
                                else
                                {
                                    const fs::path dp = cache / (std::to_string(dt) + "_" + std::to_string(m) + ".pak");
                                    { std::ofstream o(dp, std::ios::binary | std::ios::trunc); o.write(reinterpret_cast<const char *>(dconv.data()), (std::streamsize)dconv.size()); }
                                    pick[2][dt] = {m, dp};
                                    rep << "  bt3: damaged costume -> " << bt4CharName(chara) << " outfit " << (slotj + 1) << " damaged (PZS4US2 #" << dt << ")\n";
                                }
                            }
                        }
                        // mod.txt hud.<folder>=yes: this costume's face becomes the character's HUD face
                        if (auto hk = keys.find("hud." + lower(u8(file.parent_path().filename()))); hk != keys.end() && lower(hk->second) == "yes")
                        {
                            std::vector<std::pair<uint32_t, uint32_t>> mp, hp;
                            std::vector<uint8_t> *hud = hudFor(chara);
                            if (hud && pakParse(normal, mp) && !mp.empty() && pakParse(*hud, hp) && !hp.empty() && mp[0].second == hp[0].second && mp[0].second > 0)
                            {
                                std::memcpy(hud->data() + hp[0].first, normal.data() + mp[0].first, mp[0].second);
                                rep << "  hud: " << shown << " face -> " << bt4CharName(chara) << " (all outfits, PZS4US2 #" << (750 + chara) << ")\n";
                            }
                            else rep << "  hud: " << shown << ": face not used (its size differs from BT4's)\n";
                        }
                    }
                    // mod.txt transform.<id>= : edit a character's transformation list (any mod, BT3 or not)
                    for (const auto &[k, v] : keys)
                    {
                        if (k.compare(0, 10, "transform.") != 0) continue;
                        const uint32_t c = (uint32_t)std::atoi(k.c_str() + 10);
                        const uint32_t target = 750 + c;
                        const std::vector<std::string> t = splitList(v);
                        if (target >= count[2] || t.size() != 4) { rep << "  transform: bad mod.txt line " << k << "=" << v << " (needs 4 targets)\n"; ++rejected; continue; }
                        std::vector<uint8_t> *hud = hudFor(c);
                        std::vector<std::pair<uint32_t, uint32_t>> parts;
                        if (!hud || !pakParse(*hud, parts) || parts.size() < 2 || parts[1].second < 160) { rep << "  transform: PZS4US2 #" << target << " is not a character HUD PAK, " << k << " ignored\n"; ++rejected; continue; }
                        std::vector<uint8_t> &d = *hud;
                        uint8_t *list = d.data() + parts[1].first + 152, *cost = list + 4;
                        for (int i = 0; i < 4; ++i)
                        {
                            const std::string &e = t[i];
                            if (e.empty() || e == "-") { list[i] = 0xFF; continue; }
                            list[i] = (uint8_t)std::atoi(e.c_str());
                            const size_t colon = e.find(':');
                            if (colon != std::string::npos) cost[i] = (uint8_t)std::atoi(e.c_str() + colon + 1);
                            else if (cost[i] == 0 || cost[i] == 0xFF) cost[i] = 3;
                        }
                        rep << "  transform: " << bt4CharName(c) << " (id " << c << ") -> " << v << " (PZS4US2 #" << target << ")\n";
                    }
                    // mod.txt lightning.<id>=yes|ss3|<n> : the character's Saiyan class (750+id part 1 byte 0: 1 base, 2 SS1,
                    // 4 SS2, 8 SS3, 16 SS4). Classes 4 and 8 get the SS2/SS3 lightning (Super Perfect Cell is class 4).
                    for (const auto &[k, v] : keys)
                    {
                        if (k.compare(0, 10, "lightning.") != 0) continue;
                        const uint32_t c = (uint32_t)std::atoi(k.c_str() + 10);
                        const std::string lv = lower(v);
                        const int cls = lv == "yes" ? 4 : lv == "ss3" ? 8 : std::atoi(lv.c_str());
                        std::vector<uint8_t> *hud = hudFor(c);
                        std::vector<std::pair<uint32_t, uint32_t>> parts;
                        if (cls <= 0 || cls > 255 || !hud || !pakParse(*hud, parts) || parts.size() < 2 || parts[1].second < 160) { rep << "  lightning: bad mod.txt line " << k << "=" << v << "\n"; ++rejected; continue; }
                        (*hud)[parts[1].first] = (uint8_t)cls;
                        rep << "  lightning: " << bt4CharName(c) << " (id " << c << ") Saiyan class -> " << cls << " (PZS4US2 #" << (750 + c) << ")\n";
                    }
                    // mod.txt cinematic.<entry>=<char>/<outfit>[/damaged] : some ultimates (Goku (End)'s Spirit Bomb: #3773 normal,
                    // #3782 damaged) swap in their OWN copy of an outfit for the cinematic. Rebuild that copy from this mod's
                    // converted outfit: model, skeleton and textures from the conversion, cinematic-only slots (0 face, 28) kept.
                    for (const auto &[k, v] : keys)
                    {
                        if (k.compare(0, 10, "cinematic.") != 0) continue;
                        const uint32_t ce = (uint32_t)std::atoi(k.c_str() + 10);
                        const size_t gt = k.find('>');
                        const uint32_t dst = gt == std::string::npos ? ce : (uint32_t)std::atoi(k.c_str() + gt + 1);
                        std::vector<std::string> t; { std::string cur; for (char ch : v) { if (ch == '/') { t.push_back(cur); cur.clear(); } else cur += ch; } t.push_back(cur); }
                        const int c = t.size() >= 2 ? std::atoi(t[0].c_str()) : -1, o = t.size() >= 2 ? std::atoi(t[1].c_str()) : 0;
                        const bool dmgd = t.size() >= 3 && lower(t[2]) == "damaged";
                        const int st = c >= 0 ? bt4OutfitStart((uint32_t)c) : -1;
                        const uint32_t n = c >= 0 ? bt4OutfitCount((uint32_t)c) : 0;
                        if (st < 0 || n == 0 || o < 1 || (uint32_t)o > n || ce >= count[2] || dst >= count[2]) { rep << "  cinematic: bad mod.txt line " << k << "=" << v << "\n"; ++rejected; continue; }
                        const uint32_t src = (uint32_t)st + (dmgd ? n : 0) + (uint32_t)(o - 1);
                        auto sp = pick[2].find(src);
                        if (sp == pick[2].end() || sp->second.mod != m) { rep << "  cinematic: " << k << " needs " << bt4CharName((uint32_t)c) << " outfit " << o << (dmgd ? " damaged" : "") << " from this mod (PZS4US2 #" << src << ")\n"; ++rejected; continue; }
                        if (auto prev = pick[2].find(dst); prev != pick[2].end() && prev->second.mod <= m) continue;
                        const std::vector<uint8_t> conv = slurp(sp->second.file);
                        std::vector<uint8_t> corig((size_t)(off2[ce + 1] - off2[ce]));
                        readFile(afs2, off2[ce], corig.data(), corig.size());
                        std::vector<std::pair<uint32_t, uint32_t>> cp, op;
                        if (!pakParse(conv, cp) || cp.size() != 52 || !pakParse(corig, op) || op.size() != 52) { rep << "  cinematic: PZS4US2 #" << ce << " is not a 52-slot outfit copy\n"; ++rejected; continue; }
                        std::vector<std::vector<uint8_t>> out(52);
                        for (int j = 0; j < 52; ++j)
                        {
                            const bool keepOrig = !cp[j].second && (j == 0 || j == 28);   // cinematic-only slots
                            const auto &e2 = keepOrig ? op[j] : cp[j];
                            const auto &d2 = keepOrig ? corig : conv;
                            out[j].assign(d2.begin() + e2.first, d2.begin() + e2.first + e2.second);
                        }
                        size_t ctotal = 0; for (const auto &x : out) ctotal += x.size();
                        // which buffer the cinematic copy is loaded into is unknown: stay within the stock outfit size
                        if (ctotal > kBt4LimitStock) { out[25].clear(); out[51].clear(); }
                        const std::vector<uint8_t> built = pakBuild(out);
                        if (!cacheMade) { fs::create_directories(cache, ec); cacheMade = true; }
                        const fs::path cpth = cache / (std::to_string(dst) + "_" + std::to_string(m) + ".pak");
                        { std::ofstream of(cpth, std::ios::binary | std::ios::trunc); of.write(reinterpret_cast<const char *>(built.data()), (std::streamsize)built.size()); }
                        pick[2][dst] = {m, cpth};
                        if (dst != ce) g_cineRedirect[ce] = dst;
                        rep << "  cinematic: PZS4US2 #" << dst << (dst != ce ? " (for #" + std::to_string(ce) + ")" : std::string()) << " <- " << bt4CharName((uint32_t)c) << " outfit " << o << (dmgd ? " damaged" : "") << " (" << built.size() << " bytes)\n";
                    }
                    for (auto &[e, d] : hudPak)   // the patched HUD PAKs (faces and/or transformation lists)
                    {
                        if (auto prev = pick[2].find(e); prev != pick[2].end())
                        {
                            const bool mine = m < prev->second.mod;   // top of the list wins (see above)
                            ++conflicts; rep << "  conflict PZS4US2 #" << e << ": " << mods[mine ? m : prev->second.mod].name << " wins over " << mods[mine ? prev->second.mod : m].name << " (hud/transform)\n";
                            if (!mine) continue;
                        }
                        if (!cacheMade) { fs::create_directories(cache, ec); cacheMade = true; }
                        const fs::path outp = cache / (std::to_string(e) + "_" + std::to_string(m) + ".pak");
                        {
                            std::ofstream o(outp, std::ios::binary | std::ios::trunc);
                            o.write(reinterpret_cast<const char *>(d.data()), (std::streamsize)d.size());
                        }
                        pick[2][e] = {m, outp};
                    }
                }
            }

            size_t total = 0;
            for (int k = 0; k < kArchives; ++k)
            {
                if (pick[k].empty()) continue;
                const fs::path afs = childNoCase(dataDir, recName[k].empty() ? "PZS4US" + std::to_string(k) + ".AFS" : recName[k]);
                if (afs.empty()) { rep << "  PZS4US" << k << ".AFS not found -- its replacements are ignored\n"; continue; }
                auto a = std::make_shared<ModAfs>();
                a->orig = afs; a->origSize = fs::file_size(afs, ec);
                const uint32_t n = count[k];
                a->origOff.resize(n + 1); a->newOff.resize(n + 1); a->src.assign(n, -1);
                uint8_t *sizes = &bin[recOff[k] + kHeader];
                for (uint32_t i = 0; i < n; ++i)
                {
                    uint16_t s = 0; std::memcpy(&s, sizes + 2ull * i, 2);
                    a->origOff[i + 1] = a->origOff[i] + s * kSector;
                }
                for (const auto &[idx, p] : pick[k])
                {
                    a->src[idx] = (int32_t)a->files.size();
                    a->files.push_back(p.file);
                    a->fileSize.push_back(fs::file_size(p.file, ec));
                    const uint16_t s = (uint16_t)((a->fileSize.back() + kSector - 1) / kSector);
                    std::memcpy(sizes + 2ull * idx, &s, 2);   // the patched DBZ4.BIN
                    rep << "  PZS4US" << k << " #" << idx << " <- " << mods[p.mod].name << "/" << u8(p.file.filename())
                        << " (" << a->fileSize.back() << " bytes, was " << (a->origOff[idx + 1] - a->origOff[idx]) << ")\n";
                }
                for (uint32_t i = 0; i < n; ++i)
                {
                    const uint64_t len = a->src[i] < 0 ? a->origOff[i + 1] - a->origOff[i]
                                                       : (a->fileSize[a->src[i]] + kSector - 1) / kSector * kSector;
                    a->newOff[i + 1] = a->newOff[i] + len;
                }
                const uint64_t tail = a->origSize > a->origOff[n] ? a->origSize - a->origOff[n] : 0;   // disc padding after the last entry
                a->virtSize = a->newOff[n] + tail;
                if (a->virtSize > 0xFFFFFFFFull) { rep << "  PZS4US" << k << ": modded archive would pass 4 GB -- ignored\n"; continue; }
                total += pick[k].size();
                g_arch[k] = a;
            }
            if (total == 0) { std::cerr << "[mods] " << mods.size() << " mod(s) enabled, nothing to replace (see mods\\.active\\report.txt)" << std::endl; }

            const fs::path active = root / ".active";
            fs::create_directories(active, ec);
            if (total)
            {
                g_patchedBin = active / "DBZ4.BIN";
                std::ofstream out(g_patchedBin, std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char *>(bin.data()), (std::streamsize)bin.size());
                g_active = out.good();
                if (!g_active) { for (auto &a : g_arch) a.reset(); std::cerr << "[mods] cannot write " << u8(g_patchedBin) << " -- mods disabled" << std::endl; }
            }
            {
                std::ofstream r(active / "report.txt", std::ios::trunc);
                r << "entries replaced: " << total << ", conflicts: " << conflicts << ", skipped files: " << rejected << "\n" << rep.str();
            }
            {   // [modfiles] every replaced entry, generated ones included (BT3 conversions, cinematic copies, patched
                // HUD/params paks): "<archive> <index> <file>" per line, for the decomp PC build (port/src/plat_mods.c)
                std::ofstream fl(active / "files.txt", std::ios::trunc);
                fl << "# written by the BT4 recompiled build: archive index file (the winning file of every replaced entry)\n";
                for (int k = 0; k < kArchives; ++k)
                    for (const auto &[idx, p] : pick[k])
                        fl << k << ' ' << idx << ' ' << u8(fs::absolute(p.file, ec)) << '\n';
            }
            if (g_active)
                std::cerr << "[mods] " << mods.size() << " mod(s), " << total << " archive entries replaced, " << conflicts
                          << " conflict(s), " << rejected << " skipped file(s) -- mods\\.active\\report.txt" << std::endl;
        }

        void ensureInit() { std::call_once(g_once, init); }
    }

    std::shared_ptr<const ModAfs> archiveFor(const fs::path &hostAfs)
    {
        ensureInit();
        if (!g_active) return nullptr;
        std::error_code ec;
        for (const auto &a : g_arch)
            if (a && fs::equivalent(a->orig, hostAfs, ec)) return a;
        return nullptr;
    }

    uint64_t virtualSize(const ModAfs &a) { return a.virtSize; }

    bool read(const ModAfs &a, uint64_t offset, uint8_t *dst, size_t n)
    {
        const size_t count = a.src.size();
        size_t done = 0;
        bool ok = true;
        while (done < n)
        {
            const uint64_t pos = offset + done;
            if (pos >= a.newOff[count])
            {   // tail padding (original bytes after the last entry), then zeros
                const uint64_t rel = pos - a.newOff[count];
                const uint64_t from = a.origOff[count] + rel;
                const size_t chunk = n - done;
                if (from < a.origSize) ok &= readFile(a.orig, from, dst + done, chunk);   // zero-pads past EOF
                else std::memset(dst + done, 0, chunk);
                break;
            }
            const size_t i = (size_t)(std::upper_bound(a.newOff.begin(), a.newOff.end(), pos) - a.newOff.begin()) - 1;
            const uint64_t rel = pos - a.newOff[i];
            const size_t chunk = (size_t)std::min<uint64_t>(n - done, a.newOff[i + 1] - pos);
            if (a.src[i] < 0)
                ok &= readFile(a.orig, a.origOff[i] + rel, dst + done, chunk);
            else
            {
                const uint64_t fsz = a.fileSize[a.src[i]];
                const size_t have = rel < fsz ? (size_t)std::min<uint64_t>(chunk, fsz - rel) : 0;
                if (have) ok &= readFile(a.files[a.src[i]], rel, dst + done, have);
                if (have < chunk) std::memset(dst + done + have, 0, chunk - have);   // sector padding
            }
            done += chunk;
        }
        return ok;
    }

    std::string redirectHostFile(const std::string &hostPath)
    {
        const std::string l = lower(hostPath);
        if (l.size() < 8 || l.compare(l.size() - 8, 8, "dbz4.bin") != 0) return hostPath;
        ensureInit();
        if (!g_active) return hostPath;
        static bool s_logged = false;
        if (!s_logged) { s_logged = true; std::cerr << "[mods] serving the patched DBZ4.BIN from " << u8(g_patchedBin) << std::endl; }
        return g_patchedBin.string();
    }
}
