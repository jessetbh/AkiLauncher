#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// A child process whose stdout is read line by line on a thread (aki-roadmap#228): pyfetch.ps1 (one JSON line) and the
// aki_import.py worker (one JSON line per item). stdin gets one text then closes; stderr goes to a log file. No
// console window. One process per batch: never start one per item (aki-roadmap#111, process churn leaks pool).
class LineProcess {
public:
    LineProcess();
    ~LineProcess();  // stops a process still running (it is our own worker, never a game)
    LineProcess(const LineProcess&) = delete;
    LineProcess& operator=(const LineProcess&) = delete;

    // commandLine: the full line (quoted exe first). false + errorOut if it couldn't start.
    bool start(const std::wstring& commandLine, const std::filesystem::path& cwd, const std::string& stdinText,
               const std::filesystem::path& stderrLog, std::wstring* errorOut);
    // The complete lines read since the last call (without the line ends).
    std::vector<std::string> take_lines();
    bool running() const;      // still running, or lines still to be read
    int exit_code() const;     // once !running(); -1 if killed or unknown
    void stop();               // TerminateProcess (our worker only) and wait

private:
    struct State;
    std::shared_ptr<State> s_;
};

// A command-line argument quoted for CreateProcess (the MSVC rules: backslashes before a quote doubled).
std::wstring quote_arg(const std::wstring& a);
