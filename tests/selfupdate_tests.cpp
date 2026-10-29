// Checks for AKI Launcher's self-update (aki-roadmap#117): versions, picking a
// release from a fake feed, the dev-build rule, which zip entries may be
// written, staging (size + SHA256SUMS), the swap and the rollback. No network:
// the feed is a string and the "app folder" a temp folder with fake files.
#include "../src/installer.h"
#include "../src/selfupdate.h"
#include "check.h"

#include <windows.h>

#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "miniz.h"

namespace fs = std::filesystem;

namespace {
void put(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
}
std::string get(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
std::vector<uint8_t> make_zip(const std::vector<std::pair<std::string, std::string>>& entries) {
    mz_zip_archive za{};
    mz_zip_writer_init_heap(&za, 0, 0);
    for (auto& [name, data] : entries)
        mz_zip_writer_add_mem(&za, name.c_str(), data.data(), data.size(), MZ_DEFAULT_COMPRESSION);
    void* buf = nullptr;
    size_t size = 0;
    mz_zip_writer_finalize_heap_archive(&za, &buf, &size);
    std::vector<uint8_t> out((uint8_t*)buf, (uint8_t*)buf + size);
    mz_zip_writer_end(&za);
    return out;
}
Version ver(const wchar_t* s) {
    Version v;
    parse_version(s, &v);
    return v;
}

// A /releases page like GitHub's (newest first; a draft; a pre-release; one without a zip).
const char* kFeed = R"([
  {"tag_name":"v0.3.0-beta.1","name":"0.3.0 beta","body":"Beta notes","prerelease":true,"draft":false,
   "html_url":"https://example/0.3.0-beta.1",
   "assets":[{"name":"AkiLauncher-v0.3.0-beta.1-Windows.zip","browser_download_url":"https://x/b.zip","size":10},
             {"name":"SHA256SUMS","browser_download_url":"https://x/b.sums","size":1}]},
  {"tag_name":"v0.9.0","name":"Draft","draft":true,"assets":[{"name":"AkiLauncher-v0.9.0-Windows.zip","browser_download_url":"https://x/d.zip"}]},
  {"tag_name":"v0.2.1","name":"AKI Launcher 0.2.1","body":"Fixes:\r\n- one\r\n- two","prerelease":false,
   "assets":[{"name":"AkiLauncher-v0.2.1-Windows.zip","browser_download_url":"https://x/a.zip","size":1234},
             {"name":"SHA256SUMS","browser_download_url":"https://x/a.sums","size":90}]},
  {"tag_name":"v0.2.0","name":"no zip","assets":[{"name":"notes.txt","browser_download_url":"https://x/n.txt"}]},
  {"tag_name":"nightly","name":"not a version","assets":[{"name":"AkiLauncher-nightly-Windows.zip","browser_download_url":"https://x/n.zip"}]},
  {"tag_name":"v0.1.4","name":"0.1.4","assets":[{"name":"AkiLauncher-v0.1.4-Windows.zip","browser_download_url":"https://x/o.zip","size":5}]}
])";
}  // namespace

static void test_versions() {
    Version v;
    CHECK(parse_version(L"v0.2.0", &v) && v.major == 0 && v.minor == 2 && v.patch == 0 && v.pre.empty());
    CHECK(parse_version(L"1.10.3-beta.2", &v) && v.minor == 10 && v.pre == "beta.2");
    for (const wchar_t* bad : {L"", L"v1", L"1.2", L"1.2.x", L"1.2.3-", L"1.2.3+meta", L"nightly", L"1.2.3 beta"})
        CHECK(!parse_version(bad, &v));
    CHECK(compare_versions(ver(L"0.2.0"), ver(L"0.1.4")) > 0);
    CHECK(compare_versions(ver(L"0.10.0"), ver(L"0.9.9")) > 0);  // numbers, not text
    CHECK(compare_versions(ver(L"0.2.0-beta.1"), ver(L"0.2.0")) < 0);
    CHECK(compare_versions(ver(L"0.2.0-beta.2"), ver(L"0.2.0-beta.10")) < 0);
    CHECK(compare_versions(ver(L"0.2.0-alpha"), ver(L"0.2.0-beta")) < 0);
    CHECK(compare_versions(ver(L"0.2.0-beta"), ver(L"0.2.0-beta.1")) < 0);
    CHECK(compare_versions(ver(L"v1.0.0"), ver(L"1.0.0")) == 0);
    CHECK(version_text(ver(L"v0.3.0-rc.1")) == L"0.3.0-rc.1");
}

static void test_feed() {
    auto rels = parse_launcher_releases(kFeed);
    CHECK(rels.size() == 3);  // beta, 0.2.1, 0.1.4 (no draft, no zip-less, no non-version tag)
    const LauncherRelease* r = pick_launcher_update(rels, ver(L"0.1.4"), false, L"");
    CHECK(r && r->tag == L"v0.2.1" && r->zipSize == 1234 && r->sumsUrl == L"https://x/a.sums");
    CHECK(r && r->zipName == L"AkiLauncher-v0.2.1-Windows.zip" && r->notes == L"Fixes:\r\n- one\r\n- two");
    // The pre-release channel offers the beta.
    r = pick_launcher_update(rels, ver(L"0.1.4"), true, L"");
    CHECK(r && r->tag == L"v0.3.0-beta.1");
    // Up to date, or newer than everything: nothing.
    CHECK(!pick_launcher_update(rels, ver(L"0.2.1"), false, L""));
    CHECK(!pick_launcher_update(rels, ver(L"0.4.0"), true, L""));
    // Skip this version: hidden, but a newer one would still show.
    CHECK(!pick_launcher_update(rels, ver(L"0.1.4"), false, L"v0.2.1"));
    r = pick_launcher_update(rels, ver(L"0.1.4"), true, L"v0.2.1");
    CHECK(r && r->tag == L"v0.3.0-beta.1");
    CHECK(parse_launcher_releases("{not json").empty() && parse_launcher_releases("{}").empty());
}

static void test_dev_rule(const fs::path& dir) {
    put(dir / L"pkg" / L"AkiLauncher.exe", "exe");
    CHECK(!is_dev_build(dir / L"pkg"));
    put(dir / L"checkout" / L"CMakeLists.txt", "");
    fs::create_directories(dir / L"checkout" / L".git");
    put(dir / L"checkout" / L"build-msvc" / L"AkiLauncher.exe", "exe");
    CHECK(is_dev_build(dir / L"checkout" / L"build-msvc"));
    put(dir / L"other" / L"CMakeCache.txt", "");
    CHECK(is_dev_build(dir / L"other"));
}

static void test_entries() {
    for (const char* ok : {"AkiLauncher.exe", "README.txt", "assets/boxart/nomercy_front.png", "assets/games.json"})
        CHECK(update_entry_allowed(ok));
    for (const char* no : {"settings.ini", "Settings.INI", "portable.txt", "games/nomercy/x.exe", "mods/a.nrm",
                           ".update/x", "../evil.exe", "/abs.exe", "C:/x.exe", "assets/../../x", "aki-launcher.log"})
        CHECK(!update_entry_allowed(no));
}

void run_selfupdate_tests(const fs::path& dir) {
    test_versions();
    test_feed();
    test_dev_rule(dir / L"dev");
    test_entries();

    // Staging: size and SHA256SUMS must both match; player data in the zip is never staged.
    auto zip = make_zip({{"AkiLauncher.exe", "NEW EXE"},
                         {"assets/games.json", "{\"new\":1}"},
                         {"assets/boxart/new_front.png", "png"},
                         {"settings.ini", "hijack=1"},
                         {"games/nomercy/evil.exe", "x"},
                         {"../outside.txt", "x"}});
    const std::wstring zipName = L"AkiLauncher-v0.2.1-Windows.zip";
    std::string sums = sha256_hex(zip) + "  AkiLauncher-v0.2.1-Windows.zip\n";
    fs::path staging = dir / L"app" / L".update" / L"staged";
    std::wstring err;
    CHECK(!stage_update(zip, (long long)zip.size() + 1, sums, zipName, staging, &err) && !err.empty());
    CHECK(!stage_update(zip, (long long)zip.size(), "", zipName, staging, &err));
    std::string wrong = std::string(64, '0') + "  AkiLauncher-v0.2.1-Windows.zip\n";
    CHECK(!stage_update(zip, (long long)zip.size(), wrong, zipName, staging, &err) && !fs::exists(staging));
    auto noExe = make_zip({{"README.txt", "x"}});
    CHECK(!stage_update(noExe, (long long)noExe.size(), sha256_hex(noExe) + "  " + "AkiLauncher-v0.2.1-Windows.zip\n",
                        zipName, staging, &err) &&
          err.find(L"AkiLauncher.exe") != std::wstring::npos);
    CHECK(stage_update(zip, (long long)zip.size(), sums, zipName, staging, &err));
    CHECK(get(staging / L"AkiLauncher.exe") == "NEW EXE" && fs::exists(staging / L"assets" / L"boxart" / L"new_front.png"));
    CHECK(!fs::exists(staging / L"settings.ini") && !fs::exists(staging / L"games") && !fs::exists(dir / L"app" / L"outside.txt"));

    // The app folder as installed, with the player's things next to it.
    fs::path app = dir / L"app", previous = app / L".update" / L"previous";
    put(app / L"AkiLauncher.exe", "OLD EXE");
    put(app / L"assets" / L"games.json", "{\"old\":1}");
    put(app / L"assets" / L"boxart" / L"nomercy_front.png", "old art");
    put(app / L"settings.ini", "mine=1");
    put(app / L"games" / L"nomercy" / L"save.bin", "save");

    // Swap: replaced files move to previous, new files land, the rest is untouched.
    CHECK(apply_staged_update(app, staging, previous, &err));
    CHECK(get(app / L"AkiLauncher.exe") == "NEW EXE" && get(app / L"assets" / L"games.json") == "{\"new\":1}");
    CHECK(fs::exists(app / L"assets" / L"boxart" / L"new_front.png"));
    CHECK(get(previous / L"AkiLauncher.exe") == "OLD EXE" && get(previous / L"assets" / L"games.json") == "{\"old\":1}");
    CHECK(get(app / L"settings.ini") == "mine=1" && get(app / L"games" / L"nomercy" / L"save.bin") == "save");
    CHECK(get(app / L"assets" / L"boxart" / L"nomercy_front.png") == "old art");  // not in the update: kept

    // Rollback (the new version didn't come up): exactly the old files again.
    CHECK(restore_previous(app, staging, previous, &err));
    CHECK(get(app / L"AkiLauncher.exe") == "OLD EXE" && get(app / L"assets" / L"games.json") == "{\"old\":1}");
    CHECK(!fs::exists(app / L"assets" / L"boxart" / L"new_front.png"));  // added by the update: gone
    CHECK(get(app / L"settings.ini") == "mine=1" && get(app / L"games" / L"nomercy" / L"save.bin") == "save");

    // A swap that can't move a file aside puts everything back and says so.
    {
        // Hold the exe open without FILE_SHARE_DELETE: renaming it fails.
        HANDLE h = CreateFileW((app / L"AkiLauncher.exe").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        CHECK(!apply_staged_update(app, staging, previous, &err) && !err.empty());
        CloseHandle(h);
        CHECK(get(app / L"AkiLauncher.exe") == "OLD EXE" && get(app / L"assets" / L"games.json") == "{\"old\":1}");
        CHECK(!fs::exists(app / L"assets" / L"boxart" / L"new_front.png"));
    }
}
