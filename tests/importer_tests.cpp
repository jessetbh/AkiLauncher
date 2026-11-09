// Checks for bulk import (aki-roadmap#228): Rice/Jabo texture-pack naming and wrapping (src/texpacks.cpp), the Mods
// page installs that use it (src/mods.cpp), and the import model (src/importer.cpp): the worker's JSON lines, counts
// and summary, sources, the Import folder rules, the ledger. Synthetic files only: no ROM, no real pack.
#include "../src/importer.h"
#include "../src/json.h"
#include "../src/mods.h"
#include "../src/texpacks.h"
#include "check.h"

#include <windows.h>

#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "miniz.h"

namespace fs = std::filesystem;

static void put_text(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
}

static std::string get_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static void put_zip(const fs::path& p, const std::vector<std::pair<std::string, std::string>>& entries) {
    mz_zip_archive za{};
    mz_zip_writer_init_heap(&za, 0, 0);
    for (auto& [name, data] : entries) mz_zip_writer_add_mem(&za, name.c_str(), data.data(), data.size(), MZ_DEFAULT_COMPRESSION);
    void* buf = nullptr;
    size_t size = 0;
    mz_zip_writer_finalize_heap_archive(&za, &buf, &size);
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write((const char*)buf, (std::streamsize)size);
    f.close();
    mz_zip_writer_end(&za);
}

static bool contains(const std::wstring& s, const wchar_t* part) { return s.find(part) != std::wstring::npos; }
static bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

static const char* kRice1 = "WWF No Mercy#221D3601#2#1#91632C19_ciByRGBA.png";
static const char* kRice2 = "WWF No Mercy#1D9090D7#$#$#10C18724_all.png";
static const char* kRice3 = "WWF No Mercy#0EDA299C#3#2_all.png";
static const char* kJabo = "E56C7997D16F4AE3D1C15C33C96173CB.png";

static void test_names() {
    std::wstring rn;
    CHECK(texture_name_kind(L"WWF No Mercy#221D3601#2#1#91632C19_ciByRGBA.png", &rn) == TexNaming::Rice && rn == L"WWF No Mercy");
    CHECK(texture_name_kind(L"WWF No Mercy#1D9090D7#$#$#10C18724_all.png") == TexNaming::Rice);
    CHECK(texture_name_kind(L"WWF No Mercy#0EDA299C#3#2_all.dds") == TexNaming::Rice);
    CHECK(texture_name_kind(L"1WWF No Mercy#0EDA299C#2#0#EDD0190A_ciByRGBA.png", &rn) == TexNaming::Rice && rn == L"1WWF No Mercy");
    CHECK(texture_name_kind(L"E56C7997D16F4AE3D1C15C33C96173CB.png") == TexNaming::Jabo);
    CHECK(texture_name_kind(L"0123456789abcdef.png") == TexNaming::Rt64);
    CHECK(texture_name_kind(L"0123456789abcdef.dds") == TexNaming::Rt64);
    // Not textures: other extensions, broken Rice names, near-miss hex lengths.
    CHECK(texture_name_kind(L"WWF No Mercy#221D3601#2#1_all.txt") == TexNaming::None);
    CHECK(texture_name_kind(L"WWF No Mercy#221D36#2#1_all.png") == TexNaming::None);      // short CRC
    CHECK(texture_name_kind(L"WWF No Mercy#221D3601_all.png") == TexNaming::None);        // no fmt/siz
    CHECK(texture_name_kind(L"WWF No Mercy#221D3601#x#1_all.png") == TexNaming::None);    // fmt not a digit
    CHECK(texture_name_kind(L"WWF No Mercy#221D3601#2#1#91632C1_all.png") == TexNaming::None);  // short pal CRC
    CHECK(texture_name_kind(L"WWF No Mercy#221D3601#2#1") == TexNaming::None);
    CHECK(texture_name_kind(L"E56C7997D16F4AE3D1C15C33C96173C.png") == TexNaming::None);  // 31 hex
    CHECK(texture_name_kind(L"thumb.png") == TexNaming::None);

    TexScan s = scan_texture_names({L"pack/" + std::wstring(L"WWF No Mercy#221D3601#2#1#91632C19_ciByRGBA.png"),
                                    L"pack\\sub\\WWF No Mercy#0EDA299C#3#2_all.png", L"pack/E56C7997D16F4AE3D1C15C33C96173CB.png",
                                    L"readme.txt", L"rt64.json"});
    CHECK(s.rice == 2 && s.jabo == 1 && s.rt64 == 0 && s.images == 3 && s.hasRt64Json && !s.hasModJson);
    CHECK(s.naming() == TexNaming::Rt64);  // an rt64.json at the root: it loads as it is
    s = scan_texture_names({L"a/x/rt64.json", L"a/WWF No Mercy#221D3601#2#1#91632C19_ciByRGBA.png"});
    CHECK(!s.hasRt64Json && s.naming() == TexNaming::Rice && s.rice_name() == L"WWF No Mercy");
    s = scan_texture_names({L"E56C7997D16F4AE3D1C15C33C96173CB.png", L"F56C7997D16F4AE3D1C15C33C96173CB.png"});
    CHECK(s.naming() == TexNaming::Jabo && s.rice_name().empty());
    CHECK(scan_texture_names({L"a.png", L"b.txt"}).naming() == TexNaming::None);

    CHECK(rice_name_game(L"WWF No Mercy") == L"wwfnomercy");
    CHECK(rice_name_game(L"1WWF No Mercy") == L"wwfnomercy");
    CHECK(rice_name_game(L"WCW-nWo REVENGE") == L"wcwnworevenge");  // '/' can't be in a file name
    CHECK(rice_name_game(L"WCWvs.NWO-World Tour") == L"wcwnwoworldtour");
    CHECK(rice_name_game(L"WRESTLEMANIA 2000") == L"wwfwm2k");
    CHECK(rice_name_game(L"TESTHACK").empty() && rice_name_game(L"").empty());

    CHECK(legacy_pack_id(L"WWF No Mercy HD (v2)") == L"tex_wwf_no_mercy_hd_v2");
    CHECK(legacy_pack_id(L"!!!") == L"tex_pack");
    CHECK(legacy_pack_id(std::wstring(80, L'a')).size() == 48);

    JsonValue j;
    CHECK(json_parse(legacy_rt64_json(TexNaming::Rice), &j) && contains(legacy_rt64_json(TexNaming::Rice), "\"autoPath\": \"rice\""));
    CHECK(contains(legacy_rt64_json(TexNaming::Jabo), "\"autoPath\": \"rt64\""));
    std::string mj = legacy_mod_json(L"tex_x", L"Pack \"X\"", L"wwfnomercy", TexNaming::Rice, 12);
    CHECK(json_parse(mj, &j) && j.wstr("game_id") == L"wwfnomercy" && j.wstr("id") == L"tex_x" &&
          j.wstr("display_name") == L"Pack \"X\"" && j.wstr("version") == L"1.0.0" &&
          j.wstr("minimum_recomp_version") == L"0.0.0" && j.get("authors") && contains(j.wstr("short_description"), L"12 textures"));
}

static void test_wrap_installs(const fs::path& dir) {
    fs::path mods = dir / L"mods";
    // A Rice folder (textures in subfolders, as packs ship): copied, rt64.json + mod.json written, the original untouched.
    fs::path rice = dir / L"src" / L"No Mercy HD";
    put_text(rice / L"hires_texture" / L"WWF No Mercy" / L"Arenas" / kRice1, "png");
    put_text(rice / L"hires_texture" / L"WWF No Mercy" / kRice2, "png");
    put_text(rice / L"readme.txt", "by someone");
    InstallResult r = install_mod(rice, mods, L"wwfnomercy", L"WWF No Mercy");
    CHECK(r.ok && r.installed.size() == 1 && contains(r.message, L"Rice/GLideN64") && contains(r.message, L"2 textures"));
    CHECK(contains(get_text(mods / L"No Mercy HD" / L"rt64.json"), "\"rice\""));
    JsonValue j;
    CHECK(json_parse(get_text(mods / L"No Mercy HD" / L"mod.json"), &j) && j.wstr("game_id") == L"wwfnomercy" &&
          j.wstr("id") == L"tex_no_mercy_hd");
    CHECK(fs::exists(mods / L"No Mercy HD" / L"hires_texture" / L"WWF No Mercy" / L"Arenas" / kRice1));
    CHECK(!fs::exists(rice / L"rt64.json") && !fs::exists(rice / L"mod.json"));
    ModItem m = inspect_mod(mods / L"No Mercy HD");
    CHECK(m.textures && m.problem.empty() && m.kind_text() == L"Texture pack" && m.manifest.gameId == L"wwfnomercy");
    // Again: refused, the installed copy kept.
    r = install_mod(rice, mods, L"wwfnomercy", L"WWF No Mercy");
    CHECK(!r.ok && r.already && contains(r.message, L"installed already"));
    // From another game's page: refused by its Rice name.
    r = install_mod(rice, dir / L"mods-vpw2", L"wcwnworevenge", L"WCW/nWo Revenge");
    CHECK(!r.ok && contains(r.message, L"WWF No Mercy") && !fs::exists(dir / L"mods-vpw2" / L"No Mercy HD"));
    // A hack's own header name: accepted, with a note.
    fs::path hackPack = dir / L"src" / L"Hack Pack";
    put_text(hackPack / L"TESTHACK#5A17C0DE#2#1#0C3E9B71_ciByRGBA.png", "png");
    r = install_mod(hackPack, mods, L"wwfnomercy", L"WWF No Mercy");
    CHECK(r.ok && contains(r.message, L"TESTHACK"));

    // A Jabo folder: autoPath rt64.
    fs::path jabo = dir / L"src" / L"Jabo Pack";
    put_text(jabo / L"Arenas" / kJabo, "png");
    r = install_mod(jabo, mods, L"vpw2", L"Virtual Pro Wrestling 2");
    CHECK(r.ok && contains(r.message, L"Jabo") && contains(get_text(mods / L"Jabo Pack" / L"rt64.json"), "\"rt64\""));
    CHECK(json_parse(get_text(mods / L"Jabo Pack" / L"mod.json"), &j) && j.wstr("game_id") == L"vpw2");

    // A zipped Rice pack with one top folder: the folder's contents become the pack, named after the zip.
    fs::path zip = dir / L"src" / L"Zipped Pack.zip";
    put_zip(zip, {{std::string("Zipped Pack/") + kRice1, "png"}, {std::string("Zipped Pack/sub/") + kRice3, "png"},
                  {"Zipped Pack/../evil.png", "x"}});
    r = install_mod(zip, mods, L"wwfnomercy", L"WWF No Mercy");
    CHECK(r.ok && fs::exists(mods / L"Zipped Pack" / L"rt64.json") && fs::exists(mods / L"Zipped Pack" / kRice1) &&
          fs::exists(mods / L"Zipped Pack" / L"sub" / kRice3) && !fs::exists(mods / L"evil.png") &&
          !fs::exists(mods / L".aki-install-tex"));
    // A zip with an rt64.json in a subfolder isn't wrapped (an rt64 pack zipped with its folder: the old message).
    fs::path nested = dir / L"src" / L"Nested.zip";
    put_zip(nested, {{"Nested/rt64.json", "{}"}, {std::string("Nested/") + kRice1, "png"}});
    r = install_mod(nested, mods, L"wwfnomercy", L"WWF No Mercy");
    CHECK(!r.ok && !fs::exists(mods / L"Nested"));
    // A mod.json the author shipped is kept.
    fs::path withMod = dir / L"src" / L"With Manifest";
    put_text(withMod / kRice1, "png");
    put_text(withMod / L"mod.json", R"({"id":"authors_id","display_name":"Theirs","version":"2.0.0","authors":["Them"],"game_id":"wwfnomercy","minimum_recomp_version":"0.1.0"})");
    r = install_mod(withMod, mods, L"wwfnomercy", L"WWF No Mercy");
    CHECK(r.ok && json_parse(get_text(mods / L"With Manifest" / L"mod.json"), &j) && j.wstr("id") == L"authors_id" &&
          fs::exists(mods / L"With Manifest" / L"rt64.json"));

    // What isn't a mod at all is flagged for the converter; a broken mod isn't.
    fs::path junk = dir / L"src" / L"hack.bps";
    put_text(junk, "BPS1....");
    r = install_mod(junk, mods, L"wwfnomercy", L"WWF No Mercy");
    CHECK(!r.ok && r.unrecognized);
    fs::path emptyDir = dir / L"src" / L"Some Bundle";
    put_text(emptyDir / L"Project64.cht", "[x]");
    r = install_mod(emptyDir, mods, L"wwfnomercy", L"WWF No Mercy");
    CHECK(!r.ok && r.unrecognized);
    fs::path plainZip = dir / L"src" / L"hacks.zip";
    put_zip(plainZip, {{"Some Hack.z64", "not really"}});
    r = install_mod(plainZip, mods, L"wwfnomercy", L"WWF No Mercy");
    CHECK(!r.ok && r.unrecognized);
    fs::path badRtz = dir / L"src" / L"bad.rtz";
    put_zip(badRtz, {{"x.png", "png"}});
    r = install_mod(badRtz, mods, L"wwfnomercy", L"WWF No Mercy");
    CHECK(!r.ok && !r.unrecognized);
}

static void test_worker_lines() {
    WorkerEvent e = parse_worker_line(
        R"({"event":"item","item":"C:\\dl\\NMPV3.z64","status":"converted","kind":"rom","game":"nomercy","name":"No Mercy Plus V3",)"
        R"("id":"no_mercy_plus_v3","aki_tier":"verified","converter":"derelink","package":"C:\\c\\nomercy\\no_mercy_plus_v3-1.0.0.zip"})");
    CHECK(e.type == WorkerEvent::Type::Item && e.line.outcome == ImportOutcome::Converted && e.line.kind == L"conversion");
    CHECK(e.line.name == L"No Mercy Plus V3" && e.line.gameKey == L"nomercy" && e.line.tier == L"Verified" && !e.line.untested);
    CHECK(e.line.packages.size() == 1 && e.line.packages[0] == fs::path(L"C:\\c\\nomercy\\no_mercy_plus_v3-1.0.0.zip"));
    CHECK(contains(e.line.detail, L"derelink") && e.line.viaWorker);

    e = parse_worker_line(R"({"event":"item","item":"C:\\dl\\new.z64","status":"converted","kind":"rom","game":"nomercy",)"
                          R"("aki_tier":"untested","converter":"make_patch_mod","package":"a.zip","packages":["a.zip","b.zip"]})");
    CHECK(e.line.untested && e.line.tier == L"Untested" && contains(e.line.detail, L"Untested") && e.line.packages.size() == 2);
    CHECK(e.line.name == L"new.z64");

    e = parse_worker_line(R"({"event":"item","item":"C:\\dl\\pack.zip","status":"converted","kind":"texture-pack","images":4212,"game":"nomercy"})");
    CHECK(e.line.kind == L"texture pack" && contains(e.line.detail, L"4212 textures"));

    e = parse_worker_line(R"({"event":"item","item":"C:\\dl\\BL.xdelta","status":"needs-rom","kind":"patch","needs":"WWF No Mercy (USA) 1.0"})");
    CHECK(e.line.outcome == ImportOutcome::NeedsRom && e.line.needs == L"WWF No Mercy (USA) 1.0" && contains(e.line.detail, L"needs WWF No Mercy"));
    CHECK(e.line.kind == L"patch");  // "conversion" only once it is one
    e = parse_worker_line(R"({"event":"item","item":"x","status":"unsupported","why":"PAL version"})");
    CHECK(e.line.outcome == ImportOutcome::Unsupported && e.line.detail == L"PAL version");
    e = parse_worker_line(R"({"event":"item","item":"x","status":"failed","why":"derelink: cannot resolve symbol"})");
    CHECK(e.line.outcome == ImportOutcome::Failed && contains(e.line.log, L"cannot resolve symbol"));
    e = parse_worker_line(R"J({"event":"item","item":"x","status":"have-it","why":"imported before (2026-11-08)"})J");
    CHECK(e.line.outcome == ImportOutcome::HaveIt);
    e = parse_worker_line(R"({"event":"item","item":"x","status":"stock"})");
    CHECK(e.line.outcome == ImportOutcome::Stock);
    // aki-roadmap#271: the reason with the need, and the card hint for a stock ROM inside a folder or zip.
    e = parse_worker_line(R"({"event":"item","item":"C:\\dl\\wzbb.xdelta","status":"needs-rom","kind":"patch","needs":"the ROM it was made for","why":"it does not apply to: WWF No Mercy"})");
    CHECK(e.line.detail == L"needs the ROM it was made for (it does not apply to: WWF No Mercy)");
    e = parse_worker_line(R"({"event":"item","item":"x!nm.z64","status":"stock","why":"the stock WWF No Mercy ROM: nothing to convert"})");
    CHECK(e.line.detail.find(L"Set it as the game's ROM on its card") != std::wstring::npos);
    e = parse_worker_line(R"({"event":"item","item":"x","status":"needs-game"})");
    CHECK(e.line.outcome == ImportOutcome::NeedsGame);
    // A mod the worker found in a folder: the launcher installs it; inside an archive it can't.
    e = parse_worker_line(R"({"event":"item","item":"C:\\dl\\mods\\a.nrm","status":"ready","kind":"nrm"})");
    CHECK(e.line.installAsIs && e.line.outcome == ImportOutcome::Waiting);
    e = parse_worker_line(R"({"event":"item","item":"C:\\dl\\all.zip!a.nrm","status":"ready","kind":"nrm"})");
    CHECK(!e.line.installAsIs && e.line.outcome == ImportOutcome::Unsupported);
    e = parse_worker_line(R"({"event":"item","item":"x","status":"something-new"})");
    CHECK(e.line.outcome == ImportOutcome::Failed);

    e = parse_worker_line(R"({"event":"done","counts":{"converted":3,"failed":1},"engine":"0.1.0","patch_engine":true})");
    CHECK(e.type == WorkerEvent::Type::Done && e.counts["converted"] == 3 && e.counts["failed"] == 1 && e.engine == "0.1.0");
    CHECK(parse_worker_line("Traceback (most recent call last):").type == WorkerEvent::Type::Bad);
    CHECK(parse_worker_line(R"({"event":"progress"})").type == WorkerEvent::Type::Bad);

    CHECK(engine_game_mod_id(L"nomercy") == L"wwfnomercy" && engine_game_key(L"wwfwm2k") == L"wm2k" && engine_game_key(L"x").empty());

    WorkerJob job;
    job.paths = {L"C:\\dl\\a b.zip"};
    job.roms[L"nomercy"] = L"C:\\games\\nomercy.z64";
    job.ledger = L"C:\\c\\import-ledger.json";
    job.out = L"C:\\c";
    job.game = L"nomercy";
    JsonValue j;
    CHECK(json_parse(worker_job_json(job), &j));
    CHECK(j.get("paths") && j.get("paths")->items.size() == 1 && json_utf8_to_wide(j.get("paths")->items[0].s) == L"C:\\dl\\a b.zip");
    CHECK(j.get("roms") && j.get("roms")->wstr("nomercy") == L"C:\\games\\nomercy.z64");
    CHECK(j.wstr("out") == L"C:\\c" && j.wstr("game") == L"nomercy" && j.boolean("build", false) && !j.get("work"));
}

static ImportLine line(ImportOutcome o, const wchar_t* kind = L"conversion", bool untested = false) {
    ImportLine l;
    l.outcome = o;
    l.kind = kind;
    l.untested = untested;
    l.item = L"item";
    l.detail = L"why";
    return l;
}

static void test_counts() {
    std::vector<ImportLine> v = {line(ImportOutcome::Converted), line(ImportOutcome::Converted, L"conversion", true),
                                 line(ImportOutcome::Converted, L"texture pack"), line(ImportOutcome::Installed, L"texture pack"),
                                 line(ImportOutcome::HaveIt), line(ImportOutcome::NeedsRom), line(ImportOutcome::Unsupported),
                                 line(ImportOutcome::Unsupported), line(ImportOutcome::Failed), line(ImportOutcome::Waiting),
                                 line(ImportOutcome::Working)};
    ImportCounts c = count_import(v);
    CHECK(c.total == 11 && c.converted == 3 && c.conversions == 2 && c.texturePacks == 2 && c.installed == 1);
    CHECK(c.haveIt == 1 && c.needsRom == 1 && c.unsupported == 2 && c.failed == 1 && c.waiting == 1 && c.working == 1 && c.untested == 1);
    std::wstring s = import_summary(c);
    CHECK(contains(s, L"3 converted (2 conversions, 1 texture pack)") && contains(s, L"1 needs a ROM") &&
          contains(s, L"2 unsupported") && contains(s, L"1 failed") && contains(s, L"originals weren't changed") &&
          contains(s, L"1 Untested"));
    CHECK(import_progress(c) == L"9 done | 1 working | 1 waiting");
    CHECK(contains(import_summary(count_import({})), L"Nothing to import"));
    CHECK(import_outcome_final(ImportOutcome::NeedsRom) && !import_outcome_final(ImportOutcome::Working));
    CHECK(std::wstring(import_outcome_word(ImportOutcome::HaveIt)) == L"Have it");
}

static void test_sources_and_folder(const fs::path& dir) {
    std::vector<fs::path> in = {L"C:\\dl\\pack.zip", L"C:\\dl\\Hacks", L"C:\\dl\\Hacks2\\"};
    CHECK(import_source_of(L"C:\\dl\\pack.zip!inner\\x.z64", in) == fs::path(L"C:\\dl\\pack.zip"));
    CHECK(import_source_of(L"c:\\DL\\hacks\\sub\\a.bps", in) == fs::path(L"C:\\dl\\Hacks"));
    CHECK(import_source_of(L"C:\\dl\\Hacks2\\b.z64", in) == fs::path(L"C:\\dl\\Hacks2\\"));
    CHECK(import_source_of(L"C:\\dl\\HacksX\\b.z64", in).empty());  // a prefix of the name isn't the folder
    CHECK(import_source_of(L"C:\\dl\\Hacks", in) == fs::path(L"C:\\dl\\Hacks"));

    using O = ImportOutcome;
    CHECK(import_place_for({O::Converted, O::HaveIt, O::Stock, O::Installed}) == ImportPlace::Imported);
    CHECK(import_place_for({O::Converted, O::Failed}) == ImportPlace::NeedsAttention);
    CHECK(import_place_for({O::Unsupported}) == ImportPlace::NeedsAttention);
    CHECK(import_place_for({O::Failed, O::NeedsRom}) == ImportPlace::Stay);  // retried once the ROM is found
    CHECK(import_place_for({O::Converted, O::Stopped}) == ImportPlace::Stay);
    CHECK(import_place_for({}) == ImportPlace::Stay);

    fs::path imp = dir / L"Import";
    put_text(imp / L"b hack.z64", "x");
    put_text(imp / L"A pack" / L"x.png", "x");
    put_text(imp / L"Imported" / L"old.z64", "x");
    put_text(imp / L"Needs attention" / L"bad.z64", "x");
    put_text(imp / L".hidden", "x");
    put_text(imp / L"desktop.ini", "x");
    std::vector<fs::path> items = import_folder_items(imp);
    CHECK(items.size() == 2 && items[0].filename() == L"A pack" && items[1].filename() == L"b hack.z64");
    CHECK(import_folder_items(dir / L"nope").empty());

    CHECK(import_free_name(imp / L"Imported", L"new.z64") == imp / L"Imported" / L"new.z64");
    CHECK(import_free_name(imp / L"Imported", L"old.z64") == imp / L"Imported" / L"old (2).z64");
    put_text(imp / L"Imported" / L"old (2).z64", "x");
    CHECK(import_free_name(imp / L"Imported", L"old.z64") == imp / L"Imported" / L"old (3).z64");

    ImportLine bad = line(O::Failed);
    bad.item = L"C:\\dl\\x.xdelta";
    bad.detail = L"derelink: cannot resolve symbol";
    ImportLine good = line(O::Converted);
    std::string reason = import_reason_text({&bad, &good});
    CHECK(contains(reason, "Failed: C:\\dl\\x.xdelta") && contains(reason, "cannot resolve symbol") && !contains(reason, "Converted"));
}

static void test_ledger(const fs::path& dir) {
    fs::path led = dir / L"import-ledger.json";
    put_text(led, R"({"entries":{"AAA":{"id":"a","package":"C:\\c\\nomercy\\a-1.0.0.zip"},"BBB":{"id":"b","packages":["C:\\c\\nomercy\\b-1.0.0.zip","C:\\c\\nomercy\\b_tex-1.0.0.zip"]},"CCC":{"id":"c","package":"C:\\c\\vpw2\\c-1.0.0.zip"}},"format":"aki-import-ledger","version":1})");
    auto keys = ledger_keys_for_packages(led, {L"C:\\C\\nomercy\\a-1.0.0.zip", L"C:\\c\\nomercy\\b_tex-1.0.0.zip"});
    CHECK(keys.size() == 2 && keys[0] == "AAA" && keys[1] == "BBB");
    std::wstring err;
    CHECK(ledger_remove(led, {"AAA", "ZZZ"}, &err));
    JsonValue j;
    CHECK(json_parse(get_text(led), &j) && j.get("entries") && !j.get("entries")->get("AAA") && j.get("entries")->get("BBB") &&
          j.get("entries")->get("CCC") && j.wstr("format") == L"aki-import-ledger");
    CHECK(ledger_remove(dir / L"missing.json", {"AAA"}, &err));  // nothing recorded yet: nothing to drop
    put_text(dir / L"broken.json", "{not json");
    CHECK(!ledger_remove(dir / L"broken.json", {"AAA"}, &err) && contains(err, L"isn't valid JSON"));
    CHECK(ledger_keys_for_packages(dir / L"missing.json", {L"x"}).empty());
}

static void test_results_file() {
    std::vector<ImportLine> v = {line(ImportOutcome::Converted, L"conversion", true), line(ImportOutcome::NeedsRom)};
    v[0].name = L"Caf\u00e9 Hack";
    v[0].gameKey = L"nomercy";
    v[0].tier = L"Untested";
    v[0].packages = {L"C:\\c\\nomercy\\x-1.0.0.zip"};
    v[1].needs = L"WWF No Mercy (USA) 1.0";
    v[1].source = L"C:\\dl\\a.bps";
    std::vector<ImportLine> back;
    std::wstring st;
    CHECK(import_lines_from_json(import_lines_json(v, L"1 converted"), &back, &st) && st == L"1 converted");
    CHECK(back.size() == 2 && back[0].outcome == ImportOutcome::Converted && back[0].untested && back[0].name == v[0].name &&
          back[0].packages.size() == 1 && back[0].packages[0] == v[0].packages[0] && back[0].gameKey == L"nomercy");
    CHECK(back[1].outcome == ImportOutcome::NeedsRom && back[1].needs == v[1].needs && back[1].source == v[1].source);
    CHECK(!import_lines_from_json("{}", &back, &st) && !import_lines_from_json("nope", &back, &st));
}

void run_importer_tests(const fs::path& tmp) {
    test_results_file();
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);
    test_names();
    test_wrap_installs(tmp / L"wrap");
    test_worker_lines();
    test_counts();
    test_sources_and_folder(tmp / L"folder");
    test_ledger(tmp / L"ledger");
}
