#include "importrun.h"

#include <windows.h>

#include <atomic>
#include <mutex>
#include <thread>

struct LineProcess::State {
    std::mutex m;
    std::vector<std::string> lines;
    std::string partial;
    std::atomic<bool> reading{false};
    std::atomic<int> exitCode{-1};
    HANDLE process = nullptr;
    HANDLE out = nullptr;
    std::thread reader;
};

LineProcess::LineProcess() : s_(std::make_shared<State>()) {}

LineProcess::~LineProcess() {
    stop();
    if (s_->reader.joinable()) s_->reader.join();
    if (s_->process) CloseHandle(s_->process);
}

std::wstring quote_arg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            ++slashes;
        } else if (c == L'"') {
            out.append(slashes * 2 + 1, L'\\');
            slashes = 0;
        } else {
            out.append(slashes, L'\\');
            slashes = 0;
        }
        if (c != L'\\') out += c;
    }
    out.append(slashes * 2, L'\\');
    return out + L"\"";
}

bool LineProcess::start(const std::wstring& commandLine, const std::filesystem::path& cwd, const std::string& stdinText,
                        const std::filesystem::path& stderrLog, std::wstring* errorOut) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE outR = nullptr, outW = nullptr, inR = nullptr, inW = nullptr;
    if (!CreatePipe(&outR, &outW, &sa, 0) || !CreatePipe(&inR, &inW, &sa, 0)) {
        if (errorOut) *errorOut = L"can't create a pipe";
        return false;
    }
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    HANDLE err = CreateFileW(stderrLog.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (err == INVALID_HANDLE_VALUE) err = nullptr;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inR;
    si.hStdOutput = outW;
    si.hStdError = err ? err : outW;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = commandLine;
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                             cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    CloseHandle(outW);
    CloseHandle(inR);
    if (err) CloseHandle(err);
    if (!ok) {
        CloseHandle(outR);
        CloseHandle(inW);
        if (errorOut) *errorOut = L"can't start " + commandLine + L" (error " + std::to_wstring(GetLastError()) + L")";
        return false;
    }
    CloseHandle(pi.hThread);
    DWORD written = 0;
    if (!stdinText.empty()) WriteFile(inW, stdinText.data(), (DWORD)stdinText.size(), &written, nullptr);
    CloseHandle(inW);
    s_->process = pi.hProcess;
    s_->out = outR;
    s_->reading = true;
    std::shared_ptr<State> st = s_;
    s_->reader = std::thread([st] {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(st->out, buf, sizeof(buf), &n, nullptr) && n > 0) {
            std::lock_guard lock(st->m);
            st->partial.append(buf, n);
            size_t nl;
            while ((nl = st->partial.find('\n')) != std::string::npos) {
                std::string line = st->partial.substr(0, nl);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                st->lines.push_back(std::move(line));
                st->partial.erase(0, nl + 1);
            }
        }
        WaitForSingleObject(st->process, INFINITE);
        DWORD code = 0;
        if (GetExitCodeProcess(st->process, &code)) st->exitCode = (int)code;
        {
            std::lock_guard lock(st->m);
            if (!st->partial.empty()) st->lines.push_back(std::move(st->partial));
            st->partial.clear();
        }
        CloseHandle(st->out);
        st->out = nullptr;
        st->reading = false;
    });
    return true;
}

std::vector<std::string> LineProcess::take_lines() {
    std::lock_guard lock(s_->m);
    std::vector<std::string> v;
    v.swap(s_->lines);
    return v;
}

bool LineProcess::running() const {
    if (s_->reading) return true;
    std::lock_guard lock(s_->m);
    return !s_->lines.empty();
}

int LineProcess::exit_code() const { return s_->exitCode; }

void LineProcess::stop() {
    if (s_->process && s_->reading) {
        TerminateProcess(s_->process, 1);
        WaitForSingleObject(s_->process, 5000);
    }
}
