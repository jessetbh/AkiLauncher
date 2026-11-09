// The home card's info block (aki-roadmap#240): the action button never covers an info line (the update-available
// line among them), and a tall block moves up clear of the status note and the footer, never into the carousel.
#include "check.h"

#include "../src/cardlayout.h"

#include <filesystem>

using namespace cardlayout;

// A 720-pixel window as main.cpp lays it out: title at 432, button anchor 83 below it, button 35 high.
static Input base(float stackH, float btnH = 35) {
    Input in;
    in.infoY = 432;
    in.btnAnchor = 83;
    in.stackH = stackH;
    in.gap = 9;
    in.btnH = btnH;
    in.ceilY = 370;
    in.floorY = 640;
    return in;
}

static bool button_clear_of_lines(const Input& in, const Result& r) { return r.btnY >= r.infoY + in.stackH + in.gap; }

static void test_short_block_keeps_anchor() {
    // Title, released, status: the button stays at its anchor, nothing moves.
    Input in = base(60);
    Result r = place(in);
    CHECK(r.infoY == 432 && r.btnY == 432 + 83);
}

static void test_button_drops_below_lines() {
    // Status + last played + update available run past the anchor: the button goes under them (the first fix).
    Input in = base(100);
    Result r = place(in);
    CHECK(r.infoY == 432);
    CHECK(button_clear_of_lines(in, r));
    CHECK(r.btnY == 432 + 100 + 9);
}

static void test_tall_block_moves_up() {
    // Wrong ROM + SHA1 lines + Play as... + update + dev: the button would reach 432+190+9+35 = 666 > 640. The block
    // moves up by 26 and the button still sits under the lines, clear of the status note.
    Input in = base(190);
    Result r = place(in);
    CHECK(r.infoY == 432 - 26);
    CHECK(button_clear_of_lines(in, r));
    CHECK(r.btnY + in.btnH <= in.floorY + 0.01f);
}

static void test_never_into_carousel_never_over_lines() {
    // Taller than the room between the carousel and the footer: the title stops at ceilY and the button stays under
    // the lines (past the floor) instead of covering one.
    Input in = base(300);
    Result r = place(in);
    CHECK(r.infoY == in.ceilY);
    CHECK(button_clear_of_lines(in, r));
}

static void test_no_button() {
    // No action (installing, coming soon): only the lines count, and they move up past an overrun too.
    Input in = base(100, 0);
    CHECK(place(in).infoY == 432);
    in = base(230, 0);
    Result r = place(in);
    CHECK(r.infoY == 640 - 230);
    CHECK(r.infoY + in.stackH <= in.floorY + 0.01f);
}

static void test_every_height() {
    // Over every height, the button is under the lines and the title under the carousel.
    for (float h = 0; h <= 400; h += 1.5f) {
        Input in = base(h);
        Result r = place(in);
        CHECK(button_clear_of_lines(in, r));
        CHECK(r.infoY >= in.ceilY && r.infoY <= in.infoY);
    }
}

void run_cardlayout_tests(const std::filesystem::path&) {
    test_short_block_keeps_anchor();
    test_button_drops_below_lines();
    test_tall_block_moves_up();
    test_never_into_carousel_never_over_lines();
    test_no_button();
    test_every_height();
}
