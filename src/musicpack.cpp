#include "musicpack.h"

#include <algorithm>
#include <fstream>

#include "miniz.h"

namespace fs = std::filesystem;

namespace musicpack {
namespace {

std::string u8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}
fs::path from_u8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string base_name(const std::string& entry) {
    const size_t slash = entry.find_last_of("/\\");
    return slash == std::string::npos ? entry : entry.substr(slash + 1);
}
void keep_first(std::vector<Track>& tracks) {
    std::stable_sort(tracks.begin(), tracks.end(), [](const Track& a, const Track& b) { return a.id < b.id; });
    tracks.erase(std::unique(tracks.begin(), tracks.end(), [](const Track& a, const Track& b) { return a.id == b.id; }), tracks.end());
}
bool is_zip(const fs::path& p) {
    std::wstring ext = p.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    return ext == L".zip";
}

}  // namespace

int track_of_file(const std::string& name) {
    if (name.size() < 7 || name[0] != '0' || (name[1] != 'x' && name[1] != 'X')) return -1;
    const size_t dot = name.size() - 4;
    auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; };
    if (name[dot] != '.' || lower(name[dot + 1]) != 'm' || lower(name[dot + 2]) != 'p' || lower(name[dot + 3]) != '3') return -1;
    const size_t digits = dot - 2;
    if (digits < 1 || digits > 2) return -1;
    int v = 0;
    for (size_t i = 2; i < dot; i++) {
        const char c = lower(name[i]);
        const int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
        if (d < 0) return -1;
        v = v * 16 + d;
    }
    return v < 80 ? v : -1;
}

Scan scan(const fs::path& src) {
    Scan out;
    std::error_code ec;
    if (fs::is_directory(src, ec)) {
        for (auto it = fs::recursive_directory_iterator(src, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const int id = track_of_file(u8(it->path().filename()));
            if (id >= 0) out.tracks.push_back({ id, u8(fs::relative(it->path(), src, ec)) });
        }
        keep_first(out.tracks);
        return out;
    }
    if (!is_zip(src) || !fs::is_regular_file(src, ec)) {
        out.error = L"pick the pack's .zip, or the folder it unzips to";
        return out;
    }
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, u8(src).c_str(), 0)) {
        out.error = L"couldn't open " + src.filename().wstring() + L" as a zip";
        return out;
    }
    const mz_uint n = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st) || mz_zip_reader_is_file_a_directory(&zip, i)) continue;
        const int id = track_of_file(base_name(st.m_filename));
        if (id >= 0) out.tracks.push_back({ id, st.m_filename });
    }
    mz_zip_reader_end(&zip);
    keep_first(out.tracks);
    return out;
}

fs::path folder(const fs::path& dataDir, const std::string& id) { return dataDir / L"music" / from_u8(id); }

AddResult add(const fs::path& musicFolder, const fs::path& src) {
    AddResult r;
    Scan s = scan(src);
    if (!s.error.empty()) { r.message = L"Couldn't add the music: " + s.error + L"."; return r; }
    if (s.tracks.empty()) {
        r.message = L"No music tracks in " + src.filename().wstring() + L" (files named like 0xE.mp3).";
        return r;
    }
    std::error_code ec;
    const fs::path dst = musicFolder, tmp = musicFolder.parent_path() / (musicFolder.filename().wstring() + L".tmp");
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    if (ec) { r.message = L"Couldn't add the music: " + fs::path(ec.message()).wstring(); return r; }
    const bool zip = !fs::is_directory(src, ec);
    mz_zip_archive archive{};
    if (zip && !mz_zip_reader_init_file(&archive, u8(src).c_str(), 0)) {
        fs::remove_all(tmp, ec);
        r.message = L"Couldn't open " + src.filename().wstring() + L" as a zip.";
        return r;
    }
    bool good = true;
    for (const Track& t : s.tracks) {
        const fs::path out = tmp / from_u8(base_name(t.entry));
        if (zip) {
            size_t size = 0;
            void* data = mz_zip_reader_extract_file_to_heap(&archive, t.entry.c_str(), &size, 0);
            if (!data) { good = false; break; }
            std::ofstream f(out, std::ios::binary);
            f.write(static_cast<const char*>(data), std::streamsize(size));
            mz_free(data);
            if (!f.good()) { good = false; break; }
        } else {
            fs::copy_file(src / from_u8(t.entry), out, fs::copy_options::overwrite_existing, ec);
            if (ec) { good = false; break; }
        }
        r.tracks++;
    }
    if (zip) mz_zip_reader_end(&archive);
    if (!good) {
        fs::remove_all(tmp, ec);
        r.tracks = 0;
        r.message = L"Couldn't copy the music (disk full?). Nothing was changed.";
        return r;
    }
    fs::remove_all(dst, ec);
    fs::rename(tmp, dst, ec);
    if (ec) {
        fs::remove_all(tmp, ec);
        r.tracks = 0;
        r.message = L"Couldn't add the music: " + fs::path(ec.message()).wstring();
        return r;
    }
    r.ok = true;
    r.message = L"Added the music: " + std::to_wstring(r.tracks) + L" tracks.";
    return r;
}

int installed_tracks(const fs::path& musicFolder) {
    std::error_code ec;
    int n = 0;
    for (auto it = fs::directory_iterator(musicFolder, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec) && track_of_file(u8(it->path().filename())) >= 0) n++;
    return n;
}

bool remove(const fs::path& musicFolder) {
    std::error_code ec;
    fs::remove_all(musicFolder, ec);
    return !ec;
}

std::wstring status_line(bool wanted, int tracks) {
    if (tracks > 0) return L"Music: your pack, " + std::to_wstring(tracks) + L" tracks";
    if (wanted) return L"Music: not added. Its music is a separate download from the hack's release; add the zip here once you have it";
    return std::wstring();
}

}  // namespace musicpack
