#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

// Page screenshots (--shot / --shot-all): the launcher renders a page in a hidden window, saves the frame as a PNG
// and exits, so pages can be checked without anyone at the screen. Shot mode never launches a game and never writes
// config (settings_set_read_only, launch_set_blocked); where a page would be empty it shows demo data, made in a temp
// folder in the formats the real files use.
//
//   AkiLauncher.exe --shot <page> <out.png> [--game <id>] [--demo]
//   AkiLauncher.exe --shot-all <dir> [--game <id>] [--demo]      every page, as <dir>\<page>.png
//
// --game: a games.json id (default nomercy when the registry has it). --demo: demo data even where real data exists.

enum class ShotPage { Home, Settings, Mods, ModelPacks, QuickMatch, HistoryTour, Dev };

// The pages in --shot-all order, with their command-line names.
const std::vector<std::pair<std::wstring, ShotPage>>& shot_pages();

struct ShotRequest {
    bool active = false;
    std::vector<std::pair<ShotPage, std::filesystem::path>> shots;  // in order
    std::wstring game;  // "" = the default
    bool demo = false;
};

// The shot options in a command line (the others are left for the caller). False (with *err) for a malformed one:
// an unknown page, a missing path, --game / --demo without a shot, both --shot and --shot-all.
bool parse_shot_args(const std::vector<std::wstring>& args, ShotRequest* out, std::wstring* err);

// An RGBA image (rows of stride bytes) as a PNG, opaque.
bool write_png(const std::filesystem::path& file, int width, int height, const uint8_t* rgba, size_t stride,
               std::wstring* err);

// A demo mods folder: two code mods (one of them aki_quickmatch with No Mercy's options), a texture pack, and
// models\ with two No Mercy model packs and a models.txt assigning one of them.
bool make_demo_mods_dir(const std::filesystem::path& dir, std::wstring* err);
