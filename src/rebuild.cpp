#include "rebuild.h"

#include "games.h"
#include "launch.h"
#include "settings.h"

#include <cstdlib>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Dev mode
// ---------------------------------------------------------------------------

static bool g_devArg = false;

void set_dev_mode_from_args(bool on) { g_devArg = on; }

bool dev_mode() {
    if (g_devArg || settings().devMode) return true;
    wchar_t buf[8]{};
    DWORD n = GetEnvironmentVariableW(L"AKI_DEV", buf, 8);
    return n > 0 && n < 8 && wcscmp(buf, L"1") == 0;
}

// ---------------------------------------------------------------------------
// Output parsing (pure)
// ---------------------------------------------------------------------------

static std::string oem_to_utf8(const std::string& s) {
    if (s.empty()) return s;
    int wn = MultiByteToWideChar(CP_OEMCP, 0, s.data(), (int)s.size(), nullptr, 0);
    if (wn <= 0) return s;
    std::wstring w(wn, 0);
    MultiByteToWideChar(CP_OEMCP, 0, s.data(), (int)s.size(), w.data(), wn);
    int un = WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, nullptr, 0, nullptr, nullptr);
    std::string u(un > 0 ? un : 0, 0);
    if (un > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, u.data(), un, nullptr, nullptr);
    return u;
}

void rebuild_split_output(const std::string& bytes, std::string& partial, std::vector<std::string>& lines) {
    // '\n' is never a trail byte in the OEM code pages (DBCS trail bytes are
    // >= 0x40), so splitting the raw bytes is safe before converting a line.
    partial += bytes;
    size_t start = 0, nl;
    while ((nl = partial.find('\n', start)) != std::string::npos) {
        std::string line = partial.substr(start, nl - start);
        while (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(oem_to_utf8(line));
        start = nl + 1;
    }
    partial.erase(0, start);
}

bool rebuild_parse_step(const std::string& line, int* done, int* total) {
    if (line.size() < 5 || line[0] != '[') return false;
    char* end = nullptr;
    long d = strtol(line.c_str() + 1, &end, 10);
    if (end == line.c_str() + 1 || *end != '/') return false;
    char* end2 = nullptr;
    long t = strtol(end + 1, &end2, 10);
    if (end2 == end + 1 || *end2 != ']' || t <= 0 || d < 0 || d > t) return false;
    *done = (int)d;
    *total = (int)t;
    return true;
}

// ---------------------------------------------------------------------------
// The build process
// ---------------------------------------------------------------------------

static constexpr size_t kTailLines = 400;

static RebuildStatus g_status;
static HANDLE g_process = nullptr;
static HANDLE g_job = nullptr;
static HANDLE g_pipe = nullptr;  // read end of the child's stdout+stderr
static std::string g_partial;

static fs::path aki_build_script(const fs::path& depotRoot) { return depotRoot / L"AkiTools" / L"aki-build.ps1"; }

static fs::path checkout_dir(const GameEntry& g, const fs::path& depotRoot) { return depotRoot / g.repoDir; }

std::wstring rebuild_unavailable_reason(const GameEntry& g, const fs::path& depotRoot) {
    std::error_code ec;
    if (depotRoot.empty()) return L"no depot found (set AKI_DEPOT_ROOT)";
    if (!fs::is_regular_file(checkout_dir(g, depotRoot) / L"CMakeLists.txt", ec))
        return L"no checkout of " + g.repoDir + L" in the depot";
    if (!fs::is_regular_file(aki_build_script(depotRoot), ec)) return L"no AkiTools\\aki-build.ps1 in the depot";
    return L"";
}

std::wstring rebuild_command_line(const GameEntry& g, const fs::path& depotRoot) {
    wchar_t sysDir[MAX_PATH]{};
    GetSystemDirectoryW(sysDir, MAX_PATH);
    fs::path ps = fs::path(sysDir) / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe";
    // The CMake target is the exe's name (aki-build also accepts it); the
    // owner is what other agents see while the launcher holds the build slot.
    std::wstring target = fs::path(g.exeRel).stem().wstring();
    return L"\"" + ps.wstring() + L"\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" +
           aki_build_script(depotRoot).wstring() + L"\" -Worktree \"" + checkout_dir(g, depotRoot).wstring() +
           L"\" -Target " + target + L" -Owner Launcher";
}

static void push_lines(std::vector<std::string>& lines) {
    for (auto& l : lines) {
        int d, t;
        if (rebuild_parse_step(l, &d, &t)) {
            g_status.stepDone = d;
            g_status.stepTotal = t;
        }
        g_status.tail.push_back(std::move(l));
    }
    if (g_status.tail.size() > kTailLines)
        g_status.tail.erase(g_status.tail.begin(), g_status.tail.end() - kTailLines);
}

static void close_handles() {
    if (g_pipe) CloseHandle(g_pipe);
    if (g_process) CloseHandle(g_process);
    // KILL_ON_JOB_CLOSE: this also ends the sccache server aki-build started
    // (outside its own build job), which costs nothing: aki-build restarts it
    // before every build, and the cache itself is on disk.
    if (g_job) CloseHandle(g_job);
    g_pipe = g_process = g_job = nullptr;
}

bool rebuild_start(int gameIndex, const GameEntry& g, const fs::path& depotRoot, std::wstring* errorOut) {
    if (launch_blocked()) {
        if (errorOut) *errorOut = L"rebuilds are off in screenshot mode";
        return false;
    }
    if (g_status.phase == RebuildPhase::Running) {
        if (errorOut) *errorOut = L"a rebuild is already running (" + g_status.title + L")";
        return false;
    }
    std::wstring why = rebuild_unavailable_reason(g, depotRoot);
    if (!why.empty()) {
        if (errorOut) *errorOut = L"can't rebuild: " + why;
        return false;
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 16)) {
        if (errorOut) *errorOut = L"can't create the output pipe";
        return false;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = rebuild_command_line(g, depotRoot);
    fs::path cwd = checkout_dir(g, depotRoot);
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                             nullptr, cwd.c_str(), &si, &pi);
    CloseHandle(wr);  // only the child (and its children) write now
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        DWORD e = GetLastError();
        CloseHandle(rd);
        if (errorOut) *errorOut = L"can't start powershell.exe (error " + std::to_wstring(e) + L")";
        logf(L"rebuild: CreateProcess failed (%lu): %s", e, cmd.c_str());
        return false;
    }
    // The job holds aki-build and everything it starts, so Cancel ends them
    // all; kill-on-close, so a launcher that crashes mid-build doesn't leave a
    // headless build holding the build slot.
    g_job = CreateJobObjectW(nullptr, nullptr);
    if (g_job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li, sizeof(li));
        if (!AssignProcessToJobObject(g_job, pi.hProcess)) {
            logf(L"rebuild: no job for the build (error %lu): Cancel ends powershell only", GetLastError());
            CloseHandle(g_job);
            g_job = nullptr;
        }
    } else {
        logf(L"rebuild: CreateJobObject failed (%lu): Cancel ends powershell only", GetLastError());
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    g_process = pi.hProcess;
    g_pipe = rd;
    g_partial.clear();
    g_status = RebuildStatus{};
    g_status.phase = RebuildPhase::Running;
    g_status.gameIndex = gameIndex;
    g_status.title = g.title;
    g_status.startTick = GetTickCount64();
    logf(L"rebuild: %s", cmd.c_str());
    return true;
}

static void read_available() {
    if (!g_pipe) return;
    std::vector<std::string> lines;
    char buf[1 << 16];
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(g_pipe, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) break;
        DWORD got = 0;
        if (!ReadFile(g_pipe, buf, avail < sizeof(buf) ? avail : (DWORD)sizeof(buf), &got, nullptr) || got == 0) break;
        rebuild_split_output(std::string(buf, got), g_partial, lines);
    }
    push_lines(lines);
}

void rebuild_tick() {
    if (g_status.phase != RebuildPhase::Running) return;
    read_available();
    if (WaitForSingleObject(g_process, 0) != WAIT_OBJECT_0) return;
    // Exited: take what is left in the pipe, without waiting on any grandchild
    // that may still hold its write end.
    read_available();
    if (!g_partial.empty()) {
        std::vector<std::string> last;
        rebuild_split_output("\n", g_partial, last);
        push_lines(last);
    }
    GetExitCodeProcess(g_process, &g_status.exitCode);
    g_status.endTick = GetTickCount64();
    g_status.phase = (g_status.exitCode == 0 && !g_status.cancelled) ? RebuildPhase::Done : RebuildPhase::Failed;
    logf(L"rebuild: %s finished, exit %lu%s, %llus", g_status.title.c_str(), g_status.exitCode,
         g_status.cancelled ? L" (cancelled)" : L"", (g_status.endTick - g_status.startTick) / 1000);
    close_handles();
}

const RebuildStatus& rebuild_status() { return g_status; }

bool rebuild_running() { return g_status.phase == RebuildPhase::Running; }

void rebuild_cancel() {
    if (g_status.phase != RebuildPhase::Running) return;
    g_status.cancelled = true;
    if (g_job) TerminateJobObject(g_job, 1);
    else TerminateProcess(g_process, 1);
    logf(L"rebuild: cancel requested");
}

void rebuild_ack() {
    if (g_status.phase == RebuildPhase::Done || g_status.phase == RebuildPhase::Failed) g_status = RebuildStatus{};
}
