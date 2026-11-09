#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <vector>

// Texture packs named the old way (aki-roadmap#228, phase 0): packs made for Rice Video / GLideN64
// (NAME#crc#fmt#siz[#palcrc]_suffix.png) or Jabo Direct3D8 (<32 hex>.png) load in our rt64 as their authors shipped
// them, through rt64's texture-name provider (N64MR texnames, aki-roadmap#93), once the pack folder has an rt64.json
// with "autoPath" ("rice" for Rice names, "rt64" for Jabo names, as librecomp's conversion.cpp writes it) and a
// mod.json naming the game. The launcher writes both when such a pack is installed on the Mods page.

enum class TexNaming { None, Rice, Jabo, Rt64 };

// What one file name is: a Rice/GLideN64 name, a Jabo name, an rt64 hash name (<16 hex>.png/.dds), or none.
// riceName (optional) gets the part before the first '#': the ROM's internal name as the author's plugin saw it.
TexNaming texture_name_kind(const std::wstring& fileName, std::wstring* riceName = nullptr);

struct TexScan {
    int rice = 0, jabo = 0, rt64 = 0, images = 0;  // images = every .png/.dds
    bool hasRt64Json = false;                      // rt64.json at the pack's root
    bool hasModJson = false;                       // mod.json at the root
    std::map<std::wstring, int> riceNames;         // internal name -> file count
    TexNaming naming() const;                      // what the pack is (Rt64 when it already loads as is)
    std::wstring rice_name() const;                // the most common internal name ("" = none)
};

// File names relative to the pack's root ("a/b/c.png", either slash): a folder walk or a zip listing.
TexScan scan_texture_names(const std::vector<std::wstring>& relPaths);
// A folder on disk (stops after maxFiles files).
TexScan scan_texture_folder(const std::filesystem::path& dir, int maxFiles = 200000);

// The family game a Rice internal name belongs to: its mod_game_id ("wwfnomercy"), or "" when the name isn't one of
// the six games' ROM names (a hack with its own header name, or a Japanese name the file system mangled). Matched on
// letters and digits only, so "WCW-nWo REVENGE" (no '/' in a file name) still matches.
std::wstring rice_name_game(const std::wstring& riceName);

// The rt64.json for a pack of that naming (Rice or Jabo).
std::string legacy_rt64_json(TexNaming naming);
// The mod.json the launcher writes for a wrapped pack: the runtime's required keys, "game_id" = gameId.
std::string legacy_mod_json(const std::wstring& id, const std::wstring& displayName, const std::wstring& gameId,
                            TexNaming naming, int textures);
// A mod id from a pack's name: lower case a-z 0-9 _, "tex_" in front, at most 48 characters.
std::wstring legacy_pack_id(const std::wstring& name);

// Writes rt64.json (and mod.json, unless the pack has one) into packDir. false + errorOut on a write failure.
bool wrap_legacy_pack(const std::filesystem::path& packDir, const TexScan& scan, const std::wstring& gameId,
                      const std::wstring& displayName, std::wstring* errorOut);
