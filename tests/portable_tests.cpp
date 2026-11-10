// Checks for aki-roadmap#14 (a portable launcher starts its own installs
// portable): which games qualify, and the one-time copy of their AppData data.
#include "../src/games.h"
#include "check.h"

#include <fstream>
#include <string>

namespace fs = std::filesystem;

static void put(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
}

static std::string get(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void run_portable_tests(const fs::path& dir) {
    GameEntry inst;
    inst.exeRel = L"build-msvc\\NoMercyRecompiled.exe";
    inst.installDir = dir / L"games" / L"nomercy";
    inst.repoPath = inst.installDir;  // an in-app install: the launcher starts it there
    fs::create_directories(inst.installDir);

    GameEntry depot = inst;
    depot.repoPath = dir / L"depot" / L"NoMercyRecomp";  // a dev checkout
    fs::create_directories(depot.repoPath);

    // Which games qualify.
    CHECK(wants_portable_game(inst, true, true));
    CHECK(!wants_portable_game(inst, false, true));   // the launcher isn't portable
    CHECK(!wants_portable_game(inst, true, false));   // portable_games=0
    CHECK(!wants_portable_game(depot, true, true));   // depot checkouts are never changed
    GameEntry missing = inst;
    missing.installDir = missing.repoPath = dir / L"games" / L"notinstalled";
    CHECK(!wants_portable_game(missing, true, true));

    // aki-roadmap#276: an install into it carries it first; a sandboxed headless import never does.
    CHECK(carry_before_install(inst, true, true, false));
    CHECK(!carry_before_install(inst, true, true, true));
    CHECK(!carry_before_install(inst, false, true, false));
    CHECK(!carry_before_install(depot, true, true, false));

    // Not portable yet: the data folder is AppData.
    CHECK(!game_is_portable(inst));
    CHECK(game_data_dir(inst).filename() == L"NoMercyRecompiled" && game_data_dir(inst) != inst.repoPath);

    // The AppData side the game used so far, and something the install already has.
    fs::path appData = dir / L"appdata" / L"NoMercyRecompiled";
    put(appData / L"graphics.json", "{\"from\":\"appdata\"}");
    put(appData / L"saves" / L"slot1.bin", "save");
    put(appData / L"mods" / L"Pack.rtz", "pack");
    put(appData / L"mods.json", "{\"from\":\"appdata\"}");
    put(appData / L"NoMercyRecompiled.log", "old log");
    put(appData / L"texture_dump" / L"pack" / L"0123456789abcdef.png", "dump");
    put(inst.repoPath / L"mods.json", "{\"from\":\"install\"}");

    int n = make_game_portable(inst, appData);
    CHECK(n == 3);  // graphics.json, saves\slot1.bin, mods\Pack.rtz
    CHECK(game_is_portable(inst) && game_data_dir(inst) == inst.repoPath);
    CHECK(get(inst.repoPath / L"graphics.json") == "{\"from\":\"appdata\"}");
    CHECK(get(inst.repoPath / L"saves" / L"slot1.bin") == "save");
    CHECK(fs::exists(inst.repoPath / L"mods" / L"Pack.rtz"));
    CHECK(get(inst.repoPath / L"mods.json") == "{\"from\":\"install\"}");  // never overwritten
    CHECK(!fs::exists(inst.repoPath / L"NoMercyRecompiled.log"));
    CHECK(!fs::exists(inst.repoPath / L"texture_dump"));
    CHECK(get(appData / L"graphics.json") == "{\"from\":\"appdata\"}");  // AppData left as it was
    // Once portable, it no longer qualifies: the copy happens only once, and an install goes straight to the game folder.
    CHECK(!wants_portable_game(inst, true, true));
    CHECK(!carry_before_install(inst, true, true, false));

    // A copy that fails leaves the game on AppData (no portable.txt), and a later try finishes it.
    GameEntry partial = inst;
    partial.installDir = partial.repoPath = dir / L"games" / L"wm2k";
    put(partial.repoPath / L"saves", "a file where the saves folder would go");
    CHECK(make_game_portable(partial, appData) == -1 && !game_is_portable(partial));
    CHECK(wants_portable_game(partial, true, true));
    fs::remove(partial.repoPath / L"saves");
    CHECK(make_game_portable(partial, appData) >= 1 && game_is_portable(partial));
    CHECK(get(partial.repoPath / L"saves" / L"slot1.bin") == "save");

    // No AppData folder at all: just portable.txt.
    GameEntry fresh = inst;
    fresh.installDir = fresh.repoPath = dir / L"games" / L"vpw2";
    fs::create_directories(fresh.repoPath);
    CHECK(make_game_portable(fresh, dir / L"appdata" / L"Nothing") == 0 && game_is_portable(fresh));
}
