#pragma once
#include <functional>
#include <string>
#include <vector>

// Which ROM file the launcher hands a game (aki-roadmap#216). Pure decisions, no file system: the callers pass
// how to test a path and how to hash it, and the rules are unit-tested in tests/rom_rules_tests.cpp.
namespace rom_rules {

enum class RomStatus {
    Missing,  // no file at all; path = where a picked ROM would go
    Good,     // the file is the dump the port was recompiled from (or the game has no known SHA1)
    Wrong,    // a file exists, but it isn't that dump
};

struct RomCheck {
    std::wstring path;
    RomStatus status = RomStatus::Missing;
    std::wstring sha1;  // of path; "" when it wasn't hashed or couldn't be read
};

struct RomResolution {
    RomCheck rom;      // the file to use
    RomCheck autoRom;  // the file to use without an override
    bool overrideUsed = false;
    bool overrideMissing = false;  // rom= names a file that isn't there, so it's ignored

    // A wrong ROM would make the game exit at once, so it can't be played. A wrong rom= override still can: the
    // user chose that file on purpose (an older build of the port can need another dump).
    bool blocked() const {
        return rom.status == RomStatus::Missing || (rom.status == RomStatus::Wrong && !overrideUsed);
    }
};

using ExistsFn = std::function<bool(const std::wstring&)>;
using Sha1Fn = std::function<std::wstring(const std::wstring&)>;

// Without an override: the first candidate holding the right dump, whatever comes before it. If none does, the
// first file that exists (reported Wrong), and if there's no file, the first candidate (reported Missing).
// An empty expectedSha1 accepts any file and hashes nothing.
RomResolution resolve_rom_choice(const std::wstring& overridePath, const std::vector<std::wstring>& candidates,
                                 const std::wstring& expectedSha1, const ExistsFn& exists, const Sha1Fn& sha1);

// True when a port's log says it refused the ROM it was given ("WCW_AUTOBOOT ROM validation failed").
bool log_says_rom_refused(const std::vector<std::string>& logLines);

}  // namespace rom_rules
