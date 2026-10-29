// Checks for the Mods page's file side (src/mods.cpp): mod.json parsing,
// inspecting .nrm / .rtz / folder packs, installs (game-id check, zips,
// packages, folders, replacing) and removing. Synthetic files only.
#include "../src/games.h"
#include "../src/mods.h"
#include "../src/modelpacks.h"
#include "check.h"

#include <windows.h>

#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "miniz.h"

namespace fs = std::filesystem;

static void write_text(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
}

// A zip with the given (name, bytes) entries.
static void write_zip(const fs::path& p, const std::vector<std::pair<std::string, std::string>>& entries) {
    mz_zip_archive za{};
    mz_zip_writer_init_heap(&za, 0, 0);
    for (auto& [name, data] : entries)
        mz_zip_writer_add_mem(&za, name.c_str(), data.data(), data.size(), MZ_DEFAULT_COMPRESSION);
    void* buf = nullptr;
    size_t size = 0;
    mz_zip_writer_finalize_heap_archive(&za, &buf, &size);
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write((const char*)buf, (std::streamsize)size);
    f.close();
    mz_zip_writer_end(&za);
}

static const std::string kDb = R"({"configuration":{"autoPath":"rt64"},"textures":[]})";
static std::string manifest(const char* id, const char* game) {
    std::string m = std::string(R"({"id":")") + id + R"(","version":"1.2.0","display_name":"Pack )" + std::string(1, '\\') + R"(u00e9 \"Q\"",)"
                    R"("authors":["A","B"],"short_description":"Short.","description":"Long.",)"
                    R"("config_schema":{"options":[{"id":"x","default":[1,2]}]},"minimum_recomp_version":"0.1.0")";
    if (game) m += std::string(R"(,"game_id":")") + game + "\"";
    return m + "}";
}

static const std::wstring kName = std::wstring(L"Pack ") + (wchar_t)0xE9 + L" \"Q\"";

static bool has(const std::wstring& s, const wchar_t* part) { return s.find(part) != std::wstring::npos; }

static void test_manifest() {
    ModManifest m = parse_mod_manifest("\xEF\xBB\xBF" + manifest("my_pack", "wwfnomercy"));
    CHECK(m.present && m.valid);
    CHECK(m.id == L"my_pack" && m.version == L"1.2.0" && m.gameId == L"wwfnomercy");
    CHECK(m.name == kName);
    CHECK(m.authors.size() == 2 && m.authors[1] == L"B");
    CHECK(m.description == L"Short.");  // the short one wins for the list
    CHECK(!parse_mod_manifest("{\"id\": \"x\",").valid);
    CHECK(!parse_mod_manifest("[]").valid);
    CHECK(parse_mod_manifest("{}").valid);
    std::string deep = "{\"x\":" + std::string(100000, '[') + std::string(100000, ']') + "}";
    CHECK(!parse_mod_manifest(deep).valid);  // capped, no stack overflow
    std::string ok = "{\"x\":" + std::string(30, '[') + std::string(30, ']') + ",\"id\":\"d\"}";
    CHECK(parse_mod_manifest(ok).valid && parse_mod_manifest(ok).id == L"d");
}

// A synthetic model pack folder (no real model: model.bin is a placeholder; install doesn't read it).
static void fake_pack(const fs::path& d, const char* id, const char* profile, bool hd = false) {
    write_text(d / L"model.bin", "AKP1 placeholder");
    write_text(d / L"pack.json", std::string(R"({"schema":1,"id":")") + id + R"(","game":")" + profile +
                                     R"(","name":"Fake","license":"CC0-1.0"})");
    write_text(d / L"manifest.json", std::string(R"({"schema_version":2,"profile":")") + profile + R"("})");
    if (hd) write_text(d / (std::wstring(id, id + strlen(id)) + L"-hd.rtz"), "rtz");
}

static std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Model packs through the Mods page's Install (aki-roadmap#130): folders, folders of packs, zips; the game check;
// replacing; removing with the HD textures.
static void test_model_pack_installs(const fs::path& dir) {
    fs::path src = dir / L"src", mods = dir / L"game" / L"mods", models = mods / L"models";
    fs::create_directories(mods);
    // One pack folder, with its HD textures inside (as aki-model builds it).
    fake_pack(src / L"snake.akimodel", "snake", "nomercy", true);
    InstallResult r = install_mod(src / L"snake.akimodel", mods, L"wwfnomercy", L"WWF No Mercy", "nomercy");
    CHECK(r.ok && fs::exists(models / L"snake.akimodel" / L"model.bin") && fs::exists(mods / L"snake-hd.rtz"));
    CHECK(has(r.message, L"Installed model pack snake") && has(r.message, L"Model packs"));
    CHECK(!fs::exists(models / L"models.txt"));  // who it replaces is picked in the Model packs section
    // A folder holding packs (an unzipped share), with the .rtz next to the pack.
    fake_pack(src / L"share" / L"Banjo v2" / L"banjo.akimodel", "banjo", "nomercy");
    write_text(src / L"share" / L"Banjo v2" / L"banjo-hd.rtz", "rtz");
    fake_pack(src / L"share" / L"robot.akimodel", "robot", "nomercy");
    r = install_mod(src / L"share", mods, L"wwfnomercy", L"WWF No Mercy", "nomercy");
    CHECK(r.ok && fs::exists(models / L"banjo.akimodel") && fs::exists(models / L"robot.akimodel"));
    CHECK(fs::exists(mods / L"banjo-hd.rtz"));
    // A zip of a pack folder (paths kept, nothing outside the work folder, the work folder gone after).
    fs::path z = src / L"cesium.zip";
    write_zip(z, {{"cesium.akimodel/model.bin", "x"}, {"cesium.akimodel/pack.json", R"({"id":"cesium","game":"nomercy"})"},
                  {"cesium.akimodel/manifest.json", R"({"profile":"nomercy"})"}, {"cesium.akimodel/cesium-hd.rtz", "rtz"},
                  {"../escape.txt", "x"}});
    r = install_mod(z, mods, L"wwfnomercy", L"WWF No Mercy", "nomercy");
    CHECK(r.ok && fs::exists(models / L"cesium.akimodel" / L"pack.json") && fs::exists(mods / L"cesium-hd.rtz"));
    CHECK(!fs::exists(dir / L"game" / L"escape.txt") && !fs::exists(mods / L"escape.txt"));
    CHECK(!fs::exists(mods / L".aki-install-packs"));
    // A zip with the pack's files at its root: the id comes from pack.json.
    fs::path flat = src / L"Flat Pack.zip";
    write_zip(flat, {{"model.bin", "x"}, {"pack.json", R"({"id":"flat","game":"nomercy"})"}});
    r = install_mod(flat, mods, L"wwfnomercy", L"WWF No Mercy", "nomercy");
    CHECK(r.ok && fs::exists(models / L"flat.akimodel" / L"model.bin"));
    // The wrong game, a bad id, and a game without model packs.
    fake_pack(src / L"vpw.akimodel", "vpw", "vpw2");
    r = install_mod(src / L"vpw.akimodel", mods, L"wwfnomercy", L"WWF No Mercy", "nomercy");
    CHECK(!r.ok && has(r.message, L"built for vpw2") && !fs::exists(models / L"vpw.akimodel"));
    fake_pack(src / L"Bad Name.akimodel", "bad", "nomercy");
    r = install_mod(src / L"Bad Name.akimodel", mods, L"wwfnomercy", L"WWF No Mercy", "nomercy");
    CHECK(!r.ok && has(r.message, L"isn't a pack id"));
    r = install_mod(src / L"snake.akimodel", dir / L"wt" / L"mods", L"wcwnwoworldtour", L"World Tour", "");
    CHECK(!r.ok && has(r.message, L"doesn't take model packs"));
    // Replacing: the new copy in place, the old one gone (to the Recycle Bin; tests delete).
    write_text(src / L"snake.akimodel" / L"model.bin", "AKP1 v2");
    r = install_mod(src / L"snake.akimodel", mods, L"wwfnomercy", L"WWF No Mercy", "nomercy");
    CHECK(r.ok && has(r.message, L"Recycle Bin") && slurp(models / L"snake.akimodel" / L"model.bin") == "AKP1 v2");
    CHECK(!fs::exists(models / L"snake.akimodel.new"));
    // The installed pack itself (or the mods folder) is not a source.
    r = install_mod(models / L"snake.akimodel", mods, L"wwfnomercy", L"WWF No Mercy", "nomercy");
    CHECK(!r.ok);
    // A texture-pack folder still goes the texture-pack way.
    write_text(src / L"Tex" / L"rt64.json", R"({"configuration":{"autoPath":"rt64"},"textures":[]})");
    r = install_mod(src / L"Tex", mods, L"wwfnomercy", L"WWF No Mercy", "nomercy");
    CHECK(r.ok && fs::exists(mods / L"Tex" / L"rt64.json"));
    // The Model packs section lists them; Remove takes the pack and its HD textures.
    auto packs = list_model_packs(models);
    CHECK(packs.size() == 5);  // banjo, cesium, flat, robot, snake
    std::wstring err;
    CHECK(remove_model_pack(models / L"snake.akimodel", mods, &err));
    CHECK(!fs::exists(models / L"snake.akimodel") && !fs::exists(mods / L"snake-hd.rtz"));
    CHECK(model_pack_profile(models / L"banjo.akimodel") == "nomercy");
    CHECK(find_model_packs(src / L"share").size() == 2 && find_model_packs(src / L"Tex").empty());
}

void run_mods_tests(const fs::path& dir) {
    test_manifest();

    GameEntry g;
    g.title = L"WWF No Mercy";
    g.exeRel = L"build-msvc\\NoMercyRecompiled.exe";
    // Portable: mods\ in the working directory the launcher starts the game in (repoPath); otherwise
    // %LOCALAPPDATA%\<exe stem>\mods.
    g.exePath = dir / L"game" / L"NoMercyRecompiled.exe";
    g.repoPath = dir / L"game";
    write_text(g.exePath, "");
    std::wstring appdata = game_mods_dir(g).wstring();
    CHECK(has(appdata, L"NoMercyRecompiled\\mods") && !has(appdata, (dir / L"game").c_str()));
    write_text(dir / L"game" / L"portable.txt", "");
    CHECK(game_mods_dir(g) == dir / L"game" / L"mods");

    fs::path src = dir / L"src";
    fs::path mods = dir / L"game" / L"mods";
    const std::wstring gid = L"wwfnomercy";

    // Sources.
    write_zip(src / L"Ropes.rtz", {{"rt64.json", kDb}, {"mod.json", manifest("ropes", "wwfnomercy")},
                                   {"thumb.png", "PNGDATA"}, {"0123456789abcdef.png", "x"}});
    write_zip(src / L"Cheats.nrm", {{"mod.json", manifest("cheats", "wwfnomercy")}, {"mod_binary.bin", "code"}});
    write_zip(src / L"Mixed.nrm", {{"mod.json", manifest("mixed", nullptr)}, {"mod_binary.bin", "c"}, {"rt64.json", kDb}});
    write_zip(src / L"OtherGame.rtz", {{"rt64.json", kDb}, {"mod.json", manifest("other", "wcwnworevenge")}});
    write_zip(src / L"Nested.rtz", {{"Pack/rt64.json", kDb}});
    write_zip(src / L"NoManifest.nrm", {{"mod_binary.bin", "c"}});
    write_zip(src / L"Zipped Pack.zip", {{"rt64.json", kDb}, {"fedcba9876543210.png", "x"}});
    write_zip(src / L"Package.zip", {{"manifest.json", "{}"}, {"README.md", "hi"},
                                     {"plugins/Cheats2.nrm", ""}, {"plugins/Tex.rtz", ""}});
    // The package's members need to be real zips: rebuild it with them.
    {
        fs::path a = dir / L"tmp" / L"Cheats2.nrm", b = dir / L"tmp" / L"Tex.rtz";
        write_zip(a, {{"mod.json", manifest("cheats2", "wwfnomercy")}, {"mod_binary.bin", "c"}});
        write_zip(b, {{"rt64.json", kDb}});
        auto slurp = [](const fs::path& p) {
            std::ifstream f(p, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        };
        write_zip(src / L"Package.zip", {{"manifest.json", "{}"}, {"plugins/Cheats2.nrm", slurp(a)},
                                         {"plugins/../../Tex.rtz", slurp(b)}});
    }
    write_zip(src / L"Nothing.zip", {{"readme.txt", "no mod here"}});
    {
        auto slurp = [](const fs::path& p) {
            std::ifstream f(p, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        };
        fs::path bad = dir / L"tmp" / L"Bad.nrm";
        write_zip(bad, {{"mod_binary.bin", "c"}});  // no mod.json
        write_zip(src / L"BadPackage.zip", {{"plugins/Good.rtz", slurp(dir / L"tmp" / L"Tex.rtz")}, {"Bad.nrm", slurp(bad)}});
        write_zip(src / L"DupPackage.zip", {{"a/Tex.rtz", slurp(dir / L"tmp" / L"Tex.rtz")},
                                            {"b/Tex.rtz", slurp(dir / L"tmp" / L"Tex.rtz")}});
    }
    write_text(src / L"Folder Pack" / L"rt64.json", kDb);
    write_text(src / L"Folder Pack" / L"hud" / L"0011223344556677.png", "x");
    write_text(src / L"Dump Pack" / L"8899aabbccddeeff.png", "x");  // hash-named only: loads without rt64.json
    write_text(src / L"Empty Folder" / L"notes.txt", "x");

    // Inspect.
    ModItem r = inspect_mod(src / L"Ropes.rtz");
    CHECK(r.kind == ModKind::Rtz && r.textures && !r.code && r.hasThumb && r.problem.empty());
    CHECK(r.display_name() == kName && r.kind_text() == L"Texture pack");
    CHECK(read_mod_thumb(r) == std::vector<uint8_t>({'P', 'N', 'G', 'D', 'A', 'T', 'A'}));
    ModItem c = inspect_mod(src / L"Cheats.nrm");
    CHECK(c.code && !c.textures && c.kind_text() == L"Code mod");
    CHECK(inspect_mod(src / L"Mixed.nrm").kind_text() == L"Code + textures");
    CHECK(has(inspect_mod(src / L"Nested.rtz").problem, L"subfolder"));
    CHECK(has(inspect_mod(src / L"NoManifest.nrm").problem, L"no mod.json"));
    CHECK(inspect_mod(src / L"Folder Pack").textures && inspect_mod(src / L"Dump Pack").textures);
    CHECK(!inspect_mod(src / L"Empty Folder").problem.empty());

    // Install.
    InstallResult ir = install_mod(src / L"Ropes.rtz", mods, gid, g.title);
    CHECK(ir.ok && fs::exists(mods / L"Ropes.rtz"));
    ir = install_mod(src / L"Cheats.nrm", mods, gid, g.title);
    CHECK(ir.ok);
    ir = install_mod(src / L"Mixed.nrm", mods, gid, g.title);  // no game_id: any game
    CHECK(ir.ok);
    ir = install_mod(src / L"OtherGame.rtz", mods, gid, g.title);
    CHECK(!ir.ok && has(ir.message, L"wcwnworevenge") && !fs::exists(mods / L"OtherGame.rtz"));
    ir = install_mod(src / L"Nested.rtz", mods, gid, g.title);
    CHECK(!ir.ok && !fs::exists(mods / L"Nested.rtz"));
    ir = install_mod(src / L"NoManifest.nrm", mods, gid, g.title);
    CHECK(!ir.ok);
    ir = install_mod(src / L"Zipped Pack.zip", mods, gid, g.title);
    CHECK(ir.ok && fs::exists(mods / L"Zipped Pack.rtz"));
    ir = install_mod(src / L"Package.zip", mods, gid, g.title);
    CHECK(ir.ok && ir.installed.size() == 2 && fs::exists(mods / L"Cheats2.nrm") && fs::exists(mods / L"Tex.rtz"));
    CHECK(!fs::exists(dir / L"game" / L"Tex.rtz") && !fs::exists(mods / L".aki-install"));  // no paths from the zip
    ir = install_mod(src / L"Nothing.zip", mods, gid, g.title);
    CHECK(!ir.ok && has(ir.message, L"not a mod package"));
    // A package member that wouldn't load stops the whole package; so do two files of one name.
    ir = install_mod(src / L"BadPackage.zip", mods, gid, g.title);
    CHECK(!ir.ok && has(ir.message, L"no mod.json") && !fs::exists(mods / L"Good.rtz") && !fs::exists(mods / L"Bad.nrm"));
    ir = install_mod(src / L"DupPackage.zip", mods, gid, g.title);
    CHECK(!ir.ok && has(ir.message, L"two files named"));
    CHECK(!fs::exists(mods / L".aki-install"));
    // The mods folder (or a folder holding it) is not a pack folder.
    ir = install_mod(dir / L"game", mods, gid, g.title);
    CHECK(!ir.ok && has(ir.message, L"holds the mods folder"));
    ir = install_mod(src / L"Folder Pack", mods, gid, g.title);
    CHECK(ir.ok && fs::exists(mods / L"Folder Pack" / L"hud" / L"0011223344556677.png"));
    ir = install_mod(src / L"Folder Pack", mods, gid, g.title);
    CHECK(!ir.ok && has(ir.message, L"installed already"));
    ir = install_mod(src / L"Empty Folder", mods, gid, g.title);
    CHECK(!ir.ok && !fs::exists(mods / L"Empty Folder"));
    ir = install_mod(mods / L"Ropes.rtz", mods, gid, g.title);
    CHECK(!ir.ok && has(ir.message, L"already in the mods folder"));
    ir = install_mod(src / L"readme.md", mods, gid, g.title);
    CHECK(!ir.ok);
    // Replacing a file: the old copy leaves (to the Recycle Bin), the new one is in place.
    write_zip(src / L"Ropes.rtz", {{"rt64.json", kDb}, {"mod.json", manifest("ropes2", "wwfnomercy")}});
    ir = install_mod(src / L"Ropes.rtz", mods, gid, g.title);
    CHECK(ir.ok && has(ir.message, L"Recycle Bin"));
    CHECK(inspect_mod(mods / L"Ropes.rtz").manifest.id == L"ropes2");
    CHECK(!fs::exists(mods / L"Ropes.rtz.new") && !fs::exists(mods / L"Ropes.rtz.old"));

    // List: mods only (models\, the runtime's json and dot folders left out), bundled marked.
    write_text(mods / L"mods.json", "{}");
    write_text(mods / L"models" / L"x.akimodel", "x");
    fs::create_directories(mods / L".cache");
    write_text(dir / L"game" / L"assets" / L"mods" / L"Cheats.nrm", "");
    auto items = list_mods(mods, dir / L"game" / L"assets" / L"mods");
    CHECK(items.size() == 7);
    const ModItem* bundled = nullptr;
    const ModItem* folder = nullptr;
    for (auto& m : items) {
        if (m.path.filename() == L"Cheats.nrm") bundled = &m;
        if (m.path.filename() == L"Folder Pack") folder = &m;
        CHECK(m.path.filename() != L"models" && m.path.filename() != L"mods.json" && m.path.filename() != L".cache");
    }
    CHECK(bundled && bundled->bundled);
    CHECK(folder && folder->kind == ModKind::Folder && folder->bytes == kDb.size() + 1);

    // Remove: bundled refused; others go to the Recycle Bin.
    std::wstring err;
    CHECK(bundled && !remove_mod(*bundled, &err) && has(err, L"comes with the game"));
    CHECK(fs::exists(mods / L"Cheats.nrm"));
    CHECK(folder && remove_mod(*folder, &err) && !fs::exists(mods / L"Folder Pack"));
    CHECK(list_mods(mods, dir / L"game" / L"assets" / L"mods").size() == 6);

    test_model_pack_installs(dir / L"packs");
}
