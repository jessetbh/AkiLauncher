#include "importbatch.h"

#include "json.h"

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <map>

namespace fs = std::filesystem;

static std::wstring g_pythonPath;  // pyfetch's answer, kept for the session (one pyfetch process per session at most)

static std::wstring widen8(const std::string& s) { return json_utf8_to_wide(s); }

std::wstring converter_missing(const fs::path& toolsDir) {
    std::error_code ec;
    if (toolsDir.empty() || !fs::is_directory(toolsDir, ec)) return L"the converter folder (tools\\romhack next to the launcher)";
    for (const wchar_t* f : {L"aki_import.py", L"pyfetch.ps1", L"patchfmt.py", L"make_patch_mod.py", L"derelink.py"})
        if (!fs::is_regular_file(toolsDir / f, ec)) return std::wstring(f) + L" in " + toolsDir.wstring();
    return {};
}

static std::string read_text(const fs::path& p, size_t maxBytes = 1 << 20) {
    std::ifstream f(p, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (s.size() > maxBytes) s = "...\n" + s.substr(s.size() - maxBytes);
    return s;
}

void ImportBatch::start(const ImportEnv& env, const std::vector<fs::path>& inputs, bool fromImportFolder) {
    env_ = env;
    inputs_ = inputs;
    fromImportFolder_ = fromImportFolder;
    undone_ = false;
    lines.clear();
    moved_.clear();
    reasonFiles_.clear();
    workerInputs_.clear();
    badLines_.clear();
    workerLog.clear();
    phase_ = Phase::Converting;
    status = L"Looking at " + std::to_wstring(inputs.size()) + L" item(s)...";
    // The launcher's own installers first: mods, texture packs (Rice/Jabo wrapped), packages, stock/PAL ROMs.
    for (const auto& in : inputs) {
        ImportLine l = env_.installLocal(in);
        l.source = in;
        if (l.viaWorker) {
            l.outcome = ImportOutcome::Waiting;
            l.placeholder = true;
            l.item = in.wstring();
            l.name = in.filename().wstring();
            l.detail = L"waiting for the converter";
            workerInputs_.push_back(in);
        }
        lines.push_back(std::move(l));
    }
    if (workerInputs_.empty()) {
        finish();
        return;
    }
    begin_worker();
}

void ImportBatch::fail_placeholders(const std::wstring& why) {
    for (auto& l : lines)
        if (l.placeholder && !import_outcome_final(l.outcome)) {
            l.outcome = ImportOutcome::Failed;
            l.detail = why;
            l.log = why + (workerLog.empty() ? L"" : L"\n\n" + workerLog);
        }
}

void ImportBatch::begin_worker() {
    std::wstring missing = converter_missing(env_.toolsDir);
    if (!missing.empty()) {
        fail_placeholders(L"ROM hacks, patches and Project64 bundles need the converter, which isn't installed (missing " +
                          missing + L")");
        finish();
        return;
    }
    if (!env_.python.empty()) g_pythonPath = env_.python.wstring();
    std::error_code ec;
    if (!g_pythonPath.empty() && fs::is_regular_file(g_pythonPath, ec)) {
        start_worker_process();
        return;
    }
    // pyfetch.ps1: Python's embeddable package, downloaded (sha256-checked) the first time, then found again.
    phase_ = Phase::FetchingPython;
    status = L"Getting the converter's Python (the first time only, about 11 MB)...";
    proc_ = std::make_unique<LineProcess>();
    fs::create_directories(env_.logDir, ec);
    std::wstring cmd = L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " +
                       quote_arg((env_.toolsDir / L"pyfetch.ps1").wstring());
    std::wstring err;
    if (!proc_->start(cmd, env_.toolsDir, "", env_.logDir / L"pyfetch-stderr.txt", &err)) {
        fail_placeholders(L"couldn't get Python for the converter: " + err);
        finish();
    }
}

void ImportBatch::start_worker_process() {
    phase_ = Phase::Converting;
    std::error_code ec;
    WorkerJob job = env_.job;
    job.paths = workerInputs_;
    job.out = env_.convertedDir;
    job.ledger = env_.convertedDir / L"import-ledger.json";
    job.work = env_.convertedDir / L".work";
    if (job.catalog.empty() && fs::is_regular_file(env_.toolsDir / L"catalog.json", ec)) job.catalog = env_.toolsDir / L"catalog.json";
    job.build = true;
    fs::create_directories(job.out, ec);
    fs::create_directories(env_.logDir, ec);
    stderrLog_ = env_.logDir / L"import-worker-stderr.txt";
    for (auto& l : lines)
        if (l.placeholder && l.outcome == ImportOutcome::Waiting) {
            l.outcome = ImportOutcome::Working;
            l.detail = L"converting";
        }
    status = L"Converting " + std::to_wstring(workerInputs_.size()) + L" item(s)...";
    proc_ = std::make_unique<LineProcess>();
    // python -I: isolated, so nothing in a download's folder can stand in for a module.
    std::wstring cmd = quote_arg(g_pythonPath) + L" -I " + quote_arg((env_.toolsDir / L"aki_import.py").wstring()) + L" worker";
    std::wstring err;
    if (!proc_->start(cmd, env_.toolsDir, worker_job_json(job), stderrLog_, &err)) {
        fail_placeholders(L"couldn't start the converter: " + err);
        finish();
    }
}

void ImportBatch::add_worker_line(ImportLine l) {
    fs::path src = import_source_of(l.item, workerInputs_);
    l.source = src;
    auto ph = std::find_if(lines.begin(), lines.end(),
                           [&](const ImportLine& x) { return x.placeholder && x.source == src && !src.empty(); });
    if (l.installAsIs) {
        ImportLine got = env_.installLocal(fs::path(l.item));
        got.source = src;
        got.item = l.item;
        if (got.viaWorker) {  // the worker said "a mod", the launcher couldn't install it
            got.viaWorker = false;
            got.outcome = ImportOutcome::Failed;
        }
        l = std::move(got);
    } else if (l.outcome == ImportOutcome::Converted) {
        env_.installPackages(l);
    }
    if (ph != lines.end()) {
        *ph = std::move(l);
        return;
    }
    auto last = lines.end();
    for (auto it = lines.begin(); it != lines.end(); ++it)
        if (it->source == src) last = it + 1;
    if (src.empty() || last == lines.end()) lines.push_back(std::move(l));
    else lines.insert(last, std::move(l));
}

void ImportBatch::tick() {
    if (!proc_) return;
    for (const std::string& raw : proc_->take_lines()) {
        if (phase_ == Phase::FetchingPython) {
            JsonValue j;
            if (json_parse(raw, &j) && j.is(JsonValue::Type::Object)) {
                if (j.boolean("ok", false) && !j.wstr("python").empty()) g_pythonPath = j.wstr("python");
                else workerLog = j.wstr("message");
            }
            continue;
        }
        WorkerEvent ev = parse_worker_line(raw);
        if (ev.type == WorkerEvent::Type::Item) add_worker_line(std::move(ev.line));
        else if (ev.type == WorkerEvent::Type::Done) engine = ev.engine;
        else if (!raw.empty()) badLines_.push_back(widen8(raw));
    }
    if (proc_->running()) {
        if (phase_ == Phase::Converting) status = L"Converting: " + import_progress(count_import(lines));
        return;
    }
    if (phase_ == Phase::FetchingPython) {
        proc_.reset();
        if (g_pythonPath.empty()) {
            fail_placeholders(L"the converter needs Python, which couldn't be fetched: " +
                              (workerLog.empty() ? std::wstring(L"see pyfetch-stderr.txt in ") + env_.logDir.wstring() : workerLog));
            finish();
        } else {
            start_worker_process();
        }
        return;
    }
    int code = proc_->exit_code();
    proc_.reset();
    workerLog = widen8(read_text(stderrLog_));
    for (const auto& b : badLines_) workerLog += L"\n" + b;
    fail_placeholders(code == 0 ? L"the converter found nothing it could import in it"
                                : L"the converter stopped without a result for this (exit code " + std::to_wstring(code) + L"): see [Log]");
    finish();
}

void ImportBatch::stop() {
    if (!proc_) return;
    proc_->stop();
    proc_.reset();
    for (auto& l : lines)
        if (!import_outcome_final(l.outcome)) {
            l.outcome = ImportOutcome::Stopped;
            l.detail = L"stopped before the converter got to it";
        }
    finish();
}

void ImportBatch::finish() {
    phase_ = Phase::Done;
    // Import folder entries: done -> Imported\, unsupported/failed -> Needs attention\ (+ a reason file); the rest stay.
    if (fromImportFolder_ && !env_.importDir.empty()) {
        std::error_code ec;
        for (const auto& in : inputs_) {
            std::vector<ImportOutcome> outs;
            std::vector<const ImportLine*> mine;
            for (const auto& l : lines)
                if (l.source == in) {
                    outs.push_back(l.outcome);
                    mine.push_back(&l);
                }
            ImportPlace place = import_place_for(outs);
            if (place == ImportPlace::Stay) continue;
            fs::path dir = env_.importDir / (place == ImportPlace::Imported ? kImportedFolder : kNeedsAttentionFolder);
            fs::create_directories(dir, ec);
            fs::path to = import_free_name(dir, in.filename());
            fs::rename(in, to, ec);
            if (ec) continue;  // in use: it stays and is tried again next time
            moved_.emplace_back(in, to);
            if (place == ImportPlace::NeedsAttention) {
                fs::path reason = to;
                reason += L".txt";
                std::ofstream f(reason, std::ios::binary | std::ios::trunc);
                f << import_reason_text(mine);
                reasonFiles_.push_back(reason);
            }
        }
    }
    status = import_summary(count_import(lines));
}

bool ImportBatch::retry_needs_rom(const std::vector<fs::path>& romDirs, std::wstring* errorOut) {
    if (busy()) {
        if (errorOut) *errorOut = L"the import is still running";
        return false;
    }
    std::vector<fs::path> again;
    for (const auto& l : lines)
        if (l.outcome == ImportOutcome::NeedsRom && !l.source.empty() &&
            std::find(again.begin(), again.end(), l.source) == again.end())
            again.push_back(l.source);
    if (again.empty()) {
        if (errorOut) *errorOut = L"nothing is waiting for a ROM";
        return false;
    }
    for (const auto& romDir : romDirs)
        if (std::find(env_.job.romDirs.begin(), env_.job.romDirs.end(), romDir) == env_.job.romDirs.end())
            env_.job.romDirs.push_back(romDir);
    // Each source's lines collapse into one placeholder again; the worker's lines replace it.
    std::vector<ImportLine> kept;
    for (auto& l : lines) {
        bool redo = std::find(again.begin(), again.end(), l.source) != again.end() && l.outcome == ImportOutcome::NeedsRom;
        if (!redo) kept.push_back(std::move(l));
    }
    lines = std::move(kept);
    for (const auto& src : again) {
        ImportLine ph;
        ph.source = src;
        ph.item = src.wstring();
        ph.name = src.filename().wstring();
        ph.placeholder = true;
        ph.viaWorker = true;
        ph.outcome = ImportOutcome::Waiting;
        ph.detail = L"waiting for the converter";
        lines.push_back(std::move(ph));
    }
    workerInputs_ = again;
    // A source that went to Needs attention or Imported would not be found: needs-ROM sources always stay put.
    begin_worker();
    return true;
}

bool ImportBatch::undo(std::wstring* message) {
    if (busy() || undone_) return false;
    std::wstring problems;
    int removed = 0;
    std::vector<fs::path> packages;
    for (auto& l : lines) {
        bool any = false;
        for (const auto& in : l.installed) {
            std::wstring err;
            if (env_.uninstall && env_.uninstall(in, &err)) {
                ++removed;
                any = true;
            } else {
                problems += (problems.empty() ? L"" : L"; ") + err;
            }
        }
        for (const auto& p : l.packages) packages.push_back(p);
        if (any || !l.packages.empty()) l.detail = L"undone: removed again";
        l.installed.clear();
    }
    std::error_code ec;
    fs::path ledger = env_.convertedDir / L"import-ledger.json";
    std::vector<std::string> keys = ledger_keys_for_packages(ledger, packages);
    std::wstring err;
    if (!keys.empty() && !ledger_remove(ledger, keys, &err)) problems += (problems.empty() ? L"" : L"; ") + err;
    for (const auto& p : packages) fs::remove(p, ec);
    for (auto it = moved_.rbegin(); it != moved_.rend(); ++it) {
        fs::path back = fs::exists(it->first, ec) ? import_free_name(it->first.parent_path(), it->first.filename()) : it->first;
        fs::rename(it->second, back, ec);
    }
    for (const auto& r : reasonFiles_) fs::remove(r, ec);
    moved_.clear();
    reasonFiles_.clear();
    undone_ = true;
    status = L"Undone: " + std::to_wstring(removed) + L" install(s) removed (saves stay), " + std::to_wstring(packages.size()) +
             L" converted package(s) deleted, the Import folder put back as it was." +
             (problems.empty() ? L"" : L" Not everything: " + problems);
    if (message) *message = status;
    return problems.empty();
}

void ImportBatch::show(std::vector<ImportLine> l, const std::wstring& st) {
    stop();
    lines = std::move(l);
    status = st.empty() ? import_summary(count_import(lines)) : st;
    phase_ = Phase::Done;
    undone_ = false;
}
