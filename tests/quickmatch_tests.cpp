// Checks for Quick match's file side (src/quickmatch.cpp): reading the quickmatch mod's options from its .nrm,
// resolving picks (Random included), and one run's mods.json / mod_config changes and their undo. Synthetic files
// only.
#include "../src/json.h"
#include "../src/quickmatch.h"
#include "check.h"

#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "miniz.h"

namespace fs = std::filesystem;

static void put(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
}

static std::string get(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void put_nrm(const fs::path& p, const std::string& modJson) {
    mz_zip_archive za{};
    mz_zip_writer_init_heap(&za, 0, 0);
    mz_zip_writer_add_mem(&za, "mod.json", modJson.data(), modJson.size(), MZ_DEFAULT_COMPRESSION);
    mz_zip_writer_add_mem(&za, "mod_binary.bin", "x", 1, MZ_DEFAULT_COMPRESSION);
    void* buf = nullptr;
    size_t size = 0;
    mz_zip_writer_finalize_heap_archive(&za, &buf, &size);
    put(p, std::string((const char*)buf, size));
    mz_zip_writer_end(&za);
}

// What RecompModTool makes of the quickmatch manifest (mod.template.toml's config_options), cut down.
static std::string qm_manifest(const char* version, bool withLength) {
    std::string m = std::string(R"({"id":"aki_quickmatch","version":")") + version + R"(","game_id":"wwfnomercy",)"
                    R"("config_schema":{"options":[)"
                    R"({"id":"enabled","name":"Enabled","type":"Bool","default":false},)"
                    R"({"id":"venue","name":"Venue","type":"Enum","options":["Game's pick","Raw","SmackDown","Backstage hallway"],"default":"Game's pick"},)"
                    R"({"id":"match","name":"Match","type":"Enum","options":["Game's rotation","Tag team","Singles A"],"default":"Game's rotation"})";
    if (withLength)
        m += R"J(,{"id":"length","name":"Demo length","type":"Enum","options":["No time limit","1 minute (game)"],"default":"No time limit"})J";
    return m + "]}}";
}

static JsonValue parse(const std::string& s) {
    JsonValue v;
    json_parse(s, &v);
    return v;
}

static bool lists(const JsonValue& mj, const char* key, const char* id) {
    const JsonValue* l = mj.get(key);
    if (!l) return false;
    for (const auto& v : l->items)
        if (v.s == id) return true;
    return false;
}

static void test_manifest() {
    QuickMatchMod m;
    std::wstring err;
    CHECK(parse_quickmatch_manifest(qm_manifest("0.1.0", true), &m, &err));
    CHECK(m.version == "0.1.0");
    CHECK(m.options.size() == 3);  // the Bool "enabled" is the launcher's
    const QmOption* venue = m.option("venue");
    CHECK(venue && venue->name == "Venue" && venue->choices.size() == 4 && venue->def == "Game's pick" && venue->randomOk);
    const QmOption* len = m.option("length");
    CHECK(len && !len->randomOk);
    CHECK(!parse_quickmatch_manifest(R"({"id":"aki_cheats","config_schema":{"options":[]}})", &m, &err));
    CHECK(!parse_quickmatch_manifest(R"({"id":"aki_quickmatch"})", &m, &err));  // no venue option
    CHECK(!parse_quickmatch_manifest("not json", &m, &err));
    // A default that isn't one of the choices: the first one.
    CHECK(parse_quickmatch_manifest(
        R"({"id":"aki_quickmatch","config_schema":{"options":[{"id":"venue","type":"Enum","options":["A","B"],"default":"Z"}]}})",
        &m, &err));
    CHECK(m.options[0].def == "A" && m.options[0].name == "venue");
}

static void test_find(const fs::path& tmp) {
    fs::path bundled = tmp / L"exe" / L"assets" / L"mods", mods = tmp / L"data" / L"mods";
    QuickMatchMod m;
    std::wstring err;
    CHECK(!find_quickmatch(bundled, mods, &m, &err));
    CHECK(err.find(L"no Quick Match mod") != std::wstring::npos);
    put_nrm(mods / L"aki_quickmatch.nrm", qm_manifest("0.1.0", false));
    CHECK(find_quickmatch(bundled, mods, &m, &err) && m.nrm == mods / L"aki_quickmatch.nrm" && !m.option("length"));
    // The bundled copy wins: the game puts it over the mods folder's at start.
    put_nrm(bundled / L"aki_quickmatch.nrm", qm_manifest("0.2.0", true));
    CHECK(find_quickmatch(bundled, mods, &m, &err) && m.version == "0.2.0" && m.option("length"));
    // A broken bundled copy: the mods folder's is used.
    put(bundled / L"aki_quickmatch.nrm", "not a zip");
    CHECK(find_quickmatch(bundled, mods, &m, &err) && m.version == "0.1.0");
    CHECK(find_quickmatch({}, mods, &m, &err));
}

static void test_resolve() {
    QuickMatchMod m;
    std::wstring err;
    parse_quickmatch_manifest(qm_manifest("0.1.0", true), &m, &err);
    std::mt19937 rng(7);
    std::map<std::string, std::string> v;
    CHECK(resolve_quickmatch(m, {}, rng, &v, &err));
    CHECK(v.size() == 3 && v["venue"] == "Game's pick" && v["match"] == "Game's rotation" && v["length"] == "No time limit");
    CHECK(resolve_quickmatch(m, {{"venue", "SmackDown"}, {"length", "1 minute (game)"}}, rng, &v, &err));
    CHECK(v["venue"] == "SmackDown" && v["length"] == "1 minute (game)");
    // Random: one of the game's real venues, never "Game's pick"; over many runs, each of them.
    std::map<std::string, int> seen;
    for (int i = 0; i < 200; ++i) {
        CHECK(resolve_quickmatch(m, {{"venue", kQmRandom}, {"match", kQmRandom}}, rng, &v, &err));
        ++seen[v["venue"]];
        CHECK(v["match"] != "Game's rotation");
    }
    CHECK(seen.size() == 3 && !seen.count("Game's pick"));
    CHECK(!resolve_quickmatch(m, {{"length", kQmRandom}}, rng, &v, &err));  // not offered for the length
    CHECK(!resolve_quickmatch(m, {{"venue", "Nowhere"}}, rng, &v, &err));
    CHECK(err.find(L"Nowhere") != std::wstring::npos);
}

static void test_apply_restore(const fs::path& tmp) {
    QuickMatchMod m;
    std::wstring err;
    parse_quickmatch_manifest(qm_manifest("0.1.0", true), &m, &err);
    std::map<std::string, std::string> values{{"venue", "Raw"}, {"match", "Tag team"}, {"length", "No time limit"}};

    // A player's install: other mods on, the quickmatch config saved off with a venue of their own.
    fs::path d = tmp / L"player";
    const std::string mods = R"({"enabled_mods":["aki_cheats"],"latest_game_mode":"x","mod_order":["aki_cheats","aki_camera"],"other":{"n":3}})";
    const std::string cfg = R"({"mod_id":"aki_quickmatch","mod_version":"0.1.0","recomp_version":"0.2.5","storage":{"enabled":false,"venue":"SmackDown","extra":"kept"}})";
    put(d / L"mods.json", mods);
    put(d / L"mod_config" / L"aki_quickmatch.json", cfg);
    CHECK(!quickmatch_pending(d));
    CHECK(quickmatch_apply(d, m, values, &err));
    CHECK(quickmatch_pending(d));
    JsonValue mj = parse(get(d / L"mods.json"));
    CHECK(lists(mj, "enabled_mods", "aki_quickmatch") && lists(mj, "enabled_mods", "aki_cheats"));
    CHECK(lists(mj, "mod_order", "aki_quickmatch") && lists(mj, "mod_order", "aki_camera"));
    CHECK(mj.get("other") && mj.get("other")->integer("n", 0) == 3 && mj.wstr("latest_game_mode") == L"x");
    JsonValue c = parse(get(d / L"mod_config" / L"aki_quickmatch.json"));
    const JsonValue* s = c.get("storage");
    CHECK(s && s->boolean("enabled", false) && s->wstr("venue") == L"Raw" && s->wstr("match") == L"Tag team");
    CHECK(s && s->wstr("extra") == L"kept" && c.wstr("recomp_version") == L"0.2.5" && c.wstr("mod_id") == L"aki_quickmatch");

    // Meanwhile the player turns another mod on in the game's Mods menu: that stays after the restore.
    std::string during = get(d / L"mods.json");
    size_t at = during.find("\"aki_cheats\"");
    during.insert(at, "\"aki_new\", ");
    put(d / L"mods.json", during);
    CHECK(quickmatch_restore(d, &err));
    CHECK(!quickmatch_pending(d));
    mj = parse(get(d / L"mods.json"));
    CHECK(!lists(mj, "enabled_mods", "aki_quickmatch") && !lists(mj, "mod_order", "aki_quickmatch"));
    CHECK(lists(mj, "enabled_mods", "aki_new") && lists(mj, "enabled_mods", "aki_cheats"));
    CHECK(get(d / L"mod_config" / L"aki_quickmatch.json") == cfg);  // byte for byte
    CHECK(quickmatch_restore(d, &err));                              // nothing left to undo

    // Quickmatch already on (a maintainer's install): it stays on afterwards.
    fs::path on = tmp / L"on";
    put(on / L"mods.json", R"({"enabled_mods":["aki_quickmatch"],"mod_order":["aki_quickmatch"]})");
    CHECK(quickmatch_apply(on, m, values, &err));
    CHECK(quickmatch_restore(on, &err));
    mj = parse(get(on / L"mods.json"));
    CHECK(lists(mj, "enabled_mods", "aki_quickmatch") && lists(mj, "mod_order", "aki_quickmatch"));
    CHECK(!fs::exists(on / L"mod_config" / L"aki_quickmatch.json"));  // there was none

    // A fresh install (no mods.json, no config yet).
    fs::path fresh = tmp / L"fresh";
    fs::create_directories(fresh);
    CHECK(quickmatch_apply(fresh, m, values, &err));
    mj = parse(get(fresh / L"mods.json"));
    CHECK(lists(mj, "enabled_mods", "aki_quickmatch") && mj.get("latest_game_mode"));
    c = parse(get(fresh / L"mod_config" / L"aki_quickmatch.json"));
    CHECK(c.wstr("mod_version") == L"0.1.0" && c.wstr("recomp_version") == L"0.1.0");
    CHECK(quickmatch_restore(fresh, &err));
    CHECK(!fs::exists(fresh / L"mod_config" / L"aki_quickmatch.json"));
    CHECK(!lists(parse(get(fresh / L"mods.json")), "enabled_mods", "aki_quickmatch"));

    // The launcher stopped mid-run: the next apply undoes the old run first (the player's config, not the last run's,
    // is what comes back in the end).
    fs::path crash = tmp / L"crash";
    put(crash / L"mod_config" / L"aki_quickmatch.json", cfg);
    CHECK(quickmatch_apply(crash, m, values, &err));
    CHECK(quickmatch_apply(crash, m, {{"venue", "SmackDown"}}, &err));
    JsonValue cs = parse(get(crash / L"mod_config" / L"aki_quickmatch.json"));
    CHECK(cs.get("storage") && cs.get("storage")->wstr("venue") == L"SmackDown");
    CHECK(quickmatch_restore(crash, &err));
    CHECK(get(crash / L"mod_config" / L"aki_quickmatch.json") == cfg);

    // A malformed mods.json is refused, untouched, and nothing is recorded.
    fs::path bad = tmp / L"bad";
    put(bad / L"mods.json", "{\"enabled_mods\": [");
    CHECK(!quickmatch_apply(bad, m, values, &err));
    CHECK(get(bad / L"mods.json") == "{\"enabled_mods\": [" && !quickmatch_pending(bad));
    CHECK(!fs::exists(bad / L"mod_config"));
    put(bad / L"mods.json", R"({"enabled_mods":"aki_cheats"})");
    CHECK(!quickmatch_apply(bad, m, values, &err) && !quickmatch_pending(bad));

    // An unreadable undo record is dropped (with a message), so it can't block later runs.
    fs::path junk = tmp / L"junk";
    put(junk / L"aki-launcher-quickmatch.json", "garbage");
    CHECK(!quickmatch_restore(junk, &err) && !quickmatch_pending(junk));
    CHECK(quickmatch_apply(junk, m, values, &err));
}

static void test_json_write() {
    JsonValue v = parse(R"({"a":[1,2.5,-3,true,null,"q\"\\\n\u0001\u00e9"],"b":{},"c":[]})");
    std::string out = json_write(v);
    JsonValue back = parse(out);
    CHECK(json_write(back) == out);
    CHECK(out.find("2.5") != std::string::npos && out.find("\"b\": {}") != std::string::npos);
    CHECK(back.get("a") && back.get("a")->items.size() == 6);
    if (!back.get("a") || back.get("a")->items.size() != 6) return;
    CHECK(back.get("a")->items[5].s == std::string("q\"\\\n\x01\xC3\xA9"));
    CHECK(back.get("a")->items[0].n == 1 && out.find("1,") != std::string::npos);
}

void run_quickmatch_tests(const fs::path& tmp) {
    fs::create_directories(tmp);
    test_manifest();
    test_find(tmp / L"find");
    test_resolve();
    test_apply_restore(tmp / L"apply");
    test_json_write();
}
