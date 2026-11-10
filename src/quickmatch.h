#pragma once
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <vector>

// Quick match (aki-roadmap#22): a game started straight into a CPU-vs-CPU match through the AkiMods quickmatch mod
// (aki_quickmatch), which every port bundles, off by default. For one run the launcher turns it on with the
// player's picks; when the game closes, the player's own mod settings come back.
//
// The runtime contract (what AkiTools aki-run writes for its -Venue / -Match runs):
//   <data>\mods.json                        "enabled_mods" (and "mod_order") list aki_quickmatch
//   <data>\mod_config\aki_quickmatch.json   {"mod_id", "mod_version", "recomp_version",
//                                            "storage": {"enabled": true, "<option id>": "<choice>", ...}}
// where <data> is game_data_dir(g) and the choices are the names in the mod's manifest (mod.json inside the .nrm,
// config_schema.options: venue, match, and on some games length and entrances).

constexpr const char* kQuickMatchId = "aki_quickmatch";

// aki-roadmap#287: Quick match (and the History Tour, which runs on it) is a developer tool in 0.3.0. Players don't
// see it: the home page's Q / Back / H routes and their footer hints are on only in dev mode (--dev, AKI_DEV=1,
// dev_mode=1). Turning it on for players is #288 (0.4.0): set this true. The restore of an earlier run's settings
// on start (quickmatch_pending / quickmatch_restore) runs either way.
constexpr bool kQuickMatchForPlayers = false;
bool quick_match_ui_enabled(bool devMode);

struct QmOption {               // one Enum option of the mod
    std::string id, name;       // UTF-8
    std::vector<std::string> choices;
    std::string def;            // its default (one of the choices)
    bool randomOk = false;      // venue and match: "Random" picks one of the choices after the first (the game's own)
};

struct QuickMatchMod {
    std::filesystem::path nrm;
    std::string version;
    std::vector<QmOption> options;  // Enum options in manifest order ("enabled" is the launcher's to set)
    const QmOption* option(const std::string& id) const;
};

// The quickmatch options in a mod.json. False (with *err) when it isn't aki_quickmatch or has no venue option.
bool parse_quickmatch_manifest(const std::string& json, QuickMatchMod* out, std::wstring* err);

// The game's quickmatch mod: the bundled copy (<exe dir>\assets\mods\aki_quickmatch.nrm, which the game copies
// over the mods folder's at every start) when there is one, else the mods folder's. False (with *err) if neither.
bool find_quickmatch(const std::filesystem::path& bundledDir, const std::filesystem::path& modsDir,
                     QuickMatchMod* out, std::wstring* err);

constexpr const char* kQmRandom = "Random";  // a pick, not one of the mod's choices

// The value for each option: the pick (a choice, or Random where randomOk), or the default when there is none.
// False (with *err) for a pick that is neither.
bool resolve_quickmatch(const QuickMatchMod& mod, const std::map<std::string, std::string>& picks, std::mt19937& rng,
                        std::map<std::string, std::string>* values, std::wstring* err);

// Turns the mod on for one run with these values (option id -> choice). What it changes is recorded in
// <data>\aki-launcher-quickmatch.json first, so quickmatch_restore can undo it, even after a crash. A malformed
// mods.json is refused, not replaced.
bool quickmatch_apply(const std::filesystem::path& dataDir, const QuickMatchMod& mod,
                      const std::map<std::string, std::string>& values, std::wstring* err);
// A run's changes not undone yet (the game is running, or the launcher stopped first).
bool quickmatch_pending(const std::filesystem::path& dataDir);
// Undoes quickmatch_apply: aki_quickmatch leaves mods.json's lists again if it wasn't in them (anything else the
// game changed in mods.json meanwhile stays), and mod_config\aki_quickmatch.json is put back as it was (or removed,
// if there was none). True when there was nothing to undo.
bool quickmatch_restore(const std::filesystem::path& dataDir, std::wstring* err);
