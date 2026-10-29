// AkiLauncherTests: console checks that need no window, compiler or game.
// Build with -DAKI_LAUNCHER_TESTS=ON, run build-msvc\AkiLauncherTests.exe
// (exit code = number of failures).
#include "check.h"

#include <windows.h>

#include <filesystem>
#include <string>

int g_failed = 0;

void run_rebuild_tests(const std::filesystem::path& tmp);
void run_romscan_tests(const std::filesystem::path& tmp);
void run_mods_tests(const std::filesystem::path& tmp);
void run_portable_tests(const std::filesystem::path& tmp);
void run_registry_tests(const std::filesystem::path& tmp);
void run_selfupdate_tests(const std::filesystem::path& tmp);
void run_modelpacks_tests(const std::filesystem::path& tmp);
void run_quickmatch_tests(const std::filesystem::path& tmp);
void run_tour_tests(const std::filesystem::path& tmp);
void run_shots_tests(const std::filesystem::path& tmp);
void run_ipc_tests(const std::filesystem::path& tmp);

int main() {
    wchar_t tmpBuf[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmpBuf);
    std::filesystem::path tmp =
        std::filesystem::path(tmpBuf) / (L"akilauncher-tests-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(tmp);
    SetEnvironmentVariableW(L"AKI_LAUNCHER_TEST_NO_RECYCLE", L"1");  // mods tests: no Recycle Bin clutter
    run_rebuild_tests(tmp / L"rebuild");
    run_romscan_tests(tmp / L"romscan");
    run_mods_tests(tmp / L"mods");
    run_portable_tests(tmp / L"portable");
    run_registry_tests(tmp / L"registry");
    run_selfupdate_tests(tmp / L"selfupdate");
    run_modelpacks_tests(tmp / L"modelpacks");
    run_quickmatch_tests(tmp / L"quickmatch");
    run_tour_tests(tmp / L"tour");
    run_shots_tests(tmp / L"shots");
    run_ipc_tests(tmp / L"ipc");
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    printf("%s (%d failed)\n", g_failed ? "FAILED" : "OK", g_failed);
    return g_failed;
}
