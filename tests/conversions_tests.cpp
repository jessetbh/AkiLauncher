// AkiLauncherTests: conversions.cpp's installer (add_conversion) on bad manifests, base_rom.sha1 lists, and the
// conversion notes shared with the Mods page (aki-roadmap#243).
#include "../src/conversions.h"
#include "../src/games.h"
#include "../src/installer.h"
#include "../src/packages.h"
#include "check.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
    // A conversion folder holding only this aki.json; add_conversion refuses it before reading any game.
    std::wstring add_with(const fs::path& tmp, const char* name, const std::string& json) {
        const fs::path dir = tmp / name;
        fs::create_directories(dir);
        std::ofstream(dir / "aki.json", std::ios::binary) << json;
        const std::vector<GameEntry> games;
        try {
            return add_conversion(games, dir, -1).message;
        } catch (...) {
            return L"<threw>";
        }
    }

    void put(const fs::path& p, const std::string& text) {
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << text;
    }

    using pkg::CodesVerdict;
    CodesVerdict verdict(const std::string& text, const char* port = "nomercy-rev1") { return pkg::codes_tag_verdict(text, port); }

    bool has_note(const std::vector<std::wstring>& notes, const std::wstring& part) {
        for (const auto& n : notes)
            if (n.find(part) != std::wstring::npos) return true;
        return false;
    }

    // Item 2: the codes file's first "# rom:" line decides, as the game's parse_file does (AkiTools' cases).
    void codes_tag_tests() {
        CHECK(verdict("# rom: nomercy-rev1\n810C0000 0002\n") == CodesVerdict::Native);
        CHECK(verdict("# rom: nomercy\n") == CodesVerdict::Native);
        CHECK(verdict("#ROM: NoMercy_Rev 1\n") == CodesVerdict::Native);
        CHECK(verdict("810C0000 0002\n") == CodesVerdict::Native);
        CHECK(verdict("") == CodesVerdict::Native);
        CHECK(verdict("# rom: nomercy-rev0\n") == CodesVerdict::Translated);
        CHECK(verdict("# rom: nomercy-rev1 (US)\n") == CodesVerdict::Refused);
        CHECK(verdict("# rom: wm2k-rev0\n") == CodesVerdict::Refused);
        CHECK(verdict("# rom: nomercy-rev2\n") == CodesVerdict::Refused);
        CHECK(verdict("# rom: nomercy-rev10\n") == CodesVerdict::Refused);
        // only the first tag counts
        CHECK(verdict("# rom: nomercy-rev1\n# rom: wm2k\n") == CodesVerdict::Native);
        CHECK(verdict("# Legends codes\n# rom: wm2k\n# rom: nomercy\n") == CodesVerdict::Refused);
        // a BOM and CRLF line endings are fine
        CHECK(verdict("\xEF\xBB\xBF# rom: nomercy-rev1\r\n810C0000 0002\r\n") == CodesVerdict::Native);
        CHECK(verdict("\xEF\xBB\xBF# rom: nomercy-rev0\r\n") == CodesVerdict::Translated);
        // Rev 0 codes are translated only on No Mercy; on another port another revision is refused.
        CHECK(verdict("# rom: wm2k-rev0\n", "wm2k-rev0") == CodesVerdict::Native);
        CHECK(verdict("# rom: wm2k-rev1\n", "wm2k-rev0") == CodesVerdict::Refused);
        CHECK(verdict("# rom: nomercy-rev0\n", "wm2k-rev0") == CodesVerdict::Refused);
        std::string tag;
        CHECK(pkg::codes_tag_verdict("  # rom:  wm2k-rev0  \r\n", "nomercy-rev1", &tag) == CodesVerdict::Refused && tag == "wm2k-rev0");
    }

    void notes_tests() {
        pkg::ConversionNoteFacts f;
        f.portTag = "nomercy-rev1";
        f.codesFile = "codes.txt";
        f.codesReadable = true;
        f.codesText = "810C0000 0002\n";
        // An untagged codes file with no codes.rom: the game runs it, so no "stay off" note (the old note said it did).
        pkg::ConversionNotes n = pkg::conversion_notes(f);
        CHECK(n.notes.empty() && n.codesRunnable);
        // aki.json's codes.rom that reads differently from the file is pointed out, nothing more.
        f.manifestCodesRom = "wm2k-rev0";
        n = pkg::conversion_notes(f);
        CHECK(n.codesRunnable && n.notes.size() == 1 && has_note(n.notes, L"the codes file's # rom: line decides"));
        f.manifestCodesRom = "nomercy";
        CHECK(pkg::conversion_notes(f).notes.empty());
        f.manifestCodesRom.clear();
        f.codesText = "# rom: nomercy-rev0\n810C0000 0002\n";
        n = pkg::conversion_notes(f);
        CHECK(n.codesRunnable && n.notes.size() == 1 && has_note(n.notes, L"translated to nomercy-rev1"));
        f.codesText = "# rom: wm2k-rev0\n";
        n = pkg::conversion_notes(f);
        CHECK(!n.codesRunnable && n.notes.size() == 1 && has_note(n.notes, L"tagged wm2k-rev0, so the game won't run its codes"));
        f.codesText = "# rom:\n";
        CHECK(has_note(pkg::conversion_notes(f).notes, L"empty # rom: line"));
        f.codesReadable = false;
        n = pkg::conversion_notes(f);
        CHECK(!n.codesRunnable && has_note(n.notes, L"codes.txt is missing"));
        // No game known: no codes note.
        f.portTag.clear();
        CHECK(pkg::conversion_notes(f).notes.empty());
        // Textures and the left-out ROM patch.
        pkg::ConversionNoteFacts t;
        t.texturesPackage = "Team-Pack";
        n = pkg::conversion_notes(t);
        CHECK(!n.texturesInstalled && has_note(n.notes, L"Texture package Team-Pack isn't added yet"));
        t.texturesAdded = true;
        t.texturesSha256 = "ABCD";
        t.addedSha256 = "abcd";
        CHECK(pkg::conversion_notes(t).notes.empty());
        t.addedSha256 = "beef";
        CHECK(has_note(pkg::conversion_notes(t).notes, L"sha256 differs"));
        t = {};
        t.requiresPatchBase = "WWF No Mercy (USA) (Rev 1)";
        CHECK(has_note(pkg::conversion_notes(t).notes, L"needs your own WWF No Mercy (USA) (Rev 1) ROM"));
    }

    AddResult add_safe(const std::vector<GameEntry>& games, const fs::path& dir) {
        try {
            return add_conversion(games, dir, -1);
        } catch (...) {
            AddResult r;
            r.message = L"<threw>";
            return r;
        }
    }

    // Item 1: base_rom.sha1 as a list on the aki.json-only install path (it used to skip the ROM check), and item 3:
    // the notes reach list_conversions (the Mods page).
    void sha1_list_and_listing_tests(const fs::path& tmp) {
        GameEntry g;
        g.title = L"No Mercy";
        g.artKey = L"nomercy";
        g.exeRel = L"NoMercyRecompiled.exe";
        g.repoPath = tmp / L"game";
        put(g.repoPath / L"portable.txt", "");
        g.romPath = tmp / L"rom.z64";
        put(g.romPath, std::string(65536, '\x5A'));
        g.romFound = true;
        const std::vector<GameEntry> games{ g };
        std::string have;
        for (wchar_t ch : sha1_file(g.romPath)) have += char(towupper(ch));
        const std::string other = "0123456789ABCDEF0123456789ABCDEF01234567";
        auto add = [&](const char* id, const std::string& sha1Json) {
            const fs::path dir = tmp / L"src" / id;
            put(dir / L"aki.json", std::string(R"({"format":"aki-conversion","format_version":1,"game":"nomercy","id":")") + id +
                R"(","name":"Conv","base_rom":{"sha1":)" + sha1Json + R"(,"revision":"Rev 1"}})");
            return add_safe(games, dir);
        };
        AddResult r = add("listed", "[\"" + other + "\", \"" + have + "\"]");
        CHECK(r.ok && fs::exists(g.repoPath / L"conversions" / L"listed" / L"aki.json"));
        r = add("refused", "[\"" + other + "\"]");
        CHECK(!r.ok && r.message.find(L"needs a different No Mercy ROM") != std::wstring::npos &&
              r.message.find(L"0123456789ABCDEF0123456789ABCDEF01234567") != std::wstring::npos);
        CHECK(!fs::exists(g.repoPath / L"conversions" / L"refused"));
        r = add("two", "[\"" + other + "\", \"FFFF\"]");
        CHECK(!r.ok && r.message.find(L"0123456789ABCDEF0123456789ABCDEF01234567 or FFFF") != std::wstring::npos);
        r = add("single", "\"" + have + "\"");
        CHECK(r.ok);
        std::string lowerHave = have;
        for (char& ch : lowerHave) ch = char(std::tolower((unsigned char)ch));
        r = add("single-lower", "\"" + lowerHave + "\"");
        CHECK(r.ok);
        r = add("single-other", "\"" + other + "\"");
        CHECK(!r.ok);

        const fs::path data = g.repoPath;
        put(data / L"conversions" / L"tex" / L"aki.json", R"({"id":"tex","name":"Tex","components":{"textures":{"package":"Team-Missing"}}})");
        put(data / L"conversions" / L"wcodes" / L"aki.json", R"({"format":"aki-conversion","id":"wcodes","name":"WCodes","components":{"codes":{"file":"codes.txt"}}})");
        put(data / L"conversions" / L"wcodes" / L"codes.txt", "# rom: wm2k-rev0\n810C0000 0002\n");
        put(data / L"conversions" / L"ucodes" / L"aki.json", R"({"id":"ucodes","name":"UCodes","components":{"codes":{"file":"codes.txt"}}})");
        put(data / L"conversions" / L"ucodes" / L"codes.txt", "810C0000 0002\n");
        int seen = 0;
        for (const auto& c : pkg::list_conversions(data, "nomercy-rev1")) {
            if (c.id == "tex") { seen++; CHECK(c.notes.size() == 1 && has_note(c.notes, L"Team-Missing isn't added yet")); }
            if (c.id == "wcodes") { seen++; CHECK(c.notes.size() == 1 && has_note(c.notes, L"won't run its codes")); }
            if (c.id == "ucodes") { seen++; CHECK(c.notes.empty()); }
        }
        CHECK(seen == 3);
        // Without a port tag there's no codes note (the texture note stays).
        for (const auto& c : pkg::list_conversions(data)) {
            if (c.id == "wcodes") CHECK(c.notes.empty());
            if (c.id == "tex") CHECK(c.notes.size() == 1);
        }
        // The installer's notes (add_conversion's message, conversions.cpp list_conversions) come from the same rule.
        put(tmp / L"src" / L"wc2" / L"aki.json",
            R"({"format":"aki-conversion","format_version":1,"game":"nomercy","id":"wc2","name":"WC2","components":{"codes":{"file":"codes.txt"}}})");
        put(tmp / L"src" / L"wc2" / L"codes.txt", "810C0000 0002\n");
        r = add_safe(games, tmp / L"src" / L"wc2");
        CHECK(r.ok && r.message.find(L"stay off") == std::wstring::npos && r.message.find(L"\n") == std::wstring::npos);
        int seen2 = 0;
        for (const auto& c : list_conversions(g)) {
            if (c.id == L"wcodes") { seen2++; CHECK(!c.codesRunnable && c.notes.size() == 1); }
            if (c.id == L"wc2") { seen2++; CHECK(c.codesRunnable && c.notes.empty()); }
        }
        CHECK(seen2 == 2);

        // aki-roadmap#264: a launcher-installed game older than 0.3.0 ignores AKI_CONVERSION and plays the original;
        // the conversion says what it needs (and the launch is refused with that), a 0.3.0 install or a depot build
        // (no version.txt) runs it, and an aki.json requires.minimum_recomp_version above 0.3.0 counts too.
        put(data / L"conversions" / L"needs" / L"aki.json",
            R"({"format":"aki-conversion","format_version":1,"game":"nomercy","id":"needs","name":"Needs","requires":{"minimum_recomp_version":"0.4.1"}})");
        GameEntry old = g;
        old.installedTag = L"v0.2.0";
        old.installDir = g.repoPath;
        old.exePath = g.repoPath / L"NoMercyRecompiled.exe";
        int seen3 = 0;
        for (const auto& c : list_conversions(old)) {
            if (c.id == L"wc2") { seen3++; CHECK(c.needsGame == L"0.3.0" && !c.notes.empty() &&
                                                c.notes[0].find(L"Recompiled 0.3.0 or newer (you have v0.2.0)") != std::wstring::npos); }
            if (c.id == L"needs") { seen3++; CHECK(c.needsGame == L"0.4.1"); }
        }
        CHECK(seen3 == 2);
        // The UI's list (pkg::list_conversions: Conversions page, card, Play as) says the same.
        int seen4 = 0;
        for (const auto& c : pkg::list_conversions(data, "nomercy-rev1", game_run_version(old), old.title)) {
            if (c.id == "wc2") { seen4++; CHECK(c.needsGame == L"0.3.0" && has_note(c.notes, L"Recompiled 0.3.0 or newer")); }
            if (c.id == "needs") { seen4++; CHECK(c.needsGame == L"0.4.1"); }
        }
        CHECK(seen4 == 2);
        // An exe= override or a depot build next to the old install runs another exe: the install's version says nothing.
        GameEntry overridden = old;
        overridden.exePath = tmp / L"depot" / L"NoMercyRecomp" / L"build-msvc" / L"NoMercyRecompiled.exe";
        CHECK(game_run_version(overridden).empty());
        for (const auto& c : list_conversions(overridden)) CHECK(c.needsGame.empty());
        GameEntry otherCase = old;
        otherCase.exePath = fs::path(old.installDir.wstring() + L"/NOMERCYRECOMPILED.EXE");
        CHECK(game_run_version(otherCase) == L"v0.2.0");
        GameEntry current = old;
        current.installedTag = L"v0.3.0";
        for (const auto& c : list_conversions(current)) {
            if (c.id == L"wc2") CHECK(c.needsGame.empty() && c.notes.empty());
            if (c.id == L"needs") CHECK(c.needsGame == L"0.4.1");
        }
        for (const auto& c : list_conversions(g)) CHECK(c.needsGame.empty());  // a depot / dev build: no version
    }
}

void run_conversions_tests(const fs::path& tmp) {
    const std::wstring invalid = L"aki.json isn't a valid v1 manifest.";
    // format_version as a string threw nlohmann::json::type_error and ended the launcher.
    CHECK(add_with(tmp, "fv_string", R"({ "format": "aki-conversion", "format_version": "1", "id": "a" })") == invalid);
    CHECK(add_with(tmp, "fv_missing", R"({ "format": "aki-conversion", "id": "a" })") == invalid);
    CHECK(add_with(tmp, "fv_two", R"({ "format": "aki-conversion", "format_version": 2, "id": "a" })") == invalid);
    CHECK(add_with(tmp, "fv_float", R"({ "format": "aki-conversion", "format_version": 1.5, "id": "a" })") == invalid);
    CHECK(add_with(tmp, "not_object", R"([1])") == invalid);
    // A v1 manifest gets past that check (and is refused later for its id).
    const std::wstring v1 = add_with(tmp, "fv_one", R"({ "format": "aki-conversion", "format_version": 1, "id": "Bad Id" })");
    CHECK(v1 != invalid && v1 != L"<threw>");

    codes_tag_tests();
    notes_tests();
    sha1_list_and_listing_tests(tmp / L"sha1");
}
