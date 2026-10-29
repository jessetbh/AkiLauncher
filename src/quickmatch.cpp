#include "quickmatch.h"

#include "json.h"

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <sstream>

#include "miniz.h"

namespace fs = std::filesystem;

static const wchar_t* kStateFile = L"aki-launcher-quickmatch.json";

static std::wstring w(const std::string& s) { return json_utf8_to_wide(s); }

const QmOption* QuickMatchMod::option(const std::string& id) const {
    for (const auto& o : options)
        if (o.id == id) return &o;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

static bool read_file(const fs::path& p, std::string* out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    *out = ss.str();
    return true;
}

// Through a temp file and a rename, so a crash never leaves half a file.
static bool write_file(const fs::path& p, const std::string& text, std::wstring* err) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += L".aki-tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << text;
        if (!f.flush()) {
            if (err) *err = L"can't write " + tmp.wstring();
            return false;
        }
    }
    if (!MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (err) *err = L"can't replace " + p.wstring() + L" (error " + std::to_wstring(GetLastError()) + L")";
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// The mod's options
// ---------------------------------------------------------------------------

bool parse_quickmatch_manifest(const std::string& json, QuickMatchMod* out, std::wstring* err) {
    JsonValue root;
    std::string perr;
    if (!json_parse(json, &root, &perr) || !root.is(JsonValue::Type::Object)) {
        if (err) *err = L"mod.json: " + (perr.empty() ? std::wstring(L"not an object") : w(perr));
        return false;
    }
    const JsonValue* id = root.get("id");
    if (!id || !id->is(JsonValue::Type::String) || id->s != kQuickMatchId) {
        if (err) *err = L"mod.json: not the aki_quickmatch mod";
        return false;
    }
    out->version.clear();
    out->options.clear();
    if (const JsonValue* v = root.get("version"); v && v->is(JsonValue::Type::String)) out->version = v->s;
    const JsonValue* schema = root.get("config_schema");
    const JsonValue* opts = schema ? schema->get("options") : nullptr;
    if (opts && opts->is(JsonValue::Type::Array)) {
        for (const auto& o : opts->items) {
            const JsonValue* oid = o.get("id");
            const JsonValue* type = o.get("type");
            const JsonValue* choices = o.get("options");
            if (!oid || !oid->is(JsonValue::Type::String) || !type || !type->is(JsonValue::Type::String) ||
                type->s != "Enum" || !choices || !choices->is(JsonValue::Type::Array))
                continue;
            QmOption q;
            q.id = oid->s;
            const JsonValue* name = o.get("name");
            q.name = name && name->is(JsonValue::Type::String) ? name->s : q.id;
            for (const auto& c : choices->items)
                if (c.is(JsonValue::Type::String)) q.choices.push_back(c.s);
            if (q.choices.empty()) continue;
            const JsonValue* def = o.get("default");
            q.def = q.choices[0];
            if (def && def->is(JsonValue::Type::String))
                for (const auto& c : q.choices)
                    if (c == def->s) q.def = c;
            q.randomOk = (q.id == "venue" || q.id == "match") && q.choices.size() > 1;
            out->options.push_back(std::move(q));
        }
    }
    if (!out->option("venue")) {
        if (err) *err = L"mod.json: the quickmatch mod has no Venue option";
        return false;
    }
    return true;
}

static bool read_nrm_manifest(const fs::path& nrm, std::string* json) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, nrm.c_str(), L"rb") != 0 || !f) return false;
    std::error_code ec;
    uintmax_t size = fs::file_size(nrm, ec);
    mz_zip_archive za{};
    bool ok = false;
    if (!ec && mz_zip_reader_init_cfile(&za, f, (mz_uint64)size, 0)) {
        size_t n = 0;
        if (void* p = mz_zip_reader_extract_file_to_heap(&za, "mod.json", &n, 0)) {
            json->assign((const char*)p, n);
            mz_free(p);
            ok = true;
        }
        mz_zip_reader_end(&za);
    }
    fclose(f);
    return ok;
}

bool find_quickmatch(const fs::path& bundledDir, const fs::path& modsDir, QuickMatchMod* out, std::wstring* err) {
    std::wstring file = std::wstring(json_utf8_to_wide(kQuickMatchId)) + L".nrm";
    std::wstring firstErr;
    for (const fs::path& dir : {bundledDir, modsDir}) {
        if (dir.empty()) continue;
        fs::path nrm = dir / file;
        std::error_code ec;
        if (!fs::is_regular_file(nrm, ec)) continue;
        std::string json;
        std::wstring e;
        if (!read_nrm_manifest(nrm, &json)) e = nrm.wstring() + L": no mod.json inside";
        else if (parse_quickmatch_manifest(json, out, &e)) {
            out->nrm = nrm;
            return true;
        } else e = nrm.wstring() + L": " + e;
        if (firstErr.empty()) firstErr = e;
    }
    if (err) *err = firstErr.empty() ? L"this install has no Quick Match mod (" + file + L")" : firstErr;
    return false;
}

bool resolve_quickmatch(const QuickMatchMod& mod, const std::map<std::string, std::string>& picks, std::mt19937& rng,
                        std::map<std::string, std::string>* values, std::wstring* err) {
    values->clear();
    for (const auto& o : mod.options) {
        auto it = picks.find(o.id);
        std::string pick = it == picks.end() || it->second.empty() ? o.def : it->second;
        if (pick == kQmRandom && o.randomOk) {
            std::uniform_int_distribution<size_t> d(1, o.choices.size() - 1);
            (*values)[o.id] = o.choices[d(rng)];
            continue;
        }
        bool known = false;
        for (const auto& c : o.choices) known = known || c == pick;
        if (!known) {
            if (err) *err = w(o.name) + L": no choice \"" + w(pick) + L"\"";
            return false;
        }
        (*values)[o.id] = pick;
    }
    return true;
}

// ---------------------------------------------------------------------------
// One run's config, and undoing it
// ---------------------------------------------------------------------------

static JsonValue* member(JsonValue& obj, const char* key) {
    for (size_t i = obj.keys.size(); i-- > 0;)
        if (obj.keys[i] == key) return &obj.items[i];
    return nullptr;
}

static void set_member(JsonValue& obj, const char* key, JsonValue v) {
    if (JsonValue* m = member(obj, key)) *m = std::move(v);
    else {
        obj.keys.push_back(key);
        obj.items.push_back(std::move(v));
    }
}

// Adds the mod's id to obj[key] (made an array if missing). *added: it wasn't there. False if obj[key] isn't a list.
static bool add_to_list(JsonValue& obj, const char* key, bool* added) {
    JsonValue* list = member(obj, key);
    if (!list) {
        JsonValue a;
        a.type = JsonValue::Type::Array;
        set_member(obj, key, a);
        list = member(obj, key);
    }
    if (!list->is(JsonValue::Type::Array)) return false;
    for (const auto& v : list->items)
        if (v.is(JsonValue::Type::String) && v.s == kQuickMatchId) {
            *added = false;
            return true;
        }
    list->items.push_back(json_string(kQuickMatchId));
    *added = true;
    return true;
}

static bool remove_from_list(JsonValue& obj, const char* key) {
    JsonValue* list = member(obj, key);
    if (!list || !list->is(JsonValue::Type::Array)) return false;
    size_t before = list->items.size();
    std::erase_if(list->items, [](const JsonValue& v) { return v.is(JsonValue::Type::String) && v.s == kQuickMatchId; });
    return list->items.size() != before;
}

static fs::path config_path(const fs::path& dataDir) {
    return dataDir / L"mod_config" / (json_utf8_to_wide(kQuickMatchId) + L".json");
}

bool quickmatch_pending(const fs::path& dataDir) {
    std::error_code ec;
    return fs::exists(dataDir / kStateFile, ec);
}

bool quickmatch_apply(const fs::path& dataDir, const QuickMatchMod& mod, const std::map<std::string, std::string>& values,
                      std::wstring* err) {
    if (quickmatch_pending(dataDir) && !quickmatch_restore(dataDir, err)) return false;

    fs::path modsJson = dataDir / L"mods.json";
    std::string text;
    JsonValue mj;
    bool hadMods = read_file(modsJson, &text);
    if (hadMods) {
        std::string perr;
        if (!json_parse(text, &mj, &perr) || !mj.is(JsonValue::Type::Object)) {
            if (err) *err = modsJson.wstring() + L" isn't valid JSON (" + w(perr) + L"); left as it is";
            return false;
        }
    } else {
        mj.type = JsonValue::Type::Object;  // what the runtime writes for a fresh install
        JsonValue empty;
        empty.type = JsonValue::Type::Array;
        set_member(mj, "enabled_mods", empty);
        set_member(mj, "latest_game_mode", json_string(""));
        set_member(mj, "mod_order", empty);
    }
    bool addedEnabled = false, addedOrder = false;
    if (!add_to_list(mj, "enabled_mods", &addedEnabled) || !add_to_list(mj, "mod_order", &addedOrder)) {
        if (err) *err = modsJson.wstring() + L": enabled_mods / mod_order isn't a list; left as it is";
        return false;
    }

    // The mod's config: the old one's other keys kept, ours on top.
    fs::path cfgPath = config_path(dataDir);
    std::string oldCfg;
    bool hadCfg = read_file(cfgPath, &oldCfg);
    JsonValue old;
    if (hadCfg && (!json_parse(oldCfg, &old) || !old.is(JsonValue::Type::Object))) old = JsonValue{};
    JsonValue storage;
    if (const JsonValue* s = old.get("storage"); s && s->is(JsonValue::Type::Object)) storage = *s;
    storage.type = JsonValue::Type::Object;
    set_member(storage, "enabled", json_bool(true));
    for (const auto& [id, value] : values) set_member(storage, id.c_str(), json_string(value));
    JsonValue cfg;
    cfg.type = JsonValue::Type::Object;
    set_member(cfg, "mod_id", json_string(kQuickMatchId));
    const JsonValue* ov = old.get("mod_version");
    set_member(cfg, "mod_version", json_string(!mod.version.empty()               ? mod.version
                                               : ov && ov->is(JsonValue::Type::String) ? ov->s
                                                                                      : "0.1.0"));
    const JsonValue* rv = old.get("recomp_version");
    set_member(cfg, "recomp_version", json_string(rv && rv->is(JsonValue::Type::String) ? rv->s : "0.1.0"));
    set_member(cfg, "storage", storage);

    // The undo record first: a crash after it leaves something to restore, never changes nobody knows about.
    JsonValue state;
    state.type = JsonValue::Type::Object;
    set_member(state, "added_enabled", json_bool(addedEnabled));
    set_member(state, "added_order", json_bool(addedOrder));
    set_member(state, "config", hadCfg ? json_string(oldCfg) : JsonValue{});
    if (!write_file(dataDir / kStateFile, json_write(state), err)) return false;
    if (!write_file(modsJson, json_write(mj), err) || !write_file(cfgPath, json_write(cfg), err)) {
        std::wstring ignored;
        quickmatch_restore(dataDir, &ignored);
        return false;
    }
    return true;
}

bool quickmatch_restore(const fs::path& dataDir, std::wstring* err) {
    fs::path statePath = dataDir / kStateFile;
    std::string text;
    if (!read_file(statePath, &text)) return true;
    std::error_code ec;
    JsonValue state;
    if (!json_parse(text, &state) || !state.is(JsonValue::Type::Object)) {
        // Unreadable: nothing to go by. Dropped, so it doesn't block every later run.
        fs::remove(statePath, ec);
        if (err) *err = statePath.wstring() + L" was unreadable; check the Quick Match mod is off in the game's Mods menu";
        return false;
    }
    bool ok = true;
    std::wstring firstErr;
    fs::path modsJson = dataDir / L"mods.json";
    std::string mtext;
    JsonValue mj;
    if (read_file(modsJson, &mtext) && json_parse(mtext, &mj) && mj.is(JsonValue::Type::Object)) {
        bool changed = false;
        if (state.boolean("added_enabled", false)) changed = remove_from_list(mj, "enabled_mods") || changed;
        if (state.boolean("added_order", false)) changed = remove_from_list(mj, "mod_order") || changed;
        if (changed && !write_file(modsJson, json_write(mj), &firstErr)) ok = false;
    }
    fs::path cfgPath = config_path(dataDir);
    const JsonValue* old = state.get("config");
    if (old && old->is(JsonValue::Type::String)) {
        std::wstring e;
        if (!write_file(cfgPath, old->s, &e)) {
            ok = false;
            if (firstErr.empty()) firstErr = e;
        }
    } else {
        fs::remove(cfgPath, ec);
        if (ec) {
            ok = false;
            if (firstErr.empty()) firstErr = L"can't remove " + cfgPath.wstring();
        }
    }
    // Kept on failure: the next restore tries again.
    if (ok) fs::remove(statePath, ec);
    else if (err) *err = firstErr;
    return ok;
}
