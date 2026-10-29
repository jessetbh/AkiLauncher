// Checks for the Mods page's file side (src/mods.cpp): mod.json parsing,
// inspecting .nrm / .rtz / folder packs, installs (game-id check, zips,
// packages, folders, replacing) and removing. Synthetic files only.
#include "../src/games.h"
#include "../src/mods.h"
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
}

void run_mods_tests(const fs::path& dir) {
    test_manifest();

    GameEntry g;
    g.title = L"WWF No Mercy";
    g.exeRel = L"build-msvc\\NoMercyRecompiled.exe";
    // Portable: mods\ next to the exe; otherwise %LOCALAPPDATA%\<exe stem>\mods.
    g.exePath = dir / L"game" / L"NoMercyRecompiled.exe";
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
    CHECK(!fs::exists(mods / L"Ropes.rtz.new"));

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
}
