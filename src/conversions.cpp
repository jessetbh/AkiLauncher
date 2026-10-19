#include "conversions.h"

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <sstream>

#include "games.h"
#include "installer.h"
#include "launch.h"
#include "miniz.h"
#include "third_party/nlohmann/json.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
    std::wstring wide(const std::string& s) {
        if (s.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
        return w;
    }

    // A path as UTF-8 bytes (C++20's u8string() is std::u8string).
    std::string u8(const fs::path& p) {
        const std::u8string s = p.u8string();
        return std::string(s.begin(), s.end());
    }

    std::string str(const json& j, const char* key) {
        auto it = j.find(key);
        return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
    }

    std::wstring upper(std::wstring s) {
        std::transform(s.begin(), s.end(), s.begin(), ::towupper);
        return s;
    }

    bool valid_id(const std::string& id) {
        if (id.empty() || id.size() > 48 || id[0] == '.' || id[0] == '_') return false;
        for (char c : id) {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-')) return false;
        }
        return true;
    }

    // A package path that stays inside its folder (no "..", no drive or root).
    bool safe_rel(const std::string& p) {
        if (p.empty() || p.find("..") != std::string::npos || p.find(':') != std::string::npos) return false;
        return p[0] != '/' && p[0] != '\\';
    }

    std::string read_text(const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    // The package's files, from a zip or a folder.
    struct Source {
        fs::path path;
        bool zip = false;
        mz_zip_archive archive{};
        bool open = false;

        ~Source() {
            if (open) mz_zip_reader_end(&archive);
        }

        bool init(const fs::path& p) {
            path = p;
            std::error_code ec;
            if (fs::is_directory(p, ec)) return true;
            zip = true;
            std::string u8s = u8(p);
            open = mz_zip_reader_init_file(&archive, u8s.c_str(), 0) != 0;
            return open;
        }

        bool read(const std::string& name, std::string& out) {
            if (!zip) {
                std::error_code ec;
                if (!fs::is_regular_file(path / fs::u8path(name), ec)) return false;
                out = read_text(path / fs::u8path(name));
                return true;
            }
            size_t size = 0;
            void* data = mz_zip_reader_extract_file_to_heap(&archive, name.c_str(), &size, 0);
            if (!data) return false;
            out.assign(static_cast<char*>(data), size);
            mz_free(data);
            return true;
        }

        // Copies every file into dst; false (with err) on an unsafe name or a write error.
        bool extract_all(const fs::path& dst, std::wstring* err) {
            std::error_code ec;
            fs::create_directories(dst, ec);
            if (!zip) {
                fs::copy(path, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
                if (ec && err) *err = L"copy failed: " + wide(ec.message());
                return !ec;
            }
            const mz_uint n = mz_zip_reader_get_num_files(&archive);
            for (mz_uint i = 0; i < n; i++) {
                mz_zip_archive_file_stat st;
                if (!mz_zip_reader_file_stat(&archive, i, &st)) continue;
                std::string name = st.m_filename;
                if (mz_zip_reader_is_file_a_directory(&archive, i)) continue;
                if (!safe_rel(name)) {
                    if (err) *err = L"unsafe path in the package: " + wide(name);
                    return false;
                }
                fs::path out = dst / fs::u8path(name);
                fs::create_directories(out.parent_path(), ec);
                std::string u8s = u8(out);
                if (!mz_zip_reader_extract_to_file(&archive, i, u8s.c_str(), 0)) {
                    if (err) *err = L"couldn't extract " + wide(name);
                    return false;
                }
            }
            return true;
        }
    };

    ConversionInfo parse_info(const json& j, const fs::path& folder) {
        ConversionInfo c;
        c.id = wide(str(j, "id"));
        c.name = wide(str(j, "name"));
        c.version = wide(str(j, "version"));
        c.description = wide(str(j, "description"));
        c.credits = wide(str(j, "credits"));
        c.permission = wide(str(j, "permission"));
        c.game = wide(str(j, "game"));
        if (auto a = j.find("authors"); a != j.end() && a->is_array()) {
            for (const auto& v : *a) {
                if (!v.is_string()) continue;
                if (!c.authors.empty()) c.authors += L", ";
                c.authors += wide(v.get<std::string>());
            }
        }
        if (auto b = j.find("base_rom"); b != j.end() && b->is_object()) {
            c.baseSha1 = upper(wide(str(*b, "sha1")));
            c.baseRevision = wide(str(*b, "revision"));
        }
        const json comps = j.value("components", json::object());
        if (comps.contains("patch")) c.patch = true;
        if (comps.contains("codes")) {
            c.codes = true;
            c.codesRom = wide(str(comps["codes"], "rom"));
            if (c.codesRom.empty()) c.codesRom = wide(str(comps["codes"], "rom_revision"));
        }
        if (comps.contains("saves") && comps["saves"].is_array() && !comps["saves"].empty()) c.saves = true;
        if (comps.contains("textures")) {
            c.textures = true;
            c.texturesPackage = wide(str(comps["textures"], "package"));
            c.texturesNaming = wide(str(comps["textures"], "naming"));
            c.texturesSha256 = wide(str(comps["textures"], "sha256"));
        }
        if (comps.contains("rt")) {
            c.rt = true;
            c.rtLights = wide(str(comps["rt"], "lights"));
            c.rtMaterials = wide(str(comps["rt"], "materials"));
        }
        c.folder = folder;
        std::error_code ec;
        if (fs::is_regular_file(folder / "icon.png", ec)) c.icon = folder / "icon.png";
        return c;
    }

    void annotate(ConversionInfo& c, const GameEntry& g) {
        std::error_code ec;
        if (!c.texturesPackage.empty()) {
            const fs::path pkg = game_data_dir(g) / "conversions" / "_packages" / c.texturesPackage;
            c.texturesInstalled = fs::is_directory(pkg, ec);
            if (!c.texturesInstalled) c.notes.push_back(L"Texture package " + c.texturesPackage + L" isn't added yet: add its zip to see the textures");
            else if (!c.texturesSha256.empty()) {
                const std::string have = read_text(pkg / ".package.sha256");
                if (!have.empty() && wide(have) != c.texturesSha256)
                    c.notes.push_back(L"The added " + c.texturesPackage + L" isn't the version this conversion was made with (sha256 differs)");
            }
            if (c.texturesInstalled && c.texturesNaming == L"rice") c.notes.push_back(L"Textures use Rice names (they show once the game supports Rice names)");
            else if (c.texturesNaming == L"rice") c.notes.push_back(L"Textures use Rice names (needs the game's Rice texture support)");
        }
        if (c.codes) {
            const std::wstring port = port_rom_tag(g);
            auto game = [](const std::wstring& t) { return t.substr(0, t.find(L"-rev")); };
            c.codesRunnable = !c.codesRom.empty() && game(c.codesRom) == game(port);
            if (!c.codesRunnable) {
                c.notes.push_back(L"Its codes aren't tagged for this game (" + (c.codesRom.empty() ? std::wstring(L"no tag") : c.codesRom) + L"), so they stay off");
            } else if (c.codesRom != port) {
                c.notes.push_back(L"Its codes are for " + c.codesRom + L"; they're translated to " + port + L" as they run (codes outside the known map are skipped)");
            }
        }
    }
}

fs::path game_data_dir(const GameEntry& g) {
    std::error_code ec;
    if (fs::is_regular_file(g.repoPath / "portable.txt", ec)) return g.repoPath;
    wchar_t lad[MAX_PATH]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH);
    return fs::path(lad) / g.exePath.stem();
}

// The manifest's game key (AkiTools/AkiMods keys); only World Tour's differs from the launcher's art key.
std::wstring game_key(const GameEntry& g) {
    if (g.artKey == L"worldtour") return L"wt";
    return g.artKey;
}

std::wstring port_rom_tag(const GameEntry& g) {
    if (g.artKey == L"nomercy") return L"nomercy-rev1";
    return game_key(g) + L"-rev0";
}

std::vector<ConversionInfo> list_conversions(const GameEntry& g) {
    std::vector<ConversionInfo> out;
    std::error_code ec;
    const fs::path root = game_data_dir(g) / "conversions";
    for (const auto& e : fs::directory_iterator(root, ec)) {
        if (!e.is_directory(ec)) continue;
        const std::string name = u8(e.path().filename());
        if (!valid_id(name)) continue;
        const std::string text = read_text(e.path() / "aki.json");
        json j = json::parse(text, nullptr, false);
        if (j.is_discarded() || str(j, "format") != "aki-conversion" || str(j, "id") != name) continue;
        ConversionInfo c = parse_info(j, e.path());
        annotate(c, g);
        out.push_back(std::move(c));
    }
    std::sort(out.begin(), out.end(), [](const ConversionInfo& a, const ConversionInfo& b) { return a.name < b.name; });
    return out;
}

AddResult add_conversion(std::vector<GameEntry>& games, const fs::path& src) {
    AddResult r;
    Source s;
    if (!s.init(src)) {
        r.message = L"Can't open " + src.filename().wstring() + L" (not a zip or a folder).";
        return r;
    }
    std::string text;
    if (!s.read("aki.json", text)) {
        r.message = src.filename().wstring() + L" has no aki.json: it isn't an AKI conversion package.";
        return r;
    }
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || j.value("format_version", 0) != 1) {
        r.message = L"aki.json isn't a valid v1 manifest.";
        return r;
    }
    const std::string format = str(j, "format");
    const std::string gameKey = str(j, "game");
    for (size_t i = 0; i < games.size(); i++) {
        if (game_key(games[i]) == wide(gameKey)) r.gameIndex = (int)i;
    }
    if (r.gameIndex < 0) {
        r.message = L"This package is for \"" + wide(gameKey) + L"\", which AKI Launcher doesn't know.";
        return r;
    }
    GameEntry& g = games[r.gameIndex];
    const fs::path data = game_data_dir(g);
    std::error_code ec;

    if (format == "aki-package" && str(j, "kind") == "textures") {
        // A texture dependency: conversions\_packages\<Team-Name>\ (the zip is named Team-Name-Version.zip).
        std::wstring stem = src.stem().wstring();
        size_t dash = stem.find_last_of(L'-');
        std::wstring pkg = (s.zip && dash != std::wstring::npos) ? stem.substr(0, dash) : src.filename().wstring();
        const fs::path dst = data / "conversions" / "_packages" / pkg;
        const fs::path tmp = data / "conversions" / "_packages" / (L".tmp-" + pkg);
        fs::remove_all(tmp, ec);
        std::wstring err;
        if (!s.extract_all(tmp, &err)) {
            fs::remove_all(tmp, ec);
            r.message = L"Couldn't add the texture package: " + err;
            return r;
        }
        // Pin what was added, so conversions can tell whether it's the pack they were made with.
        if (s.zip) {
            std::ofstream(tmp / ".package.sha256", std::ios::binary) << u8(fs::path(sha256_file(src)));
        }
        fs::remove_all(dst, ec);
        fs::rename(tmp, dst, ec);
        if (ec) {
            r.message = L"Couldn't add the texture package: " + wide(ec.message());
            return r;
        }
        r.ok = true;
        r.id = pkg;
        r.message = L"Added texture package " + pkg + L" for " + g.title + L".";
        logf(L"conversions: texture package %s -> %s", pkg.c_str(), dst.c_str());
        return r;
    }

    if (format != "aki-conversion") {
        r.message = L"aki.json isn't an aki-conversion manifest.";
        return r;
    }
    const std::string id = str(j, "id");
    if (!valid_id(id)) {
        r.message = L"Invalid conversion id \"" + wide(id) + L"\".";
        return r;
    }
    // Every file the manifest names must stay inside the package.
    const json comps = j.value("components", json::object());
    for (const char* key : { "patch", "codes", "textures" }) {
        if (comps.contains(key) && comps[key].is_object()) {
            std::string f = str(comps[key], "file");
            if (!f.empty() && !safe_rel(f)) { r.message = L"Unsafe path in aki.json: " + wide(f); return r; }
        }
    }
    if (comps.contains("saves") && comps["saves"].is_array()) {
        for (const auto& sv : comps["saves"]) {
            std::string f = sv.is_object() ? str(sv, "file") : std::string();
            if (!safe_rel(f)) { r.message = L"Unsafe save path in aki.json: " + wide(f); return r; }
        }
    }
    ConversionInfo info = parse_info(j, fs::path());

    // The player's ROM must be the one the conversion was made for.
    if (!g.romFound) {
        r.message = L"Import your " + g.title + L" ROM first (Select ROM), then add the conversion.";
        return r;
    }
    if (!info.baseSha1.empty()) {
        std::wstring have = upper(sha1_file(g.romPath));
        if (have != info.baseSha1) {
            r.message = info.name + L" needs a different " + g.title + L" ROM.\nRequired SHA1: " + info.baseSha1 +
                        L" (" + info.baseRevision + L")\nYour ROM's SHA1: " + have;
            return r;
        }
    }

    const fs::path dst = data / "conversions" / fs::u8path(id);
    const fs::path tmp = data / "conversions" / fs::u8path(".tmp-" + id);
    fs::remove_all(tmp, ec);
    std::wstring err;
    if (!s.extract_all(tmp, &err)) {
        fs::remove_all(tmp, ec);
        r.message = L"Couldn't install: " + err;
        return r;
    }
    const bool update = fs::exists(dst, ec);
    fs::remove_all(dst, ec);
    fs::rename(tmp, dst, ec);
    if (ec) {
        r.message = L"Couldn't install: " + wide(ec.message());
        return r;
    }

    // Seed the conversion's saves (never over the player's progress in it).
    int seeded = 0, kept = 0;
    if (comps.contains("saves") && comps["saves"].is_array()) {
        const fs::path saves = data / "saves" / fs::u8path(id);
        fs::create_directories(saves, ec);
        for (const auto& sv : comps["saves"]) {
            const fs::path from = dst / fs::u8path(str(sv, "file"));
            const fs::path to = saves / from.filename();
            if (fs::exists(to, ec)) { kept++; continue; }
            fs::copy_file(from, to, ec);
            if (!ec) seeded++;
        }
    }

    ConversionInfo installed = parse_info(j, dst);
    annotate(installed, g);
    r.ok = true;
    r.id = wide(id);
    r.message = (update ? L"Updated " : L"Added ") + installed.name + L" for " + g.title + L".";
    if (seeded) r.message += L" Its save is ready.";
    if (kept) r.message += L" Your saves in it were kept.";
    for (const auto& n : installed.notes) r.message += L"\n" + n;
    logf(L"conversions: %s %s -> %s (seeded %d, kept %d)", update ? L"updated" : L"added", r.id.c_str(), dst.c_str(), seeded, kept);
    return r;
}

bool remove_conversion(const GameEntry& g, const std::wstring& id, std::wstring* err) {
    std::error_code ec;
    if (!valid_id(u8(fs::path(id)))) { if (err) *err = L"invalid id"; return false; }
    fs::remove_all(game_data_dir(g) / "conversions" / id, ec);
    if (ec && err) *err = wide(ec.message());
    logf(L"conversions: removed %s", id.c_str());
    return !ec;
}

std::vector<std::pair<std::wstring, std::wstring>> conversion_env(const ConversionInfo& c) {
    std::vector<std::pair<std::wstring, std::wstring>> env{ { L"AKI_CONVERSION", c.id } };
    if (!c.rtLights.empty()) env.push_back({ L"WCW_RT_LIGHTS_FILE", (c.folder / c.rtLights).wstring() });
    if (!c.rtMaterials.empty()) env.push_back({ L"WCW_RT_MATERIALS_FILE", (c.folder / c.rtMaterials).wstring() });
    return env;
}
