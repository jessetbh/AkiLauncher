#pragma once
// aki-roadmap#322: a conversion's music pack, the player's own download (e.g. WWF Legends' "Music.zip": 0x<track>.mp3
// files for the hack community's music tool). AKI Launcher only copies the player's files into the folder
// <game data>\music\<id> (user data beside saves\<id>: an update or removal of the conversion leaves it); the game
// plays them (NoMercyRecomp src/main/conversion_music.cpp). Nothing of a pack is shipped, bundled or uploaded.
#include <filesystem>
#include <string>
#include <vector>

namespace musicpack {

// The track a pack file stands for ("0xE.mp3", "0x1c.MP3" -> 0x1C), -1 for anything else. The game's rule
// (music_policy::track_of_file): one or two hex digits, tracks 0-79.
int track_of_file(const std::string& name);

struct Track {
    int id = -1;
    std::string entry;   // the zip entry or the file's path relative to the folder (UTF-8)
};
struct Scan {
    std::vector<Track> tracks;  // by id; a second file for one id (another folder in the zip) is left out
    std::wstring error;         // couldn't read the source ("" = read; tracks may still be empty)
};
// A .zip or a folder (subfolders too: packs unzip to Music\...).
Scan scan(const std::filesystem::path& src);

struct AddResult {
    bool ok = false;
    int tracks = 0;
    std::wstring message;
};
// The pack folder for conversion id: <game data>\music\<id>\.
std::filesystem::path folder(const std::filesystem::path& dataDir, const std::string& id);
// Replaces the pack folder with the source's track files (through a temporary folder, so a failed copy leaves the
// old pack). Refuses a source with no track files.
AddResult add(const std::filesystem::path& musicFolder, const std::filesystem::path& src);
// Track files in the pack folder (0 = none added).
int installed_tracks(const std::filesystem::path& musicFolder);
// Removes the pack folder. False if it couldn't.
bool remove(const std::filesystem::path& musicFolder);
// The Mods page's line for a conversion that wants a pack (aki.json components.music) or has one.
std::wstring status_line(bool wanted, int tracks);

}  // namespace musicpack
