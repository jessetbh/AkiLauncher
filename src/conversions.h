#pragma once
// Conversions: full-conversion mods packaged as one Thunderstore-style zip (manifest.json, icon.png, README.md,
// aki.json + component files; spec in the depot's plans/fullconv-manifest.md). Every kind (ROM patch, GameShark
// codes, saves, texture pack, RT data, any mix) is installed, listed and played the same way. The game runs one with
// AKI_CONVERSION=<id>; its files live in the game's data folder under conversions\<id>\, its saves in saves\<id>\.
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

struct GameEntry;

struct ConversionInfo {
    std::wstring id, name, version, authors, description, credits, permission, game;
    std::wstring baseSha1, baseRevision;
    bool patch = false, codes = false, saves = false, textures = false, rt = false;
    std::wstring codesRom;          // e.g. nomercy-rev0
    std::wstring texturesPackage;   // dependency package "Team-Name" ("" = inline or none)
    std::wstring texturesNaming;    // rt64 / rice / jabo
    std::wstring requiresPatchBase; // aki.json requires.patch.base: a ROM patch left out until the player supplies that ROM
    std::wstring texturesSha256;    // the package zip it was made with ("" = not pinned)
    std::wstring rtLights, rtMaterials;
    std::filesystem::path folder;   // installed folder
    std::filesystem::path icon;     // icon.png in it ("" = none)
    bool texturesInstalled = true;
    bool codesRunnable = true;
    std::vector<std::wstring> notes;  // shown under the entry (missing texture package, codes off, ...)
};

// The folder the game keeps mods and saves in: its working folder when that holds portable.txt, else
// %LOCALAPPDATA%\<exe stem>.
std::filesystem::path game_data_dir(const GameEntry& g);
// The ROM revision tag codes must have for this port (nomercy-rev1 for No Mercy, else the game key).
std::wstring game_key(const GameEntry& g);
std::wstring port_rom_tag(const GameEntry& g);

std::vector<ConversionInfo> list_conversions(const GameEntry& g);

struct AddResult {
    bool ok = false;
    std::wstring message;  // what happened (or why not)
    int gameIndex = -1;    // the game it went to
    std::wstring id;       // the conversion id (or the texture package name)
};
// Installs a conversion or texture package from a .zip or a folder into the game its aki.json names. Checks the
// manifest, the paths in it, and the player's ROM against base_rom.sha1 before writing anything; seeds saves\<id>\
// only when it's empty; an update replaces the conversion's files and keeps its saves.
AddResult add_conversion(std::vector<GameEntry>& games, const std::filesystem::path& src);

// Removes conversions\<id>\ (the saves in saves\<id>\ stay).
bool remove_conversion(const GameEntry& g, const std::wstring& id, std::wstring* err);

// Env vars for running this conversion: AKI_CONVERSION and the RT file overrides.
std::vector<std::pair<std::wstring, std::wstring>> conversion_env(const ConversionInfo& c);
