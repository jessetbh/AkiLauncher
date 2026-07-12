#pragma once
#include <windows.h>

#include <string>

struct GameEntry;

enum class SessionState { Idle, WaitingForWindow, Running, Closing };

// One running game. Single-threaded: call tick() every frame from the render
// loop; it advances window discovery, borderless forcing, exit detection and
// the graceful-close timeout without blocking.
struct GameSession {
    SessionState state = SessionState::Idle;
    PROCESS_INFORMATION pi{};
    HWND gameWnd = nullptr;
    bool forceBorderless = true;
    ULONGLONG launchTick = 0;
    ULONGLONG runningTick = 0;       // when the game window was found
    ULONGLONG closeRequestTick = 0;
    ULONGLONG lastWindowPoll = 0;
    std::wstring title;
    DWORD exitCode = 0;
    bool wasTerminated = false;      // TerminateProcess fallback fired
    std::wstring statusNote;         // shown in the launcher UI after finish

    bool launch(const GameEntry& g, std::wstring* errorOut);
    void tick(HWND launcherWnd);
    void requestQuickBack();
    bool active() const { return state != SessionState::Idle; }

private:
    void finish(HWND launcherWnd);
};

void bring_to_foreground(HWND wnd);

// Hold Back+Start on any XInput pad for ~600ms. Returns true once per hold.
bool poll_quickback_chord();

void logf(const wchar_t* fmt, ...);
