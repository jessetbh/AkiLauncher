#include "installer.h"

#include <windows.h>

#include <bcrypt.h>
#include <winhttp.h>

#include "miniz.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

#include "games.h"
#include "launch.h"  // logf

namespace fs = std::filesystem;

static std::wstring u8_to_w(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, 0);
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

static std::string w_to_u8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, 0);
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

// ---------------------------------------------------------------------------
// HTTP GET into memory (WinHTTP, follows redirects, https only in practice)
// ---------------------------------------------------------------------------

static bool http_get(const std::wstring& url, std::vector<uint8_t>& out,
                     std::atomic<long long>* got, std::atomic<long long>* total,
                     std::wstring* err) {
    wchar_t host[256]{}, path[2048]{}, extra[2048]{};
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2047;
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 2047;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        if (err) *err = L"bad URL";
        return false;
    }
    std::wstring fullPath = std::wstring(path) + extra;
    bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;

    HINTERNET ses = WinHttpOpen(L"AkiLauncher/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) ses = WinHttpOpen(L"AkiLauncher/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) {
        if (err) *err = L"WinHttpOpen failed";
        return false;
    }
    bool ok = false;
    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    HINTERNET req = nullptr;
    if (con) {
        req = WinHttpOpenRequest(con, L"GET", fullPath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    }
    if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
                                  0, 0, 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            DWORD len = 0;
            sz = sizeof(len);
            if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &len, &sz,
                                    WINHTTP_NO_HEADER_INDEX) &&
                total)
                *total = len;
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(req, &avail)) break;
                if (avail == 0) {
                    ok = true;
                    break;
                }
                size_t at = out.size();
                out.resize(at + avail);
                DWORD read = 0;
                if (!WinHttpReadData(req, out.data() + at, avail, &read)) break;
                out.resize(at + read);
                if (got) *got = (long long)out.size();
            }
            if (!ok && err) *err = L"read failed mid-transfer";
        } else if (err) {
            *err = L"HTTP " + std::to_wstring(status);
        }
    } else if (err) {
        *err = L"connection failed (error " + std::to_wstring(GetLastError()) + L")";
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}

// ---------------------------------------------------------------------------
// Release check
// ---------------------------------------------------------------------------

static std::mutex g_relMx;
static std::map<std::wstring, ReleaseInfo> g_releases;

// Newest (first) release in the /releases JSON: its tag_name, then the first
// .zip asset URL before the next release object begins. GitHub serializes
// tag_name before the assets array, and orders releases newest-first.
static bool parse_first_release(const std::string& body, ReleaseInfo* out) {
    const std::string tagKey = "\"tag_name\":\"";
    const std::string urlKey = "\"browser_download_url\":\"";
    size_t tp = body.find(tagKey);
    if (tp == std::string::npos) return false;
    tp += tagKey.size();
    size_t te = body.find('"', tp);
    if (te == std::string::npos) return false;
    std::string tag = body.substr(tp, te - tp);
    size_t nextRel = body.find(tagKey, te);
    std::string zip, sums;
    for (size_t at = te;;) {
        size_t up = body.find(urlKey, at);
        if (up == std::string::npos || (nextRel != std::string::npos && up > nextRel)) break;
        up += urlKey.size();
        size_t ue = body.find('"', up);
        if (ue == std::string::npos) break;
        std::string u = body.substr(up, ue - up);
        if (zip.empty() && u.size() > 4 && u.compare(u.size() - 4, 4, ".zip") == 0) zip = u;
        const std::string sk = "SHA256SUMS";
        if (sums.empty() && u.size() > sk.size() &&
            u.compare(u.size() - sk.size(), sk.size(), sk) == 0)
            sums = u;
        at = ue;
    }
    if (zip.empty()) return false;
    out->tag = u8_to_w(tag);
    out->zipUrl = u8_to_w(zip);
    out->sumsUrl = u8_to_w(sums);
    return true;
}

void releases_check_start(const std::vector<GameEntry>& games) {
    std::vector<std::pair<std::wstring, std::wstring>> targets;  // artKey, owner/repo
    for (const auto& g : games)
        if (!g.githubRepo.empty()) targets.emplace_back(g.artKey, g.githubRepo);
    if (targets.empty()) return;
    std::thread([targets] {
        for (const auto& [key, repo] : targets) {
            std::vector<uint8_t> body;
            std::wstring err;
            std::wstring url = L"https://api.github.com/repos/" + repo + L"/releases?per_page=5";
            if (!http_get(url, body, nullptr, nullptr, &err)) {
                logf(L"release check %s: %s", repo.c_str(), err.c_str());
                continue;
            }
            ReleaseInfo rel;
            if (!parse_first_release(std::string((const char*)body.data(), body.size()), &rel)) {
                logf(L"release check %s: no zip release found", repo.c_str());
                continue;
            }
            {
                std::lock_guard lk(g_relMx);
                g_releases[key] = rel;
            }
            logf(L"release check %s: %s (%s)", repo.c_str(), rel.tag.c_str(), rel.zipUrl.c_str());
        }
    }).detach();
}

bool release_for(const std::wstring& artKey, ReleaseInfo* out) {
    std::lock_guard lk(g_relMx);
    auto it = g_releases.find(artKey);
    if (it == g_releases.end()) return false;
    if (out) *out = it->second;
    return true;
}

// ---------------------------------------------------------------------------
// Install worker
// ---------------------------------------------------------------------------

static std::mutex g_instMx;
static InstallStatus g_inst;                       // error/tag/index under mutex
static std::atomic<int> g_phase{(int)InstallPhase::Idle};
static std::atomic<long long> g_got{0}, g_total{0};

static std::string sha256_mem(const std::vector<uint8_t>& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string result;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
            UCHAR digest[32]{};
            if (BCryptHashData(hash, (PUCHAR)data.data(), (ULONG)data.size(), 0) == 0 &&
                BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
                char hex[65]{};
                for (int i = 0; i < 32; ++i) sprintf_s(hex + i * 2, 3, "%02x", digest[i]);
                result = hex;
            }
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return result;
}

// SHA256SUMS line format: "<hex>  <filename>". True if the zip's hash matches
// its entry; missing entry or malformed file = mismatch (be strict once the
// release ships a sums asset at all).
static bool sums_match(const std::string& sums, const std::string& zipName,
                       const std::string& zipHash) {
    size_t at = 0;
    while (at < sums.size()) {
        size_t eol = sums.find('\n', at);
        std::string line = sums.substr(at, eol == std::string::npos ? eol : eol - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() > 66 && line.find(zipName) != std::string::npos) {
            std::string hex = line.substr(0, 64);
            for (auto& c : hex) c = (char)tolower(c);
            return hex == zipHash;
        }
        if (eol == std::string::npos) break;
        at = eol + 1;
    }
    return false;
}

static void fail(const std::wstring& why) {
    {
        std::lock_guard lk(g_instMx);
        g_inst.error = why;
    }
    logf(L"install failed: %s", why.c_str());
    g_phase = (int)InstallPhase::Error;
}

static bool extract_zip(const std::vector<uint8_t>& zip, const fs::path& dir,
                        std::wstring* err) {
    mz_zip_archive za{};
    if (!mz_zip_reader_init_mem(&za, zip.data(), zip.size(), 0)) {
        *err = L"zip open failed";
        return false;
    }
    bool ok = true;
    int n = (int)mz_zip_reader_get_num_files(&za);
    std::error_code ec;
    for (int i = 0; i < n && ok; ++i) {
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(&za, i, &st)) { *err = L"zip stat failed"; ok = false; break; }
        std::string name = st.m_filename;
        if (name.find("..") != std::string::npos) continue;  // zip-slip guard
        fs::path dst = dir / u8_to_w(name);
        if (mz_zip_reader_is_file_a_directory(&za, i)) {
            fs::create_directories(dst, ec);
            continue;
        }
        fs::create_directories(dst.parent_path(), ec);
        size_t sz = 0;
        void* p = mz_zip_reader_extract_to_heap(&za, i, &sz, 0);
        if (!p) { *err = L"zip extract failed: " + u8_to_w(name); ok = false; break; }
        std::ofstream f(dst, std::ios::binary | std::ios::trunc);
        if (!f || !f.write((const char*)p, (std::streamsize)sz)) {
            *err = L"write failed: " + dst.wstring();
            ok = false;
        }
        mz_free(p);
    }
    mz_zip_reader_end(&za);
    return ok;
}

bool install_start(int gameIndex, const GameEntry& g, const ReleaseInfo& rel) {
    int idle = (int)InstallPhase::Idle;
    if (!g_phase.compare_exchange_strong(idle, (int)InstallPhase::Downloading)) return false;
    g_got = 0;
    g_total = 0;
    {
        std::lock_guard lk(g_instMx);
        g_inst.gameIndex = gameIndex;
        g_inst.tag = rel.tag;
        g_inst.error.clear();
    }
    fs::path dir = g.installDir;
    std::wstring url = rel.zipUrl, sumsUrl = rel.sumsUrl, tag = rel.tag, title = g.title;
    logf(L"install %s %s -> %s", title.c_str(), tag.c_str(), dir.c_str());
    std::thread([dir, url, sumsUrl, tag, title] {
        std::vector<uint8_t> zip;
        std::wstring err;
        if (!http_get(url, zip, &g_got, &g_total, &err)) return fail(L"download: " + err);
        // Integrity: verify against the release's SHA256SUMS asset if it has one
        if (!sumsUrl.empty()) {
            std::vector<uint8_t> sums;
            if (!http_get(sumsUrl, sums, nullptr, nullptr, &err)) {
                logf(L"install: SHA256SUMS fetch failed (%s), continuing unverified",
                     err.c_str());
            } else {
                std::string zipName = w_to_u8(url.substr(url.find_last_of(L'/') + 1));
                if (!sums_match(std::string((const char*)sums.data(), sums.size()), zipName,
                                sha256_mem(zip)))
                    return fail(L"SHA256 mismatch - corrupted or tampered download");
                logf(L"install: SHA256 verified against SHA256SUMS");
            }
        }
        g_phase = (int)InstallPhase::Extracting;
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) return fail(L"cannot create " + dir.wstring());
        if (!extract_zip(zip, dir, &err)) return fail(err);
        std::ofstream vf(dir / L"version.txt", std::ios::trunc | std::ios::binary);
        vf << w_to_u8(tag);
        vf.close();
        logf(L"install %s %s done (%lld bytes)", title.c_str(), tag.c_str(), (long long)zip.size());
        g_phase = (int)InstallPhase::Done;
    }).detach();
    return true;
}

InstallStatus install_status() {
    std::lock_guard lk(g_instMx);
    InstallStatus s = g_inst;
    s.phase = (InstallPhase)g_phase.load();
    s.got = g_got;
    s.total = g_total;
    return s;
}

void install_ack() {
    int p = g_phase.load();
    if (p == (int)InstallPhase::Done || p == (int)InstallPhase::Error)
        g_phase = (int)InstallPhase::Idle;
}

// ---------------------------------------------------------------------------
// SHA1 (bcrypt)
// ---------------------------------------------------------------------------

std::wstring sha1_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::wstring result;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
            std::vector<char> buf(1 << 20);
            bool ok = true;
            while (f && ok) {
                f.read(buf.data(), (std::streamsize)buf.size());
                std::streamsize n = f.gcount();
                if (n > 0 && BCryptHashData(hash, (PUCHAR)buf.data(), (ULONG)n, 0) != 0) ok = false;
            }
            UCHAR digest[20]{};
            if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
                wchar_t hex[41]{};
                for (int i = 0; i < 20; ++i) swprintf_s(hex + i * 2, 3, L"%02X", digest[i]);
                result = hex;
            }
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return result;
}
