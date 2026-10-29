#include "games.h"

#include <windows.h>

#include <fstream>

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

fs::path game_data_dir(const GameEntry& g) {
    if (game_is_portable(g)) return g.repoPath;
    wchar_t lad[MAX_PATH]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH);
    return fs::path(lad) / fs::path(g.exeRel).stem();
}

bool game_is_portable(const GameEntry& g) {
    std::error_code ec;
    return !g.repoPath.empty() && fs::exists(g.repoPath / L"portable.txt", ec);
}

bool wants_portable_game(const GameEntry& g, bool launcherPortable, bool portableGames) {
    std::error_code ec;
    return launcherPortable && portableGames && !g.installDir.empty() && g.repoPath == g.installDir &&
           fs::is_directory(g.installDir, ec) && !game_is_portable(g);
}

int make_game_portable(const GameEntry& g, const fs::path& appDataDir) {
    std::error_code ec;
    {
        std::ofstream marker(g.repoPath / L"portable.txt", std::ios::binary | std::ios::app);
        if (!marker) return -1;
    }
    int copied = 0;
    if (!fs::is_directory(appDataDir, ec)) return 0;
    fs::recursive_directory_iterator it(appDataDir, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        fs::path rel = fs::relative(it->path(), appDataDir, ec);
        if (ec) break;
        std::wstring first = rel.begin()->wstring();
        std::error_code fec;
        // Texture dumps can be huge and are made again on demand; logs are per run.
        if (it->is_directory(fec)) {
            if (_wcsicmp(first.c_str(), L"texture_dump") == 0) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(fec) || _wcsicmp(it->path().extension().c_str(), L".log") == 0) continue;
        fs::path dest = g.repoPath / rel;
        if (fs::exists(dest, fec)) continue;  // never over anything the install already has
        fs::create_directories(dest.parent_path(), fec);
        fs::copy_file(it->path(), dest, fs::copy_options::skip_existing, fec);
        if (!fec) ++copied;
    }
    return copied;
}

std::vector<GameEntry> build_game_list(const std::filesystem::path& depotRoot) {
    // Ordered by original release date. JP-only titles carry their Japanese
    // release date; the rest are North America dates.
    std::vector<GameEntry> games;

    games.push_back({ L"WCW vs. nWo World Tour", L"December 2, 1997", L"North America",
                      L"WcwNwoWorldTour", L"build-msvc\\WCWRecompiled.exe", L"wcw.z64",
                      L"worldtour", true, false });
    games.back().githubRepo = L"jessetbh/WCWvsNWOWorldTourRecomp";
    games.back().romSha1 = L"5AD2D8359058C8BB71F08E3D3433B7A50D3BB645";
    games.back().romCode = L"NWNE";
    games.back().modGameId = L"wcwnwoworldtour";
    games.back().romVersion = 0;
    games.back().romNote = L"North American (NTSC-U) release";

    games.push_back({ L"Virtual Pro Wrestling 64", L"December 19, 1997", L"Japan",
                      L"Vpw64Recomp", L"build-msvc\\Vpw64Recompiled.exe", L"vpw64.z64",
                      L"vpw64", true, false });
    games.back().githubRepo = L"jessetbh/VPW64Recomp";
    games.back().romSha1 = L"F9E9FA2ED819C3A39DB5CB6AFECA186F021DB5ED";
    games.back().romCode = L"NVPJ";
    games.back().modGameId = L"vpw64";
    games.back().romVersion = 0;
    games.back().romNote = L"Japanese release (the game was Japan-only)";

    games.push_back({ L"WCW/nWo Revenge", L"October 26, 1998", L"North America",
                      L"WcwRevengeRecomp", L"build-msvc\\RevengeRecompiled.exe", L"revenge.z64",
                      L"revenge", true, false });
    games.back().githubRepo = L"jessetbh/WCWnWoRevengeRecomp";
    games.back().romSha1 = L"E1711A2511394B9357B5F1AC8CA5CC17BD674836";
    games.back().romCode = L"NW2E";
    games.back().modGameId = L"wcwnworevenge";
    games.back().romVersion = 0;
    games.back().romNote = L"North American (NTSC-U) release";

    games.push_back({ L"WWF WrestleMania 2000", L"October 31, 1999", L"North America",
                      L"Wm2kRecomp", L"build-msvc\\Wm2kRecompiled.exe", L"wm2k.z64",
                      L"wm2k", true, false });
    games.back().githubRepo = L"jessetbh/WWFWrestleMania2000Recomp";
    games.back().romSha1 = L"D7D1FAD473FEF9B61FE5F8273C975EE7C603A51B";
    games.back().romCode = L"NWXE";
    games.back().modGameId = L"wwfwm2k";
    games.back().romVersion = 0;
    games.back().romNote = L"North American (NTSC-U) release";

    games.push_back({ L"Virtual Pro Wrestling 2", L"January 28, 2000", L"Japan",
                      L"Vpw2Recomp", L"build-msvc\\Vpw2Recompiled.exe", L"vpw2.z64",
                      L"vpw2", true, false });
    games.back().githubRepo = L"jessetbh/VPW2Recomp";
    games.back().romSha1 = L"82DD25A044689EAB57AB362FE10C0DA6388C217A";
    games.back().romCode = L"NA2J";
    games.back().modGameId = L"vpw2";
    games.back().romVersion = 0;
    games.back().romNote = L"Japanese release (Japan-only; fan-translation patches won't match)";

    games.push_back({ L"WWF No Mercy", L"November 17, 2000", L"North America",
                      L"NoMercyRecomp", L"build-msvc\\NoMercyRecompiled.exe", L"nomercy.z64",
                      L"nomercy", true, false });
    games.back().githubRepo = L"jessetbh/WWFNoMercyRecomp";
    games.back().romSha1 = L"91CEE3D096F4A76644D8B35B9AEAD6448909ABD1";
    games.back().romCode = L"NW4E";
    games.back().modGameId = L"wwfnomercy";
    games.back().romVersion = 1;  // Rev 1
    games.back().romNote = L"North American (NTSC-U) REV 1 release - the later revision; Rev 0 won't match";

    std::error_code ec;
    fs::path gamesDir = games_root();
    for (auto& g : games) {
        g.installDir = gamesDir / g.artKey;
        g.repoPath = depotRoot / g.repoDir;
        g.exePath = g.repoPath / g.exeRel;
        g.romPath = g.repoPath / g.romFile;
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
