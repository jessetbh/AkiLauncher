#pragma once
#include <filesystem>
#include <string>
#include <vector>

struct GameEntry;

// ROM folder scan (aki-roadmap#26, phase 1): identify every N64 ROM in a
// folder against the launcher's games, so the player can point it at their
// ROM collection instead of picking each dump by hand.
//   - the accepted dump of a game (by SHA1, also when byte-swapped) -> it can
//     set that game up;
//   - another build of a supported game (same cartridge code, other SHA1: a
//     ROM hack, translation or revision) -> named, with how to play it (import
//     it on the Mods page; the converter builds a conversion, aki-roadmap#278);
//   - anything else -> "not a supported ROM".
// Only files whose header matches one of the games are hashed, so a big
// collection scans quickly.

enum class RomByteOrder { Z64, V64, N64, NotARom };  // big-endian, byte-swapped, little-endian

struct RomHeader {
    RomByteOrder order = RomByteOrder::NotARom;
    std::string internalName;  // 0x20, 20 bytes, trailing spaces trimmed (Shift-JIS left as is)
    std::string gameCode;      // 0x3B, 4 chars: media, 2-char id, region (e.g. "NW4E")
    int version = 0;           // 0x3F
};

// Header of a ROM image's first 64 bytes, in whatever byte order the file is.
RomHeader rom_parse_header(const unsigned char* first64, size_t len);
// Rewrites a byte-swapped or little-endian image to big-endian (.z64) order in place.
void rom_to_z64(std::vector<unsigned char>& data, RomByteOrder from);

enum class RomVerdict { BaseDump, OtherBuild, NotSupported, Unreadable };

struct RomScanEntry {
    std::filesystem::path file;
    RomVerdict verdict = RomVerdict::NotSupported;
    int gameIndex = -1;        // games[] index for BaseDump / OtherBuild
    RomHeader header;
    std::wstring sha1;         // of the .z64-order bytes; only for files that matched a game
    std::wstring note;         // one line for the UI
    bool zipWithoutRom = false;  // a zip with no N64 ROM inside: the scan leaves it out of its list (#280)
    int otherRegionGame = -1;  // NotSupported, but the same cartridge id as this games[] index in another region (PAL)
};

// The games[] index whose cartridge code (id + region) matches, or -1.
int rom_match_game(const RomHeader& h, const std::vector<GameEntry>& games);
// The games[] index with the same 2-character cartridge id in ANOTHER region (a PAL or Japanese release), or -1.
int rom_match_other_region(const RomHeader& h, const std::vector<GameEntry>& games);
// The region a header's region letter (gameCode[3]) stands for, for the player: "PAL (Europe)", "Japanese", "USA".
std::wstring rom_region_name(char regionLetter);
RomScanEntry rom_classify(const std::filesystem::path& file, const std::vector<GameEntry>& games);
// ROM-like files (.z64, .v64, .n64, .rom, .bin) in dir and up to 3 folders
// below it, sorted by path; at most maxFiles (truncated says so). A folder
// that can't be listed is skipped and counted in skippedDirs.
struct RomCandidates {
    std::vector<std::filesystem::path> files;
    bool truncated = false;
    size_t skippedDirs = 0;
};
RomCandidates rom_candidates(const std::filesystem::path& dir, size_t maxFiles = 4000);

// Background scan, one at a time; poll romscan_status() each frame.
enum class RomScanPhase { Idle, Scanning, Done };
struct RomScanStatus {
    RomScanPhase phase = RomScanPhase::Idle;
    std::filesystem::path dir;
    size_t done = 0, total = 0;        // total stays 0 while the folders are listed
    bool truncated = false;            // more files than the scan takes
    size_t skippedDirs = 0;            // folders that couldn't be listed
    unsigned generation = 0;           // changes whenever anything above does
    std::vector<RomScanEntry> results;  // complete once phase == Done
};
bool romscan_start(const std::filesystem::path& dir, const std::vector<GameEntry>& games);
RomScanStatus romscan_status();
unsigned romscan_generation();  // cheap: compare before copying romscan_status()
void romscan_clear();
// Stops a running scan between files and waits for it (call before exit).
void romscan_shutdown();

// Sets a game up from a scanned base dump: writes the ROM in .z64 order to the
// game's ROM path (re-checking its SHA1 on the way). Never overwrites a ROM
// the game already has.
bool rom_provision(GameEntry& g, const RomScanEntry& e, std::wstring* errorOut);
// The file's bytes in .z64 order (from any byte order) when their SHA1 is sha1 (upper-case hex), else false: the card's
// Select ROM takes a byte-swapped copy of the accepted dump as the scan does (aki-roadmap#274).
bool rom_read_matching_z64(const std::filesystem::path& file, const std::wstring& sha1, std::vector<unsigned char>* z64);
// A zip file (by its signature). The scan, its Use button and Select ROM read the one N64 ROM inside a zip as that
// ROM (aki-roadmap#280); a zip with none or several says so.
bool rom_is_zip(const std::filesystem::path& file);
// The ROM in .z64 order (any byte order, or the one ROM in a zip), with no SHA1 check; false if it isn't one.
bool rom_read_any_z64(const std::filesystem::path& file, std::vector<unsigned char>* z64);
// SHA1 of the ROM in .z64 order (byte order undone, the ROM inside a zip), or "" when it isn't one.
std::wstring rom_z64_sha1(const std::filesystem::path& file);
