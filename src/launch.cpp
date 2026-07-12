#include "launch.h"

#include "games.h"
#include "settings.h"

#include <xinput.h>

#include <cstdarg>
#include <cstdio>
#include <fstream>

// ---------------------------------------------------------------------------
// Logging (WIN32 subsystem has no console; append to a file next to the exe)
// ---------------------------------------------------------------------------

static FILE* log_file() {
    static FILE* f = [] {
        wchar_t exeBuf[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
        std::wstring path(exeBuf);
        size_t slash = path.find_last_of(L'\\');
        path = path.substr(0, slash + 1) + L"aki-launcher.log";
        FILE* fp = nullptr;
        _wfopen_s(&fp, path.c_str(), L"w");
        return fp;
    }();
    return f;
}

void logf(const wchar_t* fmt, ...) {
    FILE* f = log_file();
    if (!f) return;
    fwprintf(f, L"[%8llu] ", GetTickCount64());
    va_list args;
    va_start(args, fmt);
    vfwprintf(f, fmt, args);
    va_end(args);
    fwprintf(f, L"\n");
    fflush(f);
}

// ---------------------------------------------------------------------------
// Window discovery / manipulation
// ---------------------------------------------------------------------------

struct FindWindowCtx {
    DWORD pid;
    HWND found;
};

static BOOL CALLBACK enum_windows_cb(HWND hwnd, LPARAM lparam) {
    auto* ctx = reinterpret_cast<FindWindowCtx*>(lparam);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != ctx->pid) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return TRUE;
    ctx->found = hwnd;
    return FALSE;
}

static HWND find_main_window(DWORD pid) {
    FindWindowCtx ctx{pid, nullptr};
    EnumWindows(enum_windows_cb, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

// The "same window" illusion: game window fills the launcher's monitor with no
// frame, drawn over the always-fullscreen launcher behind it.
static void force_borderless_fullscreen(HWND gameWnd, HWND launcherWnd) {
    HMONITOR mon = MonitorFromWindow(launcherWnd ? launcherWnd : gameWnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(mon, &mi)) return;
    const RECT& r = mi.rcMonitor;

    LONG_PTR style = GetWindowLongPtrW(gameWnd, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
    style |= WS_POPUP;
    SetWindowLongPtrW(gameWnd, GWL_STYLE, style);
    SetWindowPos(gameWnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
}

static bool covers_monitor(HWND gameWnd, HWND launcherWnd) {
    HMONITOR mon = MonitorFromWindow(launcherWnd ? launcherWnd : gameWnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(mon, &mi)) return true;
    RECT wr{};
    if (!GetWindowRect(gameWnd, &wr)) return true;
    return wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top &&
           wr.right >= mi.rcMonitor.right && wr.bottom >= mi.rcMonitor.bottom;
}

void bring_to_foreground(HWND wnd) {
    if (!wnd) return;
    if (IsIconic(wnd)) ShowWindow(wnd, SW_RESTORE);
    HWND fg = GetForegroundWindow();
    DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD myTid = GetCurrentThreadId();
    bool attached = false;
    if (fgTid && fgTid != myTid)
        attached = AttachThreadInput(fgTid, myTid, TRUE) != 0;
    SetForegroundWindow(wnd);
    BringWindowToTop(wnd);
    SetFocus(wnd);
    if (attached) AttachThreadInput(fgTid, myTid, FALSE);
}

// ---------------------------------------------------------------------------
// GameSession
// ---------------------------------------------------------------------------

bool GameSession::launch(const GameEntry& g, std::wstring* errorOut) {
    if (active()) return false;

    std::wstring cmdline = L"\"" + g.exePath.wstring() + L"\"";
    std::wstring cwd = g.repoPath.wstring();

    // Env is inherited by the child; set the autoboot var around CreateProcess
    // only (skips the port's own ROM-select menu).
    SetEnvironmentVariableW(L"WCW_AUTOBOOT", g.romPath.wstring().c_str());

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION newPi{};
    BOOL ok = CreateProcessW(g.exePath.wstring().c_str(), cmdline.data(), nullptr, nullptr,
                             FALSE, 0, nullptr, cwd.c_str(), &si, &newPi);
    SetEnvironmentVariableW(L"WCW_AUTOBOOT", nullptr);

    if (!ok) {
        DWORD err = GetLastError();
        if (errorOut) *errorOut = L"CreateProcess failed (error " + std::to_wstring(err) + L")";
        logf(L"launch FAILED for %s (err=%lu)", g.title.c_str(), err);
        return false;
    }

    pi = newPi;
    title = g.title;
    forceBorderless = g.forceBorderless;
    gameWnd = nullptr;
    exitCode = 0;
    wasTerminated = false;
    statusNote.clear();
    logTail.clear();
    // The ports route stdout/stderr (incl. crash backtraces) to
    // %LOCALAPPDATA%\<exe stem>\<exe stem>.log when launched without a console.
    {
        wchar_t lad[MAX_PATH]{};
        GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH);
        std::wstring stem = g.exePath.stem().wstring();
        portLogPath = std::filesystem::path(lad) / stem / (stem + L".log");
    }
    launchTick = GetTickCount64();
    runningTick = 0;
    lastWindowPoll = 0;
    state = SessionState::WaitingForWindow;
    logf(L"launched %s (pid=%lu, cwd=%s)", g.title.c_str(), pi.dwProcessId, cwd.c_str());
    return true;
}

void GameSession::tick(HWND launcherWnd) {
    if (!active()) return;
    ULONGLONG now = GetTickCount64();

    // Process gone? (any state)
    if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
        finish(launcherWnd);
        return;
    }

    if (state == SessionState::WaitingForWindow) {
        if (now - lastWindowPoll >= 250) {
            lastWindowPoll = now;
            gameWnd = find_main_window(pi.dwProcessId);
            if (gameWnd) {
                logf(L"game window found (hwnd=%p) after %llums", gameWnd, now - launchTick);
                if (forceBorderless) force_borderless_fullscreen(gameWnd, launcherWnd);
                runningTick = now;
                state = SessionState::Running;
            }
        }
    } else if (state == SessionState::Running) {
        // Ports may reapply their own window mode shortly after startup;
        // re-force for the first 5 seconds.
        if (forceBorderless && gameWnd && now - runningTick < 5000 && now - lastWindowPoll >= 500) {
            lastWindowPoll = now;
            if (IsWindow(gameWnd) && !covers_monitor(gameWnd, launcherWnd)) {
                logf(L"re-forcing borderless (window shrank/moved)");
                force_borderless_fullscreen(gameWnd, launcherWnd);
            }
        }
    } else if (state == SessionState::Closing) {
        // WM_CLOSE makes the port open its own "Are you sure you want to quit?"
        // prompt (RecompFrontend input_events.cpp SDL_QUIT handler) - the user
        // confirms in-game with the controller. So a live process here is
        // normal, NOT a hang. Only terminate if the window truly stopped
        // responding; otherwise hand control back after a grace period (the
        // user may have canceled the prompt).
        if (gameWnd && IsWindow(gameWnd) && IsHungAppWindow(gameWnd) &&
            now - closeRequestTick > 5000) {
            logf(L"game window is hung; terminating pid %lu", pi.dwProcessId);
            TerminateProcess(pi.hProcess, 1);
            wasTerminated = true;
            closeRequestTick = now;  // don't re-terminate every tick
        } else if (now - closeRequestTick > 8000) {
            logf(L"quit prompt not confirmed after 8s; assuming canceled, back to Running");
            state = SessionState::Running;
        }
    }
}

void GameSession::requestQuickBack() {
    if (!active() || state == SessionState::Closing) return;
    closeRequestTick = GetTickCount64();
    if (gameWnd && IsWindow(gameWnd)) {
        logf(L"quick-back: WM_CLOSE -> %s (opens the port's quit prompt)", title.c_str());
        PostMessageW(gameWnd, WM_CLOSE, 0, 0);
    } else {
        // No window yet; nothing graceful to close.
        logf(L"quick-back before window appeared: terminating %s", title.c_str());
        TerminateProcess(pi.hProcess, 1);
        wasTerminated = true;
    }
    state = SessionState::Closing;
}

// Last non-empty lines of the port's log, trimmed for display.
static std::vector<std::string> read_log_tail(const std::filesystem::path& path, int maxLines) {
    std::vector<std::string> lines;
    std::ifstream f(path, std::ios::binary);
    if (!f) return lines;
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;
        if (line.size() > 160) line = line.substr(0, 157) + "...";
        lines.push_back(line);
        if ((int)lines.size() > maxLines * 8) lines.erase(lines.begin());  // bound memory
    }
    if ((int)lines.size() > maxLines)
        lines.erase(lines.begin(), lines.end() - maxLines);
    return lines;
}

void GameSession::finish(HWND launcherWnd) {
    GetExitCodeProcess(pi.hProcess, &exitCode);
    logf(L"%s exited (code=%lu%s)", title.c_str(), exitCode,
         wasTerminated ? L", force-terminated" : L"");
    if (wasTerminated)
        statusNote = title + L" stopped responding and was force-closed.";
    else if (exitCode != 0)
        statusNote = title + L" exited with code " + std::to_wstring(exitCode) + L" (0x" +
                     [](DWORD c) { wchar_t b[16]; swprintf_s(b, L"%08X", c); return std::wstring(b); }(exitCode) +
                     L")";
    if (wasTerminated || exitCode != 0) {
        logTail = read_log_tail(portLogPath, 10);
        logf(L"captured %zu tail lines from %s", logTail.size(), portLogPath.c_str());
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    pi = {};
    gameWnd = nullptr;
    state = SessionState::Idle;
    bring_to_foreground(launcherWnd);
}

// ---------------------------------------------------------------------------
// Quick-back controller chord
// ---------------------------------------------------------------------------

WORD pad_held_mask() {
    WORD held = 0;
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        XINPUT_STATE st{};
        if (XInputGetState(i, &st) == ERROR_SUCCESS) held |= st.Gamepad.wButtons;
    }
    return held;
}

bool poll_quickback_chord() {
    static ULONGLONG holdStart = 0;
    static bool latched = false;

    WORD chord = settings().chordMask;
    bool chordDown = chord != 0 && (pad_held_mask() & chord) == chord;

    if (!chordDown) {
        holdStart = 0;
        latched = false;
        return false;
    }
    if (latched) return false;
    ULONGLONG now = GetTickCount64();
    if (holdStart == 0) holdStart = now;
    if (now - holdStart >= (ULONGLONG)settings().chordHoldMs) {
        latched = true;
        return true;
    }
    return false;
}
