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

std::vector<GameEntry> build_game_list(const std::filesystem::path& depotRoot) {
    // Ordered by original release date. JP-only titles carry their Japanese
    // release date; the rest are North America dates.
    std::vector<GameEntry> games;

    // Mod lists: notable full conversions / major overhauls per base game,
    // researched 2026-07-11 (romhacking.net, LaunchBox DB, archive.org's
    // wwfnmprojectarchive, vpw.ajworld.net). None playable in the ports yet.
    games.push_back({ L"WCW vs. nWo World Tour", L"December 2, 1997", L"North America",
                      L"WcwNwoWorldTour", L"build-msvc\\WCWRecompiled.exe", L"wcw.z64",
                      L"worldtour", false, true, false,
                      { { L"La Fin Du Tour - Virtual Pro-Wrestling Alpha",
                          L"first-ever World Tour hack, built with VPW Studio", false } } });
    games.back().githubRepo = L"jessetbh/WCWvsNWOWorldTourRecomp";
    games.back().romSha1 = L"5AD2D8359058C8BB71F08E3D3433B7A50D3BB645";

    games.push_back({ L"Virtual Pro Wrestling 64", L"December 19, 1997", L"Japan",
                      L"Vpw64Recomp", L"build-msvc\\Vpw64Recompiled.exe", L"vpw64.z64",
                      L"vpw64", true, true, true,
                      { { L"English Translation", L"fan translation patch", false } } });
    games.back().githubRepo = L"jessetbh/VPW64Recomp";
    games.back().romSha1 = L"F9E9FA2ED819C3A39DB5CB6AFECA186F021DB5ED";

    games.push_back({ L"WCW/nWo Revenge", L"October 26, 1998", L"North America",
                      L"WcwRevengeRecomp", L"build-msvc\\RevengeRecompiled.exe", L"revenge.z64",
                      L"revenge", false, true, false,
                      { { L"Virtual Pro-Wrestling Salvo", L"RagDas - 11 roster additions incl. Ric Flair & nWo Sting", false },
                        { L"WCW/nWo Revenge Redux", L"Retro Randy - roster/visual overhaul", false } } });
    games.back().githubRepo = L"jessetbh/WCWnWoRevengeRecomp";
    games.back().romSha1 = L"E1711A2511394B9357B5F1AC8CA5CC17BD674836";

    games.push_back({ L"WWF WrestleMania 2000", L"October 31, 1999", L"North America",
                      L"Wm2kRecomp", L"build-msvc\\Wm2kRecompiled.exe", L"wm2k.z64",
                      L"wm2k", true, true, false,
                      { { L"SUMMIT: Virtual Pro-Wrestling Gamma", L"mixes VPW2 & Revenge into WM2K, restores combo/shoot systems", false },
                        { L"WCW Saturday Night", L"G.M. Spectre - early-90s WCW conversion", false } } });
    // no public repo yet - set githubRepo when the Wm2k recomp publishes releases
    games.back().romSha1 = L"D7D1FAD473FEF9B61FE5F8273C975EE7C603A51B";

    games.push_back({ L"Virtual Pro Wrestling 2", L"January 28, 2000", L"Japan",
                      L"Vpw2Recomp", L"build-msvc\\Vpw2Recompiled.exe", L"vpw2.z64",
                      L"vpw2", true, true, true,
                      { { L"VPW2 freem Edition", L"freem - roster overhaul + English translation", false },
                        { L"English Translation", L"S.K. Stylez & Zoinkity fan translation", false } } });

    GameEntry noMercy{ L"WWF No Mercy", L"November 17, 2000", L"North America",
                       L"NoMercyRecomp", L"build-msvc\\NoMercyRecompiled.exe", L"nomercy.z64",
                       L"nomercy", true, true, true, {} };
    noMercy.mods = {
        { L"WWF No Mercy Plus", L"Retro Randy - the update AKI never shipped", false },
        { L"Showdown 64", L"RandyManFoo - huge multi-promotion compilation", false },
        { L"WWF Legends: Challenge 64", L"Iccotracs - 80s/90s WWF legends", false },
        { L"WCW Feel the Bang", L"GenHex/OSR/WLF - the Revenge sequel we never got", false },
        { L"TNA vs ROH", L"Alanchiz - full retexture, ~280 costumes", false },
        { L"ECW Born to be Wired", L"Retro Randy - ECW total conversion", false },
        { L"WCCW 64", L"G.M. Spectre - World Class Championship Wrestling", false },
        { L"WarZone Attitude", L"Nekomancer - Attitude Era", false },
        { L"ECW Barely Legal", L"The_Juice", false },
        { L"TNA Impact (2008)", L"The_Juice", false },
        { L"Lucha Underground", L"Killacam", false },
        { L"INVASION", L"loco & nWo Mark", false },
    };
    games.push_back(std::move(noMercy));

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
