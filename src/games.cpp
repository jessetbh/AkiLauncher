#include "games.h"

#include "conversions.h"
#include "installer.h"
#include "launch.h"
#include "rom_rules.h"

#include <windows.h>

#include <fstream>
#include <map>
#include <tuple>

namespace fs = std::filesystem;

static bool looks_like_depot(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p / L"WcwNwoWorldTour", ec);
}

std::filesystem::path detect_depot_root() {
    wchar_t buf[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"AKI_DEPOT_ROOT", buf, MAX_PATH) > 0 && looks_like_depot(buf))
        return fs::path(buf);

    wchar_t exeBuf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
    fs::path p = fs::path(exeBuf).parent_path();
    for (int i = 0; i < 4 && !p.empty(); ++i) {
        if (looks_like_depot(p)) return p;
        p = p.parent_path();
    }
    std::error_code ec;
    fs::path cwd = fs::current_path(ec);
    for (int i = 0; i < 4 && !cwd.empty(); ++i) {
        if (looks_like_depot(cwd)) return cwd;
        cwd = cwd.parent_path();
    }
    return {};
}

std::filesystem::path app_root() {
    wchar_t exeBuf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
    fs::path dir = fs::path(exeBuf).parent_path();
    std::error_code ec;
    if (fs::is_directory(dir / L"assets", ec)) return dir;
    if (fs::is_directory(dir.parent_path() / L"assets", ec)) return dir.parent_path();
    return dir;
}

std::filesystem::path games_root() {
    wchar_t buf[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"AKI_GAMES_ROOT", buf, MAX_PATH) > 0) return fs::path(buf);
    return app_root() / L"games";
}

std::vector<GameEntry> build_game_list(const std::filesystem::path& depotRoot) {
    // Ordered by original release date. JP-only titles carry their Japanese
    // release date; the rest are North America dates.
    std::vector<GameEntry> games;

    games.push_back({ L"WCW vs. nWo World Tour", L"December 2, 1997", L"North America",
                      L"WcwNwoWorldTour", L"build-msvc\\WCWRecompiled.exe", L"wcw.z64",
                      L"worldtour", true, false });
    games.back().githubRepo = L"jessetbh/WCWvsNWOWorldTourRecomp";
    games.back().romStoredFile = L"wcw.nwo.worldtour.us.z64";
    games.back().romSha1 = L"5AD2D8359058C8BB71F08E3D3433B7A50D3BB645";
    games.back().romNote = L"North American (NTSC-U) release";

    games.push_back({ L"Virtual Pro Wrestling 64", L"December 19, 1997", L"Japan",
                      L"Vpw64Recomp", L"build-msvc\\Vpw64Recompiled.exe", L"vpw64.z64",
                      L"vpw64", true, false });
    games.back().githubRepo = L"jessetbh/VPW64Recomp";
    games.back().romStoredFile = L"vpw.vpw64.jp.z64";
    games.back().romSha1 = L"F9E9FA2ED819C3A39DB5CB6AFECA186F021DB5ED";
    games.back().romNote = L"Japanese release (Japan-only; translations go in as patch mods)";
    games.back().romHint = L"Translated ROM? Select the clean Japanese ROM here and add the translation"
                           L" as a patch mod (steps in the game's README FAQ).";

    games.push_back({ L"WCW/nWo Revenge", L"October 26, 1998", L"North America",
                      L"WcwRevengeRecomp", L"build-msvc\\RevengeRecompiled.exe", L"revenge.z64",
                      L"revenge", true, false });
    games.back().githubRepo = L"jessetbh/WCWnWoRevengeRecomp";
    games.back().romStoredFile = L"wcw.nwo.revenge.us.z64";
    games.back().romSha1 = L"E1711A2511394B9357B5F1AC8CA5CC17BD674836";
    games.back().romNote = L"North American (NTSC-U) release";

    games.push_back({ L"WWF WrestleMania 2000", L"October 31, 1999", L"North America",
                      L"Wm2kRecomp", L"build-msvc\\Wm2kRecompiled.exe", L"wm2k.z64",
                      L"wm2k", true, false });
    games.back().githubRepo = L"jessetbh/WWFWrestleMania2000Recomp";
    games.back().romStoredFile = L"wwf.wm2k.us.z64";
    games.back().romSha1 = L"442D417A52ED672CA1A47E7261A5414DEBB1E27A"; // retail USA (aki-roadmap#124)
    games.back().romNote = L"North American (NTSC-U) retail release (the \"V1.2\" file is a fan hack and won't match)";

    games.push_back({ L"Virtual Pro Wrestling 2", L"January 28, 2000", L"Japan",
                      L"Vpw2Recomp", L"build-msvc\\Vpw2Recompiled.exe", L"vpw2.z64",
                      L"vpw2", true, false });
    games.back().githubRepo = L"jessetbh/VPW2Recomp";
    games.back().romStoredFile = L"vpw.vpw2.jp.z64";
    games.back().romSha1 = L"82DD25A044689EAB57AB362FE10C0DA6388C217A";
    games.back().romNote = L"Japanese release (Japan-only; translations go in as patch mods)";
    games.back().romHint = L"Translated ROM? Select the clean Japanese ROM here and add the translation"
                           L" as a patch mod (steps in the game's README FAQ).";

    games.push_back({ L"WWF No Mercy", L"November 17, 2000", L"North America",
                      L"NoMercyRecomp", L"build-msvc\\NoMercyRecompiled.exe", L"nomercy.z64",
                      L"nomercy", true, false });
    games.back().githubRepo = L"jessetbh/WWFNoMercyRecomp";
    games.back().romStoredFile = L"wwf.nomercy.us.z64";
    games.back().romSha1 = L"91CEE3D096F4A76644D8B35B9AEAD6448909ABD1";
    games.back().romNote = L"North American (NTSC-U) REV 1 release - the later revision; Rev 0 won't match";

    std::error_code ec;
    fs::path gamesDir = games_root();
    for (auto& g : games) {
        g.installDir = gamesDir / g.artKey;
        g.repoPath = depotRoot / g.repoDir;
        g.exePath = g.repoPath / g.exeRel;
        g.romPath = g.repoPath / g.romFile;
        g.romRegistryPath = g.romPath;
        g.romInstallPath = g.romPath;
        g.romAutoPath = g.romPath;
        g.exeFound = !depotRoot.empty() && fs::is_regular_file(g.exePath, ec);
        g.romFound = !depotRoot.empty() && fs::is_regular_file(g.romPath, ec);

        // No depot build? Fall back to the in-app install dir (release zips are
        // flat: exe at the root, so cwd = installDir). Also point paths there
        // when there's no depot checkout at all, so download + ROM select know
        // where to land.
        if (!g.exeFound) {
            fs::path instExe = g.installDir / fs::path(g.exeRel).filename();
            bool instFound = fs::is_regular_file(instExe, ec);
            bool devCheckout = !depotRoot.empty() && fs::is_directory(g.repoPath, ec);
            if (instFound || !devCheckout) {
                g.repoPath = g.installDir;
                g.exePath = instExe;
                g.romPath = g.installDir / g.romFile;
                g.romRegistryPath = g.romPath;
                g.romInstallPath = g.romPath;
                g.romAutoPath = g.romPath;
                g.exeFound = instFound;
                g.romFound = fs::is_regular_file(g.romPath, ec);
            }
        }

        std::ifstream vf(g.installDir / L"version.txt");
        if (vf) {
            std::string tag;
            std::getline(vf, tag);
            while (!tag.empty() && (tag.back() == '\r' || tag.back() == '\n')) tag.pop_back();
            int n = MultiByteToWideChar(CP_UTF8, 0, tag.c_str(), -1, nullptr, 0);
            std::wstring w(n > 0 ? n - 1 : 0, 0);
            if (n > 1) MultiByteToWideChar(CP_UTF8, 0, tag.c_str(), -1, w.data(), n);
            g.installedTag = w;
        }
    }
    return games;
}

// A ROM is hashed once per launcher run, unless the file changes.
static std::wstring cached_sha1(const fs::path& p) {
    using Stamp = std::tuple<std::uintmax_t, fs::file_time_type>;
    static std::map<std::wstring, std::pair<Stamp, std::wstring>> cache;
    std::error_code ec;
    const Stamp stamp{fs::file_size(p, ec), fs::last_write_time(p, ec)};
    auto it = cache.find(p.wstring());
    if (!ec && it != cache.end() && it->second.first == stamp) return it->second.second;
    std::wstring sha1 = sha1_file(p);
    if (!ec) cache[p.wstring()] = {stamp, sha1};
    return sha1;
}

void resolve_rom(GameEntry& g, const std::wstring& romOverride) {
    using namespace rom_rules;
    g.romInstallPath = g.repoPath / g.romFile;

    std::vector<std::wstring> candidates = {g.romInstallPath.wstring()};
    if (!g.romStoredFile.empty()) candidates.push_back((game_data_dir(g) / g.romStoredFile).wstring());
    candidates.push_back(g.romRegistryPath.wstring());

    const RomResolution r = resolve_rom_choice(
        romOverride, candidates, g.romSha1,
        [](const std::wstring& p) {
            std::error_code ec;
            return fs::is_regular_file(p, ec);
        },
        [](const std::wstring& p) { return cached_sha1(p); });

    g.romPath = r.rom.path;
    g.romAutoPath = r.autoRom.path;
    g.romFound = r.rom.status != RomStatus::Missing;
    g.romWrong = r.rom.status == RomStatus::Wrong && !r.overrideUsed;
    g.romHaveSha1 = r.rom.status == RomStatus::Wrong ? r.rom.sha1 : L"";

    g.romNotice.clear();
    if (r.overrideMissing) {
        g.romNotice = L"The ROM path in Settings doesn't exist, so it's ignored: " + romOverride;
    } else if (r.overrideUsed && r.rom.status == RomStatus::Wrong) {
        g.romNotice = L"The ROM path in Settings isn't the " + g.romNote + L", so the game may refuse it (" +
                      g.romPath.filename().wstring() + L")";
    }

    logf(L"rom for %s: %s%s %s", g.artKey.c_str(),
         r.rom.status == RomStatus::Good ? L"good" : r.rom.status == RomStatus::Wrong ? L"WRONG" : L"MISSING",
         r.overrideUsed ? L" (rom= override)" : L"", g.romPath.c_str());
    if (r.overrideMissing) logf(L"override rom for %s ignored (missing): %s", g.artKey.c_str(), romOverride.c_str());
}

std::wstring rom_problem(const GameEntry& g) {
    if (!g.romFound) {
        return g.title + L" has no ROM yet. Select your own dump: " + g.romNote + L" (.z64)\nRequired SHA1: " + g.romSha1;
    }
    if (g.romWrong) {
        return g.romPath.wstring() + L" isn't the " + g.title + L" ROM this port needs: the " + g.romNote +
               L".\nRequired SHA1: " + g.romSha1 + L"\nThis file's SHA1: " +
               (g.romHaveSha1.empty() ? L"(couldn't read it)" : g.romHaveSha1);
    }
    return {};
}
