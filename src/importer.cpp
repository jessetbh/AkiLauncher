#include "importer.h"

#include "json.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <set>

namespace fs = std::filesystem;

static std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    return s;
}

static std::string u8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, 0);
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

const wchar_t* import_outcome_word(ImportOutcome o) {
    switch (o) {
    case ImportOutcome::Waiting: return L"Waiting";
    case ImportOutcome::Working: return L"Working";
    case ImportOutcome::Converted: return L"Converted";
    case ImportOutcome::Installed: return L"Installed";
    case ImportOutcome::HaveIt: return L"Have it";
    case ImportOutcome::Stock: return L"Stock game";
    case ImportOutcome::NeedsRom: return L"Needs ROM";
    case ImportOutcome::NeedsGame: return L"Needs game";
    case ImportOutcome::Unsupported: return L"Unsupported";
    case ImportOutcome::Failed: return L"Failed";
    case ImportOutcome::Stopped: return L"Stopped";
    }
    return L"?";
}

bool import_outcome_final(ImportOutcome o) { return o != ImportOutcome::Waiting && o != ImportOutcome::Working; }

ImportCounts count_import(const std::vector<ImportLine>& lines) {
    ImportCounts c;
    for (const auto& l : lines) {
        ++c.total;
        switch (l.outcome) {
        case ImportOutcome::Waiting: ++c.waiting; break;
        case ImportOutcome::Working: ++c.working; break;
        case ImportOutcome::Converted:
            ++c.converted;
            if (l.kind == L"texture pack") ++c.texturePacks;
            else ++c.conversions;
            break;
        case ImportOutcome::Installed:
            ++c.installed;
            if (l.kind == L"texture pack") ++c.texturePacks;
            break;
        case ImportOutcome::HaveIt: ++c.haveIt; break;
        case ImportOutcome::Stock: ++c.stock; break;
        case ImportOutcome::NeedsRom: ++c.needsRom; break;
        case ImportOutcome::NeedsGame: ++c.needsGame; break;
        case ImportOutcome::Unsupported: ++c.unsupported; break;
        case ImportOutcome::Failed: ++c.failed; break;
        case ImportOutcome::Stopped: ++c.stopped; break;
        }
        if (l.untested && (l.outcome == ImportOutcome::Converted)) ++c.untested;
    }
    return c;
}

static void part(std::wstring& s, int n, const wchar_t* one, const wchar_t* many) {
    if (n <= 0) return;
    if (!s.empty()) s += L", ";
    s += std::to_wstring(n) + L" " + (n == 1 ? one : many);
}

std::wstring import_summary(const ImportCounts& c) {
    std::wstring s;
    if (c.converted) {
        s = std::to_wstring(c.converted) + L" converted";
        std::wstring kinds;
        part(kinds, c.conversions, L"conversion", L"conversions");
        int convTex = c.converted - c.conversions;
        part(kinds, convTex, L"texture pack", L"texture packs");
        if (!kinds.empty()) s += L" (" + kinds + L")";
    }
    part(s, c.installed, L"installed", L"installed");
    part(s, c.haveIt, L"you had already", L"you had already");
    part(s, c.stock, L"stock game (nothing to do)", L"stock games (nothing to do)");
    part(s, c.needsRom, L"needs a ROM", L"need a ROM");
    part(s, c.needsGame, L"needs a game picked", L"need a game picked");
    part(s, c.unsupported, L"unsupported", L"unsupported");
    part(s, c.failed, L"failed", L"failed");
    part(s, c.stopped, L"stopped", L"stopped");
    if (s.empty()) s = L"Nothing to import";
    s += L". Your originals weren't changed.";
    if (c.untested) s += L" " + std::to_wstring(c.untested) + L" Untested.";
    return s;
}

std::wstring import_progress(const ImportCounts& c) {
    int done = c.total - c.waiting - c.working;
    return std::to_wstring(done) + L" done | " + std::to_wstring(c.working) + L" working | " + std::to_wstring(c.waiting) +
           L" waiting";
}

// --- worker lines ---------------------------------------------------------------------------------------------------

static const struct {
    const wchar_t* key;
    const wchar_t* modId;
} kGameIds[] = {{L"wt", L"wcwnwoworldtour"}, {L"revenge", L"wcwnworevenge"}, {L"wm2k", L"wwfwm2k"},
                {L"vpw64", L"vpw64"},        {L"nomercy", L"wwfnomercy"},    {L"vpw2", L"vpw2"}};

std::wstring engine_game_mod_id(const std::wstring& key) {
    for (const auto& g : kGameIds)
        if (key == g.key) return g.modId;
    return {};
}

std::wstring engine_game_key(const std::wstring& modGameId) {
    for (const auto& g : kGameIds)
        if (modGameId == g.modId) return g.key;
    return {};
}

static std::wstring kind_word(const std::wstring& k) {
    if (k == L"rom") return L"ROM hack";
    if (k == L"patch") return L"patch";
    if (k == L"pj64-bundle") return L"Project64 bundle";
    if (k == L"texture-pack") return L"texture pack";
    if (k == L"nrm") return L"mod";
    return k;
}

static std::wstring tier_word(const std::wstring& t) {
    if (t == L"verified") return L"Verified";
    if (t == L"experimental") return L"Experimental";
    if (t == L"unsupported") return L"Unsupported";
    if (t == L"untested") return L"Untested";
    return t;
}

WorkerEvent parse_worker_line(const std::string& text) {
    WorkerEvent ev;
    JsonValue j;
    if (!json_parse(text, &j) || !j.is(JsonValue::Type::Object)) return ev;
    std::wstring event = j.wstr("event");
    if (event == L"done") {
        ev.type = WorkerEvent::Type::Done;
        if (const JsonValue* c = j.get("counts"); c && c->is(JsonValue::Type::Object))
            for (size_t i = 0; i < c->keys.size(); ++i)
                if (c->items[i].is(JsonValue::Type::Number)) ev.counts[c->keys[i]] = (int)c->items[i].n;
        if (const JsonValue* e = j.get("engine"); e && e->is(JsonValue::Type::String)) ev.engine = e->s;
        return ev;
    }
    if (event != L"item") return ev;
    ev.type = WorkerEvent::Type::Item;
    ImportLine& l = ev.line;
    l.viaWorker = true;
    l.item = j.wstr("item");
    std::wstring status = j.wstr("status"), why = j.wstr("why"), kind = j.wstr("kind");
    l.gameKey = j.wstr("game");
    l.kind = kind_word(kind);
    l.name = j.wstr("name");
    if (l.name.empty()) {
        std::wstring leaf = l.item.substr(l.item.find_last_of(L"!\\/") == std::wstring::npos ? 0 : l.item.find_last_of(L"!\\/") + 1);
        l.name = leaf.empty() ? l.item : leaf;
    }
    std::wstring tier = j.wstr("aki_tier");
    if (tier.empty() && status == L"ready") tier = j.wstr("tier");
    l.tier = tier_word(tier);
    l.untested = tier == L"untested";
    l.needs = j.wstr("needs");
    if (const JsonValue* p = j.get("package"); p && p->is(JsonValue::Type::String)) l.packages.push_back(json_utf8_to_wide(p->s));
    if (const JsonValue* ps = j.get("packages"); ps && ps->is(JsonValue::Type::Array))
        for (const auto& v : ps->items)
            if (v.is(JsonValue::Type::String)) {
                fs::path pp = json_utf8_to_wide(v.s);
                if (std::find(l.packages.begin(), l.packages.end(), pp) == l.packages.end()) l.packages.push_back(pp);
            }
    if (status == L"converted") {
        l.outcome = ImportOutcome::Converted;
        if (kind == L"rom" || kind == L"patch" || kind == L"pj64-bundle") l.kind = L"conversion";
        std::wstring conv = j.wstr("converter");
        if (l.kind == L"texture pack") {
            int images = j.integer("images", 0);
            l.detail = images ? std::to_wstring(images) + L" textures" : L"texture pack";
        } else {
            l.detail = conv.empty() ? L"built" : L"built with " + conv;
            if (l.untested) l.detail += L"; Untested: it may crash or look wrong";
        }
        std::wstring note = j.wstr("note");
        if (!note.empty()) l.detail += L" (" + note + L")";
    } else if (status == L"stock") {
        l.outcome = ImportOutcome::Stock;
        l.detail = why.empty() ? L"this is the stock game: nothing to convert" : why;
        // The same hint a loose stock ROM gets (aki-roadmap#271): one inside a folder or zip said only "nothing to convert".
        if (l.detail.find(L"card") == std::wstring::npos) l.detail += L". Set it as the game's ROM on its card if it needs one";
    } else if (status == L"have-it") {
        l.outcome = ImportOutcome::HaveIt;
        l.detail = why.empty() ? L"imported before: skipped" : why;
    } else if (status == L"needs-rom") {
        l.outcome = ImportOutcome::NeedsRom;
        l.detail = l.needs.empty() ? (why.empty() ? L"needs its stock ROM" : why) : L"needs " + l.needs;
        // The converter's reason too (aki-roadmap#271): "needs the ROM it was made for" alone hid which ROMs it tried.
        if (!l.needs.empty() && !why.empty()) l.detail += L" (" + why + L")";
    } else if (status == L"needs-game") {
        l.outcome = ImportOutcome::NeedsGame;
        l.detail = why.empty() ? L"doesn't say which game it is for" : why;
    } else if (status == L"unsupported") {
        l.outcome = ImportOutcome::Unsupported;
        l.detail = why;
    } else if (status == L"ready" && l.item.find(L'!') == std::wstring::npos) {
        // The worker leaves mods (.nrm) and packages to the launcher: a file on disk (in a picked folder) is
        // installed from its path.
        l.outcome = ImportOutcome::Waiting;
        l.installAsIs = true;
        l.detail = L"installing";
    } else if (status == L"ready") {
        // ... but one inside an archive can't be installed by path.
        l.outcome = ImportOutcome::Unsupported;
        l.detail = L"a " + (l.kind.empty() ? std::wstring(L"mod") : l.kind) +
                   L" inside an archive: unzip it and import that file on its own";
    } else {
        l.outcome = ImportOutcome::Failed;
        l.detail = why.empty() ? L"failed (no reason given)" : why;
    }
    l.log = json_utf8_to_wide(text);
    if (!why.empty()) l.log = why + L"\n\n" + l.log;
    return ev;
}

static void put(JsonValue& o, const char* k, JsonValue v) {
    o.keys.push_back(k);
    o.items.push_back(std::move(v));
}

std::string worker_job_json(const WorkerJob& job) {
    JsonValue o;
    o.type = JsonValue::Type::Object;
    JsonValue paths;
    paths.type = JsonValue::Type::Array;
    for (const auto& p : job.paths) paths.items.push_back(json_string(u8(p.wstring())));
    put(o, "paths", paths);
    JsonValue roms;
    roms.type = JsonValue::Type::Object;
    for (const auto& [k, p] : job.roms) put(roms, u8(k).c_str(), json_string(u8(p.wstring())));
    put(o, "roms", roms);
    JsonValue dirs;
    dirs.type = JsonValue::Type::Array;
    for (const auto& d : job.romDirs) dirs.items.push_back(json_string(u8(d.wstring())));
    put(o, "rom_dirs", dirs);
    if (!job.ledger.empty()) put(o, "ledger", json_string(u8(job.ledger.wstring())));
    if (!job.out.empty()) put(o, "out", json_string(u8(job.out.wstring())));
    if (!job.work.empty()) put(o, "work", json_string(u8(job.work.wstring())));
    if (!job.catalog.empty()) put(o, "catalog", json_string(u8(job.catalog.wstring())));
    if (!job.sevenzip.empty()) put(o, "sevenzip", json_string(u8(job.sevenzip.wstring())));
    if (!job.game.empty()) put(o, "game", json_string(u8(job.game)));
    put(o, "build", json_bool(job.build));
    return json_write(o, true);
}

// --- sources --------------------------------------------------------------------------------------------------------

fs::path import_source_of(const std::wstring& label, const std::vector<fs::path>& inputs) {
    std::wstring path = label.substr(0, label.find(L'!'));
    std::wstring lp = lower(fs::path(path).lexically_normal().wstring());
    fs::path best;
    size_t bestLen = 0;
    for (const auto& in : inputs) {
        std::wstring li = lower(in.lexically_normal().wstring());
        while (li.size() > 3 && (li.back() == L'\\' || li.back() == L'/')) li.pop_back();
        bool owns = lp == li || (lp.size() > li.size() && lp.compare(0, li.size(), li) == 0 &&
                                 (lp[li.size()] == L'\\' || lp[li.size()] == L'/'));
        if (owns && li.size() > bestLen) {
            best = in;
            bestLen = li.size();
        }
    }
    return best;
}

// --- the Import folder ----------------------------------------------------------------------------------------------

std::vector<fs::path> import_folder_items(const fs::path& importDir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::directory_iterator it(importDir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::wstring leaf = it->path().filename().wstring();
        if (leaf.empty() || leaf[0] == L'.') continue;
        std::wstring low = lower(leaf);
        if (low == lower(kImportedFolder) || low == lower(kNeedsAttentionFolder)) continue;
        if (low == L"desktop.ini" || low == L"thumbs.db" || low == L"readme.txt") continue;
        DWORD attrs = GetFileAttributesW(it->path().c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
        out.push_back(it->path());
    }
    std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) {
        return lower(a.filename().wstring()) < lower(b.filename().wstring());
    });
    return out;
}

ImportPlace import_place_for(const std::vector<ImportOutcome>& outcomes) {
    if (outcomes.empty()) return ImportPlace::Stay;
    bool bad = false;
    for (ImportOutcome o : outcomes) {
        switch (o) {
        case ImportOutcome::Waiting:
        case ImportOutcome::Working:
        case ImportOutcome::Stopped:
        case ImportOutcome::NeedsRom:
        case ImportOutcome::NeedsGame: return ImportPlace::Stay;
        case ImportOutcome::Unsupported:
        case ImportOutcome::Failed: bad = true; break;
        default: break;
        }
    }
    return bad ? ImportPlace::NeedsAttention : ImportPlace::Imported;
}

fs::path import_free_name(const fs::path& dir, const fs::path& leaf) {
    std::error_code ec;
    fs::path p = dir / leaf;
    if (!fs::exists(p, ec)) return p;
    std::wstring stem = leaf.stem().wstring(), ext = leaf.extension().wstring();
    for (int n = 2; n < 10000; ++n) {
        p = dir / (stem + L" (" + std::to_wstring(n) + L")" + ext);
        if (!fs::exists(p, ec)) return p;
    }
    return dir / (stem + L" (more)" + ext);
}

std::string import_reason_text(const std::vector<const ImportLine*>& lines) {
    std::wstring s = L"AKI Launcher could not import this:\r\n\r\n";
    for (const ImportLine* l : lines) {
        if (l->outcome != ImportOutcome::Unsupported && l->outcome != ImportOutcome::Failed) continue;
        s += std::wstring(import_outcome_word(l->outcome)) + L": " + l->item + L"\r\n    " + l->detail + L"\r\n";
    }
    s += L"\r\nMove it back into the Import folder to try again (for example after a launcher update).\r\n";
    return u8(s);
}

// --- ledger ---------------------------------------------------------------------------------------------------------

static std::string read_all(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool ledger_remove(const fs::path& ledger, const std::vector<std::string>& keys, std::wstring* errorOut) {
    std::error_code ec;
    if (!fs::exists(ledger, ec)) return true;
    JsonValue j;
    if (!json_parse(read_all(ledger), &j) || !j.is(JsonValue::Type::Object)) {
        if (errorOut) *errorOut = L"the import ledger isn't valid JSON: " + ledger.wstring();
        return false;
    }
    std::set<std::string> drop(keys.begin(), keys.end());
    for (size_t i = 0; i < j.keys.size(); ++i) {
        if (j.keys[i] != "entries" || !j.items[i].is(JsonValue::Type::Object)) continue;
        JsonValue& e = j.items[i];
        JsonValue kept;
        kept.type = JsonValue::Type::Object;
        for (size_t k = 0; k < e.keys.size(); ++k)
            if (!drop.count(e.keys[k])) {
                kept.keys.push_back(e.keys[k]);
                kept.items.push_back(e.items[k]);
            }
        e = std::move(kept);
    }
    fs::path tmp = ledger;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << json_write(j);
        if (!f) {
            if (errorOut) *errorOut = L"can't write " + tmp.wstring();
            return false;
        }
    }
    if (!MoveFileExW(tmp.c_str(), ledger.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        fs::remove(tmp, ec);
        if (errorOut) *errorOut = L"can't replace " + ledger.wstring();
        return false;
    }
    return true;
}

std::vector<std::string> ledger_keys_for_packages(const fs::path& ledger, const std::vector<fs::path>& packages) {
    std::vector<std::string> out;
    JsonValue j;
    if (!json_parse(read_all(ledger), &j) || !j.is(JsonValue::Type::Object)) return out;
    const JsonValue* e = j.get("entries");
    if (!e || !e->is(JsonValue::Type::Object)) return out;
    std::set<std::wstring> want;
    for (const auto& p : packages) want.insert(lower(p.lexically_normal().wstring()));
    for (size_t k = 0; k < e->keys.size(); ++k) {
        const JsonValue& v = e->items[k];
        if (!v.is(JsonValue::Type::Object)) continue;
        bool hit = false;
        if (const JsonValue* p = v.get("package"); p && p->is(JsonValue::Type::String))
            hit = want.count(lower(fs::path(json_utf8_to_wide(p->s)).lexically_normal().wstring())) > 0;
        if (const JsonValue* ps = v.get("packages"); !hit && ps && ps->is(JsonValue::Type::Array))
            for (const auto& x : ps->items)
                if (x.is(JsonValue::Type::String) &&
                    want.count(lower(fs::path(json_utf8_to_wide(x.s)).lexically_normal().wstring())))
                    hit = true;
        if (hit) out.push_back(e->keys[k]);
    }
    return out;
}

// --- results file ---------------------------------------------------------------------------------------------------

std::string import_lines_json(const std::vector<ImportLine>& lines, const std::wstring& status) {
    JsonValue root;
    root.type = JsonValue::Type::Object;
    put(root, "format", json_string("aki-import-results"));
    put(root, "status", json_string(u8(status)));
    JsonValue arr;
    arr.type = JsonValue::Type::Array;
    for (const auto& l : lines) {
        JsonValue o;
        o.type = JsonValue::Type::Object;
        put(o, "item", json_string(u8(l.item)));
        put(o, "source", json_string(u8(l.source.wstring())));
        put(o, "outcome", json_number((double)(int)l.outcome));
        put(o, "name", json_string(u8(l.name)));
        put(o, "game", json_string(u8(l.gameKey)));
        put(o, "kind", json_string(u8(l.kind)));
        put(o, "tier", json_string(u8(l.tier)));
        put(o, "untested", json_bool(l.untested));
        put(o, "detail", json_string(u8(l.detail)));
        put(o, "needs", json_string(u8(l.needs)));
        put(o, "log", json_string(u8(l.log)));
        JsonValue pk;
        pk.type = JsonValue::Type::Array;
        for (const auto& p : l.packages) pk.items.push_back(json_string(u8(p.wstring())));
        put(o, "packages", pk);
        arr.items.push_back(std::move(o));
    }
    put(root, "lines", arr);
    return json_write(root);
}

bool import_lines_from_json(const std::string& text, std::vector<ImportLine>* lines, std::wstring* status) {
    JsonValue j;
    if (!json_parse(text, &j) || !j.is(JsonValue::Type::Object) || j.wstr("format") != L"aki-import-results") return false;
    if (status) *status = j.wstr("status");
    lines->clear();
    const JsonValue* arr = j.get("lines");
    if (!arr || !arr->is(JsonValue::Type::Array)) return false;
    for (const auto& o : arr->items) {
        if (!o.is(JsonValue::Type::Object)) continue;
        ImportLine l;
        l.item = o.wstr("item");
        l.source = o.wstr("source");
        int oc = o.integer("outcome", (int)ImportOutcome::Failed);
        if (oc < 0 || oc > (int)ImportOutcome::Stopped) oc = (int)ImportOutcome::Failed;
        l.outcome = (ImportOutcome)oc;
        l.name = o.wstr("name");
        l.gameKey = o.wstr("game");
        l.kind = o.wstr("kind");
        l.tier = o.wstr("tier");
        l.untested = o.boolean("untested", false);
        l.detail = o.wstr("detail");
        l.needs = o.wstr("needs");
        l.log = o.wstr("log");
        if (const JsonValue* pk = o.get("packages"); pk && pk->is(JsonValue::Type::Array))
            for (const auto& p : pk->items)
                if (p.is(JsonValue::Type::String)) l.packages.push_back(json_utf8_to_wide(p.s));
        lines->push_back(std::move(l));
    }
    return true;
}
