#include "gameipc.h"

#include <bcrypt.h>

#include <cstdio>

bool ipc_take_lines(std::string& buf, std::vector<std::string>* lines) {
    size_t start = 0;
    for (size_t nl; (nl = buf.find('\n', start)) != std::string::npos; start = nl + 1) {
        std::string line = buf.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();  // a peer that writes CRLF
        if (!line.empty()) lines->push_back(std::move(line));
    }
    buf.erase(0, start);
    return buf.size() <= kIpcMaxLine;
}

std::string ipc_check_hello(const JsonValue& msg, const std::string& token) {
    const JsonValue* type = msg.get("type");
    if (!msg.is(JsonValue::Type::Object) || !type || !type->is(JsonValue::Type::String) || type->s != "hello")
        return "the first message must be a hello";
    const JsonValue* tok = msg.get("token");
    if (!tok || !tok->is(JsonValue::Type::String) || tok->s != token) return "wrong token";
    int protocol = msg.integer("protocol", 0);
    if (protocol != kIpcProtocol)
        return "protocol " + std::to_string(protocol) + " (this launcher speaks " + std::to_string(kIpcProtocol) + ")";
    return "";
}

std::string ipc_line(const char* type, const std::vector<std::pair<std::string, JsonValue>>& fields) {
    JsonValue v;
    v.type = JsonValue::Type::Object;
    v.keys.push_back("type");
    v.items.push_back(json_string(type));
    for (const auto& [k, val] : fields) {
        v.keys.push_back(k);
        v.items.push_back(val);
    }
    return json_write(v, true) + "\n";
}

static std::string random_hex(size_t bytes) {
    std::vector<unsigned char> b(bytes);
    BCryptGenRandom(nullptr, b.data(), (ULONG)b.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    std::string s;
    char h[3];
    for (unsigned char c : b) {
        snprintf(h, sizeof(h), "%02x", c);
        s += h;
    }
    return s;
}

bool GameLink::open(std::wstring* err) {
    close();
    token_ = random_hex(16);
    std::string nonce = random_hex(8);
    name_ = L"\\\\.\\pipe\\aki-launcher-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::wstring(nonce.begin(), nonce.end());
    pipe_ = CreateNamedPipeW(name_.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                             PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096,
                             0, nullptr);
    if (pipe_ == INVALID_HANDLE_VALUE) {
        if (err) *err = L"CreateNamedPipe failed (error " + std::to_wstring(GetLastError()) + L")";
        return false;
    }
    connectOv_ = OVERLAPPED{};
    readOv_ = OVERLAPPED{};
    connectOv_.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    readOv_.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    state_ = LinkState::Listening;
    error_.clear();
    BOOL ok = ConnectNamedPipe(pipe_, &connectOv_);
    DWORD e = ok ? 0 : GetLastError();
    if (ok || e == ERROR_PIPE_CONNECTED) {
        SetEvent(connectOv_.hEvent);  // already there (it connected between Create and Connect)
    } else if (e != ERROR_IO_PENDING) {
        fail("ConnectNamedPipe failed (error " + std::to_string(e) + ")", false);
        if (err) *err = L"ConnectNamedPipe failed";
        return false;
    }
    return true;
}

void GameLink::fail(const std::string& why, bool tellGame) {
    error_ = why;
    if (tellGame && pipe_ != INVALID_HANDLE_VALUE) {
        std::string line = ipc_line("error", {{"message", json_string(why)}});
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD n = 0;
        if (!WriteFile(pipe_, line.data(), (DWORD)line.size(), nullptr, &ov) && GetLastError() == ERROR_IO_PENDING &&
            WaitForSingleObject(ov.hEvent, 500) != WAIT_OBJECT_0)
            CancelIoEx(pipe_, &ov);
        GetOverlappedResult(pipe_, &ov, &n, TRUE);
        CloseHandle(ov.hEvent);
        FlushFileBuffers(pipe_);
    }
    close();
    error_ = why;
}

bool GameLink::start_read() {
    ResetEvent(readOv_.hEvent);
    if (ReadFile(pipe_, readBuf_, sizeof(readBuf_), nullptr, &readOv_) || GetLastError() == ERROR_IO_PENDING) {
        reading_ = true;
        return true;
    }
    return false;
}

void GameLink::poll() {
    if (state_ == LinkState::Closed) return;
    if (state_ == LinkState::Listening) {
        DWORD n = 0;
        if (!GetOverlappedResult(pipe_, &connectOv_, &n, FALSE)) {
            DWORD e = GetLastError();
            if (e == ERROR_IO_INCOMPLETE) return;  // nobody yet
            if (e != ERROR_PIPE_CONNECTED) {
                fail("connect failed (error " + std::to_string(e) + ")", false);
                return;
            }
        }
        state_ = LinkState::Connected;
        connectedTick_ = GetTickCount64();
        if (!start_read()) {
            fail("the game hung up", false);
            return;
        }
    }
    // Everything the game has sent so far (several reads may be done between two frames).
    for (int guard = 0; reading_ && guard < 64; ++guard) {
        DWORD n = 0;
        if (!GetOverlappedResult(pipe_, &readOv_, &n, FALSE)) {
            DWORD e = GetLastError();
            if (e == ERROR_IO_INCOMPLETE) break;
            if (e != ERROR_MORE_DATA) {
                reading_ = false;
                fail(e == ERROR_BROKEN_PIPE ? "the game closed the link" : "read failed (error " + std::to_string(e) + ")",
                     false);
                return;
            }
        }
        buf_.append(readBuf_, n);
        std::vector<std::string> lines;
        if (!ipc_take_lines(buf_, &lines)) {
            fail("a message longer than " + std::to_string(kIpcMaxLine) + " bytes", true);
            return;
        }
        for (const auto& l : lines) {
            JsonValue msg;
            std::string perr;
            if (!json_parse(l, &msg, &perr) || !msg.is(JsonValue::Type::Object)) {
                if (state_ != LinkState::Ready) {
                    fail("the first message isn't JSON", true);
                    return;
                }
                continue;  // a bad line later on is dropped, the link stays
            }
            if (state_ == LinkState::Connected) {
                std::string why = ipc_check_hello(msg, token_);
                if (!why.empty()) {
                    fail(why, true);
                    return;
                }
                hello_ = msg;
                state_ = LinkState::Ready;
                send(ipc_line("welcome", {{"protocol", json_number(kIpcProtocol)}}));
                continue;
            }
            inbox_.push_back(std::move(msg));
        }
        if (state_ == LinkState::Closed || !start_read()) {
            reading_ = false;
            if (state_ != LinkState::Closed) fail("the game closed the link", false);
            return;
        }
    }
    if (state_ == LinkState::Connected && GetTickCount64() - connectedTick_ > helloMs_)
        fail("no hello within " + std::to_string(helloMs_) + " ms", true);
}

std::vector<JsonValue> GameLink::take() {
    std::vector<JsonValue> out;
    out.swap(inbox_);
    return out;
}

bool GameLink::send(const std::string& line) {
    if (state_ != LinkState::Ready || pipe_ == INVALID_HANDLE_VALUE) return false;
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD n = 0;
    bool ok = WriteFile(pipe_, line.data(), (DWORD)line.size(), nullptr, &ov) != 0;
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, 1000) == WAIT_OBJECT_0) ok = true;
        else CancelIoEx(pipe_, &ov);  // a game that stopped reading: drop the message, keep the frame loop going
    }
    ok = GetOverlappedResult(pipe_, &ov, &n, TRUE) && ok && n == line.size();
    CloseHandle(ov.hEvent);
    return ok;
}

void GameLink::close() {
    if (pipe_ != INVALID_HANDLE_VALUE) {
        CancelIoEx(pipe_, nullptr);
        DWORD n;
        if (reading_) GetOverlappedResult(pipe_, &readOv_, &n, TRUE);
        if (state_ == LinkState::Listening) GetOverlappedResult(pipe_, &connectOv_, &n, TRUE);
        DisconnectNamedPipe(pipe_);
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
    if (connectOv_.hEvent) CloseHandle(connectOv_.hEvent);
    if (readOv_.hEvent) CloseHandle(readOv_.hEvent);
    connectOv_ = OVERLAPPED{};
    readOv_ = OVERLAPPED{};
    reading_ = false;
    buf_.clear();
    state_ = LinkState::Closed;
}
