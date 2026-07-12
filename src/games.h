#pragma once
#include <filesystem>
#include <string>
#include <vector>

struct GameEntry {
    std::wstring title;
    std::wstring repoDir;   // folder name under the depot root
    std::wstring exeRel;    // exe path relative to the repo dir
    std::wstring romFile;   // ROM filename at the repo root
    bool experimental = false;
    bool forceBorderless = true;
    bool comingSoon = false;  // recomp project not started yet; auto-activates
                              // once the repo/exe/ROM show up at the usual paths

    // resolved by build_game_list
    std::filesystem::path repoPath;
    std::filesystem::path exePath;
    std::filesystem::path romPath;
    bool exeFound = false;
    bool romFound = false;

    bool launchable() const { return exeFound && romFound; }
};

// Depot root = env AKI_DEPOT_ROOT, else first ancestor of the launcher exe (or
// cwd) that contains WcwNwoWorldTour.
std::filesystem::path detect_depot_root();
std::vector<GameEntry> build_game_list(const std::filesystem::path& depotRoot);
