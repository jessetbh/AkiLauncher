#pragma once
#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

#include "gameipc.h"

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
    ULONGLONG coverStableSince = 0;  // when the game window last became stable under the mask
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
    GameLink link;                      // the game's end of the dev IPC (launch_set_ipc), when it connects
    // aki-roadmap#154: set before launch() to start the game as one of its conversions (AKI_CONVERSION=<id>, for this
    // start only; "" = the original game). conversionName is for the playing banner.
    std::wstring conversion;
    std::wstring conversionName;

    bool launch(const GameEntry& g, std::wstring* errorOut);
    void tick(HWND launcherWnd);
    void requestQuickBack();
    bool active() const { return state != SessionState::Idle; }

private:
    void finish(HWND launcherWnd);
    void poll_link();
};

// Screenshot mode: from then on nothing starts. GameSession::launch, install_start, rebuild_start, the launcher's
// update check and its update download all refuse, before any process, request or file.
void launch_set_blocked(bool blocked);
bool launch_blocked();
// Dev: each launch offers the game a GameLink (gameipc.h), its name and token in AKI_LAUNCHER_PIPE / _TOKEN.
void launch_set_ipc(bool on);
bool launch_ipc();
// The log file's name next to the exe (default aki-launcher.log); only before the first logf.
void log_set_name(const wchar_t* name);

void bring_to_foreground(HWND wnd);

// Hold the configured chord (settings().chordMask) on any XInput pad for the
// configured duration. Returns true once per hold.
bool poll_quickback_chord();

// Union of currently-held buttons across all pads (for chord rebinding).
WORD pad_held_mask();

void logf(const wchar_t* fmt, ...);
