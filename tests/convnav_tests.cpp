// Choosing and starting a conversion by controller (aki-roadmap#232): Play as... cycling, the Conversions page's
// first focus, the cart-insert art (it always plays), the tier label, the controller ROM search's folders and the
// shot runs' pad scripts.
#include "check.h"

#include "../src/convnav.h"

#include <filesystem>
#include <set>
#include <string>

namespace fs = std::filesystem;
using namespace convnav;

static void test_play_as_step() {
    // Two conversions: original -> 0 -> 1 -> original, and back the other way.
    CHECK(play_as_step(-1, 2, +1) == 0);
    CHECK(play_as_step(0, 2, +1) == 1);
    CHECK(play_as_step(1, 2, +1) == -1);
    CHECK(play_as_step(-1, 2, -1) == 1);
    CHECK(play_as_step(1, 2, -1) == 0);
    CHECK(play_as_step(0, 2, -1) == -1);
    // One conversion toggles; none stays on the original; a stale index (a conversion removed) restarts there.
    CHECK(play_as_step(-1, 1, +1) == 0 && play_as_step(0, 1, +1) == -1);
    CHECK(play_as_step(-1, 0, +1) == -1 && play_as_step(3, 0, -1) == -1);
    CHECK(play_as_step(7, 2, +1) == 0);
    // A full lap in either direction visits every slot once.
    for (int dir : {+1, -1}) {
        std::set<int> seen;
        int c = -1;
        for (int i = 0; i < 4; ++i) seen.insert(c = play_as_step(c, 3, dir));
        CHECK(seen.size() == 4 && c == -1);
    }
}

static void test_conversions_focus() {
    CHECK(conversions_focus({}, "x") == -1);
    CHECK(conversions_focus({"a", "b", "c"}, "") == 0);
    CHECK(conversions_focus({"a", "b", "c"}, "c") == 2);
    CHECK(conversions_focus({"a", "b"}, "gone") == 0);
}

static void test_insert_art() {
    // The animation never depends on art: a conversion launch plays it like a stock one.
    CHECK(insert_art(true, true) == InsertArt::Cart);
    CHECK(insert_art(true, false) == InsertArt::Cart);
    CHECK(insert_art(false, true) == InsertArt::Cover);
    CHECK(insert_art(false, false) == InsertArt::Drawn);
}

static void test_tier_badge() {
    CHECK(tier_badge(L"", false).text.empty());
    TierBadge v = tier_badge(L"Verified", false);
    CHECK(v.text == L"Verified" && !v.warn);
    CHECK(tier_badge(L"Experimental", false).warn && tier_badge(L"Unsupported", false).warn);
    // Untested wins over whatever the tier word says.
    TierBadge u = tier_badge(L"", true);
    CHECK(u.text == L"Untested" && u.warn);
    CHECK(tier_badge(L"Verified", true).text == L"Untested");
}

static void test_rom_search_dirs() {
    std::set<std::wstring> real = {L"C:\\Roms", L"D:\\N64", L"C:\\Users\\p\\Downloads"};
    auto isDir = [&](const fs::path& p) {
        std::wstring s = p.wstring();
        while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
        for (const auto& r : real)
            if (_wcsicmp(r.c_str(), s.c_str()) == 0) return true;
        return false;
    };
    auto dirs = rom_search_dirs({L"", L"C:\\Roms", L"c:\\roms\\", L"E:\\Gone", L"D:\\N64", L"C:/Roms", L"C:\\Users\\p\\Downloads"}, isDir);
    CHECK(dirs.size() == 3);
    if (dirs.size() == 3) CHECK(dirs[0] == L"C:\\Roms" && dirs[1] == L"D:\\N64" && dirs[2] == L"C:\\Users\\p\\Downloads");
    CHECK(rom_search_dirs({}, isDir).empty());
}

static void test_pad_script() {
    std::vector<PadStep> s;
    std::wstring err;
    CHECK(parse_pad_script(L"lb, DOWN shot wait3,LB+A", &s, &err));
    CHECK(s.size() == 5);
    if (s.size() == 5) {
        CHECK(s[0].buttons == 0x0100 && s[1].buttons == 0x0002 && s[2].shot && s[3].wait == 3);
        CHECK(s[4].buttons == (0x0100 | 0x1000));
    }
    CHECK(!parse_pad_script(L"A, jump", &s, &err) && err.find(L"jump") != std::wstring::npos);
    CHECK(!parse_pad_script(L"wait", &s, &err));
    CHECK(!parse_pad_script(L"wait0", &s, &err));
    CHECK(!parse_pad_script(L" , ", &s, &err));
    CHECK(!parse_pad_script(L"A+", &s, &err));

    // Playback: a press is one frame down then one up; a wait is N empty frames; a shot is one frame.
    PadStep a;
    a.buttons = 0x1000;
    PadStep w;
    w.wait = 2;
    PadStep sh;
    sh.shot = true;
    PadScriptPlayer p({a, a, w, sh});
    PadFrame f = p.next();
    CHECK(f.down == 0x1000 && !f.shot && !f.done);
    CHECK(p.next().down == 0);  // released: the second A is a new press
    CHECK(p.next().down == 0x1000);
    CHECK(p.next().down == 0);
    f = p.next();
    CHECK(f.down == 0 && !f.shot && !f.done);
    f = p.next();
    CHECK(f.down == 0 && !f.shot && !f.done);
    f = p.next();
    CHECK(f.shot && !f.done);
    CHECK(p.next().done && p.next().done);
    CHECK(!PadScriptPlayer().active() && p.active());
}

void run_convnav_tests(const fs::path&) {
    test_play_as_step();
    test_conversions_focus();
    test_insert_art();
    test_tier_badge();
    test_rom_search_dirs();
    test_pad_script();
}
