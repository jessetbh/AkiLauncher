#pragma once
#include <filesystem>
#include <string>
#include <vector>

struct GameEntry;

// Latest GitHub release of a game repo (prereleases count; drafts are
// invisible to unauthenticated callers anyway).
struct ReleaseInfo {
    std::wstring tag;     // e.g. "v0.1.2"
    std::wstring zipUrl;  // browser_download_url of the *-Windows.zip asset
};

// Fire-and-forget background check of every game with a githubRepo. Call once
// at startup; results appear in release_for() as they come in.
void releases_check_start(const std::vector<GameEntry>& games);
bool release_for(const std::wstring& artKey, ReleaseInfo* out);

// One install (download + extract into GameEntry::installDir) at a time, on a
// worker thread. Poll install_status() each frame; call install_ack() to
// consume a Done/Error result back to Idle.
enum class InstallPhase { Idle, Downloading, Extracting, Done, Error };
struct InstallStatus {
    InstallPhase phase = InstallPhase::Idle;
    int gameIndex = -1;
    long long got = 0, total = 0;  // download progress in bytes (total 0 = unknown)
    std::wstring tag;
    std::wstring error;
};
bool install_start(int gameIndex, const GameEntry& g, const ReleaseInfo& rel);
InstallStatus install_status();
void install_ack();

// SHA1 of a file as uppercase hex (bcrypt); empty on failure.
std::wstring sha1_file(const std::filesystem::path& p);
