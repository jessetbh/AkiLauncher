#include "modelpacks.h"

#include "json.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

static std::string read_all(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static std::string lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// ---------------------------------------------------------------------------
// Packs
// ---------------------------------------------------------------------------

bool valid_pack_id(const std::string& id) {
    if (id.empty() || id.size() > 31) return false;
    for (char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}

std::vector<ModelPack> list_model_packs(const fs::path& modelsDir) {
    std::vector<ModelPack> out;
    std::error_code ec;
    for (fs::directory_iterator it(modelsDir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code fec;
        if (!it->is_directory(fec)) continue;
        fs::path d = it->path();
        std::wstring ext = d.extension().wstring();
        if (_wcsicmp(ext.c_str(), L".akimodel") != 0) continue;
        ModelPack p;
        p.dir = d;
        int n = WideCharToMultiByte(CP_UTF8, 0, d.stem().c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string id(n > 0 ? n - 1 : 0, 0);
        if (n > 1) WideCharToMultiByte(CP_UTF8, 0, d.stem().c_str(), -1, id.data(), n, nullptr, nullptr);
        p.id = id;
        JsonValue pk, man;
        if (json_parse(read_all(d / L"pack.json"), &pk) && pk.is(JsonValue::Type::Object)) {
            auto s = [&](const char* k) {
                const JsonValue* v = pk.get(k);
                return v && v->is(JsonValue::Type::String) ? v->s : std::string();
            };
            p.name = s("name");
            p.author = s("author");
            p.license = s("license");
            p.source = s("source");
        }
        if (json_parse(read_all(d / L"manifest.json"), &man) && man.is(JsonValue::Type::Object))
            p.triangles = man.integer("triangles", 0);
        if (fs::is_regular_file(d / L"preview.png", fec)) p.preview = d / L"preview.png";
        out.push_back(std::move(p));
    }
    std::sort(out.begin(), out.end(), [](const ModelPack& a, const ModelPack& b) { return a.id < b.id; });
    return out;
}

// ---------------------------------------------------------------------------
// models.txt
// ---------------------------------------------------------------------------

static std::vector<std::string> words_of(const std::string& s) {
    std::vector<std::string> w;
    std::istringstream in(s);
    std::string x;
    while (in >> x) w.push_back(x);
    return w;
}

static bool parse_int(const std::string& s, int base, int* out) {
    // Plain digits only (no sign, no prefix, no spaces), at most 0xFFFF.
    if (s.empty() || s.size() > 5) return false;
    long v = 0;
    for (char c : s) {
        int d = (c >= '0' && c <= '9') ? c - '0' : (base == 16 && c >= 'a' && c <= 'f') ? c - 'a' + 10
                : (base == 16 && c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0) return false;
        v = v * base + d;
    }
    if (v > 0xFFFF) return false;
    *out = (int)v;
    return true;
}

static ModelsTxtLine parse_line(const std::string& raw) {
    ModelsTxtLine l;
    l.raw = raw;
    size_t hash = raw.find('#');
    std::string body = hash == std::string::npos ? raw : raw.substr(0, hash);
    if (hash != std::string::npos) l.comment = raw.substr(hash);
    std::vector<std::string> w = words_of(body);
    if (w.empty()) {
        l.kind = hash == std::string::npos ? ModelsTxtLine::Kind::Blank : ModelsTxtLine::Kind::Comment;
        return l;
    }
    std::string k1 = w.size() > 1 ? lower(w[1]) : "";
    if (w.size() == 2 && lower(w[0]) == "off") {
        l.kind = ModelsTxtLine::Kind::Off;
        l.pack = lower(w[1]);
    } else if (w.size() == 3 && k1 == "slot" && parse_int(w[2], 10, &l.slot) && l.slot >= 1 && l.slot <= 4) {
        l.kind = ModelsTxtLine::Kind::Slot;
        l.pack = lower(w[0]);
    } else if ((w.size() == 3 || (w.size() == 5 && lower(w[3]) == "costume")) && k1 == "wrestler" &&
               lower(w[2]).rfind("0x", 0) == 0 && parse_int(w[2].substr(2), 16, &l.wrestler) &&
               (w.size() == 3 || parse_int(w[4], 10, &l.costume))) {
        l.kind = ModelsTxtLine::Kind::Wrestler;
        l.pack = lower(w[0]);
    } else {
        l.kind = ModelsTxtLine::Kind::Other;  // kept verbatim
        l.pack.clear();
        l.slot = 0;
        l.wrestler = l.costume = -1;
    }
    return l;
}

static std::string render(const ModelsTxtLine& l) {
    char buf[64];
    std::string s;
    if (l.kind == ModelsTxtLine::Kind::Slot) {
        snprintf(buf, sizeof(buf), " slot %d", l.slot);
        s = l.pack;
        s.resize(std::max<size_t>(s.size(), 12), ' ');
        s += buf;
    } else if (l.kind == ModelsTxtLine::Kind::Wrestler) {
        snprintf(buf, sizeof(buf), " wrestler 0x%04x", l.wrestler);
        s = l.pack;
        s.resize(std::max<size_t>(s.size(), 12), ' ');
        s += buf;
        if (l.costume >= 0) s += " costume " + std::to_string(l.costume);
    } else if (l.kind == ModelsTxtLine::Kind::Off) {
        s = "off " + l.pack;
    } else {
        return l.raw;
    }
    if (!l.comment.empty()) s += "   " + l.comment;
    return s;
}

ModelsTxt ModelsTxt::parse(const std::string& text) {
    ModelsTxt t;
    std::string s = text;
    if (s.rfind("\xEF\xBB\xBF", 0) == 0) {
        s.erase(0, 3);
        t.bom = true;
    }
    t.newline = s.find("\r\n") != std::string::npos ? "\r\n" : (s.find('\n') != std::string::npos ? "\n" : "\r\n");
    size_t at = 0;
    while (at < s.size()) {
        size_t nl = s.find('\n', at);
        std::string line = s.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        t.lines.push_back(parse_line(line));
        if (nl == std::string::npos) break;
        at = nl + 1;
    }
    return t;
}

std::string ModelsTxt::serialize() const {
    std::string out = bom ? "\xEF\xBB\xBF" : "";
    for (const auto& l : lines) out += l.raw + newline;  // as read; edited and new lines carry fresh text
    return out;
}

const ModelsTxtLine* ModelsTxt::assignment(const std::string& pack) const {
    for (const auto& l : lines)
        if ((l.kind == ModelsTxtLine::Kind::Slot || l.kind == ModelsTxtLine::Kind::Wrestler) && l.pack == lower(pack))
            return &l;
    return nullptr;
}

int ModelsTxt::assignment_count(const std::string& pack) const {
    int n = 0;
    for (const auto& l : lines)
        n += (l.kind == ModelsTxtLine::Kind::Slot || l.kind == ModelsTxtLine::Kind::Wrestler) && l.pack == lower(pack);
    return n;
}

bool ModelsTxt::is_off(const std::string& pack) const {
    for (const auto& l : lines)
        if (l.kind == ModelsTxtLine::Kind::Off && l.pack == lower(pack)) return true;
    return false;
}

void ModelsTxt::set_assignment(const std::string& pack, ModelsTxtLine::Kind kind, int slot, int wrestler,
                               int costume, const std::string& comment) {
    std::string p = lower(pack);
    std::string keep;  // the old line's comment, unless a new one is given (only onto the same kind of line)
    ModelsTxtLine::Kind keepKind = ModelsTxtLine::Kind::Blank;
    std::vector<ModelsTxtLine> out;
    size_t firstRule = std::string::npos;
    for (auto& l : lines) {
        bool mine = (l.kind == ModelsTxtLine::Kind::Slot || l.kind == ModelsTxtLine::Kind::Wrestler) && l.pack == p;
        if (mine) {
            if (keepKind == ModelsTxtLine::Kind::Blank) {
                keep = l.comment;
                keepKind = l.kind;
            }
            continue;
        }
        if (firstRule == std::string::npos && (l.kind == ModelsTxtLine::Kind::Slot ||
                                               l.kind == ModelsTxtLine::Kind::Wrestler ||
                                               l.kind == ModelsTxtLine::Kind::Off || l.kind == ModelsTxtLine::Kind::Other))
            firstRule = out.size();
        out.push_back(l);
    }
    if (kind == ModelsTxtLine::Kind::Slot || kind == ModelsTxtLine::Kind::Wrestler) {
        ModelsTxtLine n;
        n.kind = kind;
        n.pack = p;
        n.slot = slot;
        n.wrestler = wrestler;
        n.costume = kind == ModelsTxtLine::Kind::Wrestler ? costume : -1;
        n.comment = !comment.empty() ? (comment[0] == '#' ? comment : "# " + comment) : keepKind == kind ? keep : "";
        n.raw = render(n);
        if (out.empty()) {
            ModelsTxtLine head;
            head.kind = ModelsTxtLine::Kind::Comment;
            head.raw = "# pack       replaces     (aki_modelimport: see its README)";
            out.push_back(head);
            firstRule = 1;
        }
        out.insert(out.begin() + (firstRule == std::string::npos ? out.size() : firstRule), n);
    }
    lines.swap(out);
}

void ModelsTxt::set_off(const std::string& pack, bool off) {
    std::string p = lower(pack);
    if (!off) {
        lines.erase(std::remove_if(lines.begin(), lines.end(),
                                   [&](const ModelsTxtLine& l) { return l.kind == ModelsTxtLine::Kind::Off && l.pack == p; }),
                    lines.end());
        return;
    }
    if (is_off(p)) return;
    ModelsTxtLine l;
    l.kind = ModelsTxtLine::Kind::Off;
    l.pack = p;
    l.raw = "off " + p;
    lines.push_back(l);
}

std::vector<std::string> ModelsTxt::missing_packs(const std::vector<ModelPack>& installed) const {
    std::vector<std::string> out;
    for (const auto& l : lines) {
        if (l.pack.empty()) continue;
        bool have = std::any_of(installed.begin(), installed.end(), [&](const ModelPack& p) { return lower(p.id) == l.pack; });
        if (!have && std::find(out.begin(), out.end(), l.pack) == out.end()) out.push_back(l.pack);
    }
    return out;
}

bool read_models_txt(const fs::path& file, ModelsTxt* out, std::wstring* err) {
    std::error_code ec;
    if (!fs::exists(file, ec)) {
        *out = ModelsTxt{};
        return true;
    }
    std::ifstream f(file, std::ios::binary);
    if (!f) {
        if (err) *err = L"can't read " + file.wstring();
        return false;
    }
    *out = ModelsTxt::parse(std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()));
    return true;
}

bool write_models_txt(const fs::path& file, const ModelsTxt& txt, std::wstring* err) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    fs::path tmp = file;
    tmp += L".new";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        std::string body = txt.serialize();
        f.write(body.data(), (std::streamsize)body.size());
        f.close();
        if (f.fail()) {
            fs::remove(tmp, ec);
            if (err) *err = L"can't write " + tmp.wstring();
            return false;
        }
    }
    // One step from the old file to the new: the game reads one or the other, never half.
    if (!MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD e = GetLastError();
        fs::remove(tmp, ec);
        if (err) *err = L"can't replace " + file.wstring() + L" (error " + std::to_wstring(e) + L")";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Rosters
// ---------------------------------------------------------------------------

const RosterEntry* Roster::find(int id) const {
    for (const auto& w : wrestlers)
        if (w.id == id) return &w;
    return nullptr;
}

bool load_roster_file(const fs::path& file, Roster* out, std::wstring* err) {
    JsonValue j;
    std::string perr;
    if (!json_parse(read_all(file), &j, &perr) || !j.is(JsonValue::Type::Object)) {
        if (err) *err = file.wstring() + L": not valid JSON " + json_utf8_to_wide(perr);
        return false;
    }
    const JsonValue* ws = j.get("wrestlers");
    if (j.integer("schema", 0) != 1 || !ws || !ws->is(JsonValue::Type::Array)) {
        if (err) *err = file.wstring() + L": not a roster (schema 1 with a wrestlers list)";
        return false;
    }
    Roster r;
    for (const auto& w : ws->items) {
        const JsonValue* id = w.get("id");
        const JsonValue* name = w.get("name");
        int v = 0;
        if (!id || !id->is(JsonValue::Type::String) || !name || !name->is(JsonValue::Type::String) ||
            lower(id->s).rfind("0x", 0) != 0 || !parse_int(id->s.substr(2), 16, &v))
            continue;  // (AkiTools' check_roster is the strict one; a bad entry is just left out here)
        RosterEntry e;
        e.id = v;
        e.name = name->s;
        if (const JsonValue* cs = w.get("costumes"); cs && cs->is(JsonValue::Type::Array))
            for (const auto& c : cs->items) {
                int n = c.integer("costume", -1);
                if (n >= 0) {
                    const JsonValue* cn = c.get("name");
                    e.costumes.push_back({n, cn && cn->is(JsonValue::Type::String) ? cn->s : std::string()});
                }
            }
        r.wrestlers.push_back(std::move(e));
    }
    std::sort(r.wrestlers.begin(), r.wrestlers.end(), [](const RosterEntry& a, const RosterEntry& b) { return a.name < b.name; });
    *out = std::move(r);
    return true;
}

fs::path find_roster(const std::string& profile, const fs::path& appRoot, const fs::path& depotRoot) {
    std::error_code ec;
    std::wstring name = json_utf8_to_wide(profile) + L"-roster.json";
    fs::path a = appRoot / L"assets" / L"rosters" / name;
    if (fs::is_regular_file(a, ec)) return a;
    if (!depotRoot.empty()) {
        fs::path b = depotRoot / L"AkiTools" / L"model-templates" / L"profiles" / name;
        if (fs::is_regular_file(b, ec)) return b;
    }
    return {};
}
