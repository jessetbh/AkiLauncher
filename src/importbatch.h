#pragma once
#include "importer.h"
#include "importrun.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// One import batch (aki-roadmap#228, phase 2): what the Import screen runs. Items the launcher can install itself go
// first (installLocal); the rest go to ONE aki_import.py worker (python fetched by pyfetch.ps1 the first time), whose
// JSON lines become result lines as they arrive; converted packages are installed through the #154 installer
// (installPackage). When the batch ends, Import-folder entries move to Imported\ or Needs attention\. Needs ROM lines
// can be retried with a folder of ROMs, and the whole batch undone. Call tick() every frame (or in a loop, headless).

struct ImportEnv {
    std::filesystem::path importDir;     // the Import folder (entries from it move when done)
    std::filesystem::path convertedDir;  // Converted\ (packages, ledger, .work)
    std::filesystem::path toolsDir;      // aki_import.py, pyfetch.ps1, ... ("" = not installed)
    std::filesystem::path python;        // a python.exe to use instead of pyfetch ("" = pyfetch)
    std::filesystem::path logDir;        // where the worker's stderr log goes
    WorkerJob job;                       // roms, rom dirs, game hint (paths/ledger/out/work are filled in)
    // A picked file/folder installed by the launcher (Phase 0's installers); viaWorker lines go to the worker.
    std::function<ImportLine(const std::filesystem::path&)> installLocal;
    // A Converted line's packages installed: sets outcome (Converted / Failed / HaveIt), detail, installed.
    std::function<void(ImportLine&)> installPackages;
    // Undo of one installed thing; false + message on failure.
    std::function<bool(const ImportLine::Installed&, std::wstring*)> uninstall;
};

// The converter's files in a tools folder: aki_import.py (the worker) and pyfetch.ps1. "" when complete, else what's missing.
std::wstring converter_missing(const std::filesystem::path& toolsDir);

class ImportBatch {
public:
    enum class Phase { Idle, FetchingPython, Converting, Done };

    void start(const ImportEnv& env, const std::vector<std::filesystem::path>& inputs, bool fromImportFolder);
    void tick();
    void stop();
    // Runs the worker again for the Needs ROM lines, with these folders added to where it finds stock ROMs.
    bool retry_needs_rom(const std::vector<std::filesystem::path>& romDirs, std::wstring* errorOut);
    // Removes what this batch installed (saves stay), the packages it wrote and their ledger entries, and moves the
    // Import folder's entries back. Lines become Stopped-looking "Undone" (outcome Stopped, detail says so).
    bool undo(std::wstring* message);

    // Shows finished lines (a results file, screenshots) without running anything.
    void show(std::vector<ImportLine> l, const std::wstring& status);

    bool busy() const { return phase_ == Phase::FetchingPython || phase_ == Phase::Converting; }
    Phase phase() const { return phase_; }
    bool undone() const { return undone_; }
    std::vector<ImportLine> lines;
    std::wstring status;      // one line: what's happening, or the summary when done
    std::wstring workerLog;   // stderr of the last worker run (for [Log] on a line without its own)
    std::string engine;       // the worker's engine version

private:
    void begin_worker();
    void start_worker_process();
    void finish();
    void fail_placeholders(const std::wstring& why);
    void add_worker_line(ImportLine l);
    void reinstall_have_it(ImportLine& l);

    ImportEnv env_;
    Phase phase_ = Phase::Idle;
    std::vector<std::filesystem::path> inputs_, workerInputs_;
    bool fromImportFolder_ = false, undone_ = false;
    std::unique_ptr<LineProcess> proc_;
    std::filesystem::path stderrLog_;
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> moved_;  // Import entry -> where it went
    std::vector<std::filesystem::path> reasonFiles_;
    std::vector<std::wstring> badLines_;  // stdout lines that weren't JSON
};
