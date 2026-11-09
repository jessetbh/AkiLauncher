// Unit tests for the launcher's ROM choice (aki-roadmap#216). No game, no ROM, no file system.
// Every launcher build runs them, and a failure fails the build.
#include "rom_rules.h"

#include <cstdio>
#include <map>
#include <set>

using namespace rom_rules;

static int g_failed = 0;
static int g_run = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_run;                                                          \
        if (!(cond)) {                                                    \
            ++g_failed;                                                   \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

static const std::wstring RETAIL = L"442D";
static const std::wstring HACK = L"D7D1";

// A pretend disk: path -> SHA1. Records which files were hashed.
struct Disk {
    std::map<std::wstring, std::wstring> files;
    std::set<std::wstring> hashed;
    RomResolution resolve(const std::wstring& overridePath, const std::vector<std::wstring>& candidates,
                          const std::wstring& expected = RETAIL) {
        return resolve_rom_choice(
            overridePath, candidates, expected, [this](const std::wstring& p) { return files.count(p) != 0; },
            [this](const std::wstring& p) {
                hashed.insert(p);
                return files.at(p);
            });
    }
};

static const std::vector<std::wstring> CANDIDATES = {L"checkout\\wm2k.z64", L"data\\wwf.wm2k.us.z64", L"main\\wm2k.z64"};

// The #216 bug: an exe= override on a fullconv checkout, main checkout's wm2k.z64 still the hack, the retail ROM
// imported into the fullconv checkout. The launcher passed the hack and the game exited at once.
static void imported_retail_beats_the_main_checkouts_hack() {
    Disk d;
    d.files = {{L"data\\wwf.wm2k.us.z64", RETAIL}, {L"main\\wm2k.z64", HACK}};
    RomResolution r = d.resolve(L"", CANDIDATES);
    CHECK(r.rom.path == L"data\\wwf.wm2k.us.z64");
    CHECK(r.rom.status == RomStatus::Good);
    CHECK(!r.blocked());
}

static void a_wrong_file_early_in_the_list_doesnt_hide_the_right_one() {
    Disk d;
    d.files = {{L"checkout\\wm2k.z64", HACK}, {L"main\\wm2k.z64", RETAIL}};
    RomResolution r = d.resolve(L"", CANDIDATES);
    CHECK(r.rom.path == L"main\\wm2k.z64");
    CHECK(!r.blocked());
}

static void first_right_file_wins_and_later_ones_arent_read() {
    Disk d;
    d.files = {{L"checkout\\wm2k.z64", RETAIL}, {L"data\\wwf.wm2k.us.z64", RETAIL}};
    RomResolution r = d.resolve(L"", CANDIDATES);
    CHECK(r.rom.path == L"checkout\\wm2k.z64");
    CHECK(d.hashed.count(L"data\\wwf.wm2k.us.z64") == 0);
}

static void only_wrong_files_blocks_play_and_reports_the_first_with_its_sha1() {
    Disk d;
    d.files = {{L"data\\wwf.wm2k.us.z64", HACK}, {L"main\\wm2k.z64", L"FFFF"}};
    RomResolution r = d.resolve(L"", CANDIDATES);
    CHECK(r.rom.status == RomStatus::Wrong);
    CHECK(r.rom.path == L"data\\wwf.wm2k.us.z64");
    CHECK(r.rom.sha1 == HACK);
    CHECK(r.blocked());
}

static void no_file_blocks_play_and_points_at_the_first_candidate() {
    Disk d;
    RomResolution r = d.resolve(L"", CANDIDATES);
    CHECK(r.rom.status == RomStatus::Missing);
    CHECK(r.rom.path == L"checkout\\wm2k.z64");
    CHECK(r.blocked());
}

static void an_unreadable_file_is_wrong() {
    Disk d;
    d.files = {{L"checkout\\wm2k.z64", L""}};
    RomResolution r = d.resolve(L"", CANDIDATES);
    CHECK(r.rom.status == RomStatus::Wrong);
    CHECK(r.blocked());
}

static void override_is_honoured_ahead_of_a_right_candidate() {
    Disk d;
    d.files = {{L"mine.z64", RETAIL}, {L"checkout\\wm2k.z64", RETAIL}};
    RomResolution r = d.resolve(L"mine.z64", CANDIDATES);
    CHECK(r.rom.path == L"mine.z64");
    CHECK(r.overrideUsed);
    CHECK(r.autoRom.path == L"checkout\\wm2k.z64");
    CHECK(!r.blocked());
}

static void wrong_override_is_used_and_flagged_but_not_blocked() {
    Disk d;
    d.files = {{L"mine.z64", HACK}, {L"checkout\\wm2k.z64", RETAIL}};
    RomResolution r = d.resolve(L"mine.z64", CANDIDATES);
    CHECK(r.rom.path == L"mine.z64");
    CHECK(r.rom.status == RomStatus::Wrong);
    CHECK(r.rom.sha1 == HACK);
    CHECK(r.overrideUsed);
    CHECK(!r.blocked());
}

static void missing_override_falls_back_and_says_so() {
    Disk d;
    d.files = {{L"checkout\\wm2k.z64", RETAIL}};
    RomResolution r = d.resolve(L"gone.z64", CANDIDATES);
    CHECK(r.rom.path == L"checkout\\wm2k.z64");
    CHECK(r.overrideMissing);
    CHECK(!r.overrideUsed);
    CHECK(!r.blocked());
}

static void no_known_sha1_accepts_the_first_file_without_hashing() {
    Disk d;
    d.files = {{L"data\\wwf.wm2k.us.z64", HACK}};
    RomResolution r = d.resolve(L"", CANDIDATES, L"");
    CHECK(r.rom.path == L"data\\wwf.wm2k.us.z64");
    CHECK(r.rom.status == RomStatus::Good);
    CHECK(d.hashed.empty());
}

static void the_ports_refusal_line_is_recognised() {
    CHECK(log_says_rom_refused({"booting", "WCW_AUTOBOOT ROM validation failed (err=5) for C:\\x\\wm2k.z64."}));
    CHECK(!log_says_rom_refused({"booting", "Unhandled exception"}));
    CHECK(!log_says_rom_refused({}));
}

// aki-roadmap#219: an imported retail copy is in use and the checkout's own ROM file is the hack. Reset, then
// closing Settings, must save no rom= override (it used to save the checkout's file, which the port refused).
static void settings_reset_saves_no_override_when_an_imported_copy_is_used() {
    Disk d;
    d.files = {{L"checkout\\wm2k.z64", HACK}, {L"data\\wwf.wm2k.us.z64", RETAIL}};
    RomResolution r = d.resolve(L"", CANDIDATES);
    CHECK(r.autoRom.path == L"data\\wwf.wm2k.us.z64");
    const std::wstring field = rom_field_reset(r.autoRom.path);
    CHECK(rom_override_from_field(field, r.autoRom.path).empty());
    // The old Reset value is another file, so it would have become the override.
    CHECK(rom_override_from_field(L"checkout\\wm2k.z64", r.autoRom.path) == L"checkout\\wm2k.z64");
}

static void rom_field_saves_an_override_only_for_another_file() {
    CHECK(rom_override_from_field(L"", L"checkout\\wm2k.z64").empty());
    CHECK(rom_override_from_field(L"Checkout/WM2K.z64", L"checkout\\wm2k.z64").empty());
    CHECK(rom_override_from_field(L"mine.z64", L"checkout\\wm2k.z64") == L"mine.z64");
}

// aki-roadmap#263: closing Settings compared the EXE field with the registry's depot path (<install>\build-msvc\X.exe),
// which a launcher-installed game never has (its exe is at the install folder's root), so every installed game got an
// absolute exe= override and a portable launcher folder couldn't move. The default is the resolved install exe.
static void settings_saves_no_exe_override_for_an_untouched_installed_game() {
    const std::wstring installExe = L"C:\\Games\\AKI\\games\\nomercy\\NoMercyRecompiled.exe";
    CHECK(exe_field_reset(installExe) == installExe);
    CHECK(exe_override_from_field(exe_field_reset(installExe), installExe).empty());
    CHECK(exe_override_from_field(L"c:/games/aki/GAMES/nomercy/NoMercyRecompiled.exe", installExe).empty());
    CHECK(exe_override_from_field(L"", installExe).empty());
    CHECK(exe_override_from_field(L"D:\\dev\\NoMercyRecomp\\build-msvc\\NoMercyRecompiled.exe", installExe) ==
          L"D:\\dev\\NoMercyRecomp\\build-msvc\\NoMercyRecompiled.exe");
}

int main() {
    settings_saves_no_exe_override_for_an_untouched_installed_game();
    settings_reset_saves_no_override_when_an_imported_copy_is_used();
    rom_field_saves_an_override_only_for_another_file();
    imported_retail_beats_the_main_checkouts_hack();
    a_wrong_file_early_in_the_list_doesnt_hide_the_right_one();
    first_right_file_wins_and_later_ones_arent_read();
    only_wrong_files_blocks_play_and_reports_the_first_with_its_sha1();
    no_file_blocks_play_and_points_at_the_first_candidate();
    an_unreadable_file_is_wrong();
    override_is_honoured_ahead_of_a_right_candidate();
    wrong_override_is_used_and_flagged_but_not_blocked();
    missing_override_falls_back_and_says_so();
    no_known_sha1_accepts_the_first_file_without_hashing();
    the_ports_refusal_line_is_recognised();
    std::printf("[launcher rom tests] %d checks, %d failed\n", g_run, g_failed);
    return g_failed ? 1 : 0;
}
