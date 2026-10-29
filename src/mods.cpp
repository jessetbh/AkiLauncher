#include "mods.h"

#include "games.h"
#include "json.h"
#include "launch.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <memory>

#include "miniz.h"

namespace fs = std::filesystem;

static std::wstring u8w(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n > 0 ? n : 0, 0);
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// A zip opened through _wfopen (any path), for miniz's reader.
struct ZipFile {
    mz_zip_archive za{};
    FILE* f = nullptr;
    bool open = false;
    explicit ZipFile(const fs::path& p) {
        if (_wfopen_s(&f, p.c_str(), L"rb") != 0 || !f) {
            f = nullptr;
            return;
        }
        std::error_code ec;
        uintmax_t size = fs::file_size(p, ec);
        open = !ec && mz_zip_reader_init_cfile(&za, f, (mz_uint64)size, 0);
    }
    ~ZipFile() {
        if (open) mz_zip_reader_end(&za);
        if (f) fclose(f);
    }
    ZipFile(const ZipFile&) = delete;
    ZipFile& operator=(const ZipFile&) = delete;
};

static std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    return s;
}

// ---------------------------------------------------------------------------
// Where the mods are
// ---------------------------------------------------------------------------

fs::path game_mods_dir(const GameEntry& g) { return game_data_dir(g) / L"mods"; }

// ---------------------------------------------------------------------------
// mod.json
// ---------------------------------------------------------------------------

ModManifest parse_mod_manifest(const std::string& raw) {
    ModManifest m;
    m.present = true;
    JsonValue j;
    if (!json_parse(raw, &j) || !j.is(JsonValue::Type::Object)) return m;
    m.valid = true;
    m.id = j.wstr("id");
    m.version = j.wstr("version");
    m.name = j.wstr("display_name");
    m.gameId = j.wstr("game_id");
    m.description = j.wstr("short_description");
    if (m.description.empty()) m.description = j.wstr("description");
    if (const JsonValue* a = j.get("authors"); a && a->is(JsonValue::Type::Array))
        for (const auto& v : a->items)
            if (v.is(JsonValue::Type::String)) m.authors.push_back(json_utf8_to_wide(v.s));
    return m;
}

// ---------------------------------------------------------------------------
// Inspecting
// ---------------------------------------------------------------------------

std::wstring ModItem::display_name() const {
    if (!manifest.name.empty()) return manifest.name;
    if (!manifest.id.empty()) return manifest.id;
    return kind == ModKind::Folder ? path.filename().wstring() : path.stem().wstring();
}

std::wstring ModItem::kind_text() const {
    if (code && textures) return L"Code + textures";
    if (code) return L"Code mod";
    if (textures) return L"Texture pack";
    return kind == ModKind::Nrm ? L"Mod" : L"Unknown";
}

static bool is_hash_image(const std::wstring& name) {
    // <16 hex>.png / .dds: loads in our rt64 without an rt64.json entry (aki-roadmap#48).
    fs::path p(name);
    std::wstring ext = lower(p.extension().wstring());
    std::wstring stem = p.stem().wstring();
    if (ext != L".png" && ext != L".dds") return false;
    if (stem.size() != 16) return false;
    for (wchar_t c : stem)
        if (!iswxdigit(c)) return false;
    return true;
}

static std::string read_text_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static void inspect_zip(ModItem& m) {
    ZipFile zf(m.path);
    if (!zf.open) {
        m.problem = L"not a valid zip file";
        return;
    }
    mz_zip_archive& za = zf.za;
    bool nestedRt64 = false;
    int n = (int)mz_zip_reader_get_num_files(&za);
    for (int i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&za, i, &st)) continue;
        std::string name = st.m_filename;
        std::string lname = name;
        std::transform(lname.begin(), lname.end(), lname.begin(), ::tolower);
        if (lname == "rt64.json") m.textures = true;
        else if (lname.size() > 10 && lname.compare(lname.size() - 10, 10, "/rt64.json") == 0) nestedRt64 = true;
        if (lname == "mod_binary.bin") m.code = true;
        if (lname == "thumb.png") m.hasThumb = true;
        if (lname == "mod.json") {
            size_t sz = 0;
            void* p = mz_zip_reader_extract_to_heap(&za, i, &sz, 0);
            if (p) {
                m.manifest = parse_mod_manifest(std::string((const char*)p, sz));
                mz_free(p);
            }
        }
    }
    if (m.kind == ModKind::Nrm && !m.manifest.present) m.problem = L"no mod.json: the game won't load it";
    else if (m.manifest.present && !m.manifest.valid) m.problem = L"mod.json is not valid JSON";
    else if (m.kind == ModKind::Rtz && !m.textures)
        m.problem = nestedRt64 ? L"rt64.json is in a subfolder, not at the root: zip the folder's contents"
                               : L"no rt64.json at the root";
}

static void inspect_folder(ModItem& m) {
    std::error_code ec;
    m.textures = fs::exists(m.path / L"rt64.json", ec);
    m.hasThumb = fs::exists(m.path / L"thumb.png", ec);
    if (fs::exists(m.path / L"mod.json", ec)) m.manifest = parse_mod_manifest(read_text_file(m.path / L"mod.json"));
    int seen = 0;
    for (fs::recursive_directory_iterator it(m.path, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end && seen < 20000; it.increment(ec), ++seen) {
        std::error_code fec;
        if (!it->is_regular_file(fec)) continue;
        m.bytes += it->file_size(fec);
        if (!m.textures && is_hash_image(it->path().filename().wstring())) m.textures = true;
    }
    if (m.manifest.present && !m.manifest.valid) m.problem = L"mod.json is not valid JSON";
    else if (!m.textures) m.problem = L"no rt64.json and no <hash>.png images: nothing would load";
}

ModItem inspect_mod(const fs::path& p, const fs::path& bundledDir) {
    ModItem m;
    m.path = p;
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
        m.kind = ModKind::Folder;
        inspect_folder(m);
        return m;
    }
    std::wstring ext = lower(p.extension().wstring());
    m.kind = ext == L".rtz" ? ModKind::Rtz : ModKind::Nrm;
    m.bytes = fs::file_size(p, ec);
    if (!bundledDir.empty() && fs::exists(bundledDir / p.filename(), ec)) m.bundled = true;
    inspect_zip(m);
    return m;
}

std::vector<ModItem> list_mods(const fs::path& modsDir, const fs::path& bundledDir) {
    std::vector<ModItem> out;
    std::error_code ec;
    for (fs::directory_iterator it(modsDir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code fec;
        std::wstring name = it->path().filename().wstring();
        if (it->is_directory(fec)) {
            // models\: aki-model packs, managed by their own tools; dot folders: tool state.
            if (lower(name) == L"models" || name.empty() || name[0] == L'.') continue;
            out.push_back(inspect_mod(it->path(), bundledDir));
        } else if (it->is_regular_file(fec)) {
            std::wstring ext = lower(it->path().extension().wstring());
            if (ext == L".nrm" || ext == L".rtz") out.push_back(inspect_mod(it->path(), bundledDir));
        }
    }
    std::sort(out.begin(), out.end(), [](const ModItem& a, const ModItem& b) {
        return lower(a.display_name()) < lower(b.display_name());
    });
    return out;
}

std::vector<uint8_t> read_mod_thumb(const ModItem& m) {
    std::vector<uint8_t> out;
    if (!m.hasThumb) return out;
    if (m.kind == ModKind::Folder) {
        std::string s = read_text_file(m.path / L"thumb.png");
        out.assign(s.begin(), s.end());
        return out;
    }
    ZipFile zf(m.path);
    if (!zf.open) return out;
    size_t sz = 0;
    void* p = mz_zip_reader_extract_file_to_heap(&zf.za, "thumb.png", &sz, 0);
    if (p) {
        out.assign((const uint8_t*)p, (const uint8_t*)p + sz);
        mz_free(p);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Installing and removing
// ---------------------------------------------------------------------------

bool recycle_path(const fs::path& p, std::wstring* errorOut) {
    // AkiLauncherTests: delete for real instead of filling the Recycle Bin with synthetic files.
    wchar_t test[4]{};
    if (GetEnvironmentVariableW(L"AKI_LAUNCHER_TEST_NO_RECYCLE", test, 4) == 1 && test[0] == L'1') {
        std::error_code ec;
        fs::remove_all(p, ec);
        if (ec && errorOut) *errorOut = L"can't delete " + p.wstring();
        return !ec;
    }
    std::wstring from = fs::absolute(p).wstring();
    from.push_back(L'\0');  // double-null-terminated list
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    // FOF_WANTNUKEWARNING: on a drive without a Recycle Bin, ask before deleting for good.
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI | FOF_WANTNUKEWARNING;
    int r = SHFileOperationW(&op);
    std::error_code ec;
    if (r != 0 || op.fAnyOperationsAborted || fs::exists(p, ec)) {
        if (errorOut) *errorOut = L"couldn't move " + p.filename().wstring() + L" to the Recycle Bin (in use? close the game)";
        return false;
    }
    return true;
}

bool remove_mod(const ModItem& m, std::wstring* errorOut) {
    if (m.bundled) {
        if (errorOut) *errorOut = m.display_name() + L" comes with the game and is copied back on its next start: "
                                  L"turn it off in the game's Mods menu instead";
        return false;
    }
    if (!recycle_path(m.path, errorOut)) return false;
    logf(L"mods: removed %s (Recycle Bin)", m.path.c_str());
    return true;
}

static bool wrong_game(const ModManifest& man, const std::wstring& gameId, const std::wstring& title,
                       const std::wstring& what, std::wstring* msg) {
    if (!man.gameId.empty() && !gameId.empty() && man.gameId != gameId) {
        *msg = what + L" is made for \"" + man.gameId + L"\", not " + title + L" (" + gameId + L")";
        return true;
    }
    return false;
}

// Copies srcFile to dest through dest.new. An existing dest is set aside
// (dest.old) until the new copy is in place, then goes to the Recycle Bin; if
// anything fails, the old copy is put back.
static bool place_file(const fs::path& dest, const fs::path& srcFile, std::wstring* msg, bool* replaced) {
    std::error_code ec;
    fs::path tmp = dest, old = dest;
    tmp += L".new";
    old += L".old";
    fs::copy_file(srcFile, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        fs::remove(tmp, ec);
        *msg = L"can't copy to " + tmp.wstring();
        return false;
    }
    *replaced = fs::exists(dest, ec);
    if (*replaced) {
        fs::remove(old, ec);
        fs::rename(dest, old, ec);
        if (ec) {
            fs::remove(tmp, ec);
            *msg = L"can't replace " + dest.filename().wstring() + L" (in use? close the game)";
            return false;
        }
    }
    fs::rename(tmp, dest, ec);
    if (ec) {
        std::error_code ec2;
        fs::remove(tmp, ec2);
        if (*replaced) fs::rename(old, dest, ec2);
        *msg = L"can't write " + dest.wstring();
        return false;
    }
    if (*replaced) {
        std::wstring ignored;
        if (!recycle_path(old, &ignored)) fs::remove(old, ec);
    }
    return true;
}

// The pack folder's mods folder must not be inside it (dropping the mods
// folder, or a parent, would copy into the tree being read).
static bool contains_path(const fs::path& outer, const fs::path& inner) {
    std::error_code ec;
    fs::path a = fs::weakly_canonical(outer, ec), b = fs::weakly_canonical(inner, ec);
    std::wstring as = lower(a.wstring()), bs = lower(b.wstring());
    if (!as.empty() && as.back() != L'\\') as += L'\\';
    return bs.size() >= as.size() && bs.compare(0, as.size(), as) == 0;
}

static bool copy_tree(const fs::path& src, const fs::path& dst, std::wstring* msg) {
    std::error_code ec;
    fs::create_directories(dst, ec);
    for (fs::recursive_directory_iterator it(src, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        fs::path rel = fs::relative(it->path(), src, ec);
        if (ec) break;
        std::error_code fec;
        if (it->is_directory(fec)) fs::create_directories(dst / rel, fec);
        else if (it->is_regular_file(fec)) fs::copy_file(it->path(), dst / rel, fs::copy_options::overwrite_existing, fec);
        if (fec) {
            *msg = L"can't copy " + rel.wstring() + L" (" + u8w(fec.message()) + L")";
            return false;
        }
    }
    if (ec) {
        *msg = L"can't read " + src.wstring();
        return false;
    }
    return true;
}

InstallResult install_mod(const fs::path& src, const fs::path& modsDir, const std::wstring& gameId,
                          const std::wstring& gameTitle) {
    InstallResult r;
    std::error_code ec;
    if (!fs::exists(src, ec)) {
        r.message = L"no such file: " + src.wstring();
        return r;
    }
    if (fs::equivalent(src.parent_path(), modsDir, ec)) {
        r.message = src.filename().wstring() + L" is already in the mods folder";
        return r;
    }
    fs::create_directories(modsDir, ec);
    std::wstring name = src.filename().wstring();
    bool replaced = false;

    // A pack folder: copied as it is (its name is the pack's name in the Mods menu).
    if (fs::is_directory(src, ec)) {
        if (name.empty() || contains_path(src, modsDir)) {
            r.message = src.wstring() + L" holds the mods folder itself: pick the pack's own folder";
            return r;
        }
        ModItem m = inspect_mod(src);
        if (!m.problem.empty()) {
            r.message = name + L": " + m.problem;
            return r;
        }
        if (wrong_game(m.manifest, gameId, gameTitle, name, &r.message)) return r;
        fs::path dest = modsDir / name;
        if (fs::exists(dest, ec)) {
            r.message = name + L" is installed already: remove it first to replace it";
            return r;
        }
        if (!copy_tree(src, dest, &r.message)) {
            fs::remove_all(dest, ec);
            return r;
        }
        r.ok = true;
        r.installed.push_back(dest);
        r.message = L"Installed " + m.display_name() + L" (folder)";
        logf(L"mods: installed folder %s -> %s", src.c_str(), dest.c_str());
        return r;
    }

    std::wstring ext = lower(src.extension().wstring());
    if (ext == L".nrm" || ext == L".rtz") {
        ModItem m = inspect_mod(src);
        if (!m.problem.empty()) {  // it wouldn't load: don't install it
            r.message = name + L": " + m.problem;
            return r;
        }
        if (wrong_game(m.manifest, gameId, gameTitle, name, &r.message)) return r;
        fs::path dest = modsDir / name;
        if (!place_file(dest, src, &r.message, &replaced)) return r;
        r.ok = true;
        r.installed.push_back(dest);
        r.message = L"Installed " + m.display_name() + (replaced ? L" (the older copy is in the Recycle Bin)" : L"");
        logf(L"mods: installed %s -> %s", src.c_str(), dest.c_str());
        return r;
    }

    if (ext != L".zip") {
        r.message = name + L": not a mod (.nrm), texture pack (.rtz, .zip or a folder)";
        return r;
    }
    // A .zip: a texture pack zipped as is (rt64.json at the root) becomes an
    // .rtz; a package with .nrm / .rtz files inside installs those.
    auto zf = std::make_unique<ZipFile>(src);
    if (!zf->open) {
        r.message = name + L": not a valid zip file";
        return r;
    }
    mz_zip_archive& za = zf->za;
    bool rootRt64 = mz_zip_reader_locate_file(&za, "rt64.json", nullptr, 0) >= 0;
    if (rootRt64) {
        ModManifest man;
        size_t sz = 0;
        if (void* p = mz_zip_reader_extract_file_to_heap(&za, "mod.json", &sz, 0)) {
            man = parse_mod_manifest(std::string((const char*)p, sz));
            mz_free(p);
        }
        zf.reset();  // closed before the copy
        if (wrong_game(man, gameId, gameTitle, name, &r.message)) return r;
        fs::path dest = modsDir / (src.stem().wstring() + L".rtz");
        if (!place_file(dest, src, &r.message, &replaced)) return r;
        r.ok = true;
        r.installed.push_back(dest);
        r.message = L"Installed " + dest.filename().wstring() + L" (a texture pack zip, saved as .rtz)" +
                    (replaced ? L"; the older copy is in the Recycle Bin" : L"");
        logf(L"mods: installed %s -> %s", src.c_str(), dest.c_str());
        return r;
    }
    // A package: its .nrm / .rtz files, extracted (to files, not memory: packs
    // can be big) into a work folder and checked like direct installs before
    // any goes in.
    fs::path tmpDir = modsDir / L".aki-install";
    fs::remove_all(tmpDir, ec);
    fs::create_directories(tmpDir, ec);
    struct Cleanup {
        fs::path d;
        ~Cleanup() { std::error_code e; fs::remove_all(d, e); }
    } cleanup{tmpDir};
    std::vector<fs::path> members;
    int n = (int)mz_zip_reader_get_num_files(&za);
    for (int i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&za, i, &st) || mz_zip_reader_is_file_a_directory(&za, i)) continue;
        std::wstring leaf = fs::path(u8w(st.m_filename)).filename().wstring();  // flat: no paths from the zip
        std::wstring mext = lower(fs::path(leaf).extension().wstring());
        if (mext != L".nrm" && mext != L".rtz") continue;
        fs::path out = tmpDir / leaf;
        if (fs::exists(out, ec)) {
            r.message = name + L" has two files named " + leaf + L": install them one at a time";
            return r;
        }
        FILE* f = nullptr;
        if (_wfopen_s(&f, out.c_str(), L"wb") != 0 || !f) {
            r.message = L"can't write " + out.wstring();
            return r;
        }
        bool okx = mz_zip_reader_extract_to_cfile(&za, i, f, 0);
        if (fclose(f) != 0) okx = false;
        if (!okx) {
            r.message = name + L": can't unpack " + leaf;
            return r;
        }
        members.push_back(out);
    }
    zf.reset();
    if (members.empty()) {
        r.message = name + L": no rt64.json at its root and no .nrm or .rtz inside: not a mod package";
        return r;
    }
    for (const auto& probe : members) {
        ModItem m = inspect_mod(probe);
        std::wstring leaf = probe.filename().wstring();
        if (!m.problem.empty()) {
            r.message = name + L": " + leaf + L": " + m.problem + L" (nothing installed)";
            return r;
        }
        if (wrong_game(m.manifest, gameId, gameTitle, leaf, &r.message)) return r;
    }
    int replacedCount = 0;
    for (const auto& probe : members) {
        fs::path dest = modsDir / probe.filename();
        std::wstring err;
        if (!place_file(dest, probe, &err, &replaced)) {
            std::wstring done;
            for (const auto& p : r.installed) done += (done.empty() ? L"" : L", ") + p.filename().wstring();
            r.message = err + (done.empty() ? L"" : L" (already installed from " + name + L": " + done + L")");
            return r;
        }
        replacedCount += replaced;
        r.installed.push_back(dest);
    }
    r.ok = true;
    r.message = L"Installed " + std::to_wstring(members.size()) + L" file(s) from " + name +
                (replacedCount ? L" (older copies are in the Recycle Bin)" : L"");
    logf(L"mods: installed %zu files from %s", members.size(), src.c_str());
    return r;
}
