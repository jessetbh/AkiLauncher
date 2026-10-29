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
    std::wstring romCode;     // cartridge code in the ROM header (0x3B, e.g. "NW4E"):
                              // the ROM folder scan finds the game's dumps and hacks by it
    int romVersion = -1;      // the accepted dump's header version byte (0x3F; -1 = unknown)
    std::wstring modGameId;   // the port's mod_game_id: mod.json "game_id" of mods made for it

    // resolved by build_game_list
    std::filesystem::path repoPath;
    std::filesystem::path exePath;
    std::filesystem::path romPath;
    std::filesystem::path installDir;  // games_root()/artKey (in-app install target)
    std::wstring installedTag;         // version.txt in installDir ("" = none)
    bool exeFound = false;
    bool romFound = false;

    bool launchable() const { return exeFound && romFound; }
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

// Where a game keeps its config, saves, mods and log: its working directory
// (repoPath, where the launcher starts it) when that has portable.txt, as the
// ports check it (recompui get_app_folder_path); else %LOCALAPPDATA%\<exe stem>.
std::filesystem::path game_data_dir(const GameEntry& g);
bool game_is_portable(const GameEntry& g);

// aki-roadmap#14: a portable launcher starts the games it installed itself
// (installDir, under games_root()) portable too, so everything stays in the
// launcher's folder. Depot checkouts are never changed (their portable.txt is
// the developer's call). True when g qualifies and isn't portable yet.
bool wants_portable_game(const GameEntry& g, bool launcherPortable, bool portableGames);
// Writes portable.txt in g's working directory and copies, once, what the game
// kept under appDataDir (config, saves, mods; not logs or texture dumps) into
// it where nothing of that name exists yet; portable.txt is written last, only
// when every copy worked. Returns how many files it copied, or -1 (not made
// portable: the next start tries again).
int make_game_portable(const GameEntry& g, const std::filesystem::path& appDataDir);
