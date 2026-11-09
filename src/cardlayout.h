#pragma once

// Where the home card's info block goes (aki-roadmap#240), kept free of ImGui so AkiLauncherTests can check it. The
// block is the title, the stacked info lines (status, SHA1 lines, last played, Play as..., update available, dev) and
// then the action button (PLAY / DOWNLOAD / SELECT ROM). The lines are drawn top-down, so their height is known only
// after drawing: main.cpp passes last frame's height and the block settles in one frame.
namespace cardlayout {

struct Input {
    float infoY = 0;      // where the title starts when everything fits (window y, pixels)
    float btnAnchor = 0;  // the button's usual distance below infoY, kept while the lines are short
    float stackH = 0;     // title + info lines, last frame
    float gap = 0;        // space between the last line and the button
    float btnH = 0;       // 0 = no button
    float ceilY = 0;      // the highest the title may go: under the carousel's year captions
    float floorY = 0;     // the lowest anything may reach: over the status note, else the footer
};

struct Result {
    float infoY = 0;  // where to draw the title
    float btnY = 0;   // where to draw the button's top
};

// The button sits at its anchor, or just under the lines when they run past it: never over them. When the block
// would then run into the status note or the footer, the whole block moves up, no higher than ceilY. When even that
// is not enough the block keeps its order (the lines are never covered) and runs past floorY.
Result place(const Input& in);

}  // namespace cardlayout
