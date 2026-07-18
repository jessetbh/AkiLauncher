#pragma once
#include <windows.h>

#include <filesystem>
#include <map>
#include <string>

// Per-game overrides, keyed by GameEntry::artKey. Empty string = no override;
// forceBorderless -1 = inherit the registry default.
struct GameOverride {
    std::wstring exePath;
    std::wstring romPath;
    int forceBorderless = -1;
    bool any() const { return !exePath.empty() || !romPath.empty() || forceBorderless != -1; }
};

// Per-game play stats, keyed by GameEntry::artKey. Recorded on session end.
struct GameStats {
    long long lastPlayedUnix = 0;  // time(nullptr) when the session ended
    long long playSeconds = 0;     // total, across all sessions
    int playCount = 0;
};

struct Settings {
    // XINPUT_GAMEPAD_BACK alone, held ~1s: Back+Start is a common in-game
    // combo on these ports and had side effects; a single-button hold can't
    // collide with gameplay inputs. Hold time is intentionally NOT exposed
    // in the settings UI (still honored if hand-edited in settings.ini).
    WORD chordMask = 0x0020;  // XINPUT_GAMEPAD_BACK
    int chordHoldMs = 1000;
    UINT hotkeyMods = MOD_SHIFT;
    UINT hotkeyVk = VK_F12;
    bool windowed = false;                       // launcher in a normal window (F11)
    int winX = 0, winY = 0, winW = 0, winH = 0;  // last windowed placement (0x0 = unset)
    std::map<std::wstring, GameOverride> overrides;
    std::map<std::wstring, GameStats> stats;
};

Settings& settings();
void settings_load();  // %LOCALAPPDATA%\AkiLauncher\settings.ini (missing = defaults)
void settings_save();
std::filesystem::path settings_path();

std::string chord_to_string(WORD mask);
std::string hotkey_to_string(UINT mods, UINT vk);

// "today" / "yesterday" / weekday within a week / date, in local time.
std::string format_last_played(long long unixTime);
// "under a minute" / "N minutes" / "X.Y hours".
std::string format_play_time(long long seconds);
