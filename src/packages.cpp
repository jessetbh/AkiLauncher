#include "packages.h"

#include "mods.h"
#include "selfupdate.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <regex>
#include <set>

#include "miniz.h"

namespace fs = std::filesystem;

namespace pkg {

static const char* kManifest = "aki-package.json";
static const char* kConversionManifest = "aki.json";
static const uint64_t kMaxTotal = 8ull << 30;     // uncompressed bytes a package may hold
static const uint64_t kMaxJson = 1ull << 20;      // a manifest larger than this isn't read
static const uint64_t kMaxInner = 512ull << 20;   // an .nrm / .rtz larger than this isn't opened
static const char kPng[8] = {'\x89', 'P', 'N', 'G', '\r', '\n', '\x1a', '\n'};

// The runtime's game ids and the short keys the launcher and AkiMods use.
static const std::pair<const char*, const char*> kGames[] = {
    {"wt", "wcwnwoworldtour"}, {"revenge", "wcwnworevenge"}, {"wm2k", "wwfwm2k"},
    {"vpw64", "vpw64"},        {"nomercy", "wwfnomercy"},    {"vpw2", "vpw2"}};

// --- small helpers ----------------------------------------------------------------------------------------------------

static std::wstring w(const std::string& s) { return json_utf8_to_wide(s); }

static std::string u8(const std::wstring& ws) {
    if (ws.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::string lower(std::string s) {
    for (auto& c : s) c = (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    return s;
}

// Case folded the way Windows compares names (not only ASCII): the key for "same file on Windows".
static std::string fold(const std::string& s) {
    std::wstring ws = w(s);
    if (!ws.empty()) CharLowerBuffW(ws.data(), (DWORD)ws.size());
    return u8(ws);
}

static bool valid_utf8(const std::string& s) {
    return s.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0) > 0;
}

// A zip entry name as UTF-8: as it is when it is valid UTF-8 (flagged or not), else the zip's old DOS codepage (437),
// as Explorer and older tools write non-ASCII names.
static std::string zip_name_utf8(const std::string& raw) {
    if (valid_utf8(raw)) return raw;
    int n = MultiByteToWideChar(437, 0, raw.data(), (int)raw.size(), nullptr, 0);
    std::wstring ws((size_t)n, L'\0');
    MultiByteToWideChar(437, 0, raw.data(), (int)raw.size(), ws.data(), n);
    return u8(ws);
}

static bool ends_with(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

static std::string base_name(std::string p) {
    while (!p.empty() && p.back() == '/') p.pop_back();
    size_t i = p.rfind('/');
    return i == std::string::npos ? p : p.substr(i + 1);
}

static std::string strip_slash(std::string p) {
    while (!p.empty() && p.back() == '/') p.pop_back();
    return p;
}

static const JsonValue* member(const JsonValue& o, const char* k) { return o.is(JsonValue::Type::Object) ? o.get(k) : nullptr; }

static std::string str(const JsonValue& o, const char* k) {
    const JsonValue* v = member(o, k);
    return v && v->is(JsonValue::Type::String) ? v->s : std::string();
}

static bool is_device_name(const std::string& part) {
    static const std::regex re("^(con|prn|aux|nul|com[1-9]|lpt[1-9])(\\..*)?$", std::regex::icase);
    return std::regex_match(part, re);
}

// A relative path that stays inside the install folder on Windows (as aki-package.py's safe_path).
static bool safe_path(const std::string& name) {
    if (name.empty() || name.find('\\') != std::string::npos || name[0] == '/' || name.find(':') != std::string::npos)
        return false;
    std::string p = strip_slash(name);
    size_t start = 0;
    while (start <= p.size()) {
        size_t end = p.find('/', start);
        if (end == std::string::npos) end = p.size();
        std::string part = p.substr(start, end - start);
        if (part.empty() || part == "." || part == ".." || is_device_name(part)) return false;
        char last = part.back();
        if (last == ' ' || last == '.') return false;
        start = end + 1;
    }
    return true;
}

static void problem(std::vector<Problem>& out, bool error, const char* field, const std::wstring& text) {
    out.push_back({error, field, text});
}

// --- ids, versions, games ---------------------------------------------------------------------------------------------

bool valid_id(const std::string& id) {
    static const std::regex re("^[a-z0-9]([a-z0-9_.-]{0,46}[a-z0-9_-])?$");
    return std::regex_match(id, re) && !is_device_name(id);
}

bool valid_version(const std::string& v) {
    static const std::regex re("^[0-9]+\\.[0-9]+\\.[0-9]+(-[0-9A-Za-z.-]+)?$");
    return std::regex_match(v, re);
}

int compare_versions(const std::string& a, const std::string& b) {
    Version va, vb;
    parse_version(w(a), &va);
    parse_version(w(b), &vb);
    return ::compare_versions(va, vb);
}

std::string runtime_game_id(const std::string& g) {
    for (auto& [shortKey, runtime] : kGames)
        if (g == shortKey || g == runtime) return runtime;
    return {};
}

// --- the manifest -------------------------------------------------------------------------------------------------------

bool Manifest::ok() const {
    return parsed && std::none_of(problems.begin(), problems.end(), [](const Problem& p) { return p.error; });
}

static std::wstring first_error_of(const std::vector<Problem>& ps) {
    for (auto& p : ps)
        if (p.error) return p.text;
    return {};
}
std::wstring Manifest::first_error() const { return parsed ? first_error_of(problems) : L"aki-package.json is not a JSON object"; }

static std::vector<std::string> strings(const JsonValue& o, const char* k, std::vector<Problem>& ps, const char* field,
                                        bool required, bool nonEmptyItems) {
    std::vector<std::string> out;
    const JsonValue* v = member(o, k);
    if (!v) {
        if (required) problem(ps, true, field, w(std::string("no \"") + field + "\""));
        return out;
    }
    if (!v->is(JsonValue::Type::Array)) {
        problem(ps, true, field, w(std::string(field) + ": must be a list"));
        return out;
    }
    for (auto& item : v->items) {
        if (!item.is(JsonValue::Type::String) || (nonEmptyItems && item.s.empty()))
            problem(ps, true, field, w(std::string(field) + ": every item must be a non-empty string"));
        else
            out.push_back(item.s);
    }
    return out;
}

static std::string string_field(const JsonValue& o, const char* k, std::vector<Problem>& ps, const char* field,
                                bool required, bool nonEmpty) {
    const JsonValue* v = member(o, k);
    if (!v) {
        if (required) problem(ps, true, field, w(std::string("no \"") + field + "\""));
        return {};
    }
    if (!v->is(JsonValue::Type::String) || (nonEmpty && v->s.empty())) {
        problem(ps, true, field, w(std::string(field) + (nonEmpty ? ": must be a non-empty string" : ": must be a string")));
        return {};
    }
    return v->s;
}

Manifest parse_manifest(const std::string& json) {
    Manifest m;
    JsonValue root;
    std::string perr;
    if (!json_parse(json, &root, &perr) || !root.is(JsonValue::Type::Object)) {
        problem(m.problems, true, "format", L"aki-package.json is not a JSON object" + (perr.empty() ? L"" : L" (" + w(perr) + L")"));
        return m;
    }
    m.parsed = true;
    auto& ps = m.problems;
    const JsonValue* fmt = root.get("format");
    if (!fmt || !fmt->is(JsonValue::Type::Number) || fmt->n != 1) {
        problem(ps, true, "format", L"aki-package.json format must be 1 (this launcher reads format 1)");
        return m;  // another format: its other fields mean something else
    }
    m.kind = string_field(root, "kind", ps, "kind", true, true);
    if (!m.kind.empty() && m.kind != "addon" && m.kind != "texture-pack" && m.kind != "model-pack" && m.kind != "conversion")
        problem(ps, true, "kind", w("kind \"" + m.kind + "\" is not one of addon, texture-pack, model-pack, conversion"));
    m.id = string_field(root, "id", ps, "id", true, true);
    if (!m.id.empty() && !valid_id(m.id))
        problem(ps, true, "id", w("id \"" + m.id + "\": a-z 0-9 _ . -, at most 48, no trailing '.', not a device name"));
    m.version = string_field(root, "version", ps, "version", true, true);
    if (!m.version.empty() && !valid_version(m.version))
        problem(ps, true, "version", w("version \"" + m.version + "\" is not x.y.z"));
    m.gameId = string_field(root, "game_id", ps, "game_id", true, true);
    if (!m.gameId.empty()) {
        bool known = false;
        for (auto& g : kGames) known = known || m.gameId == g.second;
        if (!known) {
            std::string r = runtime_game_id(m.gameId);
            problem(ps, true, "game_id",
                    w("game_id \"" + m.gameId + "\" is not a game id" + (r.empty() ? "" : " (the runtime's is \"" + r + "\")")));
        }
    }
    m.displayName = string_field(root, "display_name", ps, "display_name", true, true);
    m.authors = strings(root, "authors", ps, "authors", true, true);
    if (member(root, "authors") && member(root, "authors")->is(JsonValue::Type::Array) && m.authors.empty())
        problem(ps, true, "authors", L"authors: needs at least 1 author");
    m.shortDescription = string_field(root, "short_description", ps, "short_description", false, false);
    m.thumb = string_field(root, "thumb", ps, "thumb", false, true);
    m.license = string_field(root, "license", ps, "license", false, true);
    m.source = string_field(root, "source", ps, "source", false, false);
    m.replaces = strings(root, "replaces", ps, "replaces", false, true);
    for (auto& r : m.replaces)
        if (!valid_id(r)) problem(ps, true, "replaces", w("replaces \"" + r + "\" is not a package id"));
    if (const JsonValue* perm = member(root, "permission")) {
        if (!perm->is(JsonValue::Type::Object)) {
            problem(ps, true, "permission", L"permission: must be an object");
        } else {
            for (const char* k : {"redistribute", "modify"}) {
                std::string v = string_field(*perm, k, ps, "permission", false, false);
                if (member(*perm, k) && v != "yes" && v != "no" && v != "ask" && v != "unknown")
                    problem(ps, true, "permission", w(std::string("permission.") + k + ": yes, no, ask or unknown"));
                if (std::string(k) == "redistribute") m.redistribute = v;
            }
            m.permissionStatement = str(*perm, "statement");
        }
    }
    if (const JsonValue* br = member(root, "base_rom")) {
        if (!br->is(JsonValue::Type::Object)) {
            problem(ps, true, "base_rom", L"base_rom: must be an object");
        } else {
            m.baseSha1 = strings(*br, "sha1", ps, "base_rom", true, true);
            static const std::regex hex("^[0-9A-Fa-f]{40}$");
            for (auto& s : m.baseSha1) {
                if (!std::regex_match(s, hex)) problem(ps, true, "base_rom", w("base_rom.sha1 \"" + s + "\" is not a SHA-1"));
                s = lower(s);
            }
            if (member(*br, "sha1") && member(*br, "sha1")->is(JsonValue::Type::Array) && member(*br, "sha1")->items.empty())
                problem(ps, true, "base_rom", L"base_rom.sha1: needs at least 1");
            m.baseRomName = string_field(*br, "name", ps, "base_rom", false, false);
        }
    } else if (m.kind == "conversion") {
        problem(ps, true, "base_rom", L"a conversion needs \"base_rom\" (the ROM it is made for)");
    }
    if (const JsonValue* rq = member(root, "requires")) {
        if (!rq->is(JsonValue::Type::Object)) {
            problem(ps, true, "requires", L"requires: must be an object");
        } else {
            m.requiresLauncher = string_field(*rq, "launcher", ps, "requires", false, true);
            m.requiresRecomp = string_field(*rq, "minimum_recomp_version", ps, "requires", false, true);
            for (auto* v : {&m.requiresLauncher, &m.requiresRecomp})
                if (!v->empty() && !valid_version(*v)) problem(ps, true, "requires", w("requires: \"" + *v + "\" is not x.y.z"));
            m.requiredPackages = strings(*rq, "packages", ps, "requires", false, true);
        }
    }
    const JsonValue* pay = member(root, "payload");
    if (!pay) {
        problem(ps, true, "payload", L"no \"payload\"");
        return m;
    }
    if (!pay->is(JsonValue::Type::Object)) {
        problem(ps, true, "payload", L"payload: must be an object");
        return m;
    }
    m.nrm = string_field(*pay, "nrm", ps, "payload", false, true);
    m.rtz = string_field(*pay, "rtz", ps, "payload", false, true);
    m.folder = string_field(*pay, "folder", ps, "payload", false, true);
    m.packs = strings(*pay, "packs", ps, "payload", false, true);
    m.hd = strings(*pay, "hd", ps, "payload", false, true);
    m.addons = strings(*pay, "addons", ps, "payload", false, true);
    for (auto* p : {&m.nrm, &m.rtz, &m.folder})
        if (!p->empty() && !safe_path(*p)) problem(ps, true, "payload", w("payload \"" + *p + "\": use a relative path inside the package"));
    for (auto* list : {&m.packs, &m.hd, &m.addons})
        for (auto& p : *list)
            if (!safe_path(p)) problem(ps, true, "payload", w("payload \"" + p + "\": use a relative path inside the package"));
    // What each kind needs, and keys it doesn't use (a warning).
    auto unused = [&](std::initializer_list<const char*> keys) {
        for (const char* k : keys)
            if (member(*pay, k)) problem(ps, false, "payload", w(std::string("payload.") + k + " is not used by a " + m.kind + " (ignored)"));
    };
    if (m.kind == "addon") {
        if (!member(*pay, "nrm")) problem(ps, true, "payload", L"payload: an add-on needs \"nrm\"");
        unused({"rtz", "folder", "packs", "hd", "addons"});
    } else if (m.kind == "texture-pack") {
        if (member(*pay, "rtz") && member(*pay, "folder")) problem(ps, true, "payload", L"payload: a texture pack has \"rtz\" or \"folder\", not both");
        else if (!member(*pay, "rtz") && !member(*pay, "folder")) problem(ps, true, "payload", L"payload: a texture pack needs \"rtz\" or \"folder\"");
        unused({"nrm", "packs", "hd", "addons"});
    } else if (m.kind == "model-pack") {
        if (!member(*pay, "packs")) problem(ps, true, "payload", L"payload: a model pack needs \"packs\"");
        else if (member(*pay, "packs")->is(JsonValue::Type::Array) && member(*pay, "packs")->items.empty())
            problem(ps, true, "payload", L"payload.packs: needs at least 1");
        unused({"nrm", "rtz", "folder", "addons"});
    } else if (m.kind == "conversion") {
        if (!member(*pay, "folder")) problem(ps, true, "payload", L"payload: a conversion needs \"folder\"");
        unused({"nrm", "rtz", "packs", "hd"});
    }
    return m;
}

// --- a package's files ----------------------------------------------------------------------------------------------

static bool is_link(const fs::path& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}

Package::Package(const fs::path& p) : path_(p) {
    std::error_code ec;
    std::vector<std::pair<std::string, uint64_t>> found;
    std::map<std::string, std::string> seen;  // lower-case name -> first spelling
    auto add = [&](const std::string& rel, uint64_t size, const std::string& zipName) {
        std::string key = fold(rel);
        auto it = seen.find(key);
        if (it != seen.end()) {
            clashes.push_back(it->second == rel ? rel : it->second + " / " + rel);
            return;
        }
        seen[key] = rel;
        found.push_back({rel, size});
        zipNames_.push_back(zipName);
    };
    std::vector<std::string> zipOrder;
    if (fs::is_directory(p, ec)) {
        opened_ = true;
        for (auto it = fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            std::string rel = u8(fs::relative(it->path(), p, ec).generic_wstring());
            if (is_link(it->path())) {
                unsafe.push_back(rel + (it->is_directory(ec) ? "/" : ""));
                if (it->is_directory(ec)) it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            add(rel, it->file_size(ec), rel);
        }
    } else {
        FILE* f = _wfopen(p.c_str(), L"rb");
        if (f) {
            auto* za = new mz_zip_archive{};
            uint64_t size = fs::file_size(p, ec);
            if (!ec && mz_zip_reader_init_cfile(za, f, (mz_uint64)size, 0)) {
                zip_ = za;
                file_ = f;
                opened_ = true;
                int n = (int)mz_zip_reader_get_num_files(za);
                for (int i = 0; i < n; ++i) {
                    mz_zip_archive_file_stat st;
                    if (!mz_zip_reader_file_stat(za, i, &st) || mz_zip_reader_is_file_a_directory(za, i)) continue;
                    std::string raw = st.m_filename;
                    std::string name = zip_name_utf8(raw);
                    std::replace(name.begin(), name.end(), '\\', '/');  // Compress-Archive (PowerShell 5.1) writes '\'
                    if (!valid_utf8(name) || !safe_path(name)) {
                        unsafe.push_back(name);
                        continue;
                    }
                    add(name, st.m_uncomp_size, raw);
                }
            } else {
                delete za;
                fclose(f);
            }
        }
    }
    // Sorted by name, sizes and zip names alongside.
    std::vector<size_t> order(found.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return found[a].first < found[b].first; });
    std::vector<std::string> zn;
    for (size_t i : order) {
        names_.push_back(found[i].first);
        sizes_.push_back(found[i].second);
        zn.push_back(zipNames_[i]);
    }
    zipNames_ = zn;
}

Package::~Package() {
    if (zip_) {
        mz_zip_reader_end((mz_zip_archive*)zip_);
        delete (mz_zip_archive*)zip_;
    }
    if (file_) fclose((FILE*)file_);
}

int Package::index(const std::string& rel) const {
    auto it = std::lower_bound(names_.begin(), names_.end(), rel);
    return it != names_.end() && *it == rel ? (int)(it - names_.begin()) : -1;
}
bool Package::has(const std::string& rel) const { return index(rel) >= 0; }
uint64_t Package::size(const std::string& rel) const {
    int i = index(rel);
    return i < 0 ? 0 : sizes_[(size_t)i];
}
uint64_t Package::total() const {
    uint64_t t = 0;
    for (auto s : sizes_) t += s;
    return t;
}
std::vector<std::string> Package::under(const std::string& folder) const {
    std::string pre = strip_slash(folder) + "/";
    std::vector<std::string> out;
    for (auto& n : names_)
        if (n.compare(0, pre.size(), pre) == 0) out.push_back(n);
    return out;
}

bool Package::read(const std::string& rel, std::string* out, uint64_t limit) const {
    int i = index(rel);
    if (i < 0 || sizes_[(size_t)i] > limit) return false;
    if (zip_) {
        auto* za = (mz_zip_archive*)zip_;
        int zi = mz_zip_reader_locate_file(za, zipNames_[(size_t)i].c_str(), nullptr, 0);
        size_t sz = 0;
        void* p = zi < 0 ? nullptr : mz_zip_reader_extract_to_heap(za, (mz_uint)zi, &sz, 0);
        if (!p) return false;
        out->assign((const char*)p, sz);
        mz_free(p);
        return true;
    }
    std::ifstream f(path_ / fs::u8path(rel), std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return out->size() <= limit;
}

bool Package::extract(const std::string& rel, const fs::path& dest) const {
    int i = index(rel);
    if (i < 0) return false;
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    if (zip_) {
        auto* za = (mz_zip_archive*)zip_;
        int zi = mz_zip_reader_locate_file(za, zipNames_[(size_t)i].c_str(), nullptr, 0);
        FILE* f = zi < 0 ? nullptr : _wfopen(dest.c_str(), L"wb");
        if (!f) return false;
        bool ok = mz_zip_reader_extract_to_cfile(za, (mz_uint)zi, f, 0);
        ok = fclose(f) == 0 && ok;
        return ok;
    }
    return fs::copy_file(path_ / fs::u8path(rel), dest, fs::copy_options::overwrite_existing, ec) && !ec;
}

// --- checking a package --------------------------------------------------------------------------------------------

bool CheckResult::ok() const {
    return hasManifest && std::none_of(problems.begin(), problems.end(), [](const Problem& p) { return p.error; });
}
std::wstring CheckResult::first_error() const {
    std::wstring e = first_error_of(problems);
    return e.empty() && !hasManifest ? L"no aki-package.json: not a package" : e;
}

// An .nrm / .rtz inside the package: its names, and its mod.json (if any).
static bool inner_zip(const Package& pk, const std::string& rel, std::set<std::string>* names, std::string* modJson) {
    std::string data;
    if (!pk.read(rel, &data, kMaxInner)) return false;
    mz_zip_archive za{};
    if (!mz_zip_reader_init_mem(&za, data.data(), data.size(), 0)) return false;
    int n = (int)mz_zip_reader_get_num_files(&za);
    for (int i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st;
        if (mz_zip_reader_file_stat(&za, i, &st)) {
            std::string nm = st.m_filename;
            std::replace(nm.begin(), nm.end(), '\\', '/');
            names->insert(nm);
        }
    }
    if (names->count("mod.json")) {
        size_t sz = 0;
        void* p = mz_zip_reader_extract_file_to_heap(&za, "mod.json", &sz, 0);
        if (p) {
            if (sz <= kMaxJson) modJson->assign((const char*)p, sz);
            mz_free(p);
        }
    }
    mz_zip_reader_end(&za);
    return true;
}

static void check_inner(const Package& pk, const std::string& rel, const Manifest& m, bool nrm, std::vector<Problem>& ps) {
    std::set<std::string> names;
    std::string mj;
    if (!inner_zip(pk, rel, &names, &mj)) {
        problem(ps, true, "payload", w("payload " + rel + " is not a zip (or too big to open)"));
        return;
    }
    if (nrm) {
        if (!names.count("mod_binary.bin"))
            problem(ps, true, "payload", w("payload nrm " + rel + " has no mod_binary.bin: not a code mod" +
                                           (names.count("patch.bps") ? " (a patch-mod ROM hack is a conversion)" : "")));
        if (!names.count("mod.json")) {
            problem(ps, true, "payload", w("payload nrm " + rel + " has no mod.json"));
            return;
        }
    } else {
        if (!names.count("rt64.json")) problem(ps, true, "payload", w("payload rtz " + rel + " has no rt64.json at its root"));
        if (!names.count("mod.json")) return;
    }
    JsonValue inner;
    if (!json_parse(mj, &inner) || !inner.is(JsonValue::Type::Object)) {
        problem(ps, true, "payload", w(rel + ": its mod.json is not a JSON object"));
        return;
    }
    const std::pair<const char*, const std::string*> keys[] = {{"id", &m.id}, {"game_id", &m.gameId}, {"version", &m.version}};
    for (auto& [k, mine] : keys) {
        const JsonValue* v = inner.get(k);
        if (v && v->is(JsonValue::Type::String) && v->s != *mine)
            problem(ps, std::string(k) != "version", "payload",
                    w(rel + ": mod.json " + k + " is \"" + v->s + "\", the package says \"" + *mine + "\""));
    }
}

CheckResult check_package(const fs::path& src) {
    CheckResult r;
    auto& ps = r.problems;
    std::error_code ec;
    if (!fs::exists(src, ec)) {
        problem(ps, true, "", L"no such file or folder");
        return r;
    }
    Package pk(src);
    if (!pk.opened()) {
        problem(ps, true, "", L"not a zip or a folder");
        return r;
    }
    if (!pk.has(kManifest)) {
        problem(ps, true, "", L"no aki-package.json at the root: not a package");
        return r;
    }
    r.hasManifest = true;
    for (auto& u : pk.unsafe)
        problem(ps, true, "payload", pk.is_zip() ? w("unsafe path in the zip: " + u)
                                                 : w("a link or junction in the folder: " + u + " (never followed)"));
    for (auto& c : pk.clashes) problem(ps, true, "payload", w("entries that are the same file on Windows: " + c));
    for (auto& n : pk.files()) {
        std::string l = lower(n);
        if (ends_with(l, ".z64") || ends_with(l, ".v64") || ends_with(l, ".n64"))
            problem(ps, true, "payload", w(n + " is a ROM: a package never holds one"));
    }
    if (pk.total() > kMaxTotal) {
        problem(ps, true, "payload", L"the package holds more than 8 GB");
        return r;
    }
    std::string text;
    if (!pk.read(kManifest, &text, kMaxJson)) {
        problem(ps, true, "format", L"aki-package.json is unreadable or over 1 MB");
        return r;
    }
    r.manifest = parse_manifest(text);
    const Manifest& m = r.manifest;
    ps.insert(ps.end(), m.problems.begin(), m.problems.end());
    if (!m.parsed) return r;

    auto need_file = [&](const std::string& rel, const char* what) {
        if (rel.empty() || !safe_path(rel)) return false;
        if (!pk.has(rel)) {
            problem(ps, true, "payload", w(std::string("payload ") + what + " " + rel + " is not in the package"));
            return false;
        }
        return true;
    };
    auto need_folder = [&](const std::string& rel, const char* what) {
        if (rel.empty() || !safe_path(rel)) return std::vector<std::string>();
        auto files = pk.under(rel);
        if (files.empty()) problem(ps, true, "payload", w(std::string("payload ") + what + " " + rel + " is not in the package (or empty)"));
        return files;
    };
    auto contains = [](const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); };

    if (m.kind == "addon") {
        if (need_file(m.nrm, "nrm")) check_inner(pk, m.nrm, m, true, ps);
    } else if (m.kind == "texture-pack") {
        if (!m.rtz.empty() && m.folder.empty()) {
            if (need_file(m.rtz, "rtz")) check_inner(pk, m.rtz, m, false, ps);
        } else if (!m.folder.empty() && m.rtz.empty()) {
            auto files = need_folder(m.folder, "folder");
            if (!files.empty() && !contains(files, strip_slash(m.folder) + "/rt64.json"))
                problem(ps, true, "payload", w("payload folder " + m.folder + " has no rt64.json"));
        }
    } else if (m.kind == "model-pack") {
        for (auto& p : m.packs) {
            auto files = need_folder(p, "pack");
            if (files.empty()) continue;
            std::string base = strip_slash(p);
            if (!ends_with(lower(base), ".akimodel")) problem(ps, true, "payload", w("payload pack " + p + ": a model pack is an <id>.akimodel folder"));
            for (const char* need : {"model.bin", "pack.json"})
                if (!contains(files, base + "/" + need)) problem(ps, true, "payload", w("payload pack " + p + " has no " + need));
            std::string pj;
            JsonValue pv;
            if (pk.read(base + "/pack.json", &pj, kMaxJson) && json_parse(pj, &pv) && pv.is(JsonValue::Type::Object)) {
                std::string g = str(pv, "game");
                if (!g.empty() && runtime_game_id(g) != m.gameId)
                    problem(ps, true, "payload", w("payload pack " + p + " is built for " + g + ", the package says " + m.gameId));
            } else if (contains(files, base + "/pack.json")) {
                problem(ps, true, "payload", w("payload pack " + p + ": pack.json is not JSON"));
            }
        }
        for (auto& h : m.hd)
            if (need_file(h, "hd") && !ends_with(lower(h), ".rtz")) problem(ps, false, "payload", w("payload hd " + h + " is not an .rtz"));
    } else if (m.kind == "conversion") {
        auto files = need_folder(m.folder, "folder");
        std::string base = strip_slash(m.folder);
        if (!files.empty() && !contains(files, base + "/" + kConversionManifest))
            problem(ps, true, "payload", w("payload folder " + m.folder + " has no conversion manifest (aki.json)" +
                                           (contains(files, base + "/conversion.json") ? ": it has conversion.json, the game reads aki.json" : "")));
        // Its folder is conversions\<package id>\, and the game keys its saves on the conversion's id: the same one.
        std::string cj;
        JsonValue cv;
        if (pk.read(base + "/" + kConversionManifest, &cj, kMaxJson) && json_parse(cj, &cv) && cv.is(JsonValue::Type::Object)) {
            std::string cid = str(cv, "id");
            if (!cid.empty() && cid != m.id)
                problem(ps, true, "payload", w("the conversion's aki.json id is \"" + cid + "\", the package says \"" + m.id + "\""));
            // Its patch (the game's rules, #154): wrapped in a patch .nrm, never a bare .bps; that .nrm's own mod id
            // is the conversion id (the hack's saves are in saves\<mod id>\), and components.patch.mod_id says it too.
            const JsonValue* comp = member(cv, "components");
            const JsonValue* patch = comp ? member(*comp, "patch") : nullptr;
            if (patch && patch->is(JsonValue::Type::Object)) {
                std::string pf = str(*patch, "file"), mid = str(*patch, "mod_id");
                std::string rel = base + "/" + pf;
                if (ends_with(lower(pf), ".bps")) {
                    problem(ps, true, "payload", w("components.patch.file " + pf + " is a bare .bps: the game takes a patch "
                                                   "wrapped in a patch .nrm (aki-package package --from <hack>.nrm)"));
                } else if (pf.empty() || !safe_path(pf) || !pk.has(rel)) {
                    problem(ps, true, "payload", w("components.patch.file \"" + pf + "\" is not in the conversion folder"));
                } else {
                    std::set<std::string> names;
                    std::string mj;
                    JsonValue mv;
                    std::string modId;
                    if (inner_zip(pk, rel, &names, &mj) && json_parse(mj, &mv) && mv.is(JsonValue::Type::Object)) modId = str(mv, "id");
                    if (modId.empty())
                        problem(ps, true, "payload", w("the patch " + pf + " has no mod.json id: not a patch .nrm"));
                    else if (modId != m.id)
                        problem(ps, true, "payload", w("the patch " + pf + " is mod " + modId + ", the conversion is " + m.id +
                                                       ": they must be the same id (its saves are in saves\\" + modId + "\\)"));
                    if (!mid.empty() && mid != modId)
                        problem(ps, true, "payload", w("components.patch.mod_id is \"" + mid + "\", the patch is mod \"" + modId + "\""));
                }
            }
        }
        for (auto& a : m.addons)
            if (need_file(a, "addon") && !ends_with(lower(a), ".nrm") && !ends_with(lower(a), ".rtz"))
                problem(ps, false, "payload", w("payload addon " + a + " is neither .nrm nor .rtz"));
    }
    // The thumbnail, the licence, permission.
    std::string thumb = !m.thumb.empty() ? m.thumb : (pk.has("thumb.png") ? "thumb.png" : "");
    if (!thumb.empty()) {
        std::string head;
        if (!pk.has(thumb)) problem(ps, true, "thumb", w("thumb " + thumb + " is not in the package"));
        else if (!pk.read(thumb, &head, 64ull << 20) || head.size() < 8 || memcmp(head.data(), kPng, 8) != 0)
            problem(ps, true, "thumb", w("thumb " + thumb + " is not a PNG"));
    } else {
        problem(ps, false, "thumb", L"no thumb: the Mods page shows none");
    }
    if (m.license.empty()) problem(ps, false, "license", L"no \"license\": shown as unknown");
    if (m.redistribute == "no") problem(ps, false, "permission", L"for your own use: its authors don't allow sharing it");
    return r;
}

// --- pack_hash ----------------------------------------------------------------------------------------------------------

// STAND-IN until librecomp's bundled_packs pack_hash is vendored (aki-roadmap#149/#154): FNV-1a 64; a text file
// (by extension) with CRLF read as LF; a folder hashes its files' relative paths (lower case, '/') and hashes in order.
static const uint64_t kFnvOffset = 1469598103934665603ull, kFnvPrime = 1099511628211ull;

static bool is_text_name(const std::string& n) {
    std::string l = lower(n);
    for (const char* e : {".json", ".txt", ".md", ".ini", ".cfg", ".csv", ".xml", ".toml"})
        if (ends_with(l, e)) return true;
    return false;
}

static bool hash_file(const fs::path& p, uint64_t* h) {
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return false;
    bool text = is_text_name(u8(p.filename().wstring()));
    std::vector<unsigned char> buf(1 << 20);
    bool pendingCr = false;
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), f)) > 0) {
        for (size_t i = 0; i < n; ++i) {
            unsigned char c = buf[i];
            if (text) {
                if (pendingCr) {
                    pendingCr = false;
                    if (c != '\n') *h = (*h ^ '\r') * kFnvPrime;
                }
                if (c == '\r') {
                    pendingCr = true;
                    continue;
                }
            }
            *h = (*h ^ c) * kFnvPrime;
        }
    }
    if (pendingCr) *h = (*h ^ '\r') * kFnvPrime;
    bool ok = !ferror(f);
    fclose(f);
    return ok;
}

std::string pack_hash(const fs::path& p) {
    std::error_code ec;
    uint64_t h = kFnvOffset;
    if (fs::is_directory(p, ec)) {
        std::vector<std::pair<std::string, fs::path>> files;
        for (auto it = fs::recursive_directory_iterator(p, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            if (it->is_regular_file(ec)) files.push_back({lower(u8(fs::relative(it->path(), p, ec).generic_wstring())), it->path()});
        if (ec) return {};
        std::sort(files.begin(), files.end());
        for (auto& [rel, full] : files) {
            uint64_t fh = kFnvOffset;
            if (!hash_file(full, &fh)) return {};
            char line[32];
            snprintf(line, sizeof line, "%016llx\n", (unsigned long long)fh);
            for (char c : rel + '\0' + line) h = (h ^ (unsigned char)c) * kFnvPrime;
        }
    } else if (!hash_file(p, &h)) {
        return {};
    }
    char out[17];
    snprintf(out, sizeof out, "%016llx", (unsigned long long)h);
    return out;
}

// --- the registry -------------------------------------------------------------------------------------------------------

static bool known_kind(const std::string& k) {
    return k == "addon" || k == "texture-pack" || k == "model-pack" || k == "conversion";
}

static std::string path_key(const std::string& p) { return fold(strip_slash(p)); }

const RegEntry* Registry::find(const std::string& id) const {
    for (auto& e : entries)
        if (e.id == id) return &e;
    return nullptr;
}
const RegEntry* Registry::owner(const std::string& path) const {
    std::string k = path_key(path);
    for (auto& e : entries)
        for (auto& f : e.files)
            if (path_key(f.path) == k) return &e;
    return nullptr;
}

Registry load_registry(const fs::path& file) {
    Registry r;
    std::error_code ec;
    if (!fs::exists(file, ec)) return r;
    std::ifstream f(file, std::ios::binary);
    if (!f) {
        r.status = Registry::Status::Unreadable;
        return r;
    }
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (!json_parse(text, &r.raw) || !r.raw.is(JsonValue::Type::Object)) {
        r.status = Registry::Status::Corrupt;
        return r;
    }
    const JsonValue* fmt = r.raw.get("format");
    if (!fmt || !fmt->is(JsonValue::Type::Number)) {
        r.status = Registry::Status::Corrupt;
        return r;
    }
    if (fmt->n != 1) {
        r.status = fmt->n > 1 ? Registry::Status::Newer : Registry::Status::Corrupt;
        return r;
    }
    const JsonValue* list = r.raw.get("packages");
    if (list && !list->is(JsonValue::Type::Array)) {
        r.status = Registry::Status::Corrupt;
        return r;
    }
    r.status = Registry::Status::Ok;
    if (!list) return r;
    for (auto& item : list->items) {
        if (!item.is(JsonValue::Type::Object) || !known_kind(str(item, "kind")) || str(item, "id").empty()) {
            r.others.push_back(item);  // not ours to understand: kept as it is
            continue;
        }
        RegEntry e;
        e.id = str(item, "id");
        e.kind = str(item, "kind");
        e.version = str(item, "version");
        e.source = str(item, "source");
        e.installed = str(item, "installed");
        if (const JsonValue* files = item.get("files"); files && files->is(JsonValue::Type::Array))
            for (auto& fv : files->items)
                if (fv.is(JsonValue::Type::Object) && !str(fv, "path").empty()) {
                    // A path outside the data folder (absolute, '..'): not a file of ours, and never one to remove.
                    if (!safe_path(str(fv, "path"))) {
                        r.status = Registry::Status::Corrupt;
                        r.entries.clear();
                        r.others.clear();
                        return r;
                    }
                    e.files.push_back({str(fv, "path"), str(fv, "hash")});
                }
        e.raw = item;
        r.entries.push_back(std::move(e));
    }
    return r;
}

static void set_member(JsonValue& o, const char* key, const JsonValue& v) {
    for (size_t i = 0; i < o.keys.size(); ++i)
        if (o.keys[i] == key) {
            o.items[i] = v;
            return;
        }
    o.keys.push_back(key);
    o.items.push_back(v);
}

static JsonValue entry_json(const RegEntry& e) {
    JsonValue o = e.raw.is(JsonValue::Type::Object) ? e.raw : JsonValue{};
    o.type = JsonValue::Type::Object;
    set_member(o, "id", json_string(e.id));
    set_member(o, "kind", json_string(e.kind));
    set_member(o, "version", json_string(e.version));
    set_member(o, "source", json_string(e.source));
    set_member(o, "installed", json_string(e.installed));
    // Files: known fields replaced, per-file fields this launcher doesn't know kept for paths it still lists.
    JsonValue files;
    files.type = JsonValue::Type::Array;
    const JsonValue* old = e.raw.is(JsonValue::Type::Object) ? e.raw.get("files") : nullptr;
    for (auto& f : e.files) {
        JsonValue fo;
        fo.type = JsonValue::Type::Object;
        if (old && old->is(JsonValue::Type::Array))
            for (auto& ov : old->items)
                if (ov.is(JsonValue::Type::Object) && path_key(str(ov, "path")) == path_key(f.path)) fo = ov;
        set_member(fo, "path", json_string(f.path));
        set_member(fo, "hash", json_string(f.hash));
        files.items.push_back(fo);
    }
    set_member(o, "files", files);
    return o;
}

bool save_registry(const fs::path& file, const Registry& reg, std::wstring* err) {
    if (!reg.may_save()) {
        if (err) *err = L"packages.json is " + std::wstring(reg.status == Registry::Status::Newer ? L"from a newer version" : L"unreadable") +
                        L": it was not changed";
        return false;
    }
    JsonValue root = reg.raw.is(JsonValue::Type::Object) ? reg.raw : JsonValue{};
    root.type = JsonValue::Type::Object;
    set_member(root, "format", json_number(1));
    // The file's own order: an entry nobody changed goes back exactly as it was read, a changed one rebuilt in its
    // place, a removed one left out; entries of unknown kinds stay where they were; new entries go last.
    JsonValue list;
    list.type = JsonValue::Type::Array;
    std::set<std::string> written;
    const JsonValue* before = reg.raw.is(JsonValue::Type::Object) ? reg.raw.get("packages") : nullptr;
    if (before && before->is(JsonValue::Type::Array)) {
        for (auto& item : before->items) {
            bool ours = item.is(JsonValue::Type::Object) && known_kind(str(item, "kind")) && !str(item, "id").empty();
            if (!ours) {
                list.items.push_back(item);
                continue;
            }
            const RegEntry* e = reg.find(str(item, "id"));
            if (!e || written.count(e->id)) continue;  // removed (or a duplicate id: the first one stands)
            list.items.push_back(e->changed ? entry_json(*e) : item);
            written.insert(e->id);
        }
    } else {
        for (auto& o : reg.others) list.items.push_back(o);
    }
    for (auto& e : reg.entries)
        if (!written.count(e.id)) list.items.push_back(e.changed || !e.raw.is(JsonValue::Type::Object) ? entry_json(e) : e.raw);
    set_member(root, "packages", list);
    std::string text = json_write(root) + "\n";
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    fs::path tmp = file;
    tmp += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = L"can't write " + tmp.wstring();
        return false;
    }
    DWORD wrote = 0;
    bool ok = WriteFile(h, text.data(), (DWORD)text.size(), &wrote, nullptr) && wrote == text.size() && FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        if (err) *err = L"can't write " + file.wstring();
        return false;
    }
    return true;
}

// --- install ------------------------------------------------------------------------------------------------------------

static std::string today_yyyymmdd() {
    time_t t = time(nullptr);
    tm lt{};
    localtime_s(&lt, &t);
    char b[16];
    strftime(b, sizeof b, "%Y%m%d", &lt);
    return b;
}
static std::string now_utc() {
    time_t t = time(nullptr);
    tm g{};
    gmtime_s(&g, &t);
    char b[32];
    strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &g);
    return b;
}

// <path>.edited-YYYYMMDD, or -2, -3 ... when that exists too.
static fs::path edited_name(const fs::path& p, const std::string& day) {
    std::error_code ec;
    for (int i = 1;; ++i) {
        fs::path c = p;
        c += L".edited-" + w(day) + (i > 1 ? L"-" + std::to_wstring(i) : L"");
        if (!fs::exists(c, ec)) return c;
    }
}

static bool move_path(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
}

// A recorded file going away: unedited (its hash is the recorded one) to the Recycle Bin, edited kept as .edited-<date>.
static bool retire(const fs::path& p, const std::string& recordedHash, const std::string& day, PackageInstall* r) {
    std::error_code ec;
    if (!fs::exists(p, ec)) return true;
    if (!recordedHash.empty() && pack_hash(p) == recordedHash) return recycle_path(p, nullptr);
    fs::path kept = edited_name(p, day);
    if (!move_path(p, kept)) return false;
    if (r) {
        r->keptEdited.push_back(kept);
        r->notes.push_back(L"you had changed " + p.filename().wstring() + L": kept as " + kept.filename().wstring());
    }
    return true;
}

static bool write_zip_from(const Package& pk, const std::vector<std::string>& files, const std::string& prefix, const fs::path& dest) {
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    FILE* f = _wfopen(dest.c_str(), L"wb");
    if (!f) return false;
    mz_zip_archive za{};
    bool ok = mz_zip_writer_init_cfile(&za, f, 0);
    for (auto& rel : files) {
        if (!ok) break;
        std::string data;
        ok = pk.read(rel, &data, kMaxInner) &&
             mz_zip_writer_add_mem(&za, rel.substr(prefix.size()).c_str(), data.data(), data.size(), MZ_DEFAULT_COMPRESSION);
    }
    ok = ok && mz_zip_writer_finalize_archive(&za);
    mz_zip_writer_end(&za);
    ok = fclose(f) == 0 && ok;
    return ok;
}

// "add-on", "texture pack", ...; with an article when `a`.
static std::wstring kind_words(const std::string& kind, bool a = false) {
    std::wstring k = kind == "addon" ? L"add-on" : kind == "texture-pack" ? L"texture pack" : kind == "model-pack" ? L"model pack" : w(kind);
    return a ? (k[0] == L'a' ? L"an " : L"a ") + k : k;
}

static std::wstring join_authors(const std::vector<std::string>& a) {
    std::wstring s;
    for (auto& x : a) s += (s.empty() ? L"" : L", ") + w(x);
    return s;
}

static PackageInstall refuse(PackageInstall r, const std::wstring& why, PackageInstall::Outcome o = PackageInstall::Outcome::Refused) {
    r.outcome = o;
    r.message = why;
    return r;
}

PackageInstall install_package(const fs::path& src, const InstallTarget& t, const InstallOptions& o) {
    PackageInstall r;
    CheckResult chk = check_package(src);
    const Manifest& m = chk.manifest;
    r.id = m.id;
    r.kind = m.kind;
    r.version = m.version;
    r.name = w(m.displayName.empty() ? m.id : m.displayName);
    if (!chk.ok()) return refuse(r, L"Not installed: " + chk.first_error());
    for (auto& p : chk.problems)
        if (!p.error && p.field != "thumb" && p.field != "license") r.notes.push_back(p.text);

    // This game, this launcher, this ROM.
    if (m.gameId != t.gameId)
        return refuse(r, L"Not installed: " + r.name + L" is for " + w(m.gameId) + L", not " + t.gameTitle);
    if (!m.requiresLauncher.empty()) {
        Version cur, need;
        parse_version(w(m.requiresLauncher), &need);
        if (current_launcher_version(&cur) && ::compare_versions(cur, need) < 0)
            return refuse(r, L"Not installed: " + r.name + L" needs launcher " + w(m.requiresLauncher) + L" or newer");
    }
    if (!m.baseSha1.empty()) {
        std::vector<std::string> rom;
        for (auto& s : t.romSha1)
            if (!s.empty()) rom.push_back(lower(s));
        if (rom.empty() && t.hashRom) {
            std::string h = lower(t.hashRom());
            if (!h.empty()) rom.push_back(h);
        }
        bool match = std::any_of(rom.begin(), rom.end(), [&](const std::string& s) {
            return std::find(m.baseSha1.begin(), m.baseSha1.end(), s) != m.baseSha1.end();
        });
        std::wstring needs = m.baseRomName.empty() ? w(m.baseSha1[0]) : w(m.baseRomName);
        if (!match && m.kind == "conversion")
            return refuse(r, L"Not installed: " + r.name + L" needs " + needs + (rom.empty() ? L" (your ROM couldn't be read)" : L", and your ROM is another one"));
        if (!match) r.notes.push_back(L"made for " + needs + L": it may not work with your ROM");
    }
    if (o.gameRunning) return refuse(r, L"Not installed: close " + t.gameTitle + L" first (it reads these files)");

    fs::path regFile = t.dataDir / L"packages.json";
    Registry reg = load_registry(regFile);
    if (!reg.may_save()) {
        std::wstring why;
        save_registry(regFile, reg, &why);
        return refuse(r, L"Not installed: " + why);
    }
    const RegEntry* old = reg.find(m.id);
    if (old && old->kind != m.kind)
        return refuse(r, L"Not installed: the id " + w(m.id) + L" is already taken by " + kind_words(old->kind, true) +
                             L" (one id per package in a game)");
    PackageInstall::Outcome outcome = PackageInstall::Outcome::Installed;
    if (old) {
        r.oldVersion = old->version;
        int c = valid_version(old->version) ? compare_versions(m.version, old->version) : 1;
        if (c == 0 && !o.reinstall)
            return refuse(r, r.name + L" " + w(m.version) + L" is already installed", PackageInstall::Outcome::AlreadyInstalled);
        if (c < 0 && !o.allowDowngrade)
            return refuse(r, r.name + L" " + w(m.version) + L" is older than the installed " + w(old->version),
                          PackageInstall::Outcome::NeedsDowngradeConfirm);
        outcome = c == 0 ? PackageInstall::Outcome::Reinstalled : PackageInstall::Outcome::Upgraded;
    }

    // Where each part goes: (path in the package or a folder of it, destination relative to the data folder).
    struct Part { std::string from; bool folder; bool zipFolder; std::string to; };
    std::vector<Part> parts;
    if (m.kind == "addon") parts.push_back({m.nrm, false, false, "mods/" + base_name(m.nrm)});
    else if (m.kind == "texture-pack" && !m.rtz.empty()) parts.push_back({m.rtz, false, false, "mods/" + base_name(m.rtz)});
    else if (m.kind == "texture-pack") parts.push_back({m.folder, true, true, "mods/" + m.id + ".rtz"});
    else if (m.kind == "model-pack") {
        for (auto& p : m.packs) parts.push_back({p, true, false, "mods/models/" + base_name(p)});
        for (auto& h : m.hd) parts.push_back({h, false, false, "mods/" + base_name(h)});
    } else if (m.kind == "conversion") {
        parts.push_back({m.folder, true, false, "conversions/" + m.id});
        for (auto& a : m.addons) parts.push_back({a, false, false, "mods/" + base_name(a)});
    }
    std::set<std::string> dests;
    for (auto& p : parts) {
        if (!dests.insert(path_key(p.to)).second) return refuse(r, L"Not installed: two parts would both be " + w(p.to));
        const RegEntry* own = reg.owner(p.to);
        bool replaced = own && (own->id == m.id || std::find(m.replaces.begin(), m.replaces.end(), own->id) != m.replaces.end());
        if (own && !replaced) return refuse(r, L"Not installed: " + w(p.to) + L" belongs to " + w(own->id));
    }

    // Stage every part in the data folder (the same volume, so the moves below are renames).
    std::error_code ec;
    fs::path staging = t.dataDir / (L".aki-staging-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    struct StageGuard { fs::path p; ~StageGuard() { std::error_code e; fs::remove_all(p, e); } } guard{staging};
    {
        Package pk(src);
        for (auto& p : parts) {
            fs::path dst = staging / fs::u8path(p.to);
            bool ok;
            if (p.zipFolder) {
                std::string pre = strip_slash(p.from) + "/";
                ok = write_zip_from(pk, pk.under(p.from), pre, dst);
            } else if (p.folder) {
                std::string pre = strip_slash(p.from) + "/";
                ok = true;
                for (auto& f : pk.under(p.from)) ok = ok && pk.extract(f, dst / fs::u8path(f.substr(pre.size())));
            } else {
                ok = pk.extract(p.from, dst);
            }
            if (!ok) return refuse(r, L"Not installed: couldn't unpack " + w(p.from) + L" (disk full?)", PackageInstall::Outcome::Failed);
        }
    }
    std::string day = o.today.empty() ? today_yyyymmdd() : o.today;

    // What goes: the old version's files (and those of the packages it replaces), and whatever is where a new part
    // lands. Each is first moved aside into the staging folder (a rename), the new parts are moved in, and the registry
    // is saved; a failure at any step moves everything back, so the data folder is as it was. Only after the save do
    // the old ones go: unedited (its recorded hash, or the same as the new copy) to the Recycle Bin, edited ones
    // beside their place as <name>.edited-YYYYMMDD. A file no package recorded (put there by hand) counts as edited
    // unless it is the same as the new one.
    std::vector<const RegEntry*> going;
    if (old) going.push_back(old);
    for (auto& id : m.replaces)
        if (const RegEntry* e = reg.find(id); e && e->id != m.id) going.push_back(e);
    std::map<std::string, RegFile> recorded;  // path key -> the recorded path and hash
    for (auto* e : going)
        for (auto& f : e->files) recorded[path_key(f.path)] = f;
    struct Aside { fs::path original, backup; bool unedited; };
    std::vector<Aside> aside;
    std::vector<fs::path> placed;
    fs::path backups = staging / L".aside";
    auto rollback = [&] {
        for (auto it = placed.rbegin(); it != placed.rend(); ++it) {
            std::error_code e;
            fs::remove_all(*it, e);
        }
        for (auto it = aside.rbegin(); it != aside.rend(); ++it) move_path(it->backup, it->original);
    };
    auto put_aside = [&](const fs::path& p, bool unedited) {
        Aside a{p, backups / std::to_wstring(aside.size()), unedited};
        if (!move_path(p, a.backup)) return false;
        aside.push_back(a);
        return true;
    };
    auto fail = [&](const std::wstring& why) {
        rollback();
        return refuse(r, L"Not installed: " + why + L" (nothing was changed)", PackageInstall::Outcome::Failed);
    };
    for (auto& [key, f] : recorded) {
        fs::path p = t.dataDir / fs::u8path(strip_slash(f.path));
        if (dests.count(key) || !fs::exists(p, ec)) continue;  // a part lands there: handled below
        if (!put_aside(p, !f.hash.empty() && pack_hash(p) == f.hash)) return fail(L"couldn't move " + p.wstring() + L" (is it open?)");
    }
    RegEntry entry;
    entry.changed = true;
    entry.raw = old ? old->raw : JsonValue{};
    entry.id = m.id;
    entry.kind = m.kind;
    entry.version = m.version;
    entry.source = o.source;
    entry.installed = o.now.empty() ? now_utc() : o.now;
    for (auto& p : parts) {
        fs::path dst = t.dataDir / fs::u8path(p.to);
        fs::path staged = staging / fs::u8path(p.to);
        std::string newHash = pack_hash(staged);
        if (fs::exists(dst, ec)) {
            auto it = recorded.find(path_key(p.to));
            std::string cur = pack_hash(dst);
            bool unedited = cur == newHash || (it != recorded.end() && !it->second.hash.empty() && cur == it->second.hash);
            if (!put_aside(dst, unedited)) return fail(L"couldn't move " + dst.wstring() + L" (is it open?)");
        }
        if (!move_path(staged, dst)) return fail(L"couldn't put " + w(p.to) + L" in place");
        placed.push_back(dst);
        entry.files.push_back({p.to, newHash});
    }
    // The registry: this entry in place of the old one, the replaced ones gone.
    std::vector<RegEntry> kept;
    for (auto& e : reg.entries) {
        bool gone = e.id == m.id || std::find(m.replaces.begin(), m.replaces.end(), e.id) != m.replaces.end();
        if (!gone) kept.push_back(e);
    }
    kept.push_back(entry);
    reg.entries = kept;
    std::wstring err;
    if (!save_registry(regFile, reg, &err)) return fail(err);
    // Done: what was put aside goes (never deleted: an edited one, or one the Recycle Bin won't take, is kept).
    for (auto& a : aside) {
        if (a.unedited && recycle_path(a.backup, nullptr)) continue;
        fs::path kept2 = edited_name(a.original, day);
        if (move_path(a.backup, kept2)) {
            r.keptEdited.push_back(kept2);
            r.notes.push_back(L"you had changed " + a.original.filename().wstring() + L": kept as " + kept2.filename().wstring());
        } else {
            guard.p.clear();  // the staging folder now holds it: left in place
            r.notes.push_back(L"your copy of " + a.original.filename().wstring() + L" is in " + a.backup.wstring());
        }
    }

    r.outcome = outcome;
    std::wstring what = kind_words(m.kind);
    std::wstring by = L"by " + join_authors(m.authors) + (m.license.empty() ? L"" : L"; " + w(m.license));
    if (outcome == PackageInstall::Outcome::Upgraded)
        r.message = L"Updated " + r.name + L" " + w(r.oldVersion) + L" -> " + w(m.version);
    else
        r.message = std::wstring(outcome == PackageInstall::Outcome::Reinstalled ? L"Reinstalled " : L"Installed ") + r.name + L" " +
                    w(m.version) + L" (" + what + L", " + by + L")";
    if (!r.keptEdited.empty())
        r.message += L" (" + std::to_wstring(r.keptEdited.size()) + L" edited file" + (r.keptEdited.size() == 1 ? L"" : L"s") +
                     L" kept as .edited-" + w(day) + L")";
    if (m.kind == "conversion") r.message += L". Play it from Play as... on the home page.";
    return r;
}

// --- patch-mod ROM hacks ---------------------------------------------------------------------------------------------

static bool nrm_names(const fs::path& nrm, std::set<std::string>* names, std::string* modJson) {
    Package pk(nrm);
    if (!pk.opened() || !pk.is_zip()) return false;
    for (auto& n : pk.files()) names->insert(n);
    if (pk.has("mod.json")) pk.read("mod.json", modJson, kMaxJson);
    return true;
}

bool is_patch_mod(const fs::path& nrm) {
    std::set<std::string> names;
    std::string mj;
    return nrm_names(nrm, &names, &mj) && names.count("patch.bps") && !names.count("mod_binary.bin") && names.count("mod.json");
}

PackageInstall install_patch_mod(const fs::path& nrm, const InstallTarget& t, const InstallOptions& o) {
    PackageInstall r;
    std::set<std::string> names;
    std::string mj;
    JsonValue v;
    if (!nrm_names(nrm, &names, &mj) || !json_parse(mj, &v) || !v.is(JsonValue::Type::Object))
        return refuse(r, L"Not installed: " + nrm.filename().wstring() + L" has no readable mod.json");
    std::string id = str(v, "id"), version = str(v, "version"), name = str(v, "display_name");
    r.id = id;
    r.kind = "conversion";
    r.name = w(name.empty() ? id : name);
    if (!valid_id(id)) return refuse(r, L"Not installed: its mod id \"" + w(id) + L"\" can't be a conversion id");
    if (!valid_version(version)) version = "1.0.0";
    std::vector<std::string> authors;
    if (const JsonValue* a = v.get("authors"); a && a->is(JsonValue::Type::Array))
        for (auto& x : a->items)
            if (x.is(JsonValue::Type::String) && !x.s.empty()) authors.push_back(x.s);
    if (authors.empty()) authors.push_back("unknown");
    std::string gid = str(v, "game_id");
    if (!gid.empty() && gid != t.gameId) return refuse(r, L"Not installed: " + r.name + L" is for " + w(gid) + L", not " + t.gameTitle);
    if (o.gameRunning) return refuse(r, L"Not installed: close " + t.gameTitle + L" first (it reads these files)");
    // Its base ROM is the player's own: the hack was applied to this game's ROM until now.
    std::string rom;
    for (auto& s : t.romSha1)
        if (!s.empty()) rom = lower(s);
    if (rom.empty() && t.hashRom) rom = lower(t.hashRom());
    if (rom.empty()) return refuse(r, L"Not installed: your ROM for " + t.gameTitle + L" couldn't be read");

    // A package folder in the data folder, then the normal install.
    std::error_code ec;
    fs::path stage = t.dataDir / (L".aki-patchmod-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    struct Guard { fs::path p; ~Guard() { std::error_code e; fs::remove_all(p, e); } } guard{stage};
    fs::create_directories(stage / L"conversion", ec);
    if (!fs::copy_file(nrm, stage / L"conversion" / L"patch.nrm", ec))
        return refuse(r, L"Not installed: couldn't copy " + nrm.filename().wstring(), PackageInstall::Outcome::Failed);
    auto arr = [](const std::vector<std::string>& xs) {
        JsonValue a;
        a.type = JsonValue::Type::Array;
        for (auto& x : xs) a.items.push_back(json_string(x));
        return a;
    };
    auto obj = [](std::initializer_list<std::pair<const char*, JsonValue>> kv) {
        JsonValue o2;
        o2.type = JsonValue::Type::Object;
        for (auto& [k, val] : kv) {
            o2.keys.push_back(k);
            o2.items.push_back(val);
        }
        return o2;
    };
    std::string shortKey;
    for (auto& g : kGames)
        if (t.gameId == g.second) shortKey = g.first;
    JsonValue conv = obj({{"format", json_string("aki-conversion")}, {"format_version", json_number(1)}, {"id", json_string(id)},
                          {"name", json_string(name.empty() ? id : name)}, {"version", json_string(version)},
                          {"game", json_string(shortKey)}, {"authors", arr(authors)},
                          {"base_rom", obj({{"sha1", arr({rom})}})},
                          {"components", obj({{"patch", obj({{"file", json_string("patch.nrm")}, {"mod_id", json_string(id)}})}})}});
    JsonValue man = obj({{"format", json_number(1)}, {"kind", json_string("conversion")}, {"id", json_string(id)},
                         {"version", json_string(version)}, {"game_id", json_string(t.gameId)},
                         {"display_name", json_string(name.empty() ? id : name)}, {"authors", arr(authors)},
                         {"base_rom", obj({{"sha1", arr({rom})}})}, {"payload", obj({{"folder", json_string("conversion/")}})}});
    std::ofstream(stage / L"conversion" / L"aki.json", std::ios::binary) << json_write(conv) << "\n";
    std::ofstream(stage / L"aki-package.json", std::ios::binary) << json_write(man) << "\n";
    // Every .nrm in mods\ with this mod id (the hack itself on a Move to Conversions, or an older copy) leaves it: the
    // game refuses a conversion whose mod id is also in mods\. Put aside first, back on a failed install; afterwards
    // to the Recycle Bin (its content is in conversions\<id>\ now).
    std::vector<std::pair<fs::path, fs::path>> aside;  // (in mods\, put aside)
    fs::path dups = t.dataDir / (L".aki-dups-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    struct DupGuard { fs::path p; ~DupGuard() { std::error_code e; fs::remove(p, e); } } dupGuard{dups};  // only when empty
    auto put_back = [&] {
        for (auto it = aside.rbegin(); it != aside.rend(); ++it) move_path(it->second, it->first);
    };
    for (auto it = fs::directory_iterator(t.dataDir / L"mods", ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec) || _wcsicmp(it->path().extension().c_str(), L".nrm") != 0) continue;
        std::set<std::string> n2;
        std::string mj2;
        JsonValue v2;
        if (!nrm_names(it->path(), &n2, &mj2) || !json_parse(mj2, &v2) || str(v2, "id") != id) continue;
        aside.push_back({it->path(), dups / std::to_wstring(aside.size())});
    }
    for (size_t i = 0; i < aside.size(); ++i) {
        if (!move_path(aside[i].first, aside[i].second)) {
            fs::path failed = aside[i].first;
            aside.resize(i);
            put_back();
            return refuse(r, L"Not installed: couldn't move " + failed.filename().wstring() + L" out of mods\\ (is it open?)",
                          PackageInstall::Outcome::Failed);
        }
    }
    PackageInstall res = install_package(stage, t, o);
    if (!res.ok()) {
        put_back();
        return res;
    }
    res.message += L" (a ROM hack: now a conversion, one at a time, with its saves in saves\\" + w(id) + L"\\)";
    for (auto& [inMods, held] : aside) {
        if (recycle_path(held, nullptr)) {
            res.notes.push_back(inMods.filename().wstring() + L" moved out of mods\\");
        } else {
            fs::path kept = edited_name(inMods, o.today.empty() ? today_yyyymmdd() : o.today);
            move_path(held, kept);
            res.notes.push_back(inMods.filename().wstring() + L" moved out of mods\\ as " + kept.filename().wstring());
        }
    }
    return res;
}

// --- remove -------------------------------------------------------------------------------------------------------------

bool remove_package(const std::string& id, const fs::path& dataDir, bool gameRunning, std::wstring* msg) {
    if (gameRunning) {
        if (msg) *msg = L"close the game first";
        return false;
    }
    fs::path regFile = dataDir / L"packages.json";
    Registry reg = load_registry(regFile);
    if (!reg.may_save()) return save_registry(regFile, reg, msg);
    const RegEntry* e = reg.find(id);
    if (!e) {
        if (msg) *msg = L"not installed as a package";
        return false;
    }
    // Unedited files to the Recycle Bin; one changed since (by the player, or the game) kept as .edited-<date>.
    std::error_code ec;
    PackageInstall kept;
    std::string day = today_yyyymmdd();
    for (auto& f : e->files) {
        fs::path p = dataDir / fs::u8path(strip_slash(f.path));
        if (fs::exists(p, ec) && !retire(p, f.hash, day, &kept)) {
            if (msg) *msg = L"couldn't move " + p.wstring() + L" (is it open?)";
            return false;
        }
    }
    std::wstring name = w(e->id);
    bool conversion = e->kind == "conversion";
    reg.entries.erase(reg.entries.begin() + (e - reg.entries.data()));
    if (!save_registry(regFile, reg, msg)) return false;
    if (msg) {
        *msg = L"Removed " + name;
        if (conversion && fs::exists(dataDir / L"saves" / w(id), ec)) *msg += L" (its saves stay in saves\\" + w(id) + L"\\)";
        for (auto& n : kept.notes) *msg += L" (" + n + L")";
    }
    return true;
}

// --- conversions ----------------------------------------------------------------------------------------------------------

std::vector<ConversionInfo> list_conversions(const fs::path& dataDir) {
    std::vector<ConversionInfo> out;
    Registry reg = load_registry(dataDir / L"packages.json");
    std::error_code ec;
    for (auto it = fs::directory_iterator(dataDir / L"conversions", ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        std::string dir = u8(it->path().filename().wstring());
        if (dir.empty() || dir[0] == '_' || dir[0] == '.' || dir.find(".edited-") != std::string::npos) continue;
        std::ifstream f(it->path() / L"aki.json", std::ios::binary);
        if (!f) continue;
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        JsonValue v;
        if (!json_parse(text, &v) || !v.is(JsonValue::Type::Object)) continue;
        ConversionInfo c;
        c.id = str(v, "id").empty() ? dir : str(v, "id");
        if (c.id != dir) c.id = dir;  // the game finds it by its folder
        c.name = w(str(v, "name").empty() ? dir : str(v, "name"));
        c.version = w(str(v, "version"));
        if (const JsonValue* a = v.get("authors"); a && a->is(JsonValue::Type::Array))
            for (auto& x : a->items)
                if (x.is(JsonValue::Type::String)) c.authors += (c.authors.empty() ? L"" : L", ") + w(x.s);
        if (const JsonValue* br = v.get("base_rom"); br && br->is(JsonValue::Type::Object)) {
            if (const JsonValue* s = br->get("sha1")) {
                if (s->is(JsonValue::Type::String)) c.baseSha1.push_back(lower(s->s));
                else if (s->is(JsonValue::Type::Array))
                    for (auto& x : s->items)
                        if (x.is(JsonValue::Type::String)) c.baseSha1.push_back(lower(x.s));
            }
        }
        c.folder = it->path();
        c.saves = dataDir / L"saves" / it->path().filename();
        if (const RegEntry* e = reg.owner("conversions/" + dir); e && e->kind == "conversion") {
            c.recorded = true;
            if (!e->version.empty()) c.version = w(e->version);
            for (auto& file : e->files)
                if (file.path.compare(0, 5, "mods/") == 0) c.addons.push_back(file.path.substr(5));
        }
        out.push_back(std::move(c));
    }
    std::sort(out.begin(), out.end(), [](const ConversionInfo& a, const ConversionInfo& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    return out;
}

bool conversion_rom_matches(const ConversionInfo& c, const std::vector<std::string>& romSha1) {
    if (c.baseSha1.empty()) return true;
    for (auto& s : romSha1)
        if (std::find(c.baseSha1.begin(), c.baseSha1.end(), lower(s)) != c.baseSha1.end()) return true;
    return false;
}

}  // namespace pkg
