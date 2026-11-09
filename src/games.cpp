#include "games.h"

#include "conversions.h"
#include "installer.h"
#include "launch.h"
#include "rom_rules.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <map>
#include <tuple>

#include "json.h"

#include "launch.h"  // logf

namespace fs = std::filesystem;

static bool looks_like_depot(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p / L"WcwNwoWorldTour", ec);
}

std::filesystem::path detect_depot_root() {
    wchar_t buf[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"AKI_DEPOT_ROOT", buf, MAX_PATH) > 0 && looks_like_depot(buf))
        return fs::path(buf);

    wchar_t exeBuf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
    fs::path p = fs::path(exeBuf).parent_path();
    for (int i = 0; i < 4 && !p.empty(); ++i) {
        if (looks_like_depot(p)) return p;
        p = p.parent_path();
    }
    std::error_code ec;
    fs::path cwd = fs::current_path(ec);
    for (int i = 0; i < 4 && !cwd.empty(); ++i) {
        if (looks_like_depot(cwd)) return cwd;
        cwd = cwd.parent_path();
    }
    return {};
}

std::filesystem::path app_root() {
    wchar_t exeBuf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
    fs::path dir = fs::path(exeBuf).parent_path();
    std::error_code ec;
    if (fs::is_directory(dir / L"assets", ec)) return dir;
    if (fs::is_directory(dir.parent_path() / L"assets", ec)) return dir.parent_path();
    return dir;
}

std::filesystem::path games_root() {
    wchar_t buf[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"AKI_GAMES_ROOT", buf, MAX_PATH) > 0) return fs::path(buf);
    return app_root() / L"games";
}

fs::path game_data_dir(const GameEntry& g) {
    if (game_is_portable(g)) return g.repoPath;
    wchar_t lad[MAX_PATH]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH);
    return fs::path(lad) / fs::path(g.exeRel).stem();
}

std::wstring game_run_version(const GameEntry& g) {
    if (g.installedTag.empty() || g.installDir.empty() || g.exePath.empty()) return L"";
    auto key = [](const fs::path& p) {
        std::wstring s = p.lexically_normal().wstring();
        for (auto& ch : s) ch = (ch == L'/') ? L'\\' : (wchar_t)towlower(ch);
        while (!s.empty() && s.back() == L'\\') s.pop_back();
        return s;
    };
    const std::wstring dir = key(g.installDir) + L"\\", exe = key(g.exePath);
    return exe.compare(0, dir.size(), dir) == 0 ? g.installedTag : L"";
}

bool game_is_portable(const GameEntry& g) {
    std::error_code ec;
    return !g.repoPath.empty() && fs::exists(g.repoPath / L"portable.txt", ec);
}

bool wants_portable_game(const GameEntry& g, bool launcherPortable, bool portableGames) {
    std::error_code ec;
    return launcherPortable && portableGames && !g.installDir.empty() && g.repoPath == g.installDir &&
           fs::is_directory(g.installDir, ec) && !game_is_portable(g);
}

bool carry_before_install(const GameEntry& g, bool launcherPortable, bool portableGames, bool sandboxed) {
    return !sandboxed && wants_portable_game(g, launcherPortable, portableGames);
}

int make_game_portable(const GameEntry& g, const fs::path& appDataDir) {
    // Copy first, portable.txt last: if anything fails the game stays on its
    // AppData data (nothing looks lost) and the next start tries again;
    // already-copied files are skipped then.
    std::error_code ec;
    int copied = 0;
    bool failed = false;
    if (fs::is_directory(appDataDir, ec)) {
        fs::recursive_directory_iterator it(appDataDir, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            std::error_code fec;
            // Links are not followed: a link must not make the copy write (or read) outside these folders.
            if (it->is_symlink(fec)) {
                if (it->is_directory(fec)) it.disable_recursion_pending();
                continue;
            }
            fs::path rel = it->path().lexically_relative(appDataDir);
            if (rel.empty() || *rel.begin() == L"..") continue;
            // Texture dumps can be huge and are made again on demand; logs are per run.
            if (it->is_directory(fec)) {
                if (_wcsicmp(rel.begin()->c_str(), L"texture_dump") == 0) it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(fec) || _wcsicmp(it->path().extension().c_str(), L".log") == 0) continue;
            fs::path dest = g.repoPath / rel;
            if (fs::exists(dest, fec)) continue;  // never over anything the install already has
            fs::create_directories(dest.parent_path(), fec);
            if (!fec) fs::copy_file(it->path(), dest, fs::copy_options::skip_existing, fec);
            if (fec) {
                failed = true;
                logf(L"portable: can't copy %s (%S)", it->path().c_str(), fec.message().c_str());
            } else {
                ++copied;
            }
        }
        if (ec) {
            failed = true;
            logf(L"portable: can't read %s (%S)", appDataDir.c_str(), ec.message().c_str());
        }
    }
    if (failed) return -1;
    std::ofstream marker(g.repoPath / L"portable.txt", std::ios::binary | std::ios::app);
    return marker ? copied : -1;
}

// assets/games.json, built in: CMake writes its bytes (and a closing 0) to games_json.inc.
static const char kEmbeddedGamesJson[] = {
#include "games_json.inc"
};

const char* embedded_games_json() { return kEmbeddedGamesJson; }

bool game_from_json(const JsonValue& v, GameEntry* g, std::wstring* err) {
    if (!v.is(JsonValue::Type::Object)) {
        *err = L"a game entry is not an object";
        return false;
    }
    GameEntry e;
    e.artKey = v.wstr("id");
    e.title = v.wstr("title");
    e.exeRel = v.wstr("exe");
    if (e.artKey.empty() || e.title.empty() || e.exeRel.empty()) {
        *err = L"a game needs \"id\", \"title\" and \"exe\" (" + (e.artKey.empty() ? e.title : e.artKey) + L")";
        return false;
    }
    // The id names folders (games\<id>) and art files: it must start with a letter or digit ("." / ".." would
    // point an install at the launcher's own folder).
    if (!iswalnum(e.artKey[0])) {
        *err = L"game id \"" + e.artKey + L"\" must start with a letter or digit";
        return false;
    }
    for (wchar_t c : e.artKey)
        if (!(iswalnum(c) || c == L'_' || c == L'-' || c == L'.')) {
            *err = L"game id \"" + e.artKey + L"\" may use letters, digits, '_', '-' and '.' only";
            return false;
        }
    e.releaseDate = v.wstr("release_date");
    e.region = v.wstr("region");
    e.repoDir = v.wstr("repo_dir", e.artKey);
    e.romFile = v.wstr("rom_file", e.artKey + L".z64");
    e.forceBorderless = v.boolean("force_borderless", true);
    e.comingSoon = v.boolean("coming_soon", false);
    e.githubRepo = v.wstr("github_repo");
    e.romSha1 = v.wstr("rom_sha1");
    e.romNote = v.wstr("rom_note");
    e.romHint = v.wstr("rom_hint");                // aki-roadmap#216
    e.romStoredFile = v.wstr("rom_stored_file");  // aki-roadmap#216
    e.romCode = v.wstr("rom_code");
    e.romVersion = v.integer("rom_version", -1);
    e.modGameId = v.wstr("mod_game_id");
    e.baseId = v.wstr("base");
    if (const JsonValue* mp = v.get("model_profile"); mp && mp->is(JsonValue::Type::String)) e.modelProfile = mp->s;
    *g = std::move(e);
    return true;
}

std::vector<GameEntry> registry_from_json(const std::string& text, std::wstring* err) {
    JsonValue root;
    std::string perr;
    if (!json_parse(text, &root, &perr)) {
        *err = L"not valid JSON: " + json_utf8_to_wide(perr);
        return {};
    }
    const JsonValue* list = root.get("games");
    if (!list || !list->is(JsonValue::Type::Array) || list->items.empty()) {
        *err = L"no \"games\" list";
        return {};
    }
    std::vector<GameEntry> games;
    for (const auto& item : list->items) {
        GameEntry g;
        if (!game_from_json(item, &g, err)) return {};
        for (const auto& o : games)
            if (o.artKey == g.artKey) {
                *err = L"game id \"" + g.artKey + L"\" is listed twice";
                return {};
            }
        games.push_back(std::move(g));
    }
    return games;
}

static std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::vector<GameEntry> load_registry(const fs::path& file) {
    std::error_code ec;
    if (!file.empty() && fs::is_regular_file(file, ec)) {
        std::wstring err;
        std::vector<GameEntry> games = registry_from_json(read_file(file), &err);
        if (!games.empty()) return games;
        logf(L"games registry %s ignored (%s): using the built-in list", file.c_str(), err.c_str());
    }
    std::wstring err;
    std::vector<GameEntry> games = registry_from_json(kEmbeddedGamesJson, &err);
    if (games.empty()) logf(L"built-in games registry is broken: %s", err.c_str());
    return games;
}

// Puts each variant right after its base game (and that base's earlier
// variants); a variant of a variant follows that one.
static void place_with_variants(const std::vector<GameEntry>& games, size_t i, std::vector<bool>& placed,
                                std::vector<GameEntry>& out) {
    out.push_back(games[i]);
    placed[i] = true;
    for (size_t k = 0; k < games.size(); ++k)
        if (!placed[k] && games[k].baseId == games[i].artKey) place_with_variants(games, k, placed, out);
}

static void order_variants(std::vector<GameEntry>& games) {
    std::vector<GameEntry> out;
    std::vector<bool> placed(games.size(), false);
    for (size_t i = 0; i < games.size(); ++i)
        if (!placed[i] && games[i].baseId.empty()) place_with_variants(games, i, placed, out);
    for (size_t i = 0; i < games.size(); ++i)  // variants whose base isn't listed (or a cycle)
        if (!placed[i]) place_with_variants(games, i, placed, out);
    games.swap(out);
}

void discover_games(std::vector<GameEntry>& games, const fs::path& depotRoot, const fs::path& gamesRoot) {
    std::error_code ec;
    std::vector<std::pair<fs::path, bool>> roots;  // (root, is the depot)
    if (!depotRoot.empty()) roots.push_back({depotRoot, true});
    if (!gamesRoot.empty() && gamesRoot != depotRoot) roots.push_back({gamesRoot, false});
    std::vector<GameEntry> found;
    for (const auto& [root, isDepot] : roots) {
        std::vector<fs::path> dirs;
        for (fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            std::error_code fec;
            if (it->is_directory(fec) && fs::is_regular_file(it->path() / L"aki-game.json", fec))
                dirs.push_back(it->path());
        }
        ec.clear();
        std::sort(dirs.begin(), dirs.end());
        for (const auto& d : dirs) {
            fs::path manifest = d / L"aki-game.json";
            JsonValue v;
            std::string perr;
            std::wstring err;
            GameEntry g;
            if (!json_parse(read_file(manifest), &v, &perr)) {
                logf(L"discovery: %s is not valid JSON (%S)", manifest.c_str(), perr.c_str());
                continue;
            }
            if (!game_from_json(v, &g, &err)) {
                logf(L"discovery: %s skipped: %s", manifest.c_str(), err.c_str());
                continue;
            }
            bool known = false;
            for (const auto& o : games) known = known || o.artKey == g.artKey;
            for (const auto& o : found) known = known || o.artKey == g.artKey;
            if (known) continue;  // additive: the registry (or an earlier folder) has it
            if (isDepot) {
                g.repoDir = d.filename().wstring();  // a clone or worktree under any name
            } else if (d.filename().wstring() != g.artKey) {
                logf(L"discovery: %s skipped: an install folder must be named after its id (%s)", manifest.c_str(),
                     g.artKey.c_str());
                continue;
            }
            g.manifestPath = manifest;
            logf(L"discovery: %s (%s) from %s", g.title.c_str(), g.artKey.c_str(), manifest.c_str());
            found.push_back(std::move(g));
        }
    }
    for (auto& g : found) games.push_back(std::move(g));
    order_variants(games);
}

std::vector<GameEntry> build_game_list(const std::filesystem::path& depotRoot) {
    std::vector<GameEntry> games = load_registry(app_root() / L"assets" / L"games.json");
    discover_games(games, depotRoot, games_root());
    resolve_game_paths(games, depotRoot, games_root());
    return games;
}

void resolve_game_paths(std::vector<GameEntry>& games, const fs::path& depotRoot, const fs::path& gamesDir) {
    std::error_code ec;
    for (auto& g : games) {
        g.installDir = gamesDir / g.artKey;
        g.repoPath = depotRoot / g.repoDir;
        g.exePath = g.repoPath / g.exeRel;
        g.romPath = g.repoPath / g.romFile;
        g.romRegistryPath = g.romPath;
        g.romInstallPath = g.romPath;
        g.romAutoPath = g.romPath;
        g.exeFound = !depotRoot.empty() && fs::is_regular_file(g.exePath, ec);
        g.romFound = !depotRoot.empty() && fs::is_regular_file(g.romPath, ec);

        // No depot build? Fall back to the in-app install dir (release zips are
        // flat: exe at the root, so cwd = installDir). Also point paths there
        // when there's no depot checkout at all, so download + ROM select know
        // where to land.
        if (!g.exeFound) {
            fs::path instExe = g.installDir / fs::path(g.exeRel).filename();
            bool instFound = fs::is_regular_file(instExe, ec);
            bool devCheckout = !depotRoot.empty() && fs::is_directory(g.repoPath, ec);
            if (instFound || !devCheckout) {
                g.repoPath = g.installDir;
                g.exePath = instExe;
                g.romPath = g.installDir / g.romFile;
                g.romRegistryPath = g.romPath;
                g.romInstallPath = g.romPath;
                g.romAutoPath = g.romPath;
                g.exeFound = instFound;
                g.romFound = fs::is_regular_file(g.romPath, ec);
            }
        }

        g.repoRegistryPath = g.repoPath;
        g.exeRegistryPath = g.exePath;

        std::ifstream vf(g.installDir / L"version.txt");
        if (vf) {
            std::string tag;
            std::getline(vf, tag);
            while (!tag.empty() && (tag.back() == '\r' || tag.back() == '\n')) tag.pop_back();
            int n = MultiByteToWideChar(CP_UTF8, 0, tag.c_str(), -1, nullptr, 0);
            std::wstring w(n > 0 ? n - 1 : 0, 0);
            if (n > 1) MultiByteToWideChar(CP_UTF8, 0, tag.c_str(), -1, w.data(), n);
            g.installedTag = w;
        }
    }
}

// A ROM is hashed once per launcher run, unless the file changes.
static std::wstring cached_sha1(const fs::path& p) {
    using Stamp = std::tuple<std::uintmax_t, fs::file_time_type>;
    static std::map<std::wstring, std::pair<Stamp, std::wstring>> cache;
    std::error_code ec;
    const Stamp stamp{fs::file_size(p, ec), fs::last_write_time(p, ec)};
    auto it = cache.find(p.wstring());
    if (!ec && it != cache.end() && it->second.first == stamp) return it->second.second;
    std::wstring sha1 = sha1_file(p);
    if (!ec) cache[p.wstring()] = {stamp, sha1};
    return sha1;
}

void resolve_rom(GameEntry& g, const std::wstring& romOverride) {
    using namespace rom_rules;
    g.romInstallPath = g.repoPath / g.romFile;

    std::vector<std::wstring> candidates = {g.romInstallPath.wstring()};
    if (!g.romStoredFile.empty()) candidates.push_back((game_data_dir(g) / g.romStoredFile).wstring());
    candidates.push_back(g.romRegistryPath.wstring());

    const RomResolution r = resolve_rom_choice(
        romOverride, candidates, g.romSha1,
        [](const std::wstring& p) {
            std::error_code ec;
            return fs::is_regular_file(p, ec);
        },
        [](const std::wstring& p) { return cached_sha1(p); });

    g.romPath = r.rom.path;
    g.romAutoPath = r.autoRom.path;
    g.romFound = r.rom.status != RomStatus::Missing;
    g.romWrong = r.rom.status == RomStatus::Wrong && !r.overrideUsed;
    g.romHaveSha1 = r.rom.status == RomStatus::Wrong ? r.rom.sha1 : L"";

    g.romNotice.clear();
    if (r.overrideMissing) {
        g.romNotice = L"The ROM path in Settings doesn't exist, so it's ignored: " + romOverride;
    } else if (r.overrideUsed && r.rom.status == RomStatus::Wrong) {
        g.romNotice = L"The ROM path in Settings isn't the " + g.romNote + L", so the game may refuse it (" +
                      g.romPath.filename().wstring() + L")";
    }

    logf(L"rom for %s: %s%s %s", g.artKey.c_str(),
         r.rom.status == RomStatus::Good ? L"good" : r.rom.status == RomStatus::Wrong ? L"WRONG" : L"MISSING",
         r.overrideUsed ? L" (rom= override)" : L"", g.romPath.c_str());
    if (r.overrideMissing) logf(L"override rom for %s ignored (missing): %s", g.artKey.c_str(), romOverride.c_str());
}

std::wstring rom_problem(const GameEntry& g) {
    if (!g.romFound) {
        return g.title + L" has no ROM yet. Select your own dump: " + g.romNote + L" (.z64)\nRequired SHA1: " + g.romSha1;
    }
    if (g.romWrong) {
        return g.romPath.wstring() + L" isn't the " + g.title + L" ROM this port needs: the " + g.romNote +
               L".\nRequired SHA1: " + g.romSha1 + L"\nThis file's SHA1: " +
               (g.romHaveSha1.empty() ? L"(couldn't read it)" : g.romHaveSha1);
    }
    return {};
}
