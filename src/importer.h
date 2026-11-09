#pragma once
// (aki-roadmap#277) ImportRomRoute / import_rom_route: where the Import screen sends a picked ROM file.
#include <filesystem>
#include <map>
#include <string>
#include <vector>

struct JsonValue;

// Bulk import (aki-roadmap#228): the results model and the rules around the Import folder, without any UI or
// process code (tests/importer_tests.cpp). The launcher installs what it can itself (mods, texture packs, packages,
// Rice/Jabo packs) and hands ROM hacks, patches, Project64 bundles and unknown archives to ONE aki_import.py worker
// per batch (aki-roadmap#111: one process, not one per item), which streams one JSON line per item.

enum class ImportOutcome {
    Waiting,      // not looked at yet
    Working,      // the worker has it
    Converted,    // the worker built a package (Converted\<game>\...) and the launcher installed it
    Installed,    // the launcher installed it as it is (mod, texture pack, package)
    HaveIt,       // imported before (the ledger) or the same file twice in this batch
    Stock,        // the unmodified game: nothing to install
    NeedsRom,     // a patch whose base ROM the launcher doesn't have yet ([Find it...])
    NeedsGame,    // a texture pack or bundle that doesn't say its game
    Unsupported,  // not one of the six games, another region, a format we don't read
    Failed,       // something broke ([Log])
    Stopped,      // the batch was stopped before it got there
};

// The result screen's word for an outcome ("Converted", "Have it", "Needs ROM", ...).
const wchar_t* import_outcome_word(ImportOutcome o);
// Done with it: anything but Waiting / Working.
bool import_outcome_final(ImportOutcome o);

struct ImportLine {
    std::wstring item;          // what the player picked, or "archive!member" for something inside an archive
    std::filesystem::path source;  // the top-level input it came from (a picked file or folder, or an Import entry)
    ImportOutcome outcome = ImportOutcome::Waiting;
    std::wstring name;          // the mod's name when known, else the file name
    std::wstring gameKey;       // the engine's game key (nomercy, vpw2, ...) or ""
    std::wstring kind;          // "conversion", "texture pack", "mod", "package", "ROM", "patch", ...
    std::wstring tier;          // Verified / Experimental / Unsupported / Untested ("" = none)
    bool untested = false;      // tier Untested: built here, not tested by the AKI team
    std::wstring detail;        // one line: why, what it needs, what was installed
    std::wstring needs;         // NeedsRom: the ROM it needs ("WWF No Mercy (USA) 1.0")
    std::vector<std::filesystem::path> packages;  // Converted: the package zips the worker wrote
    // HaveIt: the package zips an earlier import built (the ledger's). Installed again when the conversion was removed
    // (aki-roadmap#272); never deleted by Undo, which deletes only what this batch built (packages).
    std::vector<std::filesystem::path> reinstall;
    std::wstring log;           // the full text behind [Log] (the worker's why, the converter's output)
    bool viaWorker = false;     // the worker handles it (a ROM hack, patch, bundle, unknown archive)
    bool installAsIs = false;   // the worker found a mod or package file in a picked folder: the launcher installs it
    bool placeholder = false;   // stands for a worker input until the worker's own lines for it arrive

    // Undo: what installing it put in place.
    struct Installed {
        std::wstring gameArtKey;  // the game it went into
        std::string packageId;    // a #154 package id (removed through the registry), or ""
        std::filesystem::path path;  // a mods-folder file or folder (recycled), or ""
    };
    std::vector<Installed> installed;
};

struct ImportCounts {
    int total = 0, waiting = 0, working = 0;
    int converted = 0, conversions = 0, texturePacks = 0, installed = 0, haveIt = 0, stock = 0;
    int needsRom = 0, needsGame = 0, unsupported = 0, failed = 0, stopped = 0, untested = 0;
};
ImportCounts count_import(const std::vector<ImportLine>& lines);
// "14 converted (9 conversions, 5 texture packs), 1 needs a ROM, 2 unsupported, 1 failed. Your originals weren't
// changed." The progress head while running: "14 done | 1 working | 8 waiting".
std::wstring import_summary(const ImportCounts& c);
std::wstring import_progress(const ImportCounts& c);

// --- the worker's JSON lines (aki_import.py worker) ----------------------------------------------------------------

struct WorkerEvent {
    enum class Type { Item, Done, Bad } type = Type::Bad;
    ImportLine line;                     // Item
    std::map<std::string, int> counts;   // Done
    std::string engine;                  // Done: engine version
};
// One stdout line. A line that isn't JSON (a stray print) is Bad and is kept for the log by the caller.
WorkerEvent parse_worker_line(const std::string& line);
// The engine's game key -> the runtime's mod_game_id (nomercy -> wwfnomercy), "" if unknown.
std::wstring engine_game_mod_id(const std::wstring& key);
// The other way: mod_game_id -> engine key ("" if unknown).
std::wstring engine_game_key(const std::wstring& modGameId);

// The job the launcher writes to the worker's stdin (aki_import.py worker): paths, the port ROMs it holds
// ({engine key: path}), extra folders to find stock ROMs in, the ledger, the Converted folder, a work folder.
struct WorkerJob {
    std::vector<std::filesystem::path> paths;
    std::map<std::wstring, std::filesystem::path> roms;
    std::vector<std::filesystem::path> romDirs;
    std::filesystem::path ledger, out, work, catalog, sevenzip;
    std::wstring game;  // engine key for packs that don't say (the Mods page's game)
    bool build = true;
};
std::string worker_job_json(const WorkerJob& j);

// --- where the item came from --------------------------------------------------------------------------------------

// The top-level input an engine item label belongs to: "C:\a\b.zip!x\y.z64" -> C:\a\b.zip; a file found inside a picked
// folder -> that folder. Empty when none of the inputs owns it.
std::filesystem::path import_source_of(const std::wstring& label, const std::vector<std::filesystem::path>& inputs);

// --- the Import folder ---------------------------------------------------------------------------------------------

inline const wchar_t* kImportedFolder = L"Imported";
inline const wchar_t* kNeedsAttentionFolder = L"Needs attention";

// The entries waiting in an Import folder: every file and folder at its top, except Imported\, Needs attention\,
// hidden/dot entries and the reason .txt files the launcher writes. Sorted by name.
std::vector<std::filesystem::path> import_folder_items(const std::filesystem::path& importDir);

// Where an Import folder entry goes when its batch is done, from the outcomes of every line it produced:
// all done fine (converted, installed, have it, stock) -> Imported\; any NeedsRom / NeedsGame / Waiting / Working /
// Stopped -> stays (retried next time); otherwise (unsupported, failed) -> Needs attention\.
enum class ImportPlace { Stay, Imported, NeedsAttention };
ImportPlace import_place_for(const std::vector<ImportOutcome>& outcomes);
// A free name for p's leaf in dir: dir\leaf, else dir\stem (2).ext, (3), ...
std::filesystem::path import_free_name(const std::filesystem::path& dir, const std::filesystem::path& leaf);
// The reason file written next to an entry moved to Needs attention\: one line per item.
std::string import_reason_text(const std::vector<const ImportLine*>& lines);

// --- the ledger (Converted\import-ledger.json, written by the worker) --------------------------------------------

// Drops entries from the ledger (Undo: the next import builds them again). Keys absent are ignored. false on a
// read/write failure.
bool ledger_remove(const std::filesystem::path& ledger, const std::vector<std::string>& keys, std::wstring* errorOut);
// The ledger keys whose entry records one of these package zips (as the worker wrote them), for Undo.
std::vector<std::string> ledger_keys_for_packages(const std::filesystem::path& ledger,
                                                  const std::vector<std::filesystem::path>& packages);

// --- results file (the headless --import-batch run writes it; --shot import reads it) ------------------------------
std::string import_lines_json(const std::vector<ImportLine>& lines, const std::wstring& status);
bool import_lines_from_json(const std::string& text, std::vector<ImportLine>* lines, std::wstring* status);

// Where the Import screen sends a picked ROM file (aki-roadmap#277): the stock dump of one of the six (nothing to do),
// the converter (a hack or other build of one of them, or a cartridge code it may know: catalog hacks such as VPW2
// Freem carry their own code, and renamed No Mercy hacks are routed by title, #268), Unsupported (a dump of one of the
// six from another region), or Failed (unreadable).
enum class ImportRomRoute { Stock, Converter, Unsupported, Failed };
ImportRomRoute import_rom_route(bool baseDump, bool otherBuild, bool otherRegion, bool unreadable);
