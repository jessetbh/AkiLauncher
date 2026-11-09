#include "romscan.h"

#include "games.h"
#include "launch.h"

#include <windows.h>
#include "miniz.h"
#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <atomic>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Header and byte order
// ---------------------------------------------------------------------------

static RomByteOrder detect_order(const unsigned char* b) {
    if (b[0] == 0x80 && b[1] == 0x37 && b[2] == 0x12 && b[3] == 0x40) return RomByteOrder::Z64;
    if (b[0] == 0x37 && b[1] == 0x80 && b[2] == 0x40 && b[3] == 0x12) return RomByteOrder::V64;
    if (b[0] == 0x40 && b[1] == 0x12 && b[2] == 0x37 && b[3] == 0x80) return RomByteOrder::N64;
    return RomByteOrder::NotARom;
}

static void to_z64(unsigned char* p, size_t n, RomByteOrder from) {
    if (from == RomByteOrder::V64) {
        for (size_t i = 0; i + 1 < n; i += 2) std::swap(p[i], p[i + 1]);
    } else if (from == RomByteOrder::N64) {
        for (size_t i = 0; i + 3 < n; i += 4) {
            std::swap(p[i], p[i + 3]);
            std::swap(p[i + 1], p[i + 2]);
        }
    }
}

void rom_to_z64(std::vector<unsigned char>& data, RomByteOrder from) { to_z64(data.data(), data.size(), from); }

RomHeader rom_parse_header(const unsigned char* first64, size_t len) {
    RomHeader h;
    if (len < 64) return h;
    unsigned char b[64];
    memcpy(b, first64, 64);
    h.order = detect_order(b);
    if (h.order == RomByteOrder::NotARom) return h;
    to_z64(b, 64, h.order);
    h.internalName.assign((const char*)b + 0x20, 20);
    while (!h.internalName.empty() && (h.internalName.back() == ' ' || h.internalName.back() == '\0'))
        h.internalName.pop_back();
    h.gameCode.assign((const char*)b + 0x3B, 4);
    h.version = b[0x3F];
    return h;
}

int rom_match_game(const RomHeader& h, const std::vector<GameEntry>& games) {
    if (h.order == RomByteOrder::NotARom || h.gameCode.size() != 4) return -1;
    // The first character is the media type (N = cartridge, others for the
    // 64DD and re-releases): compare the 2-character id and the region.
    for (size_t i = 0; i < games.size(); ++i) {
        const std::wstring& c = games[i].romCode;
        if (c.size() != 4) continue;
        bool same = true;
        for (int k = 1; k < 4; ++k)
            if ((wchar_t)(unsigned char)h.gameCode[k] != c[k]) same = false;
        if (same) return (int)i;
    }
    return -1;
}

int rom_match_other_region(const RomHeader& h, const std::vector<GameEntry>& games) {
    if (h.order == RomByteOrder::NotARom || h.gameCode.size() != 4) return -1;
    for (size_t i = 0; i < games.size(); ++i) {
        const std::wstring& c = games[i].romCode;
        if (c.size() != 4) continue;
        if ((wchar_t)(unsigned char)h.gameCode[1] == c[1] && (wchar_t)(unsigned char)h.gameCode[2] == c[2] &&
            (wchar_t)(unsigned char)h.gameCode[3] != c[3])
            return (int)i;
    }
    return -1;
}

std::wstring rom_region_name(char r) {
    switch (r) {
    case 'E': return L"USA";
    case 'J': return L"Japanese";
    case 'P': case 'X': case 'Y': case 'Z': return L"PAL (Europe)";
    case 'D': return L"PAL (German)";
    case 'F': return L"PAL (French)";
    case 'I': return L"PAL (Italian)";
    case 'S': return L"PAL (Spanish)";
    case 'U': return L"PAL (Australian)";
    default: return std::wstring(L"region ") + (wchar_t)(unsigned char)r;
    }
}

// ---------------------------------------------------------------------------
// Hashing and reading
// ---------------------------------------------------------------------------

static std::wstring sha1_bytes(const std::vector<unsigned char>& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::wstring result;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0) return result;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        UCHAR digest[20]{};
        if (BCryptHashData(hash, (PUCHAR)data.data(), (ULONG)data.size(), 0) == 0 &&
            BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
            wchar_t hex[41]{};
            for (int i = 0; i < 20; ++i) swprintf_s(hex + i * 2, 3, L"%02X", digest[i]);
            result = hex;
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return result;
}

static constexpr uintmax_t kMaxRomBytes = 128ull << 20;  // N64 cartridges top out at 64 MB

// ---------------------------------------------------------------------------
// Zipped ROMs (aki-roadmap#280): a zip holding one N64 ROM reads as that ROM
// ---------------------------------------------------------------------------

bool rom_is_zip(const fs::path& file) {
    unsigned char sig[4]{};
    std::ifstream f(file, std::ios::binary);
    if (!f) return false;
    f.read((char*)sig, 4);
    return f.gcount() == 4 && sig[0] == 'P' && sig[1] == 'K' && sig[2] == 3 && sig[3] == 4;
}

namespace {
// A zip opened through _wfopen (any path), for miniz's reader.
struct RomZip {
    mz_zip_archive za{};
    FILE* f = nullptr;
    bool open = false;
    explicit RomZip(const fs::path& p) {
        if (_wfopen_s(&f, p.c_str(), L"rb") != 0 || !f) {
            f = nullptr;
            return;
        }
        std::error_code ec;
        uintmax_t size = fs::file_size(p, ec);
        open = !ec && mz_zip_reader_init_cfile(&za, f, (mz_uint64)size, 0);
    }
    ~RomZip() {
        if (open) mz_zip_reader_end(&za);
        if (f) fclose(f);
    }
    RomZip(const RomZip&) = delete;
    RomZip& operator=(const RomZip&) = delete;
};
}  // namespace

static constexpr uintmax_t kMinRomBytes = 64;
static constexpr uintmax_t kMinUnnamedRomBytes = 4ull << 20;  // a zip entry not named like a ROM is opened from 4 MB

static std::wstring zip_entry_name(const mz_zip_archive_file_stat& st) {
    std::string nm = st.m_filename;
    size_t slash = nm.find_last_of("/\\");
    if (slash != std::string::npos) nm = nm.substr(slash + 1);
    int len = MultiByteToWideChar(CP_UTF8, 0, nm.data(), (int)nm.size(), nullptr, 0);
    std::wstring w(len > 0 ? len : 0, 0);
    if (len > 0) MultiByteToWideChar(CP_UTF8, 0, nm.data(), (int)nm.size(), w.data(), len);
    return w;
}

// The entries of a zip whose first bytes are an N64 ROM header (any byte order), in zip order.
static std::vector<mz_uint> zip_rom_entries(RomZip& z) {
    std::vector<mz_uint> out;
    mz_uint n = mz_zip_reader_get_num_files(&z.za);
    for (mz_uint i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&z.za, i, &st) || st.m_is_directory) continue;
        if (st.m_uncomp_size < kMinRomBytes || st.m_uncomp_size > kMaxRomBytes) continue;
        // Only entries named like a ROM, or big enough for one, are opened (a texture-pack zip has thousands of files).
        std::string nm = st.m_filename;
        std::transform(nm.begin(), nm.end(), nm.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        auto ends = [&](const char* x) { size_t n = strlen(x); return nm.size() >= n && nm.compare(nm.size() - n, n, x) == 0; };
        bool romName = ends(".z64") || ends(".v64") || ends(".n64") || ends(".rom") || ends(".bin");
        bool notRom = ends(".png") || ends(".dds") || ends(".jpg") || ends(".jpeg") || ends(".bmp") || ends(".txt") ||
                      ends(".htc") || ends(".hts") || ends(".rtz") || ends(".nrm") || ends(".json");
        if (notRom || (!romName && st.m_uncomp_size < kMinUnnamedRomBytes)) continue;
        mz_zip_reader_extract_iter_state* it = mz_zip_reader_extract_iter_new(&z.za, i, 0);
        if (!it) continue;
        unsigned char head[4]{};
        size_t got = mz_zip_reader_extract_iter_read(it, head, sizeof head);
        mz_zip_reader_extract_iter_free(it);
        if (got == sizeof head && detect_order(head) != RomByteOrder::NotARom) out.push_back(i);
    }
    return out;
}

// The file's bytes as they are: the file itself, or the one N64 ROM inside a zip. romsInZip: -1 for a plain file,
// else how many ROMs the zip holds (the bytes are read only when it is exactly one); innerName: that ROM's name.
static bool read_rom_bytes(const fs::path& file, std::vector<unsigned char>& data, int* romsInZip = nullptr,
                           std::wstring* innerName = nullptr) {  // zips: #280
    if (romsInZip) *romsInZip = -1;
    if (rom_is_zip(file)) {
        RomZip z(file);
        if (!z.open) return false;
        std::vector<mz_uint> roms = zip_rom_entries(z);
        if (romsInZip) *romsInZip = (int)roms.size();
        if (roms.size() != 1) return false;
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&z.za, roms[0], &st)) return false;
        data.resize((size_t)st.m_uncomp_size);
        if (!mz_zip_reader_extract_to_mem(&z.za, roms[0], data.data(), data.size(), 0)) return false;
        if (innerName) *innerName = zip_entry_name(st);
        return true;
    }
    std::error_code ec;
    uintmax_t size = fs::file_size(file, ec);
    if (ec || size < kMinRomBytes || size > kMaxRomBytes) return false;
    std::ifstream f(file, std::ios::binary);
    if (!f) return false;
    data.resize((size_t)size);
    f.read((char*)data.data(), (std::streamsize)size);
    return (uintmax_t)f.gcount() == size;
}

// The first 64 bytes of the one N64 ROM in a zip (streamed: nothing else is unpacked, so a scan of a folder of
// zipped ROMs stays quick); roms = how many the zip holds. False unless it is exactly one and readable.
static bool zip_rom_head(const fs::path& file, unsigned char* head64, size_t* got, int* roms, std::wstring* innerName) {
    *roms = -1;
    RomZip z(file);
    if (!z.open) return false;
    std::vector<mz_uint> entries = zip_rom_entries(z);
    *roms = (int)entries.size();
    if (entries.size() != 1) return false;
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(&z.za, entries[0], &st)) return false;
    *innerName = zip_entry_name(st);
    mz_zip_reader_extract_iter_state* it = mz_zip_reader_extract_iter_new(&z.za, entries[0], 0);
    if (!it) return false;
    *got = mz_zip_reader_extract_iter_read(it, head64, 64);
    mz_zip_reader_extract_iter_free(it);
    return *got == 64;
}

// Raw ROM bytes put in .z64 order; false if they aren't a ROM.
static bool bytes_to_z64(std::vector<unsigned char>& data, RomByteOrder* orderOut) {
    if (data.size() < kMinRomBytes) return false;
    RomByteOrder order = detect_order(data.data());
    if (order == RomByteOrder::NotARom) return false;
    to_z64(data.data(), data.size(), order);
    if (orderOut) *orderOut = order;
    return true;
}

// The whole ROM in .z64 order (also the one ROM in a zip); false if unreadable or not a ROM.
static bool read_z64(const fs::path& file, std::vector<unsigned char>& data, RomByteOrder* orderOut) {
    return read_rom_bytes(file, data) && bytes_to_z64(data, orderOut);
}

static std::wstring widen_name(const std::string& s) {
    // Japanese internal names are Shift-JIS; ASCII reads the same in it.
    int n = MultiByteToWideChar(932, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n > 0 ? n : 0, 0);
    if (n > 0) MultiByteToWideChar(932, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

static std::wstring describe(const RomHeader& h) {
    std::wstring code;
    for (char c : h.gameCode) code += (wchar_t)(unsigned char)c;
    return L"\"" + widen_name(h.internalName) + L"\", " + code;
}

RomScanEntry rom_classify(const fs::path& file, const std::vector<GameEntry>& games) {
    RomScanEntry e;
    e.file = file;
    unsigned char head[64]{};
    size_t got = 0;
    std::wstring inner;  // the ROM's name inside a zip (aki-roadmap#280)
    if (rom_is_zip(file)) {
        int roms = -1;
        if (!zip_rom_head(file, head, &got, &roms, &inner)) {
            if (roms == 0) {
                e.zipWithoutRom = true;
                e.note = L"a zip with no N64 ROM inside";
            } else if (roms > 1) {
                e.note = L"a zip with " + std::to_wstring(roms) + L" N64 ROMs inside: extract it to scan each one";
            } else {
                e.verdict = RomVerdict::Unreadable;
                e.note = L"can't unpack the zip";
            }
            return e;
        }
    } else {
        std::ifstream f(file, std::ios::binary);
        if (!f) {
            e.verdict = RomVerdict::Unreadable;
            e.note = L"can't read the file";
            return e;
        }
        f.read((char*)head, 64);
        got = (size_t)f.gcount();
    }
    e.header = rom_parse_header(head, got);
    if (e.header.order == RomByteOrder::NotARom) {
        e.note = L"not an N64 ROM";
        return e;
    }
    int gi = rom_match_game(e.header, games);
    if (gi < 0) {
        int og = rom_match_other_region(e.header, games);
        if (og >= 0) {
            e.otherRegionGame = og;
            const std::wstring& want = games[og].romCode;
            e.note = games[og].title + L", " + rom_region_name(e.header.gameCode[3]) + L" version (" + describe(e.header) +
                     L"): the port needs the " + (games[og].romNote.empty() ? rom_region_name((char)want[3]) + L" ROM"
                                                                             : games[og].romNote);
            return e;
        }
        e.note = L"not one of the AKI games (" + describe(e.header) + L")";
        return e;
    }
    const GameEntry& g = games[gi];
    e.gameIndex = gi;
    std::vector<unsigned char> data;
    RomByteOrder order;
    if (!read_z64(file, data, &order)) {
        e.verdict = RomVerdict::Unreadable;
        e.note = L"can't read the whole ROM";
        return e;
    }
    e.sha1 = sha1_bytes(data);
    if (g.romSha1.empty() || e.sha1 == g.romSha1) {
        e.verdict = RomVerdict::BaseDump;
        e.note = g.title + L": the accepted dump" +
                 (!inner.empty()                ? L" (" + inner + L" in the zip; set up as .z64)"
                  : order == RomByteOrder::Z64 ? std::wstring()
                                               : std::wstring(L" (byte-swapped; set up as .z64)"));
    } else if (g.romVersion >= 0 && e.header.version != g.romVersion) {
        // Another official revision (e.g. No Mercy Rev 0): not a hack, just not the dump the port uses.
        e.verdict = RomVerdict::OtherBuild;
        e.note = g.title + L": another revision (" + describe(e.header) + L", v" +
                 std::to_wstring(e.header.version) + L"); this port needs the " + g.romNote + L" (v" +
                 std::to_wstring(g.romVersion) + L").";
    } else {
        e.verdict = RomVerdict::OtherBuild;
        // A hack or translation: players bring it in through Import, which converts it (aki-roadmap#278).
        e.note = g.title + L": a hack, translation or other build (" + describe(e.header) +
                 L"). To play it, import it: Mods page > Import... turns it into a conversion of " + g.title +
                 L", which needs the " + g.romNote + L".";
    }
    return e;
}

RomCandidates rom_candidates(const fs::path& dir, size_t maxFiles) {
    static const wchar_t* exts[] = {L".z64", L".v64", L".n64", L".rom", L".bin", L".zip"};  // zipped ROMs: #280
    RomCandidates out;
    // One folder at a time, so a folder that fails to list (a broken link, an
    // offline share) skips only itself.
    std::function<void(const fs::path&, int)> walk = [&](const fs::path& d, int depth) {
        std::error_code ec;
        fs::directory_iterator it(d, fs::directory_options::skip_permission_denied, ec), end;
        if (ec) {
            ++out.skippedDirs;
            logf(L"rom scan: can't list %s (%S)", d.c_str(), ec.message().c_str());
            return;
        }
        std::vector<fs::path> subdirs;
        for (; it != end; it.increment(ec)) {
            if (ec) {
                ++out.skippedDirs;
                logf(L"rom scan: listing %s stopped (%S)", d.c_str(), ec.message().c_str());
                break;
            }
            std::error_code fec;
            if (it->is_directory(fec)) {
                if (depth < 3) subdirs.push_back(it->path());
                continue;
            }
            if (!it->is_regular_file(fec)) continue;
            std::wstring ext = it->path().extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
            for (const wchar_t* x : exts)
                if (ext == x) {
                    if (out.files.size() >= maxFiles) {
                        out.truncated = true;
                        return;
                    }
                    out.files.push_back(it->path());
                    break;
                }
        }
        for (const auto& s : subdirs) {
            if (out.truncated) return;
            walk(s, depth + 1);
        }
    };
    walk(dir, 0);
    std::sort(out.files.begin(), out.files.end());
    return out;
}

// ---------------------------------------------------------------------------
// Background scan
// ---------------------------------------------------------------------------

static std::mutex g_mu;
static RomScanStatus g_scan;
static std::thread g_worker;
static std::atomic<bool> g_stop{false};

bool romscan_start(const fs::path& dir, const std::vector<GameEntry>& games) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_scan.phase == RomScanPhase::Scanning) return false;
        unsigned gen = g_scan.generation + 1;
        g_scan = RomScanStatus{};
        g_scan.phase = RomScanPhase::Scanning;
        g_scan.dir = dir;
        g_scan.generation = gen;
    }
    if (g_worker.joinable()) g_worker.join();  // the previous scan has finished
    g_stop = false;
    g_worker = std::thread([dir, games]() {
        RomCandidates cand = rom_candidates(dir);
        const std::vector<fs::path>& files = cand.files;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_scan.total = files.size();
            g_scan.truncated = cand.truncated;
            g_scan.skippedDirs = cand.skippedDirs;
            ++g_scan.generation;
        }
        std::vector<RomScanEntry> results;
        size_t scanned = 0;
        for (const auto& f : files) {
            if (g_stop) break;
            RomScanEntry r = rom_classify(f, games);
            if (!r.zipWithoutRom) results.push_back(std::move(r));  // an unrelated zip isn't listed (#280)
            std::lock_guard<std::mutex> lk(g_mu);
            g_scan.done = ++scanned;
            ++g_scan.generation;
        }
        size_t base = 0, other = 0;
        for (auto& r : results) {
            if (r.verdict == RomVerdict::BaseDump) ++base;
            else if (r.verdict == RomVerdict::OtherBuild) ++other;
        }
        logf(L"rom scan: %s: %zu files, %zu accepted dumps, %zu other builds", dir.c_str(), results.size(),
             base, other);
        std::lock_guard<std::mutex> lk(g_mu);
        g_scan.results = std::move(results);
        g_scan.phase = RomScanPhase::Done;
        ++g_scan.generation;
    });
    return true;
}

RomScanStatus romscan_status() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_scan;
}

unsigned romscan_generation() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_scan.generation;
}

void romscan_clear() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_scan.phase != RomScanPhase::Done) return;
    unsigned gen = g_scan.generation + 1;
    g_scan = RomScanStatus{};
    g_scan.generation = gen;
}

void romscan_shutdown() {
    g_stop = true;
    if (g_worker.joinable()) g_worker.join();
}

// ---------------------------------------------------------------------------
// Provisioning
// ---------------------------------------------------------------------------

bool rom_read_any_z64(const fs::path& file, std::vector<unsigned char>* z64) {
    std::vector<unsigned char> data;
    if (!read_z64(file, data, nullptr)) return false;
    if (z64) *z64 = std::move(data);
    return true;
}

std::wstring rom_z64_sha1(const fs::path& file) {
    std::vector<unsigned char> data;
    return read_z64(file, data, nullptr) ? sha1_bytes(data) : std::wstring();
}

bool rom_read_matching_z64(const fs::path& file, const std::wstring& sha1, std::vector<unsigned char>* z64) {
    std::vector<unsigned char> data;
    if (sha1.empty() || !read_z64(file, data, nullptr) || sha1_bytes(data) != sha1) return false;
    if (z64) *z64 = std::move(data);
    return true;
}

bool rom_provision(GameEntry& g, const RomScanEntry& e, std::wstring* errorOut) {
    std::error_code ec;
    if (e.verdict != RomVerdict::BaseDump) {
        *errorOut = L"that file isn't the accepted " + g.title + L" ROM";
        return false;
    }
    if (fs::exists(g.romPath, ec)) {
        *errorOut = g.title + L" already has a ROM (" + g.romPath.wstring() + L")";
        return false;
    }
    std::vector<unsigned char> data;
    if (!read_z64(e.file, data, nullptr)) {
        *errorOut = L"can't read " + e.file.wstring();
        return false;
    }
    if (!g.romSha1.empty() && sha1_bytes(data) != g.romSha1) {
        *errorOut = e.file.wstring() + L" changed since the scan";
        return false;
    }
    fs::create_directories(g.romPath.parent_path(), ec);
    fs::path tmp = g.romPath;
    tmp += L".new";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write((const char*)data.data(), (std::streamsize)data.size());
        f.close();  // the last chunk is written here: check after it
        if (f.fail() || fs::file_size(tmp, ec) != data.size() || ec) {
            *errorOut = L"can't write " + tmp.wstring() + L" (disk full?)";
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, g.romPath, ec);
    if (ec) {
        *errorOut = L"can't write " + g.romPath.wstring();
        fs::remove(tmp, ec);
        return false;
    }
    logf(L"rom scan: set up %s from %s", g.title.c_str(), e.file.c_str());
    return true;
}
