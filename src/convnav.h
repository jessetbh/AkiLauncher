#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// Choosing and starting a conversion by controller (aki-roadmap#232): the decisions behind the home card's Play as...
// line, the Conversions page and the cart-insert launch, kept free of ImGui and Win32 so AkiLauncherTests can check
// them. The UI in main.cpp calls these.
namespace convnav {

// Play as... cycling. current: -1 = the original game, 0..count-1 = a conversion; dir +1 (LB, C) or -1 (Shift+C).
// Wraps both ways: original -> each conversion -> original. A current out of range counts as the original.
int play_as_step(int current, int count, int dir);

// The conversion the Conversions page puts the controller on when it opens: the one Play as... has picked (by id),
// else the first. -1 when there are none.
int conversions_focus(const std::vector<std::string>& ids, const std::string& pickedId);

// What the cart-insert animation draws. It always plays: the cartridge scan, else the front cover, else a drawn
// cartridge, so no launch (stock or conversion) skips it for want of art.
enum class InsertArt { Cart, Cover, Drawn };
InsertArt insert_art(bool haveCart, bool haveCover);

// The tier as one short label for the card and the Conversions page. warn: shown in the warning colour (Untested,
// Unsupported, Experimental). An empty tier gives an empty text.
struct TierBadge {
    std::wstring text;
    bool warn = false;
};
TierBadge tier_badge(const std::wstring& tier, bool untested);

// aki-roadmap#289: the home card's ways into the Mods page. Down (arrow or D-pad) is not one: it reads as "move".
// M / Start open the Mods page, on its Conversions section (the Play as... pick's details) when the
// game has conversions. No V shortcut any more: it did the same as M (aki-roadmap#297).
struct CardKeys {
    bool keyM = false, padStart = false;
    bool keyDown = false, padDown = false;  // never open anything: card_open ignores them on purpose (#289)
};
enum class CardOpen { None, Mods, ModsOnConversions };
CardOpen card_open(const CardKeys& k, int conversions);
// The hint line under the Play as... line.
std::string play_as_hint(bool padInput);
// What Play as... calls the unconverted game wherever a player reads it (card, Conversions page, refusal note):
// a name, so title case (aki-roadmap#298).
inline constexpr const char* kOriginalGame = "The Original Game";

// The home page's key map in one place (aki-roadmap#287 #289 #291 #293 #294). The bottom hint bar: browse, play,
// flip, Settings (S / Esc, Y on a pad; Quit is a button in Settings), the in-game return key; Quick match and the
// History Tour only when quickMatch (dev mode). No Mods there: the card says it under its button (mods_hint_line).
std::string home_hint_line(bool padInput, bool quickMatch, const std::string& returnKey);
// Under the selected game's Play button: "M  Manage <title> Mods" ("Start  Manage <title> Mods" on a pad).
std::string mods_hint_line(bool padInput, const std::string& title);

// The folders a controller ROM search looks in (no file dialog): the candidates in order, existing ones only, each
// once (paths compared without case and without a trailing slash).
std::vector<std::filesystem::path> rom_search_dirs(const std::vector<std::filesystem::path>& candidates,
                                                   const std::function<bool(const std::filesystem::path&)>& isDir);

// --- Scripted controller input for shot runs (--pad-script) ------------------------------------------------------
// A shot run can drive the real UI with a pad script, so a controller-only path is shown frame by frame without
// anyone holding a pad. Steps, separated by commas or spaces, any case:
//   A B X Y LB RB START BACK UP DOWN LEFT RIGHT   press (one frame down, one frame up); join with + for a chord: LB+A
//   wait<N>                                        N frames with nothing pressed (frames are 1/60 s in a script run)
//   shot                                           save a frame
// Buttons are XInput's XINPUT_GAMEPAD_* bits.
struct PadStep {
    uint16_t buttons = 0;
    int wait = 0;
    bool shot = false;
};
bool parse_pad_script(const std::wstring& text, std::vector<PadStep>* out, std::wstring* err);

struct PadFrame {
    uint16_t down = 0;  // buttons down this frame (each press lasts one frame)
    bool shot = false;  // save this frame
    bool done = false;  // the script is over (nothing more happens)
};
class PadScriptPlayer {
public:
    explicit PadScriptPlayer(std::vector<PadStep> steps = {}) : steps_(std::move(steps)) {}
    PadFrame next();  // one call per frame
    bool active() const { return !steps_.empty(); }

private:
    std::vector<PadStep> steps_;
    size_t at_ = 0;
    int phase_ = 0;  // within the current step: frames spent on it
};

}  // namespace convnav
