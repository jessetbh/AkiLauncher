#pragma once
#include <filesystem>
#include <string>
#include <vector>

struct GameEntry {
    std::wstring title;
    std::wstring releaseDate;  // display string, e.g. "December 2, 1997"
    std::wstring region;       // "North America" or "Japan" (JP-only titles)
    std::wstring repoDir;      // folder name under the depot root
    std::wstring exeRel;       // exe path relative to the repo dir
    std::wstring romFile;      // ROM filename at the repo root
    std::wstring artKey;       // boxart file stem: assets\boxart\<artKey>_front.png
    bool forceBorderless = true;
    bool comingSoon = false;  // recomp project not started yet; auto-activates
                              // once the repo/exe/ROM show up at the usual paths
    std::wstring githubRepo;  // "owner/repo" for in-app download ("" = none)
    std::wstring romSha1;     // expected ROM SHA1, uppercase hex ("" = accept any)
    std::wstring romNote;     // human description of the exact dump required
                              // (region/revision), shown in the ROM-select UI
    std::wstring romHint;     // extra line for the wrong-ROM error ("" = none)
    std::wstring romStoredFile;  // the copy the game keeps when it imports a ROM: <game id>.z64 in its data folder

    // resolved by build_game_list
    std::filesystem::path repoPath;
    std::filesystem::path exePath;
    std::filesystem::path romPath;         // the ROM a launch passes (resolve_rom)
    std::filesystem::path romRegistryPath; // the default repo's or install's ROM, before any exe= override
    std::filesystem::path romInstallPath;  // where Select ROM copies to: romFile in the checkout that runs
    std::filesystem::path romAutoPath;     // what romPath is without a rom= override
    std::wstring romHaveSha1;              // romPath's SHA1 when it's wrong ("" otherwise)
    std::wstring romNotice;                // an override problem to show on the card ("" = none)
    std::filesystem::path installDir;  // games_root()/artKey (in-app install target)
    std::wstring installedTag;         // version.txt in installDir ("" = none)
    bool exeFound = false;
    bool romFound = false;
    bool romWrong = false;  // romPath isn't the dump the port needs and no rom= override asked for it

    bool launchable() const { return exeFound && romFound && !romWrong; }
};

// Depot root = env AKI_DEPOT_ROOT, else first ancestor of the launcher exe (or
// cwd) that contains WcwNwoWorldTour.
std::filesystem::path detect_depot_root();
// Dir containing assets\: the exe dir (packaged) or its parent (dev build-msvc).
std::filesystem::path app_root();
// Where in-app installs land: env AKI_GAMES_ROOT, else app_root()\games.
std::filesystem::path games_root();
// Entries are ordered by original release date (the launcher shows them as-is).
std::vector<GameEntry> build_game_list(const std::filesystem::path& depotRoot);
// Picks g's ROM (rules in rom_rules.h): the rom= override, else the right dump among romFile in the checkout that
// runs, the copy the game imported, and the registry default. Call it again when repoPath or the override changes.
void resolve_rom(GameEntry& g, const std::wstring& romOverride);
// Why g can't start for lack of the right ROM, for the player ("" = nothing wrong).
std::wstring rom_problem(const GameEntry& g);
