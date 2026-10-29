#pragma once
#include <filesystem>
#include <string>
#include <vector>

struct JsonValue;

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
    std::wstring baseId;      // a variant (e.g. a mod build) of this game id: its tile follows the base's
    std::string modelProfile; // aki_modelimport's profile (nomercy, vpw2): the Mods page shows Model packs ("" = none)
    std::filesystem::path manifestPath;  // the aki-game.json it was discovered from ("" = games.json)

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
// The registry (aki-roadmap#24): assets\games.json next to the launcher if it
// parses, else the copy built into the exe; then any recomp found through an
// aki-game.json manifest (in a depot folder or an install folder) whose id is
// new. Paths resolved for the depot and the in-app install dir. Ordered as the
// file lists them (original release dates), discovered games after, variants
// right after their base game.
std::vector<GameEntry> build_game_list(const std::filesystem::path& depotRoot);

// The pieces, for tests and logging.
const char* embedded_games_json();  // assets/games.json as built into the exe
// Games from a games.json text; empty (with *err) if it isn't a valid registry.
std::vector<GameEntry> registry_from_json(const std::string& text, std::wstring* err);
// One game from a games.json entry or an aki-game.json manifest (same keys).
bool game_from_json(const JsonValue& v, GameEntry* g, std::wstring* err);
// The registry file's games, or the built-in ones when it is missing or invalid.
std::vector<GameEntry> load_registry(const std::filesystem::path& file);
// Adds games from <root>\<folder>\aki-game.json (one level down, each root),
// skipping ids already present; a depot folder's name becomes its repo_dir.
void discover_games(std::vector<GameEntry>& games, const std::filesystem::path& depotRoot,
                    const std::filesystem::path& gamesRoot);
// Fills repoPath / exePath / romPath / installDir / found flags (the depot
// build first, else the in-app install).
void resolve_game_paths(std::vector<GameEntry>& games, const std::filesystem::path& depotRoot,
                        const std::filesystem::path& gamesRoot);

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
