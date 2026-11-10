#include "texpacks.h"

#include "json.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <fstream>

namespace fs = std::filesystem;

static std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    return s;
}

static bool all_hex(const std::wstring& s, size_t from, size_t n) {
    if (from + n > s.size()) return false;
    for (size_t i = from; i < from + n; ++i)
        if (!iswxdigit(s[i])) return false;
    return true;
}

TexNaming texture_name_kind(const std::wstring& fileName, std::wstring* riceName) {
    fs::path p(fileName);
    std::wstring ext = lower(p.extension().wstring());
    if (ext != L".png" && ext != L".dds") return TexNaming::None;
    std::wstring stem = p.stem().wstring();
    if (stem.size() == 16 && all_hex(stem, 0, 16)) return TexNaming::Rt64;
    if (stem.size() == 32 && all_hex(stem, 0, 32)) return TexNaming::Jabo;
    // Rice / GLideN64: NAME#CRC32#fmt#siz[#PALCRC]_suffix ("$" stands for any format/size in some packs).
    size_t h1 = stem.find(L'#');
    if (h1 == std::wstring::npos || !all_hex(stem, h1 + 1, 8) || h1 + 9 >= stem.size() || stem[h1 + 9] != L'#')
        return TexNaming::None;
    size_t us = stem.rfind(L'_');
    if (us == std::wstring::npos || us < h1 + 10) return TexNaming::None;
    // fmt and siz: one field each between '#'s, digits or '$'.
    std::wstring mid = stem.substr(h1 + 10, us - (h1 + 10));  // "2#1" or "2#1#91632C19"
    int fields = 0;
    size_t start = 0;
    while (start <= mid.size()) {
        size_t end = mid.find(L'#', start);
        std::wstring f = mid.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        ++fields;
        if (fields <= 2) {
            if (f.empty()) return TexNaming::None;
            for (wchar_t c : f)
                if (!iswdigit(c) && c != L'$') return TexNaming::None;
        } else if (fields == 3) {
            if (f.size() != 8 || !all_hex(f, 0, 8)) return TexNaming::None;
        } else {
            return TexNaming::None;
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    if (fields < 2) return TexNaming::None;
    if (riceName) *riceName = stem.substr(0, h1);
    return TexNaming::Rice;
}

TexNaming TexScan::naming() const {
    if (hasRt64Json || rt64 > 0) return TexNaming::Rt64;
    if (rice == 0 && jabo == 0) return TexNaming::None;
    return rice >= jabo ? TexNaming::Rice : TexNaming::Jabo;
}

std::wstring TexScan::rice_name() const {
    std::wstring best;
    int n = 0;
    for (const auto& [name, count] : riceNames)
        if (count > n) {
            best = name;
            n = count;
        }
    return best;
}

static void count_name(TexScan& s, const std::wstring& rel) {
    std::wstring leaf = rel;
    size_t slash = leaf.find_last_of(L"/\\");
    bool atRoot = slash == std::wstring::npos;
    if (!atRoot) leaf = leaf.substr(slash + 1);
    std::wstring low = lower(leaf);
    if (atRoot && low == L"rt64.json") s.hasRt64Json = true;
    if (atRoot && low == L"mod.json") s.hasModJson = true;
    std::wstring ext = fs::path(low).extension().wstring();
    if (ext == L".png" || ext == L".dds") ++s.images;
    std::wstring riceName;
    switch (texture_name_kind(leaf, &riceName)) {
    case TexNaming::Rice:
        ++s.rice;
        ++s.riceNames[riceName];
        break;
    case TexNaming::Jabo: ++s.jabo; break;
    case TexNaming::Rt64: ++s.rt64; break;
    default: break;
    }
}

TexScan scan_texture_names(const std::vector<std::wstring>& relPaths) {
    TexScan s;
    for (const auto& r : relPaths) count_name(s, r);
    return s;
}

TexScan scan_texture_folder(const fs::path& dir, int maxFiles) {
    TexScan s;
    std::error_code ec;
    int seen = 0;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end && seen < maxFiles; it.increment(ec)) {
        std::error_code fec;
        if (!it->is_regular_file(fec)) continue;
        ++seen;
        fs::path rel = fs::relative(it->path(), dir, fec);
        if (!fec) count_name(s, rel.wstring());
    }
    return s;
}

static std::wstring alnum_lower(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s)
        if (c < 128 && iswalnum(c)) out += (wchar_t)towlower(c);
    return out;
}

std::wstring rice_name_game(const std::wstring& riceName) {
    // The ROM header names (0x20) of the four games with ASCII names, letters and digits only. The two VPW games have
    // Shift-JIS names, which don't survive as file names reliably: those packs go to the game whose page they're
    // installed from.
    static const struct {
        const wchar_t* key;
        const wchar_t* gameId;
    } kNames[] = {
        {L"wcwvsnwoworldtour", L"wcwnwoworldtour"},  // "WCWvs.NWO:World Tour"
        {L"wcwnworevenge", L"wcwnworevenge"},        // "WCW/nWo REVENGE"
        {L"wrestlemania2000", L"wwfwm2k"},           // "WRESTLEMANIA 2000"
        {L"wwfnomercy", L"wwfnomercy"},              // "WWF No Mercy"
    };
    std::wstring k = alnum_lower(riceName);
    if (k.empty()) return {};
    for (const auto& n : kNames)
        if (k.find(n.key) != std::wstring::npos) return n.gameId;
    return {};
}

std::string legacy_rt64_json(TexNaming naming) {
    // The same file librecomp's conversion.cpp writes for a conversion's rice/jabo texture folder.
    return std::string(R"({ "configuration": { "configurationVersion": 3, "hashVersion": 5, "autoPath": ")") +
           (naming == TexNaming::Rice ? "rice" : "rt64") + R"(", "defaultShift": "none" }, "textures": [] })";
}

static std::string u8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, 0);
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::string legacy_mod_json(const std::wstring& id, const std::wstring& displayName, const std::wstring& gameId,
                            TexNaming naming, int textures) {
    JsonValue o;
    o.type = JsonValue::Type::Object;
    auto add = [&](const char* k, JsonValue v) {
        o.keys.push_back(k);
        o.items.push_back(std::move(v));
    };
    JsonValue authors;
    authors.type = JsonValue::Type::Array;
    authors.items.push_back(json_string("Unknown"));
    std::string kind = naming == TexNaming::Rice ? "Rice/GLideN64" : "Jabo";
    add("game_id", json_string(u8(gameId)));
    add("id", json_string(u8(id)));
    add("display_name", json_string(u8(displayName)));
    add("version", json_string("1.0.0"));
    add("authors", authors);
    add("short_description", json_string(kind + " texture pack (" + std::to_string(textures) +
                                         " textures), set up by AKI Launcher"));
    add("minimum_recomp_version", json_string("0.0.0"));
    return json_write(o);
}

std::wstring legacy_pack_id(const std::wstring& name) {
    std::wstring id = L"tex_";
    bool under = false;
    for (wchar_t c : name) {
        if (c < 128 && iswalnum(c)) {
            id += (wchar_t)towlower(c);
            under = false;
        } else if (!under && id.size() > 4) {
            id += L'_';
            under = true;
        }
        if (id.size() >= 48) break;
    }
    while (!id.empty() && id.back() == L'_') id.pop_back();
    if (id.size() <= 4) id = L"tex_pack";
    return id;
}

static bool write_file(const fs::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
    f.close();
    return !f.fail();
}

bool wrap_legacy_pack(const fs::path& packDir, const TexScan& scan, const std::wstring& gameId,
                      const std::wstring& displayName, std::wstring* errorOut) {
    TexNaming n = scan.naming();
    if (n != TexNaming::Rice && n != TexNaming::Jabo) {
        if (errorOut) *errorOut = L"not a Rice or Jabo texture pack";
        return false;
    }
    if (!write_file(packDir / L"rt64.json", legacy_rt64_json(n))) {
        if (errorOut) *errorOut = L"can't write " + (packDir / L"rt64.json").wstring();
        return false;
    }
    if (!scan.hasModJson &&
        !write_file(packDir / L"mod.json",
                    legacy_mod_json(legacy_pack_id(displayName), displayName, gameId, n, n == TexNaming::Rice ? scan.rice : scan.jabo))) {
        if (errorOut) *errorOut = L"can't write " + (packDir / L"mod.json").wstring();
        return false;
    }
    return true;
}
