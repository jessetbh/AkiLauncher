// Checks for the import batch (src/importbatch.cpp, aki-roadmap#228): local installs, Import folder moves and reason
// files, Undo, a missing converter, and a stub worker (a few lines of Python that print what aki_import.py worker
// prints) for streaming, package installs, Needs ROM and Find it... retry. The stub part needs a python.exe
// (AKI_TEST_PYTHON, else Program Files\Python3*\python.exe); without one it's skipped with a note.
#include "../src/importbatch.h"
#include "../src/json.h"
#include "check.h"

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static void put(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
}

static bool has(const std::wstring& s, const wchar_t* part) { return s.find(part) != std::wstring::npos; }

static ImportEnv base_env(const fs::path& dir, std::vector<std::wstring>* uninstalled) {
    ImportEnv env;
    env.importDir = dir / L"Import";
    env.convertedDir = dir / L"Converted";
    env.logDir = dir / L"Converted" / L".logs";
    env.toolsDir = dir / L"no-tools";
    env.installLocal = [](const fs::path& p) {
        ImportLine l;
        l.item = p.wstring();
        l.name = p.filename().wstring();
        std::wstring n = p.filename().wstring();
        if (n.find(L"pack") != std::wstring::npos) {
            l.outcome = ImportOutcome::Installed;
            l.kind = L"texture pack";
            l.installed.push_back({L"nomercy", "", p});
        } else if (n.find(L"broken") != std::wstring::npos) {
            l.outcome = ImportOutcome::Failed;
            l.detail = L"broken";
        } else {
            l.viaWorker = true;
        }
        return l;
    };
    env.installPackages = [](ImportLine& l) {
        for (const auto& p : l.packages) {
            if (p.stem().wstring().find(L"kept") != std::wstring::npos) {  // install_converted: already installed
                l.outcome = ImportOutcome::HaveIt;
                continue;
            }
            l.installed.push_back({L"nomercy", "pkg_" + p.stem().string(), {}});
        }
    };
    env.uninstall = [uninstalled](const ImportLine::Installed& in, std::wstring*) {
        uninstalled->push_back(in.packageId.empty() ? in.path.filename().wstring() : std::wstring(in.packageId.begin(), in.packageId.end()));
        return true;
    };
    return env;
}

static void test_local_batch(const fs::path& dir) {
    std::vector<std::wstring> un;
    ImportEnv env = base_env(dir, &un);
    put(env.importDir / L"a pack.zip", "x");
    put(env.importDir / L"broken.nrm", "x");
    put(env.importDir / L"hack.z64", "x");  // the converter isn't there: failed
    ImportBatch b;
    b.start(env, import_folder_items(env.importDir), true);
    CHECK(!b.busy() && b.phase() == ImportBatch::Phase::Done && b.lines.size() == 3);
    CHECK(b.lines[0].outcome == ImportOutcome::Installed && b.lines[1].outcome == ImportOutcome::Failed);
    CHECK(b.lines[2].outcome == ImportOutcome::Failed && has(b.lines[2].detail, L"converter") && b.lines[2].placeholder);
    CHECK(fs::exists(env.importDir / L"Imported" / L"a pack.zip") && !fs::exists(env.importDir / L"a pack.zip"));
    CHECK(fs::exists(env.importDir / L"Needs attention" / L"broken.nrm") && fs::exists(env.importDir / L"Needs attention" / L"broken.nrm.txt"));
    CHECK(fs::exists(env.importDir / L"Needs attention" / L"hack.z64"));
    CHECK(import_folder_items(env.importDir).empty());
    CHECK(has(b.status, L"1 installed") && has(b.status, L"2 failed"));
    std::wstring msg;
    CHECK(b.undo(&msg) && b.undone() && un.size() == 1 && un[0] == L"a pack.zip");
    CHECK(fs::exists(env.importDir / L"a pack.zip") && fs::exists(env.importDir / L"broken.nrm") &&
          !fs::exists(env.importDir / L"Needs attention" / L"broken.nrm.txt") && import_folder_items(env.importDir).size() == 3);
    CHECK(!b.undo(&msg));  // once

    // Picked files (not the Import folder) never move.
    put(dir / L"picked" / L"other pack.zip", "x");
    ImportBatch p;
    p.start(env, {dir / L"picked" / L"other pack.zip"}, false);
    CHECK(fs::exists(dir / L"picked" / L"other pack.zip") && p.lines.size() == 1 && p.lines[0].outcome == ImportOutcome::Installed);
}

static fs::path find_python() {
    wchar_t buf[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"AKI_TEST_PYTHON", buf, MAX_PATH)) return buf;
    std::error_code ec;
    for (const wchar_t* root : {L"C:\\Program Files", L"C:\\Program Files (x86)"})
        for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
            std::wstring n = it->path().filename().wstring();
            if (n.rfind(L"Python3", 0) == 0 && fs::is_regular_file(it->path() / L"python.exe", ec)) return it->path() / L"python.exe";
        }
    return {};
}

// What aki_import.py worker prints, for the paths in the job: hack.z64 -> converted (a package under out), patch.bps ->
// needs-rom until rom_dirs has a folder, folder "bundle" -> two items, then done.
static const char* kStubWorker = R"PY(
import json, os, sys
job = json.loads(sys.stdin.read())
def emit(o):
    sys.stdout.write(json.dumps(o) + "\n"); sys.stdout.flush()
for p in job["paths"]:
    name = os.path.basename(p)
    if name == "hack.z64":
        pk = os.path.join(job["out"], "nomercy", "hack-1.0.0.zip")
        os.makedirs(os.path.dirname(pk), exist_ok=True)
        open(pk, "wb").write(b"PK")
        emit(dict(event="item", item=p, status="converted", kind="rom", game="nomercy", name="Hack", aki_tier="untested", converter="make_patch_mod", package=pk))
    elif name == "patch.bps":
        if job.get("rom_dirs"):
            emit(dict(event="item", item=p, status="converted", kind="patch", game="nomercy", name="Patched", aki_tier="verified", converter="derelink", package=os.path.join(job["out"], "nomercy", "patched-1.0.0.zip")))
        else:
            emit(dict(event="item", item=p, status="needs-rom", kind="patch", needs="WWF No Mercy (USA) 1.0"))
    elif name in ("again.z64", "kept.z64"):
        # aki-roadmap#272: imported before; the ledger's package is still in Converted.
        pk = os.path.join(job["out"], "nomercy", name.replace(".z64", "-1.0.0.zip"))
        os.makedirs(os.path.dirname(pk), exist_ok=True)
        open(pk, "wb").write(b"PK")
        emit(dict(event="item", item=p, status="have-it", kind="rom", game="nomercy", name=name, why="imported before (2026-11-09T14:01:58)", package=pk, packages=[pk]))
    elif name == "bundle":
        emit(dict(event="item", item=os.path.join(p, "a.z64"), status="unsupported", why="PAL version"))
        emit(dict(event="item", item=p + "!x.ips", status="failed", why="bad patch"))
print("not json")
emit(dict(event="done", counts={}, engine="stub"))
)PY";

static void test_stub_worker(const fs::path& dir) {
    fs::path py = find_python();
    if (py.empty()) {
        printf("note: importbatch stub-worker checks skipped (no python.exe; set AKI_TEST_PYTHON)\n");
        return;
    }
    std::vector<std::wstring> un;
    ImportEnv env = base_env(dir, &un);
    env.toolsDir = dir / L"tools";
    for (const char* f : {"pyfetch.ps1", "patchfmt.py", "make_patch_mod.py", "derelink.py"}) put(env.toolsDir / f, "");
    put(env.toolsDir / L"aki_import.py", std::string("import sys\nif sys.argv[1:] == ['worker']:\n") +
                                             "    exec(open(__file__.replace('aki_import.py', 'stub.py')).read())\n");
    put(env.toolsDir / L"stub.py", kStubWorker);
    env.python = py;
    put(env.importDir / L"hack.z64", "x");
    put(env.importDir / L"patch.bps", "x");
    put(env.importDir / L"bundle" / L"a.z64", "x");
    put(env.importDir / L"z pack", "x");
    put(env.importDir / L"again.z64", "x");
    put(env.importDir / L"kept.z64", "x");
    ImportBatch b;
    b.start(env, import_folder_items(env.importDir), true);
    CHECK(b.busy());
    for (int i = 0; i < 300 && b.busy(); ++i) {
        b.tick();
        Sleep(50);
    }
    CHECK(!b.busy() && b.engine == "stub");
    // again, bundle -> 2 lines (in its place), hack, kept, patch, z pack (local)
    CHECK(b.lines.size() == 7);
    if (b.lines.size() == 7) {
        // aki-roadmap#272: imported before, its conversion removed since: installed again from the ledger's package.
        CHECK(b.lines[0].outcome == ImportOutcome::Installed && b.lines[0].installed.size() == 1 &&
              b.lines[0].installed[0].packageId == "pkg_again-1.0.0" && b.lines[0].packages.empty() &&
              has(b.lines[0].detail, L"installed again") && b.lines[0].kind == L"conversion");
        CHECK(b.lines[1].outcome == ImportOutcome::Unsupported && b.lines[1].source.filename() == L"bundle");
        CHECK(b.lines[2].outcome == ImportOutcome::Failed && b.lines[2].source.filename() == L"bundle");
        CHECK(b.lines[3].outcome == ImportOutcome::Converted && b.lines[3].untested && b.lines[3].installed.size() == 1 &&
              b.lines[3].installed[0].packageId == "pkg_hack-1.0.0");
        // ... and one still installed stays Have it, as before.
        CHECK(b.lines[4].outcome == ImportOutcome::HaveIt && b.lines[4].installed.empty() && has(b.lines[4].detail, L"imported before"));
        CHECK(b.lines[5].outcome == ImportOutcome::NeedsRom && b.lines[5].source.filename() == L"patch.bps");
        CHECK(b.lines[6].outcome == ImportOutcome::Installed);
    }
    CHECK(fs::exists(env.importDir / L"Imported" / L"hack.z64") && fs::exists(env.importDir / L"Needs attention" / L"bundle"));
    CHECK(fs::exists(env.importDir / L"patch.bps"));  // waits for its ROM
    // Find it...: the needs-ROM line goes back to the worker with the folder.
    std::wstring err;
    CHECK(b.retry_needs_rom({dir / L"roms"}, &err) && b.busy());
    for (int i = 0; i < 300 && b.busy(); ++i) {
        b.tick();
        Sleep(50);
    }
    CHECK(b.lines.size() == 7 && b.lines.back().outcome == ImportOutcome::Converted && b.lines.back().tier == L"Verified");
    CHECK(!b.retry_needs_rom({dir / L"roms"}, &err));  // nothing left waiting
    // Undo: both packages' installs removed, the stub's package deleted.
    std::wstring msg;
    b.undo(&msg);
    CHECK(un.size() >= 3 && !fs::exists(env.convertedDir / L"nomercy" / L"hack-1.0.0.zip") && fs::exists(env.importDir / L"hack.z64"));
    // The reinstall is undone, but an earlier import's packages are never deleted (aki-roadmap#272).
    CHECK(std::find(un.begin(), un.end(), L"pkg_again-1.0.0") != un.end());
    CHECK(fs::exists(env.convertedDir / L"nomercy" / L"again-1.0.0.zip") && fs::exists(env.convertedDir / L"nomercy" / L"kept-1.0.0.zip"));
}

void run_importbatch_tests(const fs::path& tmp) {
    std::error_code ec;
    fs::remove_all(tmp, ec);
    test_local_batch(tmp / L"local");
    test_stub_worker(tmp / L"stub");
    CHECK(has(converter_missing(tmp / L"nowhere"), L"converter folder"));
    CHECK(quote_arg(L"C:\\a b\\") == L"\"C:\\a b\\\\\"" && quote_arg(L"plain") == L"plain" && quote_arg(L"a\"b") == L"\"a\\\"b\"");
}
