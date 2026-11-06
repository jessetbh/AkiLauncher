#include "rom_rules.h"

namespace rom_rules {

static RomCheck check_file(const std::wstring& path, const std::wstring& expectedSha1, const Sha1Fn& sha1) {
    RomCheck c;
    c.path = path;
    if (expectedSha1.empty()) {
        c.status = RomStatus::Good;
        return c;
    }
    c.sha1 = sha1(path);
    c.status = c.sha1 == expectedSha1 ? RomStatus::Good : RomStatus::Wrong;
    return c;
}

RomResolution resolve_rom_choice(const std::wstring& overridePath, const std::vector<std::wstring>& candidates,
                                 const std::wstring& expectedSha1, const ExistsFn& exists, const Sha1Fn& sha1) {
    RomResolution r;

    bool haveFile = false;
    for (const std::wstring& path : candidates) {
        if (!exists(path)) continue;
        RomCheck c = check_file(path, expectedSha1, sha1);
        if (c.status == RomStatus::Good) {
            r.autoRom = c;
            haveFile = true;
            break;
        }
        if (!haveFile) r.autoRom = c;
        haveFile = true;
    }
    if (!haveFile && !candidates.empty()) r.autoRom.path = candidates.front();

    r.rom = r.autoRom;
    if (!overridePath.empty()) {
        if (exists(overridePath)) {
            r.rom = check_file(overridePath, expectedSha1, sha1);
            r.overrideUsed = true;
        } else {
            r.overrideMissing = true;
        }
    }
    return r;
}

bool log_says_rom_refused(const std::vector<std::string>& logLines) {
    for (const std::string& line : logLines) {
        if (line.find("ROM validation failed") != std::string::npos) return true;
    }
    return false;
}

}  // namespace rom_rules
