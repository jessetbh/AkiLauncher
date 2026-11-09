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
//   AkiLauncher.exe --shot <page> <out.png> [--game <id>] [--demo] [--size WxH]
//   AkiLauncher.exe --shot-all <dir> [--game <id>] [--demo] [--size WxH]      every page, as <dir>\<page>.png
//   AkiLauncher.exe --shot <page> <out.png> --pad-script "DOWN,shot,A,wait40,shot"  a controller path, frame by frame
//
// --game: a games.json id (default nomercy when the registry has it). --demo: demo data even where real data exists.
// --size: the frame in pixels (default 1600x900; 3840x2160 for a 4K screen), 320x240 to 7680x4320.

enum class ShotPage { Home, Settings, Mods, ModelPacks, QuickMatch, HistoryTour, Dev, Import };

// The pages in --shot-all order, with their command-line names.
const std::vector<std::pair<std::wstring, ShotPage>>& shot_pages();

struct ShotRequest {
    bool active = false;
    std::vector<std::pair<ShotPage, std::filesystem::path>> shots;  // in order
    std::wstring game;  // "" = the default
    bool demo = false;
    int width = 1600, height = 900;
    // aki-roadmap#232: drive the page with a controller script (convnav::parse_pad_script) and save frames: each
    // "shot" step as <out>-NN.png, the last frame as <out>. Launches stay blocked; the cart insert still plays.
    std::wstring padScript;
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
// A demo data folder for conversions (aki-roadmap#154): conversions\ with one recorded in packages.json (with a
// texture pack that came with it, and saves; Experimental with known issues), one Untested import, and one added by
// hand for another ROM.
bool make_demo_conversions(const std::filesystem::path& dataDir, std::wstring* err);
