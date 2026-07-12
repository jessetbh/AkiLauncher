#include "games.h"

#include <windows.h>

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

std::vector<GameEntry> build_game_list(const std::filesystem::path& depotRoot) {
    std::vector<GameEntry> games = {
        { L"WCW vs. nWo World Tour",   L"WcwNwoWorldTour",  L"build-msvc\\WCWRecompiled.exe",     L"wcw.z64",     false, true },
        { L"WCW/nWo Revenge",          L"WcwRevengeRecomp", L"build-msvc\\RevengeRecompiled.exe", L"revenge.z64", false, true },
        { L"WWF WrestleMania 2000",    L"Wm2kRecomp",       L"build-msvc\\Wm2kRecompiled.exe",    L"wm2k.z64",    true,  true },
        // Future recomp projects - paths follow the family convention so these
        // activate on their own once the repos exist and build.
        { L"Virtual Pro Wrestling 64", L"Vpw64Recomp",      L"build-msvc\\Vpw64Recompiled.exe",   L"vpw64.z64",   true,  true, true },
        { L"Virtual Pro Wrestling 2",  L"Vpw2Recomp",       L"build-msvc\\Vpw2Recompiled.exe",    L"vpw2.z64",    true,  true, true },
        { L"WWF No Mercy",             L"NoMercyRecomp",    L"build-msvc\\NoMercyRecompiled.exe", L"nomercy.z64", true,  true, true },
    };
    std::error_code ec;
    for (auto& g : games) {
        g.repoPath = depotRoot / g.repoDir;
        g.exePath = g.repoPath / g.exeRel;
        g.romPath = g.repoPath / g.romFile;
        g.exeFound = fs::is_regular_file(g.exePath, ec);
        g.romFound = fs::is_regular_file(g.romPath, ec);
    }
    return games;
}
