#pragma once
#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

struct GameEntry;

enum class SessionState { Idle, WaitingForWindow, Running, Closing };

// One running game. Single-threaded: call tick() every frame from the render
// loop; it advances window discovery, borderless forcing, exit detection and
// the graceful-close timeout without blocking.
struct GameSession {
    SessionState state = SessionState::Idle;
    PROCESS_INFORMATION pi{};
    HWND gameWnd = nullptr;
    HWND coverWnd = nullptr;         // black topmost mask during the launch transition
    bool forceBorderless = true;
    ULONGLONG launchTick = 0;
    ULONGLONG runningTick = 0;       // when the game window was found
    ULONGLONG closeRequestTick = 0;
    ULONGLONG lastWindowPoll = 0;
    RECT lastForcedRect{};           // where the game window was last forced to
    std::wstring title;
    std::wstring artKey;             // stats key of the launched game
    DWORD exitCode = 0;
    bool wasTerminated = false;      // TerminateProcess fallback fired
    std::wstring statusNote;         // shown in the launcher UI after finish
    std::filesystem::path portLogPath;  // %LOCALAPPDATA%\<exe stem>\<exe stem>.log
    std::vector<std::string> logTail;   // tail of the port's log after a bad exit

    bool launch(const GameEntry& g, std::wstring* errorOut);
    void tick(HWND launcherWnd);
    void requestQuickBack();
    bool active() const { return state != SessionState::Idle; }

private:
    void finish(HWND launcherWnd);
};

void bring_to_foreground(HWND wnd);

// Hold the configured chord (settings().chordMask) on any XInput pad for the
// configured duration. Returns true once per hold.
bool poll_quickback_chord();

// Union of currently-held buttons across all pads (for chord rebinding).
WORD pad_held_mask();

void logf(const wchar_t* fmt, ...);
