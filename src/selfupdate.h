#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// AKI Launcher's own updates (aki-roadmap#117): check jessetbh/AkiLauncher's
// GitHub releases, show a notice, and on the user's click download, verify
// (size + SHA256SUMS), swap the files and restart, rolling back if the new
// version doesn't come up. The pure pieces are separate for the tests.

// --- versions ---------------------------------------------------------------
struct Version {
    int major = 0, minor = 0, patch = 0;
    std::string pre;  // "beta.1" for 0.2.0-beta.1; "" = a release
};
bool parse_version(const std::wstring& text, Version* out);  // "v0.2.0", "0.2.0-beta.1"
int compare_versions(const Version& a, const Version& b);     // <0, 0, >0 (semver precedence)
std::wstring version_text(const Version& v);                  // "0.2.0" / "0.2.0-beta.1"
// A game card's "Update vX available" (aki-roadmap#262): only for a release strictly newer than the installed one.
// A different tag is not enough: before a version is published, or on a prerelease or a locally installed build,
// the newest GitHub release is older than what's installed, and offering it was a downgrade. A release tag that
// isn't a version is never offered; an installed tag that isn't one (an old or hand-made version.txt) is, as before.
bool game_update_offered(const std::wstring& installedTag, const std::wstring& releaseTag);
// A conversion on a game too old to run it (aki-roadmap#264): games before 0.3.0 don't know AKI_CONVERSION and
// silently play the original game. Returns the version the game needs ("0.3.0", or the conversion's own
// minimum_recomp_version when that is newer), or "" when it can run it. 0.3.0 pre-releases count as 0.3.0. A game without a version (a depot or dev
// build) or with one that isn't a version can: nothing to compare.
extern const wchar_t* const kFirstConversionGameVersion;
std::wstring conversion_needs_newer_game(const std::wstring& installedTag, const std::wstring& minimumVersion);
// The running exe's ProductVersion (icons/app.rc); false without a version resource.
bool current_launcher_version(Version* out);

// --- the release feed -------------------------------------------------------
struct LauncherRelease {
    std::wstring tag, name, notes, page;  // notes = the release body (Markdown, shown as text)
    Version version;
    bool prerelease = false;
    std::wstring zipName, zipUrl, sumsUrl;
    long long zipSize = 0;
};
// GitHub's /releases JSON: published releases with a *-Windows.zip asset and a
// version-like tag, newest first as listed.
std::vector<LauncherRelease> parse_launcher_releases(const std::string& json);
// The release to offer: the newest one newer than current (pre-releases only on
// that channel), unless it is the skipped tag. nullptr = nothing to offer.
const LauncherRelease* pick_launcher_update(const std::vector<LauncherRelease>& releases, const Version& current,
                                            bool prerelease, const std::wstring& skippedTag);
// aki-roadmap#292: the channel self-update follows is the running launcher's own: a release build (0.3.0) takes
// releases only, a pre-release build (0.3.1-beta.1) newer pre-releases too. Drafts never (parse_launcher_releases).
// No setting: the old update_prerelease=1 in settings.ini is ignored. Game installs count pre-releases regardless.
bool launcher_prerelease_channel(const Version& current);
// A build-folder exe (CMakeCache.txt beside it, or a checkout above it): no updates.
bool is_dev_build(const std::filesystem::path& exeDir);

// --- staging, swapping, rolling back ---------------------------------------
// Zip entries an update may write: never the player's data (settings.ini,
// portable.txt, games\, mods\, .update\) and never paths that leave the folder.
bool update_entry_allowed(const std::string& zipPath);
// Verifies the download (its size against the release, its SHA256 against
// SHA256SUMS: both required) and unpacks the allowed entries into stagingDir.
bool stage_update(const std::vector<uint8_t>& zip, long long expectedSize, const std::string& sums,
                  const std::wstring& zipName, const std::filesystem::path& stagingDir, std::wstring* err);
// Copies the staged files next to their targets (<name>.aki-new) first, then
// swaps each with two quick renames (current -> previousDir, new -> current;
// the running exe too, last), noting every step in previousDir\applied.txt.
// Existing art in assets\boxart\ is never replaced (only missing art added).
// On any failure it puts everything back before returning false.
bool apply_staged_update(const std::filesystem::path& appDir, const std::filesystem::path& stagingDir,
                         const std::filesystem::path& previousDir, std::wstring* err);
// Undoes apply_staged_update from its journal: added files go, replaced ones come back.
bool restore_previous(const std::filesystem::path& appDir, const std::filesystem::path& stagingDir,
                      const std::filesystem::path& previousDir, std::wstring* err);

// --- the background check and download --------------------------------------
enum class UpdatePhase { Off, DevBuild, Idle, Checking, UpToDate, Available, Downloading, Staged, Failed };
struct UpdateStatus {
    UpdatePhase phase = UpdatePhase::Idle;
    Version current;
    LauncherRelease release;  // Available / Downloading / Staged
    long long got = 0, total = 0;
    std::wstring error;
    std::wstring checkedAt;  // local time of the last answer
};
// Starts a check (startup, every few hours, or "Check now"). The feed is
// AKI_UPDATE_FEED (a local JSON file, for tests and trials) or GitHub with ETag.
void update_check_start();
// Call every frame: starts the periodic check when due.
void update_tick();
UpdateStatus update_status();
bool update_download_start();  // the Available release, into <app>\.update\staged
void update_dismiss();         // Later: hide the notice until the next check finds something newer
void update_mark_failed(const std::wstring& why);  // an install that didn't happen: "Try again" is offered
void update_skip_version();    // Skip this version (remembered in settings.ini)
