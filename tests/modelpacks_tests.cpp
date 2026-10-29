// Checks for the Mods page's Model packs section (src/modelpacks.cpp): listing
// synthetic packs, models.txt parsing / editing / writing (comments and
// unknown lines kept, line ends kept, atomic), and the roster reader.
#include "../src/modelpacks.h"
#include "check.h"

#include <fstream>
#include <string>

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
}  // namespace

static void test_parse_and_round_trip() {
    const std::string src =
        "# my packs\r\n"
        "\r\n"
        "banjo        slot 1\r\n"
        "cesiumman    wrestler 0x0302   # the champ\r\n"
        "snake        wrestler 0404 costume 2\r\n"
        "off banjo\r\n"
        "weird line the launcher doesn't know\r\n"
        "toolong      slot 9\r\n";
    ModelsTxt t = ModelsTxt::parse(src);
    CHECK(t.serialize() == src);  // untouched: byte for byte
    CHECK(t.newline == "\r\n" && t.lines.size() == 8);
    using K = ModelsTxtLine::Kind;
    CHECK(t.lines[0].kind == K::Comment && t.lines[1].kind == K::Blank);
    CHECK(t.lines[2].kind == K::Slot && t.lines[2].pack == "banjo" && t.lines[2].slot == 1);
    CHECK(t.lines[3].kind == K::Wrestler && t.lines[3].wrestler == 0x302 && t.lines[3].costume == -1 &&
          t.lines[3].comment == "# the champ");
    CHECK(t.lines[4].kind == K::Wrestler && t.lines[4].wrestler == 0x404 && t.lines[4].costume == 2);
    CHECK(t.lines[5].kind == K::Off && t.lines[5].pack == "banjo");
    CHECK(t.lines[6].kind == K::Other && t.lines[7].kind == K::Other);  // slot 9 isn't a slot: kept as is
    CHECK(t.is_off("banjo") && !t.is_off("snake"));
    CHECK(t.assignment("cesiumman") && t.assignment("cesiumman")->wrestler == 0x302);
    CHECK(!t.assignment("nobody"));
    // LF files stay LF; a BOM stays.
    ModelsTxt lf = ModelsTxt::parse("\xEF\xBB\xBFsnake slot 1\n");
    CHECK(lf.bom && lf.newline == "\n" && lf.serialize() == "\xEF\xBB\xBFsnake slot 1\n");
}

static void test_edit() {
    using K = ModelsTxtLine::Kind;
    ModelsTxt t = ModelsTxt::parse("# my packs\nbanjo slot 1\ncesiumman wrestler 0x0302   # the champ\noff snake\nmystery\n");
    // A new assignment goes first among the rules (it wins), the pack's old line goes.
    t.set_assignment("cesiumman", K::Slot, 2, -1, -1);
    std::string s = t.serialize();
    CHECK(s == "# my packs\ncesiumman    slot 2\nbanjo slot 1\noff snake\nmystery\n");  // a slot doesn't keep a wrestler's comment
    t.set_assignment("cesiumman", K::Wrestler, 0, 0x404, 1, "Test Grappler");
    CHECK(t.serialize() == "# my packs\ncesiumman    wrestler 0x0404 costume 1   # Test Grappler\nbanjo slot 1\noff snake\nmystery\n");
    // The same kind of line keeps its comment when none is given.
    t.set_assignment("cesiumman", K::Wrestler, 0, 0x405, -1);
    CHECK(t.assignment("cesiumman")->comment == "# Test Grappler" && t.assignment("cesiumman")->costume == -1);
    // None: the pack's lines go, nothing else changes.
    t.set_assignment("cesiumman", K::Blank, 0, 0, 0);
    CHECK(t.serialize() == "# my packs\nbanjo slot 1\noff snake\nmystery\n");
    // On / off.
    t.set_off("snake", false);
    t.set_off("banjo", true);
    t.set_off("banjo", true);  // once
    CHECK(t.serialize() == "# my packs\nbanjo slot 1\nmystery\noff banjo\n");
    // An empty file gets a header comment and the line.
    ModelsTxt e = ModelsTxt::parse("");
    e.set_assignment("q-human", K::Slot, 3, -1, -1);
    CHECK(e.serialize() == "# pack       replaces     (aki_modelimport: see its README)\r\nq-human      slot 3\r\n");
    // Pack ids compare in lower case, like the runtime's.
    ModelsTxt c = ModelsTxt::parse("Snake slot 1\n");
    CHECK(c.assignment("snake") && c.assignment("SNAKE"));
}

static void test_files(const fs::path& dir) {
    fs::path models = dir / L"mods" / L"models";
    put(models / L"banjo.akimodel" / L"pack.json",
        R"({"schema":1,"name":"Banjo","author":"Someone","license":"private use","source":"rip"})");
    put(models / L"banjo.akimodel" / L"manifest.json", R"({"triangles": 1234})");
    put(models / L"banjo.akimodel" / L"preview.png", "png");
    put(models / L"bare.akimodel" / L"model.bin", "x");  // no pack.json: still listed
    put(models / L"notapack" / L"x.txt", "x");
    put(models / L"models.txt", "banjo slot 1\r\nghost slot 2\r\n");
    auto packs = list_model_packs(models);
    CHECK(packs.size() == 2 && packs[0].id == "banjo" && packs[1].id == "bare");
    CHECK(packs[0].name == "Banjo" && packs[0].author == "Someone" && packs[0].license == "private use");
    CHECK(packs[0].triangles == 1234 && packs[0].preview.filename() == L"preview.png");
    CHECK(packs[1].name.empty() && packs[1].preview.empty());

    ModelsTxt t;
    std::wstring err;
    CHECK(read_models_txt(models / L"models.txt", &t, &err));
    auto missing = t.missing_packs(packs);
    CHECK(missing.size() == 1 && missing[0] == "ghost");
    t.set_assignment("bare", ModelsTxtLine::Kind::Slot, 3, -1, -1);
    CHECK(write_models_txt(models / L"models.txt", t, &err));
    CHECK(get(models / L"models.txt") == "bare         slot 3\r\nbanjo slot 1\r\nghost slot 2\r\n");
    CHECK(!fs::exists(models / L"models.txt.new"));
    // No models.txt yet: an empty one; writing creates the folder and file.
    ModelsTxt none;
    CHECK(read_models_txt(dir / L"other" / L"models.txt", &none, &err) && none.lines.empty());
    none.set_off("x", true);
    CHECK(write_models_txt(dir / L"other" / L"models" / L"models.txt", none, &err));
    CHECK(get(dir / L"other" / L"models" / L"models.txt") == "off x\r\n");
}

static void test_roster(const fs::path& dir) {
    // SYNTHETIC roster (made-up names, as in AkiTools' test fixture).
    put(dir / L"r" / L"nomercy-roster.json",
        R"({"schema":1,"game":"nomercy","wrestlers":[
            {"id":"0x0102","name":"Mr. Fixture"},
            {"id":"0x0101","name":"Test Grappler","costumes":[{"costume":0,"name":"Classic"},{"costume":1,"name":"Gold"}]},
            {"id":"bad","name":"Skipped"}]})");
    Roster r;
    std::wstring err;
    CHECK(load_roster_file(dir / L"r" / L"nomercy-roster.json", &r, &err));
    CHECK(r.wrestlers.size() == 2 && r.wrestlers[0].name == "Mr. Fixture");  // sorted by name, the bad entry left out
    CHECK(r.find(0x101) && r.find(0x101)->costumes.size() == 2 && r.find(0x101)->costumes[1].second == "Gold");
    CHECK(!r.find(0x999));
    put(dir / L"r" / L"broken.json", "{oops");
    CHECK(!load_roster_file(dir / L"r" / L"broken.json", &r, &err) && !err.empty());
    // Where it's found: the launcher's assets first, then the depot's AkiTools.
    fs::path app = dir / L"app", depot = dir / L"depot";
    CHECK(find_roster("nomercy", app, depot).empty());
    put(depot / L"AkiTools" / L"model-templates" / L"profiles" / L"nomercy-roster.json", "{}");
    CHECK(find_roster("nomercy", app, depot) == depot / L"AkiTools" / L"model-templates" / L"profiles" / L"nomercy-roster.json");
    put(app / L"assets" / L"rosters" / L"nomercy-roster.json", "{}");
    CHECK(find_roster("nomercy", app, depot) == app / L"assets" / L"rosters" / L"nomercy-roster.json");
    CHECK(find_roster("vpw2", app, depot).empty());
}

void run_modelpacks_tests(const fs::path& dir) {
    test_parse_and_round_trip();
    test_edit();
    test_files(dir / L"files");
    test_roster(dir);
}
