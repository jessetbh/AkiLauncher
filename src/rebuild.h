#pragma once
#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

struct GameEntry;

// Dev mode (aki-roadmap#25): developer-only actions, hidden from players. On
// with --dev, env AKI_DEV=1, or dev_mode=1 in settings.ini.
bool dev_mode();
void set_dev_mode_from_args(bool on);

// "Rebuild game": runs AkiTools' aki-build.ps1 for the game's depot checkout
// (one build at a time, through AkiTools' build slot, as owner "Launcher")
// and streams its output. Single-threaded like GameSession: call
// rebuild_tick() every frame; it reads the pipe without blocking.
enum class RebuildPhase { Idle, Running, Done, Failed };
struct RebuildStatus {
    RebuildPhase phase = RebuildPhase::Idle;
    int gameIndex = -1;
    std::wstring title;
    int stepDone = 0, stepTotal = 0;  // last Ninja "[n/m]" line (0/0 = none yet)
    DWORD exitCode = 0;
    bool cancelled = false;
    ULONGLONG startTick = 0, endTick = 0;
    std::vector<std::string> tail;    // last output lines (UTF-8)
};

// Why the game can't be rebuilt here ("" = it can): needs a depot checkout of
// the game (CMakeLists.txt) and AkiTools\aki-build.ps1 in the same depot.
std::wstring rebuild_unavailable_reason(const GameEntry& g, const std::filesystem::path& depotRoot);
// The command line rebuild_start runs (exposed for logging and review).
std::wstring rebuild_command_line(const GameEntry& g, const std::filesystem::path& depotRoot);
bool rebuild_start(int gameIndex, const GameEntry& g, const std::filesystem::path& depotRoot,
                   std::wstring* errorOut);
void rebuild_tick();
const RebuildStatus& rebuild_status();
bool rebuild_running();
// Ends the build and everything it started (the job object); the build slot's
// lock file is delete-on-close, so the slot frees itself.
void rebuild_cancel();
// Consume a Done/Failed result back to Idle.
void rebuild_ack();

// Pure helpers (no process state), for the parser and its checks.
// Appends complete lines from `bytes` (OEM code page, as powershell.exe writes
// to a pipe) to `lines` as UTF-8; an unfinished last line stays in `partial`.
void rebuild_split_output(const std::string& bytes, std::string& partial, std::vector<std::string>& lines);
// Ninja's "[12/345] ..." progress prefix; false if the line has none.
bool rebuild_parse_step(const std::string& line, int* done, int* total);
