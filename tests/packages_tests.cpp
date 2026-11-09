// Checks for the shared package installer (src/packages.cpp, aki-roadmap#154): the manifest rules against AkiTools'
// shared fixtures, packages.json (unknown entries and fields kept, newer or broken files left alone), installs of
// every kind, upgrades (unedited replaced, edited kept as .edited-<date>), downgrades, the game and ROM checks,
// patch-mod ROM hacks as conversions, removing, and the conversions list. Synthetic files only.
#include "../src/json.h"
#include "../src/packages.h"
#include "check.h"

#include <windows.h>

#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "miniz.h"

namespace fs = std::filesystem;
using Outcome = pkg::PackageInstall::Outcome;

#ifndef AKI_TESTS_DIR
#error AKI_TESTS_DIR (the tests folder) is set by CMakeLists.txt
#endif

static void put(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
}
static std::string get(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static std::string zip_bytes(const std::vector<std::pair<std::string, std::string>>& entries) {
    mz_zip_archive za{};
    mz_zip_writer_init_heap(&za, 0, 0);
    for (auto& [name, data] : entries) mz_zip_writer_add_mem(&za, name.c_str(), data.data(), data.size(), MZ_DEFAULT_COMPRESSION);
    void* buf = nullptr;
    size_t size = 0;
    mz_zip_writer_finalize_heap_archive(&za, &buf, &size);
    std::string out((const char*)buf, size);
    mz_zip_writer_end(&za);
    return out;
}
static bool has(const std::wstring& s, const wchar_t* part) { return s.find(part) != std::wstring::npos; }

static const std::string kPng = std::string("\x89PNG\r\n\x1a\n", 8) + std::string(24, '\0');
static const std::string kRomSha1 = "0123456789abcdef0123456789abcdef01234567";

static std::string mod_json(const std::string& id, const std::string& version = "1.0.0") {
    return R"({"id":")" + id + R"(","version":")" + version + R"(","game_id":"wwfnomercy","display_name":")" + id +
           R"(","authors":["T"]})";
}
static std::string manifest(const std::string& kind, const std::string& id, const std::string& version, const std::string& payload,
                            const std::string& extra = "") {
    return R"({"format":1,"kind":")" + kind + R"(","id":")" + id + R"(","version":")" + version +
           R"(","game_id":"wwfnomercy","display_name":"Demo )" + id + R"(","authors":["T"],"license":"CC0-1.0","thumb":"thumb.png",)"
           R"("payload":)" + payload + extra + "}";
}
// A package folder: aki-package.json, thumb.png and the given files.
static fs::path package(const fs::path& dir, const std::string& man, const std::vector<std::pair<std::string, std::string>>& files) {
    std::error_code ec;
    fs::remove_all(dir, ec);
    put(dir / L"aki-package.json", man);
    put(dir / L"thumb.png", kPng);
    for (auto& [rel, data] : files) put(dir / fs::u8path(rel), data);
    return dir;
}
static fs::path addon(const fs::path& dir, const std::string& id, const std::string& version, const std::string& binary = "B1") {
    return package(dir, manifest("addon", id, version, R"({"nrm":"demo.nrm"})"),
                   {{"demo.nrm", zip_bytes({{"mod.json", mod_json(id, version)}, {"mod_binary.bin", binary}})}});
}
static fs::path conversion(const fs::path& dir, const std::string& id, const std::string& version, const std::string& sha1,
                           const std::string& codes = "810C0000 0001\n") {
    return package(dir,
                   manifest("conversion", id, version, R"({"folder":"conversion/","addons":["conv_tex.rtz"]})",
                            R"(,"base_rom":{"sha1":[")" + sha1 + R"("],"name":"Test ROM"})"),
                   {{"conversion/aki.json", R"({"format":"aki-conversion","id":")" + id + R"(","name":"Conv )" + id +
                                                R"(","version":")" + version + R"(","base_rom":{"sha1":")" + sha1 + R"("}})"},
                    {"conversion/codes.txt", codes},
                    {"conv_tex.rtz", zip_bytes({{"rt64.json", "{}"}})}});
}

static pkg::InstallTarget target(const fs::path& data) {
    pkg::InstallTarget t;
    t.dataDir = data;
    t.gameTitle = L"WWF No Mercy";
    t.gameId = "wwfnomercy";
    t.romSha1 = {kRomSha1};
    return t;
}

static void test_fixtures() {
    // The launcher refuses exactly the fixtures aki-package.py refuses (exit 2), and installs the rest.
    fs::path dir = fs::path(AKI_TESTS_DIR) / L"fixtures" / L"packages";
    JsonValue exp;
    CHECK(json_parse(get(dir / L"expected.json"), &exp));
    const JsonValue* fx = exp.get("fixtures");
    CHECK(fx && fx->items.size() >= 20);
    int checked = 0;
    for (size_t i = 0; fx && i < fx->items.size(); ++i) {
        bool refuse = fx->items[i].integer("exit", 0) == 2;
        pkg::CheckResult r = pkg::check_package(dir / fs::u8path(fx->keys[i]));
        if (r.ok() == refuse) printf("  fixture %s: launcher %s, expected %s\n", fx->keys[i].c_str(), r.ok() ? "accepts" : "refuses",
                                     refuse ? "refuse" : "accept");
        CHECK(r.ok() != refuse);
        ++checked;
    }
    CHECK(checked >= 20);
}

static void test_rules() {
    CHECK(pkg::valid_id("nmp_v3") && pkg::valid_id(std::string(48, 'a')) && pkg::valid_id("a.b-c_d"));
    CHECK(!pkg::valid_id(std::string(49, 'a')) && !pkg::valid_id("demo.") && !pkg::valid_id("Demo") && !pkg::valid_id("con") &&
          !pkg::valid_id("lpt1.x") && !pkg::valid_id("") && !pkg::valid_id("-x"));
    CHECK(pkg::valid_version("1.2.3") && pkg::valid_version("1.2.3-beta.1") && !pkg::valid_version("1.2"));
    CHECK(pkg::compare_versions("1.10.0", "1.9.0") > 0 && pkg::compare_versions("1.0.0-beta", "1.0.0") < 0 &&
          pkg::compare_versions("2.0.0", "2.0.0") == 0);
    CHECK(pkg::runtime_game_id("nomercy") == "wwfnomercy" && pkg::runtime_game_id("vpw2") == "vpw2" && pkg::runtime_game_id("x").empty());
    // Fields of the wrong type are problems, not crashes; "format": true is not 1.
    pkg::Manifest m = pkg::parse_manifest(R"({"format":true,"kind":"addon"})");
    CHECK(!m.ok());
    m = pkg::parse_manifest(R"({"format":1,"kind":[],"id":5,"version":{},"game_id":"nomercy","display_name":"","authors":"me","payload":[1]})");
    CHECK(!m.ok() && m.problems.size() >= 6);
    CHECK(has(m.first_error(), L"kind"));
    m = pkg::parse_manifest("not json");
    CHECK(!m.parsed && !m.ok());
    // A conversion needs base_rom; an unused payload key is a warning.
    m = pkg::parse_manifest(manifest("conversion", "c", "1.0.0", R"({"folder":"conversion/","nrm":"x.nrm"})"));
    bool baseRom = false, unusedWarn = false;
    for (auto& p : m.problems) {
        baseRom = baseRom || (p.error && p.field == "base_rom");
        unusedWarn = unusedWarn || (!p.error && has(p.text, L"payload.nrm is not used"));
    }
    CHECK(baseRom && unusedWarn);
}

static void test_registry(const fs::path& tmp) {
    fs::path f = tmp / L"reg" / L"packages.json";
    // Missing: empty, may be written.
    pkg::Registry r = pkg::load_registry(f);
    CHECK(r.status == pkg::Registry::Status::Missing && r.may_save());
    // Unknown top-level keys, unknown entry kinds and unknown fields survive a load and save.
    put(f, R"({"format":1,"future":{"x":1},"packages":[)"
           R"({"id":"a","kind":"addon","version":"1.0.0","source":"launcher","installed":"t","files":[{"path":"mods/a.nrm","hash":"h","extra":7}],"note":"keep"},)"
           R"({"id":"z","kind":"shader-pack","version":"9.0.0"}]})");
    r = pkg::load_registry(f);
    CHECK(r.status == pkg::Registry::Status::Ok && r.entries.size() == 1 && r.others.size() == 1);
    CHECK(r.owner("MODS/A.NRM/") == &r.entries[0] && !r.owner("mods/b.nrm"));
    r.entries[0].version = "1.1.0";
    r.entries[0].changed = true;  // unchanged entries go back exactly as read
    std::wstring err;
    CHECK(pkg::save_registry(f, r, &err));
    std::string text = get(f);
    CHECK(text.find("\"future\"") != std::string::npos && text.find("\"note\": \"keep\"") != std::string::npos &&
          text.find("shader-pack") != std::string::npos && text.find("\"extra\": 7") != std::string::npos &&
          text.find("1.1.0") != std::string::npos);
    // A newer format, a broken file: never written over.
    for (const char* bad : {R"({"format":2,"packages":[]})", "{not json", R"({"packages":[]})", R"({"format":1,"packages":{}})"}) {
        put(f, bad);
        r = pkg::load_registry(f);
        CHECK(!r.may_save());
        CHECK(!pkg::save_registry(f, r, &err));
        CHECK(get(f) == bad);
    }
}

static void test_hash(const fs::path& tmp) {
    // librecomp's pack_hash, byte for byte (golden values from AkiMods pack_history.py, the game's other twin).
    put(tmp / L"g" / L"pack.akimodel" / L"pack.json", "{\"id\": \"x\"}\r\n");
    put(tmp / L"g" / L"pack.akimodel" / L"sub" / L"model.bin", std::string("\x00\x01\r\n", 4));
    CHECK(pkg::pack_hash(tmp / L"g" / L"pack.akimodel") == "541a1356f7cb5eb8");
    put(tmp / L"g" / L"a.nrm", "zip bytes");
    CHECK(pkg::pack_hash(tmp / L"g" / L"a.nrm") == "6730593789596cdf");   // a file: one entry named by its file name
    put(tmp / L"g" / L"notes.txt", "line1\r\nline2\r\r\nend\r");
    CHECK(pkg::pack_hash(tmp / L"g" / L"notes.txt") == "c2ca3c7ea557d4bc");
    // CRLF-blind for text, not for binaries (same names in two folders).
    put(tmp / L"h" / L"a" / L"x.txt", "one\r\ntwo\r\n");
    put(tmp / L"h" / L"b" / L"x.txt", "one\ntwo\n");
    CHECK(pkg::pack_hash(tmp / L"h" / L"a" / L"x.txt") == pkg::pack_hash(tmp / L"h" / L"b" / L"x.txt"));
    put(tmp / L"h" / L"a" / L"x.bin", "one\r\n");
    put(tmp / L"h" / L"b" / L"x.bin", "one\n");
    CHECK(pkg::pack_hash(tmp / L"h" / L"a" / L"x.bin") != pkg::pack_hash(tmp / L"h" / L"b" / L"x.bin"));
    put(tmp / L"h1" / L"x" / L"f.txt", "1");
    put(tmp / L"h2" / L"x" / L"f.txt", "1");
    CHECK(pkg::pack_hash(tmp / L"h1") == pkg::pack_hash(tmp / L"h2") && pkg::pack_hash(tmp / L"h1").size() == 16);
    put(tmp / L"h2" / L"x" / L"g.txt", "");
    CHECK(pkg::pack_hash(tmp / L"h1") != pkg::pack_hash(tmp / L"h2"));
    CHECK(pkg::pack_hash(tmp / L"h" / L"missing").empty());
}


static void test_install_and_upgrade(const fs::path& tmp) {
    fs::path data = tmp / L"data";
    pkg::InstallTarget t = target(data);
    pkg::InstallOptions o;
    o.today = "20261029";
    // Install.
    pkg::PackageInstall r = pkg::install_package(addon(tmp / L"p1", "demo-addon", "1.0.0"), t, o);
    CHECK(r.outcome == Outcome::Installed);
    CHECK(has(r.message, L"Installed Demo demo-addon 1.0.0 (add-on, by T; CC0-1.0)"));
    CHECK(fs::exists(data / L"mods" / L"demo.nrm"));
    pkg::Registry reg = pkg::load_registry(data / L"packages.json");
    CHECK(reg.entries.size() == 1 && reg.entries[0].files.size() == 1 && reg.entries[0].files[0].path == "mods/demo.nrm" &&
          reg.entries[0].files[0].hash == pkg::pack_hash(data / L"mods" / L"demo.nrm") && reg.entries[0].source == "launcher");
    // No staging folder left.
    int stray = 0;
    for (auto& e : fs::directory_iterator(data)) stray += e.path().filename().wstring().rfind(L".aki-", 0) == 0;
    CHECK(stray == 0);
    // The same version: already installed, unless reinstalled.
    r = pkg::install_package(tmp / L"p1", t, o);
    CHECK(r.outcome == Outcome::AlreadyInstalled);
    pkg::InstallOptions re = o;
    re.reinstall = true;
    CHECK(pkg::install_package(tmp / L"p1", t, re).outcome == Outcome::Reinstalled);
    // A newer version over an unedited copy: replaced, nothing kept.
    r = pkg::install_package(addon(tmp / L"p2", "demo-addon", "1.1.0", "B2"), t, o);
    CHECK(r.outcome == Outcome::Upgraded && r.keptEdited.empty());
    CHECK(has(r.message, L"Updated Demo demo-addon 1.0.0 -> 1.1.0"));
    CHECK(get(data / L"mods" / L"demo.nrm").find("B2") != std::string::npos);
    // A newer version over an edited copy: the edit is kept as .edited-<date>.
    put(data / L"mods" / L"demo.nrm", "my own edit");
    r = pkg::install_package(addon(tmp / L"p3", "demo-addon", "1.2.0", "B3"), t, o);
    CHECK(r.outcome == Outcome::Upgraded && r.keptEdited.size() == 1);
    CHECK(get(data / L"kept" / L"mods" / L"demo.nrm.edited-20261029") == "my own edit");   // where no scan loads it twice
    CHECK(!fs::exists(data / L"mods" / L"demo.nrm.edited-20261029"));
    CHECK(has(r.message, L"1 edited file kept as .edited-20261029"));
    // An older version: needs confirming.
    r = pkg::install_package(tmp / L"p2", t, o);
    CHECK(r.outcome == Outcome::NeedsDowngradeConfirm);
    CHECK(get(data / L"mods" / L"demo.nrm").find("B3") != std::string::npos);
    pkg::InstallOptions down = o;
    down.allowDowngrade = true;
    CHECK(pkg::install_package(tmp / L"p2", t, down).outcome == Outcome::Upgraded);
    CHECK(pkg::load_registry(data / L"packages.json").entries[0].version == "1.1.0");
    // One id per game across kinds; one owner per path.
    r = pkg::install_package(package(tmp / L"p4", manifest("texture-pack", "demo-addon", "1.0.0", R"({"rtz":"t.rtz"})"),
                                     {{"t.rtz", zip_bytes({{"rt64.json", "{}"}})}}),
                             t, o);
    CHECK(r.outcome == Outcome::Refused && has(r.message, L"the id demo-addon is already taken by an add-on"));
    r = pkg::install_package(addon(tmp / L"p5", "other-addon", "1.0.0"), t, o);
    CHECK(r.outcome == Outcome::Refused && has(r.message, L"mods/demo.nrm belongs to demo-addon"));
    // replaces: the old package's files go, its entry too.
    r = pkg::install_package(package(tmp / L"p6", manifest("addon", "renamed-addon", "2.0.0", R"({"nrm":"demo.nrm"})", R"(,"replaces":["demo-addon"])"),
                                     {{"demo.nrm", zip_bytes({{"mod.json", mod_json("renamed-addon", "2.0.0")}, {"mod_binary.bin", "B4"}})}}),
                             t, o);
    CHECK(r.outcome == Outcome::Installed);
    reg = pkg::load_registry(data / L"packages.json");
    CHECK(reg.entries.size() == 1 && reg.entries[0].id == "renamed-addon");
}

static void test_refusals(const fs::path& tmp) {
    fs::path data = tmp / L"data-r";
    pkg::InstallTarget t = target(data);
    // Another game.
    fs::path p = package(tmp / L"r1", R"({"format":1,"kind":"addon","id":"x","version":"1.0.0","game_id":"vpw2","display_name":"X","authors":["T"],"payload":{"nrm":"x.nrm"}})",
                         {{"x.nrm", zip_bytes({{"mod.json", R"({"id":"x","version":"1.0.0","game_id":"vpw2"})"}, {"mod_binary.bin", "b"}})}});
    pkg::PackageInstall r = pkg::install_package(p, t);
    CHECK(r.outcome == Outcome::Refused && has(r.message, L"is for vpw2, not WWF No Mercy"));
    // A conversion for another ROM: refused, naming the ROM; for its own ROM: installed.
    r = pkg::install_package(conversion(tmp / L"r2", "conv-a", "1.0.0", std::string(40, 'f')), t);
    CHECK(r.outcome == Outcome::Refused && has(r.message, L"needs Test ROM"));
    // No known ROM hash: the target hashes the ROM (once, on demand).
    pkg::InstallTarget lazy = t;
    lazy.romSha1.clear();
    int calls = 0;
    lazy.hashRom = [&] { ++calls; return std::string("0123456789ABCDEF0123456789ABCDEF01234567"); };
    r = pkg::install_package(conversion(tmp / L"r3", "conv-a", "1.0.0", kRomSha1), lazy);
    CHECK(r.outcome == Outcome::Installed && calls == 1);
    CHECK(has(r.message, L"Play as..."));
    // An add-on for another ROM: installed with a note.
    r = pkg::install_package(package(tmp / L"r4", manifest("addon", "addon-b", "1.0.0", R"({"nrm":"b.nrm"})",
                                                           R"(,"base_rom":{"sha1":[")" + std::string(40, 'e') + R"("]})"),
                                     {{"b.nrm", zip_bytes({{"mod.json", mod_json("addon-b")}, {"mod_binary.bin", "b"}})}}),
                             t);
    CHECK(r.outcome == Outcome::Installed && !r.notes.empty() && has(r.notes[0], L"may not work with your ROM"));
    // The game running: nothing written.
    pkg::InstallOptions running;
    running.gameRunning = true;
    r = pkg::install_package(addon(tmp / L"r5", "addon-c", "1.0.0"), t, running);
    CHECK(r.outcome == Outcome::Refused && has(r.message, L"close WWF No Mercy first"));
    CHECK(!fs::exists(data / L"mods" / L"demo.nrm"));
    // A newer packages.json: nothing written.
    std::string newer = R"({"format":2,"packages":[]})";
    put(data / L"packages.json", newer);
    r = pkg::install_package(addon(tmp / L"r6", "addon-d", "1.0.0"), t);
    CHECK(r.outcome == Outcome::Refused && has(r.message, L"newer version"));
    CHECK(get(data / L"packages.json") == newer && !fs::exists(data / L"mods" / L"demo.nrm"));
    // Not a package.
    put(tmp / L"r7" / L"readme.txt", "hi");
    CHECK(pkg::install_package(tmp / L"r7", t).outcome == Outcome::Refused);
}

static void test_kinds_zip_and_conversions(const fs::path& tmp) {
    fs::path data = tmp / L"data-k";
    pkg::InstallTarget t = target(data);
    // A texture folder becomes mods\<id>.rtz.
    pkg::PackageInstall r = pkg::install_package(
        package(tmp / L"k1", manifest("texture-pack", "arena-tex", "1.0.0", R"({"folder":"textures/"})"),
                {{"textures/rt64.json", "{}"}, {"textures/sub/a.png", kPng}}),
        t);
    CHECK(r.outcome == Outcome::Installed);
    {
        pkg::Package rtz(data / L"mods" / L"arena-tex.rtz");
        CHECK(rtz.is_zip() && rtz.has("rt64.json") && rtz.has("sub/a.png"));
    }
    // A model pack (and its HD textures), from a zip.
    fs::path mp = package(tmp / L"k2", manifest("model-pack", "q-robot", "1.0.0", R"({"packs":["q-robot.akimodel"],"hd":["q-robot-hd.rtz"]})"),
                          {{"q-robot.akimodel/model.bin", "AKP1"}, {"q-robot.akimodel/pack.json", R"({"id":"q-robot","game":"nomercy"})"},
                           {"q-robot-hd.rtz", zip_bytes({{"rt64.json", "{}"}})}});
    std::vector<std::pair<std::string, std::string>> entries;
    for (auto& e : fs::recursive_directory_iterator(mp))
        if (e.is_regular_file()) {
            std::u8string rel = fs::relative(e.path(), mp).generic_u8string();
            entries.push_back({std::string((const char*)rel.c_str()), get(e.path())});
        }
    put(tmp / L"k2.zip", zip_bytes(entries));
    r = pkg::install_package(tmp / L"k2.zip", t);
    CHECK(r.outcome == Outcome::Installed);
    CHECK(fs::exists(data / L"mods" / L"models" / L"q-robot.akimodel" / L"model.bin") && fs::exists(data / L"mods" / L"q-robot-hd.rtz"));
    // A conversion: its folder and its add-ons, tied to it.
    r = pkg::install_package(conversion(tmp / L"k3", "conv-b", "1.0.0", kRomSha1), t);
    CHECK(r.outcome == Outcome::Installed);
    CHECK(fs::exists(data / L"conversions" / L"conv-b" / L"aki.json") && fs::exists(data / L"mods" / L"conv_tex.rtz"));
    // Its saves exist; an upgrade with an edited component keeps the edited folder, and the saves.
    put(data / L"saves" / L"conv-b" / L"save.bin", "SAVE");
    put(data / L"conversions" / L"conv-b" / L"codes.txt", "edited codes\n");
    r = pkg::install_package(conversion(tmp / L"k4", "conv-b", "1.1.0", kRomSha1, "810C0000 0002\n"), t);
    CHECK(r.outcome == Outcome::Upgraded && r.keptEdited.size() == 1);
    CHECK(!r.keptEdited.empty() && r.keptEdited[0].filename().wstring().rfind(L"conv-b.edited-", 0) == 0 &&
          get(r.keptEdited[0] / L"codes.txt") == "edited codes\n");
    CHECK(get(data / L"conversions" / L"conv-b" / L"codes.txt") == "810C0000 0002\n");
    CHECK(get(data / L"saves" / L"conv-b" / L"save.bin") == "SAVE");
    // A conversion folder put there by hand is listed too; edited copies and _packages aren't.
    put(data / L"conversions" / L"hand" / L"aki.json", R"({"id":"hand","name":"By Hand","version":"0.1.0","base_rom":{"sha1":"FFFF"}})");
    put(data / L"conversions" / L"_packages" / L"aki.json", "{}");
    auto convs = pkg::list_conversions(data);
    CHECK(convs.size() == 2);
    if (convs.size() == 2) {
        CHECK(convs[0].name == L"By Hand" && !convs[0].recorded && !pkg::conversion_rom_matches(convs[0], {kRomSha1}));
        CHECK(convs[1].id == "conv-b" && convs[1].recorded && convs[1].version == L"1.1.0" &&
              pkg::conversion_rom_matches(convs[1], {"0123456789ABCDEF0123456789ABCDEF01234567"}));
        CHECK(convs[1].addons.size() == 1 && convs[1].addons[0] == "conv_tex.rtz");
        CHECK(convs[0].tier.empty() && !convs[0].untested && convs[0].knownIssues.empty());
    }
    // aki-roadmap#232: the details the player reads before picking one (the #228 converter's survey block).
    put(data / L"conversions" / L"surveyed" / L"aki.json",
        R"({"id":"surveyed","name":"Surveyed","description":"A hack.","survey":{"aki_tier":"experimental",)"
        R"("known_issues":["Titles flicker","","Crowd is grey"],"note":"tested on Rev 1"}})");
    put(data / L"conversions" / L"fresh" / L"aki.json",
        R"({"id":"fresh","name":"Fresh","survey":{"aki_tier":"untested","tier_note":"not in the catalog"}})");
    convs = pkg::list_conversions(data);
    CHECK(convs.size() == 4);
    for (const auto& c : convs) {
        if (c.id == "surveyed") {
            CHECK(c.tier == L"Experimental" && !c.untested && c.description == L"A hack." && c.note == L"tested on Rev 1");
            CHECK(c.knownIssues.size() == 2 && c.knownIssues[0] == L"Titles flicker" && c.knownIssues[1] == L"Crowd is grey");
        } else if (c.id == "fresh") {
            CHECK(c.tier == L"Untested" && c.untested && c.knownIssues.empty() && c.note == L"not in the catalog");
        }
    }
    CHECK(pkg::conversion_tier_word("VERIFIED") == L"Verified" && pkg::conversion_tier_word("gold") == L"gold");
    std::error_code cleanEc;
    fs::remove_all(data / L"conversions" / L"surveyed", cleanEc);
    fs::remove_all(data / L"conversions" / L"fresh", cleanEc);
    // Remove: its files go, its saves stay.
    std::wstring msg;
    CHECK(pkg::remove_package("conv-b", data, false, &msg));
    CHECK(has(msg, L"its saves stay in saves\\conv-b\\"));
    CHECK(!fs::exists(data / L"conversions" / L"conv-b") && !fs::exists(data / L"mods" / L"conv_tex.rtz"));
    CHECK(get(data / L"saves" / L"conv-b" / L"save.bin") == "SAVE");
    CHECK(!pkg::load_registry(data / L"packages.json").find("conv-b"));
    CHECK(!pkg::remove_package("conv-b", data, false, &msg));
    CHECK(!pkg::remove_package("q-robot", data, true, &msg) && fs::exists(data / L"mods" / L"q-robot-hd.rtz"));
}

static void test_patch_mod(const fs::path& tmp) {
    fs::path data = tmp / L"data-p";
    pkg::InstallTarget t = target(data);
    fs::path hack = tmp / L"unlocked.nrm";
    put(hack, zip_bytes({{"mod.json", R"({"id":"nm_unlocked","version":"1.0.0","game_id":"wwfnomercy","display_name":"NM Unlocked","authors":["X"]})"},
                         {"patch.bps", "BPS1...."}}));
    fs::path code = tmp / L"code.nrm";
    put(code, zip_bytes({{"mod.json", mod_json("code")}, {"mod_binary.bin", "b"}}));
    CHECK(pkg::is_patch_mod(hack) && !pkg::is_patch_mod(code));
    pkg::PackageInstall r = pkg::install_patch_mod(hack, t);
    CHECK(r.outcome == Outcome::Installed && r.kind == "conversion");
    CHECK(has(r.message, L"saves\\nm_unlocked\\"));
    CHECK(fs::exists(data / L"conversions" / L"nm_unlocked" / L"patch.nrm"));
    JsonValue conv;
    CHECK(json_parse(get(data / L"conversions" / L"nm_unlocked" / L"aki.json"), &conv));
    const JsonValue* comp = conv.get("components");
    CHECK(comp && comp->get("patch") && comp->get("patch")->wstr("file") == L"patch.nrm" && conv.wstr("game") == L"nomercy");
    auto convs = pkg::list_conversions(data);
    CHECK(convs.size() == 1 && convs[0].name == L"NM Unlocked" && pkg::conversion_rom_matches(convs[0], {kRomSha1}));
    int stray = 0;
    for (auto& e : fs::directory_iterator(data)) stray += e.path().filename().wstring().rfind(L".aki-", 0) == 0;
    CHECK(stray == 0);
}


static void test_review_fixes(const fs::path& tmp) {
    fs::path data = tmp / L"data-x";
    pkg::InstallTarget t = target(data);
    pkg::InstallOptions o;
    o.today = "20261029";
    // A failure halfway (a file the new version replaces is open elsewhere) puts everything back.
    CHECK(pkg::install_package(conversion(tmp / L"x1", "conv-x", "1.0.0", kRomSha1, "OLD\n"), t, o).outcome == Outcome::Installed);
    std::string regBefore = get(data / L"packages.json");
    HANDLE lock = CreateFileW((data / L"mods" / L"conv_tex.rtz").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(lock != INVALID_HANDLE_VALUE);
    pkg::PackageInstall r = pkg::install_package(conversion(tmp / L"x2", "conv-x", "2.0.0", kRomSha1, "NEW\n"), t, o);
    CloseHandle(lock);
    CHECK(r.outcome == Outcome::Failed && has(r.message, L"nothing was changed"));
    CHECK(get(data / L"conversions" / L"conv-x" / L"codes.txt") == "OLD\n");
    CHECK(get(data / L"packages.json") == regBefore);
    int stray = 0;
    for (auto& e : fs::directory_iterator(data)) stray += e.path().filename().wstring().rfind(L".aki-", 0) == 0;
    for (auto& e : fs::directory_iterator(data / L"conversions")) stray += e.path().filename().wstring() != L"conv-x";
    CHECK(stray == 0);
    // ... and with nothing locked, the same upgrade goes through.
    CHECK(pkg::install_package(tmp / L"x2", t, o).outcome == Outcome::Upgraded);
    CHECK(get(data / L"conversions" / L"conv-x" / L"codes.txt") == "NEW\n");

    // Remove keeps a file changed since it was installed.
    CHECK(pkg::install_package(addon(tmp / L"x3", "edit-me", "1.0.0"), t, o).ok());
    put(data / L"mods" / L"demo.nrm", "player edit");
    std::wstring msg;
    CHECK(pkg::remove_package("edit-me", data, false, &msg));
    CHECK(!fs::exists(data / L"mods" / L"demo.nrm") && has(msg, L"demo.nrm.edited-"));
    bool keptEdit = false;
    for (auto& e : fs::directory_iterator(data / L"kept" / L"mods"))
        keptEdit = keptEdit || (e.path().filename().wstring().rfind(L"demo.nrm.edited-", 0) == 0 && get(e.path()) == "player edit");
    CHECK(keptEdit);

    // packages.json: entries this install didn't touch are written back as they were, in their order.
    fs::path data2 = tmp / L"data-y";
    put(data2 / L"packages.json", R"({"format":1,"packages":[{"id":"odd","kind":"addon","files":[{"hash":"x"},{"path":"mods/odd.nrm","hash":"h","note":1}],"zz":[1,2]},)"
                                  R"({"id":"q","kind":"shader-pack"}]})");
    CHECK(pkg::install_package(addon(tmp / L"y1", "fresh", "1.0.0"), target(data2), o).ok());
    std::string text = get(data2 / L"packages.json");
    size_t a = text.find("\"odd\""), b = text.find("shader-pack"), c = text.find("\"fresh\"");
    CHECK(a != std::string::npos && b != std::string::npos && c != std::string::npos && a < b && b < c);
    CHECK(text.find("\"hash\": \"x\"") != std::string::npos && text.find("\"zz\"") != std::string::npos);
    // A recorded path outside the data folder makes the file one to leave alone.
    for (const char* bad : {"../x.nrm", "C:/x.nrm", "mods/../../x"}) {
        put(data2 / L"packages.json", std::string(R"({"format":1,"packages":[{"id":"a","kind":"addon","files":[{"path":")") + bad + R"(","hash":"h"}]}]})");
        CHECK(!pkg::load_registry(data2 / L"packages.json").may_save());
    }

    // Zip names: '\' separators (PowerShell 5.1's Compress-Archive) and DOS-codepage names (Explorer) are read, never a crash.
    std::string man = manifest("addon", "zipnames", "1.0.0", R"({"nrm":"sub/z.nrm"})");
    put(tmp / L"z.zip", zip_bytes({{"aki-package.json", man}, {"thumb.png", kPng},
                                   {"sub\\z.nrm", zip_bytes({{"mod.json", mod_json("zipnames")}, {"mod_binary.bin", "b"}})},
                                   {"caf\x82.txt", "cp437"}}));
    {
        pkg::Package pk(tmp / L"z.zip");
        CHECK(pk.has("sub/z.nrm") && pk.has("caf\xC3\xA9.txt") && pk.unsafe.empty());
    }
    CHECK(pkg::install_package(tmp / L"z.zip", target(tmp / L"data-z"), o).ok());
    // Names that are the same file on Windows beyond ASCII case.
    put(tmp / L"z2.zip", zip_bytes({{"aki-package.json", man}, {"\xC3\x89.txt", "1"}, {"\xC3\xA9.txt", "2"}}));
    {
        pkg::Package pk(tmp / L"z2.zip");
        CHECK(pk.clashes.size() == 1);
    }
    // A conversion whose own aki.json names another id is refused (the game keys its saves on that id).
    fs::path cm = conversion(tmp / L"x4", "conv-y", "1.0.0", kRomSha1);
    put(cm / L"conversion" / L"aki.json", R"({"id":"something-else"})");
    r = pkg::install_package(cm, t, o);
    CHECK(r.outcome == Outcome::Refused && has(r.message, L"aki.json id is \"something-else\""));
    // A patch-mod hack while the game runs: refused before anything is written.
    fs::path hack = tmp / L"h.nrm";
    put(hack, zip_bytes({{"mod.json", R"({"id":"h_hack","version":"1.0.0","game_id":"wwfnomercy"})"}, {"patch.bps", "BPS1"}}));
    pkg::InstallOptions running;
    running.gameRunning = true;
    fs::path data3 = tmp / L"data-h";
    CHECK(pkg::install_patch_mod(hack, target(data3), running).outcome == Outcome::Refused);
    CHECK(!fs::exists(data3));
}


// Soldier Ant's three rules for conversions with a patch (154-contract.md).
static void test_patch_rules(const fs::path& tmp) {
    fs::path data = tmp / L"data-rules";
    pkg::InstallTarget t = target(data);
    pkg::InstallOptions o;
    o.today = "20261029";
    std::string hackNrm = zip_bytes({{"mod.json", R"({"id":"nm_hack","version":"1.0.0","game_id":"wwfnomercy","display_name":"Hack"})"},
                                     {"patch.bps", "BPS1...."}});
    // 2. Moved, not copied: the hack in mods\ (Move to Conversions) and another .nrm there with its mod id both leave
    //    mods\; other mods stay. 1. The generated aki.json names the patch's mod id.
    put(data / L"mods" / L"nm_hack.nrm", hackNrm);
    put(data / L"mods" / L"Hack (old copy).nrm", hackNrm);
    put(data / L"mods" / L"other.nrm", zip_bytes({{"mod.json", mod_json("other")}, {"mod_binary.bin", "b"}}));
    pkg::PackageInstall r = pkg::install_patch_mod(data / L"mods" / L"nm_hack.nrm", t, o);
    CHECK(r.ok());
    CHECK(!fs::exists(data / L"mods" / L"nm_hack.nrm") && !fs::exists(data / L"mods" / L"Hack (old copy).nrm"));
    CHECK(fs::exists(data / L"mods" / L"other.nrm") && get(data / L"conversions" / L"nm_hack" / L"patch.nrm") == hackNrm);
    JsonValue conv;
    CHECK(json_parse(get(data / L"conversions" / L"nm_hack" / L"aki.json"), &conv));
    const JsonValue* patch = conv.get("components") ? conv.get("components")->get("patch") : nullptr;
    CHECK(patch && patch->wstr("mod_id") == L"nm_hack" && patch->wstr("file") == L"patch.nrm");
    int left = 0;
    for (auto& e : fs::directory_iterator(data)) left += e.path().filename().wstring().rfind(L".aki-", 0) == 0;
    CHECK(left == 0);
    // A refused install puts them back in mods\ as they were.
    fs::path data2 = tmp / L"data-rules2";
    put(data2 / L"mods" / L"nm_hack.nrm", hackNrm);
    put(data2 / L"packages.json", R"({"format":9,"packages":[]})");
    r = pkg::install_patch_mod(data2 / L"mods" / L"nm_hack.nrm", target(data2), o);
    CHECK(!r.ok() && get(data2 / L"mods" / L"nm_hack.nrm") == hackNrm && !fs::exists(data2 / L"conversions"));

    // 3. No bare .bps; 1. the patch .nrm's mod id is the conversion id, and mod_id (when given) agrees.
    auto convWithPatch = [&](const wchar_t* name, const std::string& patchJson, const std::string& file, const std::string& bytes) {
        fs::path p = conversion(tmp / name, "nm_hack", "1.0.0", kRomSha1);
        put(p / L"conversion" / L"aki.json", R"({"id":"nm_hack","components":{"patch":)" + patchJson + "}}");
        put(p / L"conversion" / fs::u8path(file), bytes);
        return pkg::check_package(p);
    };
    pkg::CheckResult c = convWithPatch(L"c1", R"({"file":"patch.bps"})", "patch.bps", "BPS1");
    CHECK(!c.ok() && has(c.first_error(), L"bare .bps"));
    c = convWithPatch(L"c2", R"({"file":"patch.nrm"})", "patch.nrm",
                      zip_bytes({{"mod.json", R"({"id":"another_hack"})"}, {"patch.bps", "BPS1"}}));
    CHECK(!c.ok() && has(c.first_error(), L"is mod another_hack, the conversion is nm_hack"));
    c = convWithPatch(L"c3", R"({"file":"patch.nrm","mod_id":"x"})", "patch.nrm", hackNrm);
    CHECK(!c.ok() && has(c.first_error(), L"components.patch.mod_id is \"x\""));
    c = convWithPatch(L"c4", R"({"file":"missing.nrm"})", "patch.nrm", hackNrm);
    CHECK(!c.ok() && has(c.first_error(), L"not in the conversion folder"));
    c = convWithPatch(L"c5", R"({"file":"patch.nrm","mod_id":"nm_hack"})", "patch.nrm", hackNrm);
    CHECK(c.ok());
}

// aki-roadmap#226: a conversion's models line on the Mods page, and the game's refusal rule.
static void test_conversion_models(const fs::path& tmp) {
    auto refusal = [](const char* text) {
        JsonValue v;
        CHECK(json_parse(text, &v));
        return pkg::conversion_models_refusal(&v, false);
    };
    CHECK(pkg::conversion_models_refusal(nullptr, false).empty());                              // a plain patch
    CHECK(refusal(R"({"code_ranges": [], "data_ranges": []})").empty());                        // de-relinked, moved nothing
    std::wstring why = refusal(R"({"code_ranges": [[2151682016, 2152333312]]})");                // BTBW
    CHECK(has(why, L"0x80400FE0") && has(why, L"#156"));
    CHECK(!refusal(R"({"zero": [{"ram": 2153775104, "size": 6864}]})").empty());              // Legends: 0x80600000
    CHECK(refusal(R"({"code_ranges": [[2399141888, 2399146444]]})").empty());                   // above 8 MB
    CHECK(!refusal(R"({"zero": [{"ram": 2689597440, "size": 16}]})").empty());                // KSEG1
    CHECK(refusal(R"({"zero": [{"ram": 2147483648, "size": 4194304}]})").empty());              // ends at the window
    CHECK(!refusal(R"({"code_ranges": [[2155872252, 2155872256]]})").empty());                // its last word
    CHECK(!pkg::conversion_models_refusal(nullptr, true).empty());                              // unreadable

    fs::path data = tmp / L"data-m";
    const std::string slots = R"("models": {"dir": "models", "format": 2, "slots": [{"wrestler": "0x0404", "name": "Kenny Omega", "pack": "kenny"},
        {"wrestler": "0x0405", "name": "Jon Moxley", "pack": "mox"}]})";
    put(data / L"conversions" / L"a-models" / L"aki.json", R"({"id":"a-models","name":"A","components":{)" + slots + "}}");
    put(data / L"conversions" / L"b-relinked" / L"aki.json",
        R"({"id":"b-relinked","name":"B","components":{"patch":{"file":"patch.nrm"},)" + slots + "}}");
    put(data / L"conversions" / L"b-relinked" / L"patch.nrm", zip_bytes({{"mod.json", mod_json("b-relinked")},
        {"patch.bps", "BPS1"}, {"relink.json", R"({"code_ranges": [[2151682016, 2152333312]]})"}}));
    put(data / L"conversions" / L"c-newer" / L"aki.json", R"({"id":"c-newer","name":"C","components":{"models":{"dir":"models","format":9}}})");
    put(data / L"conversions" / L"d-none" / L"aki.json", R"({"id":"d-none","name":"D","components":{"codes":{"file":"c.txt"}}})");
    auto convs = pkg::list_conversions(data);
    CHECK(convs.size() == 4);
    if (convs.size() == 4) {
        CHECK(!convs[0].modelsOff && convs[0].models == L"Custom models for 2 wrestlers: Kenny Omega, Jon Moxley");
        CHECK(convs[1].modelsOff && has(convs[1].models, L"Custom models are off for this conversion") && has(convs[1].models, L"#156"));
        CHECK(convs[2].modelsOff && has(convs[2].models, L"format 9"));
        CHECK(convs[3].models.empty() && !convs[3].modelsOff);
    }
}

// aki-roadmap#265: a converted Project64 bundle nests its textures deep (WWF Legends' menu textures reached 317
// characters once staged); the install used to fail with "couldn't unpack conversion/ (disk full?)".
static void test_install_deep_paths(const fs::path& tmp) {
    const std::string deep = "conversion/textures/Menus/08. VS Screens/03 - Saturday Night's Main Event/Title Match Graphics/"
                             "Real Worlds Championship/Vs. Screen/WWF No Mercy#FDF74B47#3#0#00000000_ciByRGBA.png";
    const std::string man = manifest("conversion", "deep-conv", "1.0.0", R"({"folder":"conversion/"})",
                                     R"(,"base_rom":{"sha1":[")" + std::string(kRomSha1) + R"("],"name":"Test ROM"})");
    const fs::path zip = tmp / L"deep" / L"deep-conv-1.0.0.zip";
    put(zip, zip_bytes({{"aki-package.json", man}, {"thumb.png", kPng},
                        {"conversion/aki.json", R"({"format":"aki-conversion","id":"deep-conv","name":"Deep","version":"1.0.0","base_rom":{"sha1":")" +
                                                    std::string(kRomSha1) + R"("}})"},
                        {deep, "png"}}));
    const fs::path data = tmp / L"deep" / L"a data folder far down the tree for this test" / L"games" / L"nomercy";
    pkg::InstallOptions o;
    o.today = "20261109";
    pkg::PackageInstall r = pkg::install_package(zip, target(data), o);
    CHECK(r.outcome == Outcome::Installed);
    std::error_code ec;
    CHECK(fs::exists(pkg::long_path_of(data / L"conversions" / L"deep-conv" / fs::u8path(deep.substr(11))), ec));
    CHECK(pkg::long_path_of(L"C:\\x\\y") == fs::path(L"\\\\?\\C:\\x\\y"));
}

void run_packages_tests(const fs::path& tmp) {
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);
    test_fixtures();
    test_rules();
    test_install_deep_paths(tmp);
    test_registry(tmp);
    test_hash(tmp);
    test_install_and_upgrade(tmp);
    test_refusals(tmp);
    test_kinds_zip_and_conversions(tmp);
    test_patch_mod(tmp);
    test_review_fixes(tmp);
    test_patch_rules(tmp);
    test_conversion_models(tmp);
}
