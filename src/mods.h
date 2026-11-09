#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct GameEntry;

// Mods page (aki-roadmap#96, launcher install side): a game's mods folder,
// what is in it, installing texture packs and mods into it, removing them.
// The game's runtime does the loading, ordering and enabling (its Mods menu);
// the launcher only manages files.

// game_data_dir(g)\mods: %LOCALAPPDATA%\<exe stem>\mods, or mods\ in the
// game's working directory when it runs portable, as the ports resolve it.
std::filesystem::path game_mods_dir(const GameEntry& g);

// The fields of mod.json the launcher shows (the runtime's manifest keys).
struct ModManifest {
    bool present = false;
    bool valid = false;  // parsed as a JSON object
    std::wstring id, version, name, gameId, description;
    std::vector<std::wstring> authors;
};
// A small JSON reader for mod.json: top-level string fields and the authors
// array; anything else is skipped.
ModManifest parse_mod_manifest(const std::string& json);

enum class ModKind { Nrm, Rtz, Folder };
struct ModItem {
    std::filesystem::path path;
    ModKind kind = ModKind::Nrm;
    bool textures = false;   // has rt64.json (or hash-named images, for a folder)
    bool code = false;       // .nrm with code (mod_binary.bin)
    bool hasThumb = false;   // thumb.png at the root
    bool bundled = false;    // the game copies it back on start (assets\mods\)
    uintmax_t bytes = 0;
    ModManifest manifest;
    std::wstring problem;    // why it may not load ("" = looks fine)

    std::wstring display_name() const;
    std::wstring kind_text() const;  // "Texture pack", "Code mod", "Code + textures"
};

ModItem inspect_mod(const std::filesystem::path& p, const std::filesystem::path& bundledDir = {});
// Everything in the mods folder that is a mod (the models\ folder and the
// runtime's own json files are left out), sorted by name.
std::vector<ModItem> list_mods(const std::filesystem::path& modsDir, const std::filesystem::path& bundledDir = {});

// The thumbnail's PNG bytes (empty if none).
std::vector<uint8_t> read_mod_thumb(const ModItem& m);

struct InstallResult {
    bool ok = false;
    std::vector<std::filesystem::path> installed;
    std::wstring message;  // for the UI, success or not
    bool unrecognized = false;  // not ok because it isn't a mod, pack or package at all (the importer's worker may know it)
};
// Installs an .nrm, .rtz, a zip (rt64.json at its root -> an .rtz; a package
// with .nrm/.rtz files inside -> those) or a pack folder into modsDir.
// Refuses a mod whose mod.json names another game. A file replaces an older
// copy of the same name (that one goes to the Recycle Bin); a folder never
// replaces one.
//
// Model packs (aki-roadmap#130): an <id>.akimodel folder (model.bin + pack.json / manifest.json), a folder holding
// such folders, or a zip of either, goes into modsDir\models\<id>.akimodel, its <id>-hd.rtz into modsDir\. Only for
// a game with modelProfile (refused when the pack is built for another profile). models.txt is not touched: the Model
// packs section picks who it replaces. A pack of the same id is replaced (the old one to the Recycle Bin).
InstallResult install_mod(const std::filesystem::path& src, const std::filesystem::path& modsDir,
                          const std::wstring& gameId, const std::wstring& gameTitle,
                          const std::string& modelProfile = "");
// The model packs found in a folder: the folder itself when it is one, else its subfolders (two levels down).
std::vector<std::filesystem::path> find_model_packs(const std::filesystem::path& dir);
// The profile a pack is built for (manifest.json "profile", else pack.json "profile" / "game"); "" if neither says.
std::string model_pack_profile(const std::filesystem::path& packDir);
// Moves an installed model pack and its <id>-hd.rtz in modsDir to the Recycle Bin. models.txt keeps its lines (the
// Model packs section then lists them as naming a pack that isn't installed).
bool remove_model_pack(const std::filesystem::path& packDir, const std::filesystem::path& modsDir, std::wstring* errorOut);
// Moves a mod to the Recycle Bin (undoable). Bundled mods are refused.
bool remove_mod(const ModItem& m, std::wstring* errorOut);
// Recycle Bin move for any file or folder (also used for replaced copies).
bool recycle_path(const std::filesystem::path& p, std::wstring* errorOut);
