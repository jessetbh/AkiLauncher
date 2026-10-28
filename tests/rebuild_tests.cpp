// Checks for the dev-mode Rebuild game action (src/rebuild.cpp): the output
// parser, and the real process path (pipe, job, exit code, cancel) against a
// fake depot whose AkiTools\aki-build.ps1 is a short script. No window, no
// compiler, no game: build with -DAKI_LAUNCHER_TESTS=ON, run
// build-msvc\AkiLauncherTests.exe (exit code = number of failures).
#include "../src/games.h"
#include "../src/rebuild.h"

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static int g_failed = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            ++g_failed;                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                 \
    } while (0)

static void write_file(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
}

static bool contains(const std::vector<std::string>& lines, const std::string& s) {
    for (auto& l : lines)
        if (l.find(s) != std::string::npos) return true;
    return false;
}

static void test_parser() {
    std::string partial;
    std::vector<std::string> lines;
    rebuild_split_output("[1/10] Building C", partial, lines);
    CHECK(lines.empty() && partial == "[1/10] Building C");
    rebuild_split_output("XX object a.obj\r\n[2/10] Linking\r\nhalf", partial, lines);
    CHECK(lines.size() == 2 && lines[0] == "[1/10] Building CXX object a.obj" && lines[1] == "[2/10] Linking");
    CHECK(partial == "half");

    int d = 0, t = 0;
    CHECK(rebuild_parse_step("[12/345] Building CXX object x.obj", &d, &t) && d == 12 && t == 345);
    CHECK(!rebuild_parse_step("[12/345 Building", &d, &t));
    CHECK(!rebuild_parse_step("[/5] x", &d, &t));
    CHECK(!rebuild_parse_step("[6/5] x", &d, &t));
    CHECK(!rebuild_parse_step("BUILD-DONE x exit=0 3s", &d, &t));
    CHECK(!rebuild_parse_step("[0/0] x", &d, &t));
}

static GameEntry nomercy() {
    GameEntry g;
    g.title = L"WWF No Mercy";
    g.repoDir = L"NoMercyRecomp";
    g.exeRel = L"build-msvc\\NoMercyRecompiled.exe";
    return g;
}

static void run_until_done(DWORD timeoutMs) {
    ULONGLONG t0 = GetTickCount64();
    while (rebuild_running() && GetTickCount64() - t0 < timeoutMs) {
        rebuild_tick();
        Sleep(20);
    }
}

// A fake aki-build: echoes its arguments, prints Ninja-style progress, and
// exits with $env:FAKE_EXIT; with $env:FAKE_SLEEP it starts a child that
// sleeps (its PID goes to child.pid) and waits on it.
static const char* kFakeBuild = R"PS(param([string]$Worktree, [string]$Target, [string]$Owner)
Write-Output "args: $Worktree | $Target | $Owner"
Write-Output "[1/3] Building CXX object a.obj"
[Console]::Error.WriteLine("a warning on stderr")
Write-Output "[3/3] Linking CXX executable $Target.exe"
if ($env:FAKE_SLEEP) {
    $p = Start-Process powershell.exe -ArgumentList '-NoProfile', '-Command', 'Start-Sleep 60' -NoNewWindow -PassThru
    Set-Content (Join-Path $PSScriptRoot 'child.pid') $p.Id
    $p.WaitForExit()
}
Write-Output "BUILD-DONE $Worktree exit=$env:FAKE_EXIT 1s"
exit [int]$env:FAKE_EXIT
)PS";

static void test_process(const fs::path& depot) {
    GameEntry g = nomercy();
    // Unavailable: no checkout, then no aki-build.
    CHECK(!rebuild_unavailable_reason(g, depot).empty());
    write_file(depot / L"NoMercyRecomp" / L"CMakeLists.txt", "");
    CHECK(rebuild_unavailable_reason(g, depot).find(L"aki-build") != std::wstring::npos);
    write_file(depot / L"AkiTools" / L"aki-build.ps1", kFakeBuild);
    CHECK(rebuild_unavailable_reason(g, depot).empty());
    CHECK(rebuild_unavailable_reason(g, fs::path()).find(L"depot") != std::wstring::npos);
    std::wstring cmd = rebuild_command_line(g, depot);
    CHECK(cmd.find(L"-Target NoMercyRecompiled -Owner Launcher") != std::wstring::npos);

    // Success: output streams, stderr too, progress parsed, exit 0 -> Done.
    SetEnvironmentVariableW(L"FAKE_EXIT", L"0");
    std::wstring err;
    CHECK(rebuild_start(5, g, depot, &err));
    CHECK(!rebuild_start(5, g, depot, &err) && err.find(L"already running") != std::wstring::npos);
    run_until_done(60000);
    const RebuildStatus& s = rebuild_status();
    CHECK(s.phase == RebuildPhase::Done && s.exitCode == 0 && s.gameIndex == 5);
    CHECK(contains(s.tail, "| NoMercyRecompiled | Launcher"));
    CHECK(contains(s.tail, (depot / L"NoMercyRecomp").string()));
    CHECK(contains(s.tail, "a warning on stderr"));
    CHECK(contains(s.tail, "BUILD-DONE"));
    CHECK(s.stepDone == 3 && s.stepTotal == 3);
    rebuild_ack();
    CHECK(rebuild_status().phase == RebuildPhase::Idle);

    // Failure: the script's exit code comes through.
    SetEnvironmentVariableW(L"FAKE_EXIT", L"7");
    CHECK(rebuild_start(5, g, depot, &err));
    run_until_done(60000);
    CHECK(rebuild_status().phase == RebuildPhase::Failed && rebuild_status().exitCode == 7);
    CHECK(!rebuild_status().cancelled);
    rebuild_ack();

    // Cancel: ends the script and the child it started (the job).
    SetEnvironmentVariableW(L"FAKE_EXIT", L"0");
    SetEnvironmentVariableW(L"FAKE_SLEEP", L"1");
    fs::path pidFile = depot / L"AkiTools" / L"child.pid";
    CHECK(rebuild_start(5, g, depot, &err));
    ULONGLONG t0 = GetTickCount64();
    while (!fs::exists(pidFile) && GetTickCount64() - t0 < 30000) {
        rebuild_tick();
        Sleep(50);
    }
    Sleep(300);
    DWORD childPid = 0;
    {
        std::ifstream f(pidFile);
        f >> childPid;
    }
    CHECK(childPid != 0);
    rebuild_cancel();
    run_until_done(15000);
    CHECK(rebuild_status().phase == RebuildPhase::Failed && rebuild_status().cancelled);
    HANDLE child = OpenProcess(SYNCHRONIZE, FALSE, childPid);
    bool childGone = !child || WaitForSingleObject(child, 5000) == WAIT_OBJECT_0;
    if (child) CloseHandle(child);
    CHECK(childGone);
    rebuild_ack();
    SetEnvironmentVariableW(L"FAKE_SLEEP", nullptr);
}

int main() {
    wchar_t tmpBuf[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmpBuf);
    fs::path depot = fs::path(tmpBuf) / (L"akilauncher-rebuild-" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(depot);
    test_parser();
    test_process(depot);
    std::error_code ec;
    fs::remove_all(depot, ec);
    printf("%s (%d failed)\n", g_failed ? "FAILED" : "OK", g_failed);
    return g_failed;
}
