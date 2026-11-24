// aki-roadmap#322: a conversion's music pack (musicpack.h): which files are tracks, adding from a zip and a folder,
// a failed add leaving the old pack, removal, the Mods page line, and the listing seeing it. Fake bytes, no real music.
#include "check.h"

#include "../src/musicpack.h"
#include "../src/packages.h"
#include "miniz.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

void write_file(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}
std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
void write_zip(const fs::path& p, const std::vector<std::pair<std::string, std::string>>& entries) {
    mz_zip_archive za{};
    mz_zip_writer_init_heap(&za, 0, 0);
    for (auto& [name, data] : entries) mz_zip_writer_add_mem(&za, name.c_str(), data.data(), data.size(), MZ_NO_COMPRESSION);
    void* buf = nullptr;
    size_t size = 0;
    mz_zip_writer_finalize_heap_archive(&za, &buf, &size);
    std::ofstream(p, std::ios::binary).write(static_cast<const char*>(buf), std::streamsize(size));
    mz_free(buf);
    mz_zip_writer_end(&za);
}

void test_track_of_file() {
    using musicpack::track_of_file;
    CHECK(track_of_file("0xE.mp3") == 0xE);
    CHECK(track_of_file("0xe.MP3") == 0xE);
    CHECK(track_of_file("0x4F.mp3") == 0x4F);
    CHECK(track_of_file("0x0.mp3") == 0);
    CHECK(track_of_file("0x50.mp3") == -1);
    CHECK(track_of_file("0x1C - Razor Ramon.txt") == -1);
    CHECK(track_of_file("0x23_2.mp3") == -1);
    CHECK(track_of_file("theme.mp3") == -1);
}

void test_zip_add_replace_remove(const fs::path& tmp) {
    // the Legends pack's shape: Music\0x...mp3 with a note per track, plus a stray file
    const fs::path zip = tmp / L"Music.zip";
    write_zip(zip, { { "Music/0xE.mp3", "main menu" }, { "Music/0xE - Main Menu.txt", "" }, { "Music/0x38.mp3", "hogan" },
                     { "Music/0x1.mp3", "commentary" }, { "Music/readme.txt", "x" }, { "Other/0x38.mp3", "second copy" } });
    musicpack::Scan s = musicpack::scan(zip);
    CHECK(s.error.empty());
    CHECK(s.tracks.size() == 3);  // 0x1, 0xE, 0x38 (the second 0x38 left out)
    const fs::path dst = musicpack::folder(tmp / L"data", "legends_c64");
    CHECK(dst == tmp / L"data" / L"music" / L"legends_c64");
    musicpack::AddResult r = musicpack::add(dst, zip);
    CHECK(r.ok && r.tracks == 3);
    CHECK(read_file(dst / L"0x38.mp3") == "hogan");  // the first one in the zip
    CHECK(!fs::exists(dst / L"readme.txt"));
    CHECK(musicpack::installed_tracks(dst) == 3);
    CHECK(!fs::exists(tmp / L"data" / L"music" / L"legends_c64.tmp"));

    // a source with no tracks is refused and the pack stays
    const fs::path empty = tmp / L"empty.zip";
    write_zip(empty, { { "notes.txt", "x" } });
    r = musicpack::add(dst, empty);
    CHECK(!r.ok && musicpack::installed_tracks(dst) == 3);
    // not a zip, not a folder
    write_file(tmp / L"song.mp3", "x");
    r = musicpack::add(dst, tmp / L"song.mp3");
    CHECK(!r.ok && musicpack::installed_tracks(dst) == 3);

    // a folder (subfolders too) replaces the pack as a whole
    write_file(tmp / L"folder" / L"Music" / L"0x10.mp3", "exhibition");
    write_file(tmp / L"folder" / L"0x11.mp3", "options");
    r = musicpack::add(dst, tmp / L"folder");
    CHECK(r.ok && r.tracks == 2);
    CHECK(musicpack::installed_tracks(dst) == 2 && !fs::exists(dst / L"0xE.mp3"));
    CHECK(read_file(dst / L"0x10.mp3") == "exhibition");

    CHECK(musicpack::remove(dst));
    CHECK(musicpack::installed_tracks(dst) == 0 && !fs::exists(dst));
}

void test_replace_while_the_game_holds_a_file(const fs::path& tmp) {
    // review: replacing deleted the old pack in place, and a file the game held open left it half gone
    const fs::path dst = musicpack::folder(tmp / L"held", "legends_c64");
    write_file(dst / L"0xE.mp3", "old menu");
    write_file(dst / L"0x38.mp3", "old hogan");
    write_file(tmp / L"newpack" / L"0x10.mp3", "new");
    FILE* held = nullptr;
    CHECK(_wfopen_s(&held, (dst / L"0xE.mp3").c_str(), L"rb") == 0);  // as Media Foundation holds the playing track
    musicpack::AddResult r = musicpack::add(dst, tmp / L"newpack");
    CHECK(!r.ok && r.message.find(L"close the game") != std::wstring::npos);
    CHECK(musicpack::installed_tracks(dst) == 2 && read_file(dst / L"0x38.mp3") == "old hogan");
    CHECK(!fs::exists(dst.parent_path() / L"legends_c64.tmp") && !fs::exists(dst.parent_path() / L"legends_c64.old"));
    CHECK(!musicpack::remove(dst) && musicpack::installed_tracks(dst) == 2);  // Remove is all or nothing too
    if (held) fclose(held);
    r = musicpack::add(dst, tmp / L"newpack");  // the game closed: it goes
    CHECK(r.ok && musicpack::installed_tracks(dst) == 1 && !fs::exists(dst.parent_path() / L"legends_c64.old"));
}

void test_non_ascii_zip_path(const fs::path& tmp) {
    // review: zips are opened through _wfopen, so a pack under a non-ASCII folder name works
    const fs::path zip = tmp / L"müsica 日本" / L"Music.zip";
    fs::create_directories(zip.parent_path());
    write_zip(zip, { { "Music/0xE.mp3", "menu" } });
    CHECK(musicpack::scan(zip).tracks.size() == 1);
    const fs::path dst = musicpack::folder(tmp / L"däta", "legends_c64");
    CHECK(musicpack::add(dst, zip).ok && read_file(dst / L"0xE.mp3") == "menu");
}

void test_damaged_zip(const fs::path& tmp) {
    write_file(tmp / L"bad.zip", "this isn't a zip");
    musicpack::AddResult r = musicpack::add(musicpack::folder(tmp / L"bad", "x"), tmp / L"bad.zip");
    CHECK(!r.ok && r.message.find(L"disk full") == std::wstring::npos);
}

void test_status_line() {
    CHECK(musicpack::status_line(false, 0).empty());
    CHECK(musicpack::status_line(true, 0).find(L"not added") != std::wstring::npos);
    CHECK(musicpack::status_line(true, 65).find(L"65 tracks") != std::wstring::npos);
    CHECK(musicpack::status_line(false, 2).find(L"2 tracks") != std::wstring::npos);  // added by hand to one that doesn't say
}

void test_listing(const fs::path& tmp) {
    // the Mods page's list: components.music says it wants a pack; the count is the player's music\<id>\ folder
    const fs::path data = tmp / L"list";
    write_file(data / L"conversions" / L"legends_c64" / L"aki.json",
               R"({"format":"aki-conversion","format_version":1,"id":"legends_c64","name":"WWF Legends","game":"nomercy",)"
               R"("components":{"music":{"naming":"track-hex-mp3","source":"player"}}})");
    write_file(data / L"conversions" / L"plain" / L"aki.json",
               R"({"format":"aki-conversion","format_version":1,"id":"plain","name":"Plain","game":"nomercy","components":{}})");
    auto list = pkg::list_conversions(data);
    CHECK(list.size() == 2);
    for (auto& c : list) {
        if (c.id == "legends_c64") CHECK(c.musicWanted && c.musicTracks == 0);
        if (c.id == "plain") CHECK(!c.musicWanted && c.musicTracks == 0);
    }
    write_file(data / L"music" / L"legends_c64" / L"0xE.mp3", "x");
    list = pkg::list_conversions(data);
    for (auto& c : list)
        if (c.id == "legends_c64") CHECK(c.musicTracks == 1);
}

}  // namespace

void run_musicpack_tests(const fs::path& tmp) {
    fs::create_directories(tmp);
    test_track_of_file();
    test_zip_add_replace_remove(tmp);
    test_replace_while_the_game_holds_a_file(tmp);
    test_non_ascii_zip_path(tmp);
    test_damaged_zip(tmp);
    test_status_line();
    test_listing(tmp);
}
