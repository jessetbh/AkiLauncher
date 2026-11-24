#include "musicpack.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <memory>

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
// The zip opened through _wfopen (any path, as mods.cpp's ZipFile does), not miniz's own fopen of a UTF-8 name.
struct Zip {
    FILE* f = nullptr;
    mz_zip_archive a{};
    bool open = false;
    explicit Zip(const fs::path& p) {
        if (_wfopen_s(&f, p.c_str(), L"rb") != 0) { f = nullptr; return; }
        open = mz_zip_reader_init_cfile(&a, f, 0, 0) != 0;
    }
    ~Zip() {
        if (open) mz_zip_reader_end(&a);
        if (f) fclose(f);
    }
};

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
    Zip zip(src);
    if (!zip.open) {
        out.error = L"couldn't open " + src.filename().wstring() + L" as a zip";
        return out;
    }
    const mz_uint n = mz_zip_reader_get_num_files(&zip.a);
    for (mz_uint i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip.a, i, &st) || mz_zip_reader_is_file_a_directory(&zip.a, i)) continue;
        const int id = track_of_file(base_name(st.m_filename));
        if (id >= 0) out.tracks.push_back({ id, st.m_filename });
    }
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
    const fs::path dst = musicFolder;
    const fs::path tmp = musicFolder.parent_path() / (musicFolder.filename().wstring() + L".tmp");
    const fs::path old = musicFolder.parent_path() / (musicFolder.filename().wstring() + L".old");
    fs::remove_all(tmp, ec);
    fs::remove_all(old, ec);
    fs::create_directories(tmp, ec);
    if (ec) { r.message = L"Couldn't add the music: " + fs::path(ec.message()).wstring(); return r; }
    const bool zip = !fs::is_directory(src, ec);
    std::unique_ptr<Zip> archive;
    if (zip) {
        archive = std::make_unique<Zip>(src);
        if (!archive->open) {
            fs::remove_all(tmp, ec);
            r.message = L"Couldn't open " + src.filename().wstring() + L" as a zip.";
            return r;
        }
    }
    std::wstring failed;
    for (const Track& t : s.tracks) {
        const fs::path out = tmp / from_u8(base_name(t.entry));
        if (zip) {
            size_t size = 0;
            void* data = mz_zip_reader_extract_file_to_heap(&archive->a, t.entry.c_str(), &size, 0);
            if (!data) { failed = L"couldn't read " + from_u8(t.entry).wstring() + L" from the zip (is it damaged?)"; break; }
            std::ofstream f(out, std::ios::binary);
            f.write(static_cast<const char*>(data), std::streamsize(size));
            mz_free(data);
            if (!f.good()) { failed = L"couldn't write the music (is the disk full?)"; break; }
        } else {
            fs::copy_file(src / from_u8(t.entry), out, fs::copy_options::overwrite_existing, ec);
            if (ec) { failed = L"couldn't copy " + from_u8(t.entry).wstring() + L": " + fs::path(ec.message()).wstring(); break; }
        }
        r.tracks++;
    }
    archive.reset();
    if (!failed.empty()) {
        fs::remove_all(tmp, ec);
        r.tracks = 0;
        r.message = L"Couldn't add the music: " + failed + L". Nothing was changed.";
        return r;
    }
    // The old pack goes aside whole first (review: deleting it in place half-failed while the game held a file open).
    if (fs::exists(dst, ec)) {
        fs::rename(dst, old, ec);
        if (ec) {
            fs::remove_all(tmp, ec);
            r.tracks = 0;
            r.message = L"Couldn't replace the music: close the game first. Nothing was changed.";
            return r;
        }
    }
    fs::rename(tmp, dst, ec);
    if (ec) {
        std::error_code ec2;
        fs::rename(old, dst, ec2);  // the old pack back
        fs::remove_all(tmp, ec2);
        r.tracks = 0;
        r.message = L"Couldn't add the music: " + fs::path(ec.message()).wstring() + L". Nothing was changed.";
        return r;
    }
    fs::remove_all(old, ec);
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
    // aside first, whole (re-review: deleting in place while the game held a file open left part of the pack)
    std::error_code ec;
    if (!fs::exists(musicFolder, ec)) return true;
    const fs::path gone = musicFolder.parent_path() / (musicFolder.filename().wstring() + L".removing");
    fs::remove_all(gone, ec);
    fs::rename(musicFolder, gone, ec);
    if (ec) return false;
    fs::remove_all(gone, ec);
    return true;
}

std::wstring status_line(bool wanted, int tracks) {
    if (tracks > 0) return L"Music: your pack, " + std::to_wstring(tracks) + L" tracks";
    if (wanted) return L"Music: not added. Its music is a separate download from the hack's release; add the zip here once you have it";
    return std::wstring();
}

}  // namespace musicpack
