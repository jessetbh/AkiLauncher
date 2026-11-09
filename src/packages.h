#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "json.h"

struct GameEntry;

// One package format for every kind (aki-roadmap#154): a zip or folder with aki-package.json (format 1) at its root,
// next to its payload: an add-on .nrm, a texture pack (.rtz or a folder with rt64.json), <id>.akimodel model packs,
// or a conversion folder (its own aki.json) plus the add-ons that come with it. The same rules as AkiTools
// aki-package.py and the game's packages module (librecomp); tests\fixtures\packages is AkiTools' shared set.
//
// Installs are recorded in packages.json in the game's data folder, the one list the launcher and the game read.
// The launcher writes it only while the game isn't running.
namespace pkg {

// --- the manifest ---------------------------------------------------------------------------------------------------

struct Problem {
    bool error = true;  // false: a warning (the install goes on)
    std::string field;  // "id", "payload", "thumb", ...
    std::wstring text;
};

struct Manifest {
    bool parsed = false;  // a JSON object
    std::string kind, id, version, gameId, displayName, shortDescription, license, source, thumb;
    std::vector<std::string> authors, replaces, requiredPackages;
    std::string requiresLauncher, requiresRecomp;
    std::vector<std::string> baseSha1;  // lower-case hex
    std::string baseRomName;
    std::string redistribute, permissionStatement;
    // payload, by kind
    std::string nrm, rtz, folder;
    std::vector<std::string> packs, hd, addons;
    std::vector<Problem> problems;

    bool ok() const;                  // no errors
    std::wstring first_error() const;
};

// The manifest's own checks (types, kind, id, version, game_id, base_rom, payload keys for the kind).
Manifest parse_manifest(const std::string& json);

// Ids: [a-z0-9], then up to 47 of [a-z0-9_.-], not ending in '.', never a Windows device name. One namespace per game
// across every kind (a conversion's id is also its save folder).
bool valid_id(const std::string& id);
// Semantic versions: x.y.z with an optional -prerelease. compare_versions: <0, 0, >0 (both must be valid).
bool valid_version(const std::string& v);
int compare_versions(const std::string& a, const std::string& b);
// The short keys the launcher and AkiMods use, as runtime game ids ("nomercy" -> "wwfnomercy"); "" if unknown.
std::string runtime_game_id(const std::string& shortOrRuntime);

// --- a package's files ----------------------------------------------------------------------------------------------

// A zip or a folder, by relative '/' path. Unsafe names (absolute, '..', '\', drive, ':', device names, links in a
// folder) are listed, never readable.
class Package {
public:
    explicit Package(const std::filesystem::path& p);
    ~Package();
    Package(const Package&) = delete;
    Package& operator=(const Package&) = delete;

    bool opened() const { return opened_; }
    bool is_zip() const { return zip_ != nullptr; }
    const std::vector<std::string>& files() const { return names_; }  // sorted
    bool has(const std::string& rel) const;
    uint64_t size(const std::string& rel) const;
    uint64_t total() const;
    std::vector<std::string> under(const std::string& folder) const;  // files below folder/
    // Up to `limit` bytes (false when bigger or unreadable).
    bool read(const std::string& rel, std::string* out, uint64_t limit) const;
    // Copies one file to dest (its folder is created).
    bool extract(const std::string& rel, const std::filesystem::path& dest) const;

    std::vector<std::string> unsafe;   // names that would escape or that Windows can't hold
    std::vector<std::string> clashes;  // names that are the same file on Windows

private:
    std::filesystem::path path_;
    void* zip_ = nullptr;  // mz_zip_archive
    void* file_ = nullptr; // FILE*
    bool opened_ = false;
    std::vector<std::string> names_;
    std::vector<uint64_t> sizes_;
    std::vector<std::string> zipNames_;  // the zip's own entry names
    int index(const std::string& rel) const;
};

struct CheckResult {
    Manifest manifest;
    std::vector<Problem> problems;  // the manifest's, then the content's
    bool hasManifest = false;       // aki-package.json at the root
    bool ok() const;
    std::wstring first_error() const;
};
// Everything an install checks before it changes anything, except this game and its ROM (install_package does those).
CheckResult check_package(const std::filesystem::path& src);

// --- the registry: packages.json ------------------------------------------------------------------------------------

struct RegFile {
    std::string path;  // relative to the data folder, '/' separated: "mods/x.nrm", "conversions/nmp_v3"
    std::string hash;  // pack_hash at install time
};
struct RegEntry {
    std::string id, kind, version, source, installed;
    std::vector<RegFile> files;
    JsonValue raw;         // the entry as read: written back as it is unless changed
    bool changed = false;  // rebuilt from the fields above (fields this launcher doesn't know kept from raw)
};
struct Registry {
    enum class Status { Missing, Ok, Newer, Corrupt, Unreadable };
    Status status = Status::Missing;
    JsonValue raw;                  // the whole file as read (unknown top-level keys are kept)
    std::vector<RegEntry> entries;  // known kinds; an entry removed from here is dropped from the file
    std::vector<JsonValue> others;  // entries of a kind this launcher doesn't know: kept as they are, where they are
    bool may_save() const { return status == Status::Missing || status == Status::Ok; }
    const RegEntry* find(const std::string& id) const;
    // The entry owning a path (case-insensitive, a trailing '/' ignored), or nullptr.
    const RegEntry* owner(const std::string& path) const;
};
Registry load_registry(const std::filesystem::path& file);
// Written to a temp file, flushed, then renamed into place. Refuses when !may_save().
bool save_registry(const std::filesystem::path& file, const Registry& reg, std::wstring* err);

// The content hash the registry records (the #149 pack_hash: FNV-1a 64, CRLF-blind for text, of a file or a whole
// folder), as 16 lower-case hex digits; "" when it can't be read.
std::string pack_hash(const std::filesystem::path& p);

// --- install, upgrade, remove ---------------------------------------------------------------------------------------

struct InstallTarget {
    std::filesystem::path dataDir;  // game_data_dir(g): mods\, conversions\, packages.json
    std::wstring gameTitle;
    std::string gameId;             // the port's mod_game_id (runtime id)
    std::vector<std::string> romSha1;  // the player's ROM, lower-case hex ("" entries ignored)
    // Hashes the player's ROM when romSha1 is empty and a conversion needs it (may be slow; may return "").
    std::function<std::string()> hashRom;
};

struct InstallOptions {
    bool reinstall = false;       // the same version again
    bool allowDowngrade = false;  // the player confirmed an older version
    bool gameRunning = false;     // then nothing is written
    std::string source = "launcher";
    std::string today;            // YYYYMMDD for .edited-<date> ("" = today)
    std::string now;              // UTC ISO time for "installed" ("" = now)
};

struct PackageInstall {
    enum class Outcome { Installed, Upgraded, Reinstalled, AlreadyInstalled, NeedsDowngradeConfirm, Refused, Failed };
    Outcome outcome = Outcome::Failed;
    std::string id, kind, version, oldVersion;
    std::wstring name;
    std::wstring message;                        // one line for the UI
    std::vector<std::wstring> notes;             // warnings, kept edited files, permission
    std::vector<std::filesystem::path> keptEdited;
    bool ok() const {
        return outcome == Outcome::Installed || outcome == Outcome::Upgraded || outcome == Outcome::Reinstalled;
    }
};

// Installs a package (zip or folder with aki-package.json). Checks first, then stages every file in the data folder
// and moves it into place; a failure before the moves changes nothing. Upgrades: a newer version replaces the
// recorded files, an unedited one silently (to the Recycle Bin), an edited one kept as <name>.edited-YYYYMMDD; the
// same version needs reinstall, an older one allowDowngrade. Saves are never touched.
PackageInstall install_package(const std::filesystem::path& src, const InstallTarget& t, const InstallOptions& o = {});

// A patch-mod ROM hack (.nrm with patch.bps, no code): installed as a conversion under its mod id, so its saves
// (saves\<mod id>\) are found again. Its base ROM is the player's own (it was made for that game's ROM).
bool is_patch_mod(const std::filesystem::path& nrm);
PackageInstall install_patch_mod(const std::filesystem::path& nrm, const InstallTarget& t, const InstallOptions& o = {});

// Removes a recorded package: every file it recorded to the Recycle Bin, its entry from packages.json. Saves stay.
bool remove_package(const std::string& id, const std::filesystem::path& dataDir, bool gameRunning, std::wstring* msg);

// --- conversions, for the Mods page and Play as... -------------------------------------------------------------------

struct ConversionInfo {
    std::string id;
    std::wstring name, version, authors;
    std::vector<std::string> baseSha1;
    bool recorded = false;  // in packages.json (else a folder put there by hand)
    std::filesystem::path folder, saves;
    std::vector<std::string> addons;  // add-on files that came with it (recorded)
    // aki.json components.models (aki-roadmap#226): a line for the player ("" = no models component), and whether the
    // game will run the conversion without them (then the line says why).
    std::wstring models;
    bool modelsOff = false;
    // aki-roadmap#232: what the player reads before picking it (aki.json; the #228 converter writes survey.*).
    std::wstring description;
    std::wstring tier;                     // Verified / Experimental / Unsupported / Untested ("" = not said)
    bool untested = false;                 // built on this PC by the converter, not tested by the AKI team
    std::vector<std::wstring> knownIssues;
    std::wstring note;                     // the catalog's note, or why the converter chose the tier
    // aki-roadmap#243: what the player should know before playing it (texture package missing, ROM patch left out,
    // codes the game won't run); conversion_notes() writes them, the Mods page shows them.
    std::vector<std::wstring> notes;
    // aki-roadmap#264: the game version it needs when the one that runs is older ("" = it runs). The launch refuses it.
    std::wstring needsGame;
};

// --- conversion notes (aki-roadmap#243), shared by the Mods page (list_conversions) and the installer -----------------

// How the game treats a conversion's codes file on the port whose ROM tag is portTag (nomercy-rev1, wt-rev0, ...). The
// file's first "# rom:" line decides, as in librecomp conversion::codes::parse_file / parse_rom_tag:
//   Native: no tag, or this game's key with no revision or the port's own ("nomercy", "NoMercy_Rev 1");
//   Translated: No Mercy Rev 0 codes on No Mercy (Rev 1): the cheats mod translates them as they run;
//   Refused: anything else (another game, another revision, words after the tag): the game won't run them.
// tag (optional) gets the tag as written, trimmed ("" = no tag).
enum class CodesVerdict { Native, Translated, Refused };
CodesVerdict codes_tag_verdict(const std::string& text, const std::string& portTag, std::string* tag = nullptr);

struct ConversionNoteFacts {
    std::string texturesPackage;    // components.textures.package ("" = none)
    bool texturesAdded = false;     // conversions\_packages\<package>\ is there
    std::string texturesSha256;     // the package zip it was made with ("" = not pinned)
    std::string addedSha256;        // the added package's .package.sha256 ("" = unknown)
    std::string requiresPatchBase;  // requires.patch.base: a ROM patch left out until the player supplies that ROM
    std::string codesFile;          // components.codes.file ("" = no codes)
    bool codesReadable = false;     // the codes file could be read
    std::string codesText;
    std::string manifestCodesRom;   // components.codes.rom (or rom_revision): mentioned only when it disagrees
    std::string portTag;            // port_rom_tag of the game ("" = unknown: no codes note)
    std::wstring gameVersion;       // game_run_version: the installed version of the exe that runs ("" = unknown)
    std::wstring minimumGame;       // requires.minimum_recomp_version ("" = none)
    std::wstring gameTitle;         // for the note
};
struct ConversionNotes {
    std::vector<std::wstring> notes;
    bool texturesInstalled = true;
    bool codesRunnable = true;      // Native or Translated
    std::wstring needsGame;         // the game version it needs when the one that runs is older (#264; "" = it runs)
};
ConversionNotes conversion_notes(const ConversionNoteFacts& f);
// Fills f's file facts from the game's data folder and the installed conversion folder: whether the texture package
// is added and its sha256, and the codes file's text (texturesPackage and codesFile must be set first).
void read_conversion_note_files(const std::filesystem::path& dataDir, const std::filesystem::path& folder, ConversionNoteFacts& f);
// Why the game runs a conversion without its models, "" when it runs with them: the game's rule (librecomp
// conversion::models_refusal), kept the same. relink: the patch's relink.json (nullptr: a plain patch or none).
// Packs load into Expansion RAM, physical 0x00400000-0x00800000; a relinked hack using it needs aki-roadmap#156.
std::wstring conversion_models_refusal(const JsonValue* relink, bool relinkUnreadable);
// The newest model pack format (AKP1 version) the games read (librecomp conversion::ModelsFormat).
constexpr int kModelsFormat = 2;
// A conversion tier as the player reads it ("verified" -> "Verified"; unknown words are kept as they are).
std::wstring conversion_tier_word(const std::string& tier);
// conversions\<id>\ folders with an aki.json, recorded or not, sorted by name. portTag: the game's port_rom_tag, for
// the codes note ("" = no codes note).
std::vector<ConversionInfo> list_conversions(const std::filesystem::path& dataDir, const std::string& portTag = std::string(),
                                             const std::wstring& gameVersion = std::wstring(),
                                             const std::wstring& gameTitle = std::wstring());
// Whether the player's ROM is one the conversion names (no names: any ROM).
bool conversion_rom_matches(const ConversionInfo& c, const std::vector<std::string>& romSha1);

}  // namespace pkg
