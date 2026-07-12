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

struct Settings {
    WORD chordMask = 0x0030;  // XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START
    int chordHoldMs = 600;
    UINT hotkeyMods = MOD_SHIFT;
    UINT hotkeyVk = VK_F12;
    std::map<std::wstring, GameOverride> overrides;
};

Settings& settings();
void settings_load();  // %LOCALAPPDATA%\AkiLauncher\settings.ini (missing = defaults)
void settings_save();
std::filesystem::path settings_path();

std::string chord_to_string(WORD mask);
std::string hotkey_to_string(UINT mods, UINT vk);
