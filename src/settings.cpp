#include "settings.h"

#include <xinput.h>

#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <vector>

static Settings g_settings;
Settings& settings() { return g_settings; }

std::filesystem::path settings_path() {
    wchar_t buf[MAX_PATH]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    return std::filesystem::path(buf) / L"AkiLauncher" / L"settings.ini";
}

// ---------------------------------------------------------------------------
// ini load/save (tiny key=value format with [game:<key>] sections, UTF-8)
// ---------------------------------------------------------------------------

static std::wstring u8_to_w(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, 0);
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

static std::string w_to_u8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, 0);
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

void settings_load() {
    g_settings = Settings{};
    std::ifstream f(settings_path(), std::ios::binary);
    if (!f) return;
    std::string line, section;
    bool first = true;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (first) {
            first = false;
            if (line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);  // UTF-8 BOM
        }
        if (line.empty() || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq), val = line.substr(eq + 1);
        if (section.empty()) {
            if (key == "chord_mask") g_settings.chordMask = (WORD)atoi(val.c_str());
            else if (key == "chord_hold_ms") g_settings.chordHoldMs = atoi(val.c_str());
            else if (key == "hotkey_mods") g_settings.hotkeyMods = (UINT)atoi(val.c_str());
            else if (key == "hotkey_vk") g_settings.hotkeyVk = (UINT)atoi(val.c_str());
        } else if (section.rfind("game:", 0) == 0) {
            GameOverride& o = g_settings.overrides[u8_to_w(section.substr(5))];
            if (key == "exe") o.exePath = u8_to_w(val);
            else if (key == "rom") o.romPath = u8_to_w(val);
            else if (key == "borderless") o.forceBorderless = atoi(val.c_str());
        } else if (section.rfind("stats:", 0) == 0) {
            GameStats& st = g_settings.stats[u8_to_w(section.substr(6))];
            if (key == "last_played") st.lastPlayedUnix = _atoi64(val.c_str());
            else if (key == "play_seconds") st.playSeconds = _atoi64(val.c_str());
            else if (key == "play_count") st.playCount = atoi(val.c_str());
        }
    }
    if (g_settings.chordMask == 0) g_settings.chordMask = 0x0030;
    if (g_settings.chordHoldMs < 200 || g_settings.chordHoldMs > 3000) g_settings.chordHoldMs = 600;
    if (g_settings.hotkeyVk == 0) { g_settings.hotkeyMods = MOD_SHIFT; g_settings.hotkeyVk = VK_F12; }
}

void settings_save() {
    std::error_code ec;
    std::filesystem::create_directories(settings_path().parent_path(), ec);
    std::ofstream f(settings_path(), std::ios::trunc | std::ios::binary);
    if (!f) return;
    f << "chord_mask=" << g_settings.chordMask << "\n";
    f << "chord_hold_ms=" << g_settings.chordHoldMs << "\n";
    f << "hotkey_mods=" << g_settings.hotkeyMods << "\n";
    f << "hotkey_vk=" << g_settings.hotkeyVk << "\n";
    for (auto& [key, o] : g_settings.overrides) {
        if (!o.any()) continue;
        f << "[game:" << w_to_u8(key) << "]\n";
        if (!o.exePath.empty()) f << "exe=" << w_to_u8(o.exePath) << "\n";
        if (!o.romPath.empty()) f << "rom=" << w_to_u8(o.romPath) << "\n";
        if (o.forceBorderless != -1) f << "borderless=" << o.forceBorderless << "\n";
    }
    for (auto& [key, st] : g_settings.stats) {
        if (st.lastPlayedUnix == 0 && st.playSeconds == 0 && st.playCount == 0) continue;
        f << "[stats:" << w_to_u8(key) << "]\n";
        f << "last_played=" << st.lastPlayedUnix << "\n";
        f << "play_seconds=" << st.playSeconds << "\n";
        f << "play_count=" << st.playCount << "\n";
    }
}

// ---------------------------------------------------------------------------
// display names
// ---------------------------------------------------------------------------

std::string chord_to_string(WORD mask) {
    struct { WORD bit; const char* name; } names[] = {
        {XINPUT_GAMEPAD_DPAD_UP, "DpadUp"}, {XINPUT_GAMEPAD_DPAD_DOWN, "DpadDown"},
        {XINPUT_GAMEPAD_DPAD_LEFT, "DpadLeft"}, {XINPUT_GAMEPAD_DPAD_RIGHT, "DpadRight"},
        {XINPUT_GAMEPAD_BACK, "View"}, {XINPUT_GAMEPAD_START, "Menu"},
        {XINPUT_GAMEPAD_LEFT_THUMB, "LStick"}, {XINPUT_GAMEPAD_RIGHT_THUMB, "RStick"},
        {XINPUT_GAMEPAD_LEFT_SHOULDER, "LB"}, {XINPUT_GAMEPAD_RIGHT_SHOULDER, "RB"},
        {XINPUT_GAMEPAD_A, "A"}, {XINPUT_GAMEPAD_B, "B"},
        {XINPUT_GAMEPAD_X, "X"}, {XINPUT_GAMEPAD_Y, "Y"},
    };
    std::string s;
    for (auto& n : names)
        if (mask & n.bit) {
            if (!s.empty()) s += " + ";
            s += n.name;
        }
    return s.empty() ? "(none)" : s;
}

std::string hotkey_to_string(UINT mods, UINT vk) {
    std::string s;
    if (mods & MOD_CONTROL) s += "Ctrl+";
    if (mods & MOD_ALT) s += "Alt+";
    if (mods & MOD_SHIFT) s += "Shift+";
    if (mods & MOD_WIN) s += "Win+";
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    // extended keys need bit 24 for a correct name
    switch (vk) {
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
    case VK_PRIOR: case VK_NEXT:
        sc |= 0x100;
        break;
    }
    wchar_t name[64]{};
    if (GetKeyNameTextW((LONG)(sc << 16), name, 64) > 0) {
        char nbuf[64]{};
        WideCharToMultiByte(CP_UTF8, 0, name, -1, nbuf, sizeof(nbuf), nullptr, nullptr);
        s += nbuf;
    } else {
        s += "VK" + std::to_string(vk);
    }
    return s;
}

std::string format_last_played(long long unixTime) {
    time_t then = (time_t)unixTime;
    time_t now = time(nullptr);
    tm tmThen{}, tmNow{};
    localtime_s(&tmThen, &then);
    localtime_s(&tmNow, &now);
    // day difference via local midnights (DST-safe enough for display)
    tm midThen = tmThen, midNow = tmNow;
    midThen.tm_hour = midThen.tm_min = midThen.tm_sec = 0;
    midNow.tm_hour = midNow.tm_min = midNow.tm_sec = 0;
    long long days = (long long)((mktime(&midNow) - mktime(&midThen)) / 86400);
    char buf[64]{};
    if (days <= 0) return "today";
    if (days == 1) return "yesterday";
    if (days < 7) {
        strftime(buf, sizeof(buf), "%A", &tmThen);  // weekday name
        return buf;
    }
    strftime(buf, sizeof(buf), "%B %d, %Y", &tmThen);
    return buf;
}

std::string format_play_time(long long seconds) {
    if (seconds < 60) return "under a minute";
    if (seconds < 3600) {
        long long m = seconds / 60;
        return std::to_string(m) + (m == 1 ? " minute" : " minutes");
    }
    char buf[32]{};
    snprintf(buf, sizeof(buf), "%.1f hours", (double)seconds / 3600.0);
    return buf;
}
