// Checks for the ROM folder scan (src/romscan.cpp) on synthetic images: a
// made-up header and filler bytes, never real ROM data. The "accepted dump"
// of each test game is whatever SHA1 the synthetic image has.
#include "../src/games.h"
#include "../src/romscan.h"
#include "check.h"

#include <windows.h>
#include <bcrypt.h>

#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// A tiny big-endian image: N64 magic, internal name, game code, version, filler.
static std::vector<unsigned char> fake_rom(const char* name, const char* code, int version, unsigned char fill,
                                           size_t size = 4096) {
    std::vector<unsigned char> d(size, fill);
    const unsigned char magic[4] = {0x80, 0x37, 0x12, 0x40};
    memcpy(d.data(), magic, 4);
    memset(d.data() + 0x20, ' ', 20);
    memcpy(d.data() + 0x20, name, strlen(name));
    memcpy(d.data() + 0x3B, code, 4);
    d[0x3F] = (unsigned char)version;
    return d;
}

static std::vector<unsigned char> swapped(std::vector<unsigned char> d, RomByteOrder to) {
    // z64 -> v64 / n64 are the same swaps as back (each is its own inverse).
    rom_to_z64(d, to);
    return d;
}

static void write_bytes(const fs::path& p, const std::vector<unsigned char>& d) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write((const char*)d.data(), (std::streamsize)d.size());
}

static std::vector<unsigned char> read_bytes(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static std::wstring sha1_hex(const std::vector<unsigned char>& d) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    UCHAR digest[20]{};
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0);
    BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0);
    BCryptHashData(h, (PUCHAR)d.data(), (ULONG)d.size(), 0);
    BCryptFinishHash(h, digest, 20, 0);
    BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg, 0);
    wchar_t hex[41]{};
    for (int i = 0; i < 20; ++i) swprintf_s(hex + i * 2, 3, L"%02X", digest[i]);
    return hex;
}

static bool has(const std::wstring& s, const wchar_t* part) { return s.find(part) != std::wstring::npos; }

static void test_header() {
    auto z = fake_rom("TEST GAME", "NTGE", 1, 0xAB);
    RomHeader h = rom_parse_header(z.data(), z.size());
    CHECK(h.order == RomByteOrder::Z64 && h.internalName == "TEST GAME" && h.gameCode == "NTGE" && h.version == 1);
    auto v = swapped(z, RomByteOrder::V64);
    CHECK(v[0] == 0x37 && v[1] == 0x80);
    h = rom_parse_header(v.data(), v.size());
    CHECK(h.order == RomByteOrder::V64 && h.gameCode == "NTGE" && h.internalName == "TEST GAME");
    auto n = swapped(z, RomByteOrder::N64);
    CHECK(n[0] == 0x40 && n[3] == 0x80);
    h = rom_parse_header(n.data(), n.size());
    CHECK(h.order == RomByteOrder::N64 && h.gameCode == "NTGE");
    rom_to_z64(n, RomByteOrder::N64);
    CHECK(n == z);
    unsigned char junk[64] = {'P', 'K', 3, 4};
    CHECK(rom_parse_header(junk, 64).order == RomByteOrder::NotARom);
    CHECK(rom_parse_header(z.data(), 10).order == RomByteOrder::NotARom);
}

void run_romscan_tests(const fs::path& dir) {
    test_header();

    // Two made-up games; game 0's accepted dump is base0's bytes.
    auto base0 = fake_rom("TEST GAME", "NTGE", 0, 0x11);
    auto hack0 = fake_rom("TEST GAME HACK", "NTGE", 0, 0x22);  // same code, other bytes
    auto other = fake_rom("SOME OTHER GAME", "NZZE", 0, 0x33);
    auto euro0 = fake_rom("TEST GAME", "NTGP", 0, 0x11);  // another region: not this game
    auto rev1 = fake_rom("TEST GAME", "NTGE", 1, 0x11);       // an official revision
    std::vector<GameEntry> games(2);
    games[0].title = L"Test Game";
    games[0].repoDir = L"TestRecomp";
    games[0].romCode = L"NTGE";
    games[0].romSha1 = sha1_hex(base0);
    games[0].romNote = L"North American release";
    games[0].romVersion = 0;
    games[1].title = L"Second Game";
    games[1].romCode = L"NSGJ";
    games[1].romSha1 = L"0000000000000000000000000000000000000000";

    RomHeader hb = rom_parse_header(base0.data(), base0.size());
    CHECK(rom_match_game(hb, games) == 0);
    RomHeader he = rom_parse_header(euro0.data(), euro0.size());
    CHECK(rom_match_game(he, games) == -1);
    // aki-roadmap#228: another region is named (PAL), not just "not supported".
    CHECK(rom_match_other_region(he, games) == 0 && rom_match_other_region(hb, games) == -1);
    CHECK(rom_region_name('P') == L"PAL (Europe)" && rom_region_name('J') == L"Japanese");
    write_bytes(dir / L"pal" / L"Test Game (Europe).z64", euro0);
    RomScanEntry pal = rom_classify(dir / L"pal" / L"Test Game (Europe).z64", games);
    CHECK(pal.verdict == RomVerdict::NotSupported && pal.otherRegionGame == 0 && pal.gameIndex == -1);
    CHECK(has(pal.note, L"PAL (Europe) version") && has(pal.note, L"North American release"));
    auto media = fake_rom("TEST GAME", "CTGE", 0, 0x11);  // another media letter, same id + region
    RomHeader hm = rom_parse_header(media.data(), media.size());
    CHECK(rom_match_game(hm, games) == 0);

    fs::path roms = dir / L"roms";
    write_bytes(roms / L"Test Game (USA).z64", base0);
    write_bytes(roms / L"swapped" / L"Test Game (USA).v64", swapped(base0, RomByteOrder::V64));
    write_bytes(roms / L"Test Game Hack.z64", hack0);
    write_bytes(roms / L"Other.n64", swapped(other, RomByteOrder::N64));
    write_bytes(roms / L"notes.bin", std::vector<unsigned char>(100, 0));
    write_bytes(roms / L"readme.txt", std::vector<unsigned char>(10, 'x'));        // not a candidate
    write_bytes(roms / L"a" / L"b" / L"c" / L"d" / L"deep.z64", base0);            // too deep

    write_bytes(dir / L"rev" / L"Test Game (Rev 1).z64", rev1);
    RomScanEntry r1 = rom_classify(dir / L"rev" / L"Test Game (Rev 1).z64", games);
    CHECK(r1.verdict == RomVerdict::OtherBuild && has(r1.note, L"another revision") && has(r1.note, L"(v0)"));
    CHECK(!has(r1.note, L"make_patch_mod.py"));

    write_bytes(roms / L"a" / L"b" / L"c" / L"three-deep.z64", other);          // depth 3: found
    auto cand = rom_candidates(roms);
    auto& files = cand.files;
    CHECK(files.size() == 6 && !cand.truncated && cand.skippedDirs == 0);
    auto capped = rom_candidates(roms, 2);
    CHECK(capped.files.size() == 2 && capped.truncated);
    CHECK(rom_candidates(roms / L"missing").skippedDirs == 1);
    fs::remove(roms / L"a" / L"b" / L"c" / L"three-deep.z64");
    files = rom_candidates(roms).files;
    CHECK(files.size() == 5);

    std::vector<RomScanEntry> res;
    for (auto& f : files) res.push_back(rom_classify(f, games));
    auto find = [&](const wchar_t* name) -> const RomScanEntry* {
        for (auto& r : res)
            if (r.file.filename() == name) return &r;
        return nullptr;
    };
    const RomScanEntry* b = find(L"Test Game (USA).z64");
    const RomScanEntry* v = find(L"Test Game (USA).v64");
    const RomScanEntry* hk = find(L"Test Game Hack.z64");
    const RomScanEntry* o = find(L"Other.n64");
    const RomScanEntry* junk = find(L"notes.bin");
    CHECK(b && b->verdict == RomVerdict::BaseDump && b->gameIndex == 0 && b->sha1 == games[0].romSha1);
    CHECK(v && v->verdict == RomVerdict::BaseDump && v->sha1 == games[0].romSha1 && has(v->note, L"byte-swapped"));
    CHECK(hk && hk->verdict == RomVerdict::OtherBuild && hk->gameIndex == 0 && has(hk->note, L"make_patch_mod.py"));
    CHECK(hk && has(hk->note, L"TEST GAME HACK"));
    CHECK(o && o->verdict == RomVerdict::NotSupported && o->gameIndex == -1 && o->sha1.empty());
    CHECK(o && has(o->note, L"NZZE"));
    CHECK(junk && junk->verdict == RomVerdict::NotSupported && has(junk->note, L"not an N64 ROM"));

    // The background scan gives the same verdicts.
    CHECK(romscan_start(roms, games));
    for (int i = 0; i < 500 && romscan_status().phase == RomScanPhase::Scanning; ++i) Sleep(10);
    RomScanStatus st = romscan_status();
    CHECK(st.phase == RomScanPhase::Done && st.results.size() == 5 && st.done == 5 && st.total == 5);
    unsigned gen = romscan_generation();
    romscan_clear();
    CHECK(romscan_status().phase == RomScanPhase::Idle && romscan_generation() != gen);
    // Shutdown mid-scan stops between files and returns.
    CHECK(romscan_start(roms, games));
    romscan_shutdown();
    CHECK(romscan_status().phase == RomScanPhase::Done && romscan_status().results.size() <= 5);
    romscan_clear();

    // Provisioning writes .z64 order, never over an existing ROM, only base dumps.
    GameEntry g = games[0];
    g.romPath = dir / L"game" / L"test.z64";
    std::wstring err;
    CHECK(!rom_provision(g, *hk, &err) && !err.empty());
    CHECK(v && rom_provision(g, *v, &err));
    CHECK(read_bytes(g.romPath) == base0);
    // A little-endian (.n64) dump too.
    write_bytes(roms / L"le" / L"Test Game.n64", swapped(base0, RomByteOrder::N64));
    RomScanEntry le = rom_classify(roms / L"le" / L"Test Game.n64", games);
    GameEntry g2 = games[0];
    g2.romPath = dir / L"game2" / L"test.z64";
    CHECK(le.verdict == RomVerdict::BaseDump && rom_provision(g2, le, &err) && read_bytes(g2.romPath) == base0);
    CHECK(!fs::exists(dir / L"game" / L"test.z64.new"));
    // aki-roadmap#274: the card's Select ROM reads any byte order of the accepted dump (the scan already did).
    {
        std::vector<unsigned char> z;
        CHECK(rom_read_matching_z64(roms / L"le" / L"Test Game.n64", games[0].romSha1, &z) && z == base0);
        write_bytes(roms / L"bs" / L"Test Game.v64", swapped(base0, RomByteOrder::V64));
        CHECK(rom_read_matching_z64(roms / L"bs" / L"Test Game.v64", games[0].romSha1, &z) && z == base0);
        CHECK(!rom_read_matching_z64(roms / L"bs" / L"Test Game.v64", L"0000000000000000000000000000000000000000", &z));
        CHECK(!rom_read_matching_z64(roms / L"bs" / L"Test Game.v64", L"", &z));
    }
    err.clear();
    CHECK(b && !rom_provision(g, *b, &err) && has(err, L"already has a ROM"));
    // A file that changed after the scan is refused.
    fs::remove(g.romPath);
    write_bytes(roms / L"Test Game (USA).z64", hack0);
    err.clear();
    CHECK(b && !rom_provision(g, *b, &err) && has(err, L"changed since the scan"));
    CHECK(!fs::exists(g.romPath));
}
