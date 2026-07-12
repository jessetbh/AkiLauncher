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

    games.push_back({ L"Virtual Pro Wrestling 64", L"December 19, 1997", L"Japan",
                      L"Vpw64Recomp", L"build-msvc\\Vpw64Recompiled.exe", L"vpw64.z64",
                      L"vpw64", true, true, true,
                      { { L"English Translation", L"fan translation patch", false } } });

    games.push_back({ L"WCW/nWo Revenge", L"October 26, 1998", L"North America",
                      L"WcwRevengeRecomp", L"build-msvc\\RevengeRecompiled.exe", L"revenge.z64",
                      L"revenge", false, true, false,
                      { { L"Virtual Pro-Wrestling Salvo", L"RagDas - 11 roster additions incl. Ric Flair & nWo Sting", false },
                        { L"WCW/nWo Revenge Redux", L"Retro Randy - roster/visual overhaul", false } } });

    games.push_back({ L"WWF WrestleMania 2000", L"October 31, 1999", L"North America",
                      L"Wm2kRecomp", L"build-msvc\\Wm2kRecompiled.exe", L"wm2k.z64",
                      L"wm2k", true, true, false,
                      { { L"SUMMIT: Virtual Pro-Wrestling Gamma", L"mixes VPW2 & Revenge into WM2K, restores combo/shoot systems", false },
                        { L"WCW Saturday Night", L"G.M. Spectre - early-90s WCW conversion", false } } });

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
    for (auto& g : games) {
        g.repoPath = depotRoot / g.repoDir;
        g.exePath = g.repoPath / g.exeRel;
        g.romPath = g.repoPath / g.romFile;
        g.exeFound = fs::is_regular_file(g.exePath, ec);
        g.romFound = fs::is_regular_file(g.romPath, ec);
    }
    return games;
}
