// Checks for the data-driven game registry (aki-roadmap#24): the built-in
// games.json gives exactly the six games the hard-coded table had, a broken
// registry file falls back to it, and aki-game.json manifests add games
// (never replace them), with variants after their base game.
#include "../src/games.h"
#include "../src/json.h"
#include "check.h"

#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {
struct Expected {
    const wchar_t *id, *title, *date, *region, *repo, *exe, *rom, *github, *sha1, *code, *modId, *note;
    int romVersion;
    bool borderless;
};
// The table src/games.cpp had before the registry (generated from it, not typed).
const Expected kOld[] = {
    {L"worldtour", L"WCW vs. nWo World Tour", L"December 2, 1997", L"North America", L"WcwNwoWorldTour", L"build-msvc\\WCWRecompiled.exe", L"wcw.z64", L"jessetbh/WCWvsNWOWorldTourRecomp", L"5AD2D8359058C8BB71F08E3D3433B7A50D3BB645", L"NWNE", L"wcwnwoworldtour", L"North American (NTSC-U) release", 0, true},
    {L"vpw64", L"Virtual Pro Wrestling 64", L"December 19, 1997", L"Japan", L"Vpw64Recomp", L"build-msvc\\Vpw64Recompiled.exe", L"vpw64.z64", L"jessetbh/VPW64Recomp", L"F9E9FA2ED819C3A39DB5CB6AFECA186F021DB5ED", L"NVPJ", L"vpw64", L"Japanese release (Japan-only; translations go in through Import)", 0, true},
    {L"revenge", L"WCW/nWo Revenge", L"October 26, 1998", L"North America", L"WcwRevengeRecomp", L"build-msvc\\RevengeRecompiled.exe", L"revenge.z64", L"jessetbh/WCWnWoRevengeRecomp", L"E1711A2511394B9357B5F1AC8CA5CC17BD674836", L"NW2E", L"wcwnworevenge", L"North American (NTSC-U) release", 0, true},
    {L"wm2k", L"WWF WrestleMania 2000", L"October 31, 1999", L"North America", L"Wm2kRecomp", L"build-msvc\\Wm2kRecompiled.exe", L"wm2k.z64", L"jessetbh/WWFWrestleMania2000Recomp", L"442D417A52ED672CA1A47E7261A5414DEBB1E27A", L"NWXE", L"wwfwm2k", L"North American (NTSC-U) retail release (the \"V1.2\" file is a fan hack and won't match)", 0, true},
    {L"vpw2", L"Virtual Pro Wrestling 2", L"January 28, 2000", L"Japan", L"Vpw2Recomp", L"build-msvc\\Vpw2Recompiled.exe", L"vpw2.z64", L"jessetbh/VPW2Recomp", L"82DD25A044689EAB57AB362FE10C0DA6388C217A", L"NA2J", L"vpw2", L"Japanese release (Japan-only; translations go in through Import)", 0, true},
    {L"nomercy", L"WWF No Mercy", L"November 17, 2000", L"North America", L"NoMercyRecomp", L"build-msvc\\NoMercyRecompiled.exe", L"nomercy.z64", L"jessetbh/WWFNoMercyRecomp", L"91CEE3D096F4A76644D8B35B9AEAD6448909ABD1", L"NW4E", L"wwfnomercy", L"North American (NTSC-U) REV 1 release - the later revision; Rev 0 won't match", 1, true},
};

void put(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
}

std::vector<std::wstring> ids(const std::vector<GameEntry>& games) {
    std::vector<std::wstring> out;
    for (const auto& g : games) out.push_back(g.artKey);
    return out;
}
}  // namespace

static std::string narrow_key(const std::wstring& w) {  // ids are ASCII
    std::string s;
    for (wchar_t c : w) s += (char)c;
    return s;
}

static void test_json() {
    JsonValue v;
    CHECK(json_parse("\xEF\xBB\xBF{\"a\": [1, 2.5, -3e2, true, false, null, \"x\\u00e9\\ud83d\\ude00\"], \"a\": 7}", &v));
    CHECK(v.get("a") && v.get("a")->is(JsonValue::Type::Number) && v.integer("a", 0) == 7);  // last duplicate wins
    CHECK(!json_parse("{\"a\": 1,}", &v) && !json_parse("{\"a\" 1}", &v) && !json_parse("[1] x", &v));
    CHECK(!json_parse("\"tab\tinside\"", &v) && !json_parse("", &v));
    CHECK(json_parse("[\"\\u00e9\\ud83d\\ude00\"]", &v) && v.items[0].s == "\xC3\xA9\xF0\x9F\x98\x80");
    CHECK(!json_parse(std::string(100, '[') + std::string(100, ']'), &v));  // nesting cap
    CHECK(json_parse(std::string(60, '[') + std::string(60, ']'), &v));
    CHECK(json_parse("{\"n\": 1.5}", &v) && v.integer("n", -1) == -1 && v.wstr("n", L"d") == L"d");
    for (const char* bad : {"-inf", "nan", "0x10", "01", "1.", ".5", "1e", "-"}) CHECK(!json_parse(bad, &v));
    CHECK(json_parse("{\"big\": 1e10, \"neg\": -0.0}", &v) && v.integer("big", -1) == -1 && v.integer("neg", 5) == 0);
    CHECK(json_parse("[\"\\ud800\\u0041\"]", &v) && v.items[0].s == "\xEF\xBF\xBD" "A");  // unpaired: kept apart
}

void run_registry_tests(const fs::path& dir) {
    test_json();

    // The built-in registry is the old table, in the same order, with the right-ROM rules (aki-roadmap#124, #216):
    // WM2000's retail SHA1, the translation notes, and each game's imported-copy name and hint.
    std::wstring err;
    std::vector<GameEntry> games = registry_from_json(embedded_games_json(), &err);
    CHECK(games.size() == std::size(kOld));
    for (size_t i = 0; i < games.size() && i < std::size(kOld); ++i) {
        const GameEntry& g = games[i];
        const Expected& e = kOld[i];
        CHECK(g.artKey == e.id && g.title == e.title && g.releaseDate == e.date && g.region == e.region);
        CHECK(g.repoDir == e.repo && g.exeRel == e.exe && g.romFile == e.rom && g.githubRepo == e.github);
        CHECK(g.romSha1 == e.sha1 && g.romCode == e.code && g.modGameId == e.modId && g.romNote == e.note);
        CHECK(g.romVersion == e.romVersion && g.forceBorderless == e.borderless && !g.comingSoon);
        CHECK(g.baseId.empty() && g.manifestPath.empty());
        CHECK(!g.romStoredFile.empty() && g.romStoredFile.size() > 4 && g.romStoredFile.substr(g.romStoredFile.size() - 4) == L".z64");
        CHECK(g.romHint.empty() == !(g.artKey == L"vpw64" || g.artKey == L"vpw2"));
        // Model packs (aki_modelimport) on the Mods page: No Mercy and VPW2 only.
        CHECK(g.modelProfile == ((g.artKey == L"nomercy" || g.artKey == L"vpw2") ? std::string(narrow_key(g.artKey)) : std::string()));
    }

    // A registry file wins when it is valid; a broken one falls back to the built-in list.
    put(dir / L"good.json", R"({"games":[{"id":"only","title":"Only Game","exe":"Only.exe"}]})");
    auto good = load_registry(dir / L"good.json");
    CHECK(good.size() == 1 && good[0].artKey == L"only" && good[0].repoDir == L"only" && good[0].romFile == L"only.z64");
    CHECK(good.size() == 1 && good[0].romVersion == -1 && good[0].forceBorderless);
    for (const char* bad : {"{not json", R"({"games":[]})", R"({"games":[{"id":"x","title":"X"}]})",
                            R"({"games":[{"id":"a b","title":"X","exe":"x.exe"}]})",
                            R"({"games":[{"id":"x","title":"X","exe":"x.exe"},{"id":"x","title":"Y","exe":"y.exe"}]})"}) {
        put(dir / L"bad.json", bad);
        CHECK(load_registry(dir / L"bad.json").size() == std::size(kOld));
    }
    CHECK(load_registry(dir / L"missing.json").size() == std::size(kOld));

    // Discovery: manifests in depot folders and install folders add games; known ids never change.
    fs::path depot = dir / L"depot", installs = dir / L"games";
    put(depot / L"BmHeroRecomp-t3" / L"aki-game.json",
        R"({"id":"bmhero","title":"Bomberman Hero","exe":"build-msvc\\BMHeroRecompiled.exe","release_date":"April 30, 1998"})");
    put(depot / L"Vpw2Recomp-freem" / L"aki-game.json",
        R"({"id":"vpw2freem","title":"VPW2 Freem Edition","exe":"build-msvc\\Vpw2Recompiled.exe","base":"vpw2"})");
    put(depot / L"NoMercyRecomp" / L"aki-game.json", R"({"id":"nomercy","title":"Impostor","exe":"x.exe"})");
    put(depot / L"Broken" / L"aki-game.json", "{oops");
    put(depot / L"NoManifest" / L"CMakeLists.txt", "");
    put(installs / L"ecwmod" / L"aki-game.json",
        R"({"id":"ecwmod","title":"ECW Mod","exe":"build-msvc\\Wm2kRecompiled.exe","base":"wm2k"})");
    put(installs / L"misnamed" / L"aki-game.json", R"({"id":"other","title":"Other","exe":"o.exe"})");
    put(depot / L"Dots" / L"aki-game.json", R"({"id":"..","title":"Dots","exe":"x.exe"})");  // refused
    put(depot / L"Vpw2Recomp-freem2" / L"aki-game.json",
        R"({"id":"vpw2freem2","title":"Freem 2","exe":"build-msvc\\Vpw2Recompiled.exe","base":"vpw2freem"})");

    std::vector<GameEntry> all = registry_from_json(embedded_games_json(), &err);
    discover_games(all, depot, installs);
    std::vector<std::wstring> want = {L"worldtour", L"vpw64", L"revenge", L"wm2k", L"ecwmod", L"vpw2", L"vpw2freem",
                                      L"vpw2freem2", L"nomercy", L"bmhero"};
    CHECK(ids(all) == want);
    for (const auto& g : all) {
        if (g.artKey == L"nomercy") CHECK(g.title == L"WWF No Mercy" && g.manifestPath.empty());  // not replaced
        if (g.artKey == L"bmhero") CHECK(g.repoDir == L"BmHeroRecomp-t3" && !g.manifestPath.empty());
        if (g.artKey == L"vpw2freem") CHECK(g.baseId == L"vpw2" && g.repoDir == L"Vpw2Recomp-freem");
    }

    // Paths: a discovered depot game builds in its folder; a discovered install runs from games\<id>.
    put(depot / L"BmHeroRecomp-t3" / L"build-msvc" / L"BMHeroRecompiled.exe", "");
    put(installs / L"ecwmod" / L"Wm2kRecompiled.exe", "");
    resolve_game_paths(all, depot, installs);
    for (const auto& g : all) {
        if (g.artKey == L"bmhero")
            CHECK(g.exeFound && g.exePath == depot / L"BmHeroRecomp-t3" / L"build-msvc" / L"BMHeroRecompiled.exe");
        if (g.artKey == L"ecwmod")
            CHECK(g.exeFound && g.repoPath == installs / L"ecwmod" && g.exePath == installs / L"ecwmod" / L"Wm2kRecompiled.exe");
        // aki-roadmap#263: Settings compares its EXE field with this default. For an install it is the exe at the install
        // folder's root, never the registry's build-msvc path (comparing with that wrote an absolute exe= for every
        // installed game); for a depot game it is the checkout's build.
        if (g.artKey == L"ecwmod") CHECK(g.exeRegistryPath == installs / L"ecwmod" / L"Wm2kRecompiled.exe");
        if (g.artKey == L"bmhero")
            CHECK(g.exeRegistryPath == depot / L"BmHeroRecomp-t3" / L"build-msvc" / L"BMHeroRecompiled.exe");
        if (g.artKey == L"worldtour") CHECK(!g.exeFound && g.repoPath == installs / L"worldtour");
    }
}
