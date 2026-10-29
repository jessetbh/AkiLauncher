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
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    printf("%s (%d failed)\n", g_failed ? "FAILED" : "OK", g_failed);
    return g_failed;
}
