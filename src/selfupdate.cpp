#include "selfupdate.h"

#include "installer.h"
#include "json.h"
#include "launch.h"
#include "settings.h"

#include <windows.h>

#include <atomic>
#include <cwctype>
#include <ctime>
#include <fstream>
#include <mutex>
#include <thread>

#include "miniz.h"

namespace fs = std::filesystem;

static std::string w_to_u8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, 0);
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// ---------------------------------------------------------------------------
// Versions
// ---------------------------------------------------------------------------

bool parse_version(const std::wstring& text, Version* out) {
    std::wstring t = text;
    if (!t.empty() && (t[0] == L'v' || t[0] == L'V')) t.erase(0, 1);
    int parts[3] = {0, 0, 0};
    size_t i = 0;
    for (int k = 0; k < 3; ++k) {
        if (i >= t.size() || !iswdigit(t[i])) return false;
        long v = 0;
        while (i < t.size() && iswdigit(t[i])) {
            v = v * 10 + (t[i] - L'0');
            if (v > 1000000) return false;
            ++i;
        }
        parts[k] = (int)v;
        if (k < 2) {
            if (i >= t.size() || t[i] != L'.') return false;
            ++i;
        }
    }
    Version v;
    v.major = parts[0];
    v.minor = parts[1];
    v.patch = parts[2];
    if (i < t.size()) {
        if (t[i] != L'-' || i + 1 >= t.size()) return false;
        for (size_t k = i + 1; k < t.size(); ++k) {
            wchar_t c = t[k];
            if (!(iswalnum(c) || c == L'.' || c == L'-')) return false;
            v.pre += (char)c;
        }
    }
    *out = v;
    return true;
}

static int compare_pre(const std::string& a, const std::string& b) {
    // semver: a release (no pre) outranks any pre-release; identifiers compare
    // numerically when both are numbers, else as text; more identifiers win a tie.
    if (a.empty() || b.empty()) return a.empty() == b.empty() ? 0 : (a.empty() ? 1 : -1);
    size_t i = 0, j = 0;
    while (true) {
        size_t ie = a.find('.', i), je = b.find('.', j);
        std::string x = a.substr(i, ie == std::string::npos ? std::string::npos : ie - i);
        std::string y = b.substr(j, je == std::string::npos ? std::string::npos : je - j);
        bool xn = !x.empty() && x.find_first_not_of("0123456789") == std::string::npos;
        bool yn = !y.empty() && y.find_first_not_of("0123456789") == std::string::npos;
        int c;
        if (xn && yn) c = x.size() != y.size() ? (x.size() < y.size() ? -1 : 1) : x.compare(y);
        else if (xn != yn) c = xn ? -1 : 1;
        else c = x.compare(y);
        if (c) return c < 0 ? -1 : 1;
        if (ie == std::string::npos || je == std::string::npos)
            return ie == je ? 0 : (ie == std::string::npos ? -1 : 1);
        i = ie + 1;
        j = je + 1;
    }
}

int compare_versions(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return compare_pre(a.pre, b.pre);
}

std::wstring version_text(const Version& v) {
    std::wstring s = std::to_wstring(v.major) + L"." + std::to_wstring(v.minor) + L"." + std::to_wstring(v.patch);
    if (!v.pre.empty()) s += L"-" + std::wstring(v.pre.begin(), v.pre.end());
    return s;
}

bool current_launcher_version(Version* out) {
    wchar_t exe[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, exe, (DWORD)std::size(exe));
    DWORD dummy = 0;
    DWORD size = GetFileVersionInfoSizeW(exe, &dummy);
    if (!size) return false;
    std::vector<uint8_t> buf(size);
    if (!GetFileVersionInfoW(exe, 0, size, buf.data())) return false;
    VS_FIXEDFILEINFO* fi = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\", (void**)&fi, &len) || !fi) return false;
    out->major = HIWORD(fi->dwProductVersionMS);
    out->minor = LOWORD(fi->dwProductVersionMS);
    out->patch = HIWORD(fi->dwProductVersionLS);
    out->pre.clear();
    return true;
}

// ---------------------------------------------------------------------------
// Release feed
// ---------------------------------------------------------------------------

std::vector<LauncherRelease> parse_launcher_releases(const std::string& text) {
    std::vector<LauncherRelease> out;
    JsonValue root;
    if (!json_parse(text, &root) || !root.is(JsonValue::Type::Array)) return out;
    for (const auto& r : root.items) {
        if (!r.is(JsonValue::Type::Object) || r.boolean("draft", false)) continue;
        LauncherRelease rel;
        rel.tag = r.wstr("tag_name");
        if (!parse_version(rel.tag, &rel.version)) continue;
        rel.name = r.wstr("name", rel.tag);
        rel.notes = r.wstr("body");
        rel.page = r.wstr("html_url");
        rel.prerelease = r.boolean("prerelease", false);
        const JsonValue* assets = r.get("assets");
        if (!assets || !assets->is(JsonValue::Type::Array)) continue;
        for (const auto& a : assets->items) {
            std::wstring name = a.wstr("name");
            std::wstring url = a.wstr("browser_download_url");
            if (name.empty() || url.empty()) continue;
            const std::wstring suffix = L"-Windows.zip";
            if (rel.zipUrl.empty() && name.size() > suffix.size() &&
                _wcsicmp(name.c_str() + name.size() - suffix.size(), suffix.c_str()) == 0) {
                rel.zipName = name;
                rel.zipUrl = url;
                const JsonValue* sz = a.get("size");
                rel.zipSize = sz && sz->is(JsonValue::Type::Number) ? (long long)sz->n : 0;
            } else if (rel.sumsUrl.empty() && name == L"SHA256SUMS") {
                rel.sumsUrl = url;
            }
        }
        if (!rel.zipUrl.empty()) out.push_back(std::move(rel));
    }
    return out;
}

const LauncherRelease* pick_launcher_update(const std::vector<LauncherRelease>& releases, const Version& current,
                                            bool prerelease, const std::wstring& skippedTag) {
    const LauncherRelease* best = nullptr;
    for (const auto& r : releases) {
        if (r.prerelease && !prerelease) continue;
        if (!r.version.pre.empty() && !prerelease) continue;
        if (compare_versions(r.version, current) <= 0) continue;
        if (!best || compare_versions(r.version, best->version) > 0) best = &r;
    }
    if (best && !skippedTag.empty() && best->tag == skippedTag) return nullptr;
    return best;
}

bool is_dev_build(const fs::path& exeDir) {
    std::error_code ec;
    if (fs::exists(exeDir / L"CMakeCache.txt", ec)) return true;
    for (fs::path p = exeDir; !p.empty(); p = p.parent_path()) {
        if (fs::exists(p / L".git", ec) && fs::exists(p / L"CMakeLists.txt", ec)) return true;
        if (p == p.root_path()) break;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Staging and swapping
// ---------------------------------------------------------------------------

bool update_entry_allowed(const std::string& zipPath) {
    if (zipPath.empty() || zipPath.find("..") != std::string::npos || zipPath[0] == '/' || zipPath[0] == '\\' ||
        zipPath.find(':') != std::string::npos)
        return false;
    std::string p = zipPath;
    for (auto& c : p) c = (char)(c == '\\' ? '/' : tolower((unsigned char)c));
    std::string first = p.substr(0, p.find('/'));
    static const char* kept[] = {"settings.ini", "portable.txt", "aki-launcher.log", "games", "mods", ".update"};
    for (const char* k : kept)
        if (first == k) return false;
    return true;
}

static void list_files(const fs::path& root, std::vector<fs::path>& rels) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fec;
        if (it->is_regular_file(fec)) rels.push_back(it->path().lexically_relative(root));
    }
}

bool stage_update(const std::vector<uint8_t>& zip, long long expectedSize, const std::string& sums,
                  const std::wstring& zipName, const fs::path& stagingDir, std::wstring* err) {
    if (expectedSize <= 0 || (long long)zip.size() != expectedSize) {
        *err = L"the download is " + std::to_wstring(zip.size()) + L" bytes, the release says " +
               std::to_wstring(expectedSize);
        return false;
    }
    if (sums.empty() || !sha256_sums_match(sums, w_to_u8(zipName), sha256_hex(zip))) {
        *err = L"the download doesn't match the release's SHA256SUMS";
        return false;
    }
    std::error_code ec;
    fs::remove_all(stagingDir, ec);
    fs::create_directories(stagingDir, ec);
    mz_zip_archive za{};
    if (!mz_zip_reader_init_mem(&za, zip.data(), zip.size(), 0)) {
        *err = L"the download is not a zip";
        return false;
    }
    bool ok = true, hasExe = false;
    int n = (int)mz_zip_reader_get_num_files(&za);
    for (int i = 0; i < n && ok; ++i) {
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(&za, i, &st) || mz_zip_reader_is_file_a_directory(&za, i)) continue;
        std::string name = st.m_filename;
        if (!update_entry_allowed(name)) {
            logf(L"update: skipping %S from the zip (player data or outside the folder)", name.c_str());
            continue;
        }
        fs::path dst = stagingDir / json_utf8_to_wide(name);
        fs::create_directories(dst.parent_path(), ec);
        FILE* f = nullptr;
        if (_wfopen_s(&f, dst.c_str(), L"wb") != 0 || !f) {
            *err = L"can't write " + dst.wstring();
            ok = false;
            break;
        }
        bool wrote = mz_zip_reader_extract_to_cfile(&za, i, f, 0);
        if (fclose(f) != 0 || !wrote) {
            *err = L"can't unpack " + json_utf8_to_wide(name);
            ok = false;
        }
        if (_stricmp(name.c_str(), "AkiLauncher.exe") == 0) hasExe = true;
    }
    mz_zip_reader_end(&za);
    if (ok && !hasExe) {
        *err = L"the release zip has no AkiLauncher.exe at its root";
        ok = false;
    }
    if (!ok) fs::remove_all(stagingDir, ec);
    return ok;
}

bool restore_previous(const fs::path& appDir, const fs::path& stagingDir, const fs::path& previousDir,
                      std::wstring* err) {
    std::error_code ec;
    bool ok = true;
    std::vector<fs::path> added;
    list_files(stagingDir, added);
    for (const auto& rel : added) {
        // A file the update brought: back to the old copy if there was one, else gone.
        fs::path dst = appDir / rel, old = previousDir / rel;
        if (fs::exists(old, ec)) {
            fs::remove(dst, ec);
            fs::rename(old, dst, ec);
            if (ec) {
                ok = false;
                if (err) *err = L"can't put back " + dst.wstring();
            }
        } else {
            fs::remove(dst, ec);
        }
    }
    return ok;
}

bool apply_staged_update(const fs::path& appDir, const fs::path& stagingDir, const fs::path& previousDir,
                         std::wstring* err) {
    std::error_code ec;
    fs::remove_all(previousDir, ec);  // the version before last goes; the current one becomes "previous"
    fs::create_directories(previousDir, ec);
    std::vector<fs::path> files;
    list_files(stagingDir, files);
    if (files.empty()) {
        *err = L"nothing staged";
        return false;
    }
    // On failure, undo exactly what was done: files moved aside come back,
    // files created go; nothing else is touched.
    std::vector<fs::path> movedAside, created;
    auto undo = [&]() {
        std::error_code uec;
        for (const auto& rel : created) fs::remove(appDir / rel, uec);
        for (const auto& rel : movedAside) {
            fs::remove(appDir / rel, uec);
            fs::rename(previousDir / rel, appDir / rel, uec);
        }
    };
    // 1. Every file the update replaces moves aside (renaming works for the running exe).
    for (const auto& rel : files) {
        fs::path cur = appDir / rel;
        if (!fs::exists(cur, ec)) continue;
        fs::create_directories((previousDir / rel).parent_path(), ec);
        fs::rename(cur, previousDir / rel, ec);
        if (ec) {
            *err = L"can't move " + cur.wstring() + L" aside (" + json_utf8_to_wide(ec.message()) +
                   L"); is the folder writable, and nothing else using it?";
            undo();
            return false;
        }
        movedAside.push_back(rel);
    }
    // 2. The new files go in.
    for (const auto& rel : files) {
        fs::path dst = appDir / rel;
        fs::create_directories(dst.parent_path(), ec);
        fs::copy_file(stagingDir / rel, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            *err = L"can't write " + dst.wstring() + L" (" + json_utf8_to_wide(ec.message()) + L")";
            undo();
            return false;
        }
        bool wasThere = false;
        for (const auto& m : movedAside) wasThere = wasThere || m == rel;
        if (!wasThere) created.push_back(rel);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Background check + download
// ---------------------------------------------------------------------------

static std::mutex g_mu;
static UpdateStatus g_st;
static std::vector<LauncherRelease> g_releases;
static std::atomic<bool> g_busy{false};
static std::atomic<long long> g_got{0}, g_total{0};
static ULONGLONG g_lastCheck = 0;
static bool g_initDone = false;
static std::wstring g_dismissedTag;
static std::wstring g_newEtag;  // from the check thread; the UI thread stores it in settings
static constexpr ULONGLONG kCheckEveryMs = 4ull * 60 * 60 * 1000;

static fs::path app_dir() {
    wchar_t exe[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, exe, (DWORD)std::size(exe));
    return fs::path(exe).parent_path();
}

static fs::path cache_file() { return settings_path().parent_path() / L"launcher-releases.json"; }

static std::wstring now_text() {
    time_t t = time(nullptr);
    tm lt{};
    localtime_s(&lt, &t);
    wchar_t buf[32];
    wcsftime(buf, 32, L"%H:%M", &lt);
    return buf;
}

static void init_once() {
    if (g_initDone) return;
    g_initDone = true;
    Version v;
    bool haveVersion = current_launcher_version(&v);
    std::lock_guard lk(g_mu);
    g_st.current = v;
    if (!settings().updateChecks) g_st.phase = UpdatePhase::Off;
    else if (!haveVersion || is_dev_build(app_dir())) g_st.phase = UpdatePhase::DevBuild;
}

static void decide(const std::vector<LauncherRelease>& rels, bool prerelease, const std::wstring& skip) {
    std::lock_guard lk(g_mu);
    g_releases = rels;
    g_st.checkedAt = now_text();
    const LauncherRelease* r = pick_launcher_update(g_releases, g_st.current, prerelease, skip);
    if (r && r->tag != g_dismissedTag) {
        g_st.release = *r;
        g_st.phase = UpdatePhase::Available;
        logf(L"update: AKI Launcher %s is available (running %s)", r->tag.c_str(), version_text(g_st.current).c_str());
    } else {
        g_st.phase = UpdatePhase::UpToDate;
    }
}

void update_check_start() {
    init_once();
    {
        std::lock_guard lk(g_mu);
        if (g_st.phase == UpdatePhase::Off || g_st.phase == UpdatePhase::DevBuild ||
            g_st.phase == UpdatePhase::Downloading || g_st.phase == UpdatePhase::Staged)
            return;
    }
    bool expected = false;
    if (!g_busy.compare_exchange_strong(expected, true)) return;
    g_lastCheck = GetTickCount64();
    {
        std::lock_guard lk(g_mu);
        if (g_st.phase != UpdatePhase::Available) g_st.phase = UpdatePhase::Checking;
    }
    // Settings are read here, on the UI thread; the worker gets copies.
    std::wstring etag = settings().updateEtag, skip = settings().updateSkip;
    bool prerelease = settings().updatePrerelease;
    fs::path cache = cache_file();
    std::thread([etag, skip, prerelease, cache]() {
        std::string body;
        std::wstring err, newEtag;
        wchar_t feed[MAX_PATH * 2]{};
        if (GetEnvironmentVariableW(L"AKI_UPDATE_FEED", feed, (DWORD)std::size(feed))) {
            std::ifstream f(feed, std::ios::binary);  // tests and trials: a local JSON file, no network
            body.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (body.empty()) err = L"AKI_UPDATE_FEED is empty or missing";
        } else {
            std::vector<uint8_t> data;
            unsigned long status = 0;
            // Only reuse the ETag when the cached body is there to go with it.
            std::error_code ec;
            std::wstring useEtag = fs::exists(cache, ec) ? etag : L"";
            if (http_get_ex(L"https://api.github.com/repos/jessetbh/AkiLauncher/releases?per_page=10", data, nullptr,
                            nullptr, &err, useEtag, &status, &newEtag)) {
                if (status == 304) {
                    std::ifstream f(cache, std::ios::binary);
                    body.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                } else {
                    body.assign((const char*)data.data(), data.size());
                    std::ofstream f(cache, std::ios::binary | std::ios::trunc);
                    f << body;
                }
            }
        }
        if (body.empty()) {
            logf(L"update check: %s", err.c_str());
            std::lock_guard lk(g_mu);
            if (g_st.phase == UpdatePhase::Checking) {
                g_st.phase = UpdatePhase::Failed;
                g_st.error = L"update check failed: " + err;
            }
        } else {
            if (!newEtag.empty() && newEtag != etag) {
                std::lock_guard lk(g_mu);
                g_newEtag = newEtag;
            }
            decide(parse_launcher_releases(body), prerelease, skip);
        }
        g_busy = false;
    }).detach();
}

void update_tick() {
    init_once();
    ULONGLONG now = GetTickCount64();
    if (!g_lastCheck || now - g_lastCheck >= kCheckEveryMs) update_check_start();
    std::lock_guard lk(g_mu);
    if (g_st.phase == UpdatePhase::Downloading) {
        g_st.got = g_got;
        g_st.total = g_total;
    }
    if (!g_newEtag.empty()) {
        settings().updateEtag = g_newEtag;
        g_newEtag.clear();
        settings_save();
    }
}

UpdateStatus update_status() {
    std::lock_guard lk(g_mu);
    return g_st;
}

void update_dismiss() {
    std::lock_guard lk(g_mu);
    if (g_st.phase == UpdatePhase::Available || g_st.phase == UpdatePhase::Failed) {
        g_dismissedTag = g_st.release.tag;
        g_st.phase = UpdatePhase::UpToDate;
    }
}

void update_skip_version() {
    std::lock_guard lk(g_mu);
    if (g_st.phase != UpdatePhase::Available) return;
    settings().updateSkip = g_st.release.tag;
    settings_save();
    g_st.phase = UpdatePhase::UpToDate;
    logf(L"update: skipping %s", g_st.release.tag.c_str());
}

bool update_download_start() {
    LauncherRelease rel;
    {
        std::lock_guard lk(g_mu);
        if (g_st.phase != UpdatePhase::Available && g_st.phase != UpdatePhase::Failed) return false;
        if (g_st.release.zipUrl.empty()) return false;
        rel = g_st.release;
        g_st.phase = UpdatePhase::Downloading;
        g_st.error.clear();
    }
    g_got = 0;
    g_total = rel.zipSize;
    fs::path staging = app_dir() / L".update" / L"staged";
    std::thread([rel, staging]() {
        std::vector<uint8_t> zip, sums;
        std::wstring err;
        bool ok = !rel.sumsUrl.empty();
        if (!ok) err = L"the release has no SHA256SUMS: not installing an unverifiable download";
        if (ok) ok = http_get_ex(rel.zipUrl, zip, &g_got, &g_total, &err);
        if (ok) ok = http_get_ex(rel.sumsUrl, sums, nullptr, nullptr, &err);
        if (ok)
            ok = stage_update(zip, rel.zipSize, std::string((const char*)sums.data(), sums.size()), rel.zipName,
                              staging, &err);
        std::lock_guard lk(g_mu);
        if (ok) {
            g_st.phase = UpdatePhase::Staged;
            logf(L"update: %s downloaded and verified (%zu bytes)", rel.tag.c_str(), zip.size());
        } else {
            g_st.phase = UpdatePhase::Failed;
            g_st.error = L"update download failed: " + err;
            logf(L"update: %s", g_st.error.c_str());
        }
    }).detach();
    return true;
}
