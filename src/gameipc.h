#pragma once
#include <windows.h>

#include <string>
#include <vector>

#include "json.h"

// Launcher <-> game link (aki-roadmap#21, the launcher half; behind --dev-ipc / AKI_DEV_IPC=1 for now).
//
// Transport: one named pipe per launch, made by the launcher before it starts the game. Its name and a one-time
// token reach the game in its environment, the way WCW_AUTOBOOT already does:
//   AKI_LAUNCHER_PIPE   \\.\pipe\aki-launcher-<launcher pid>-<random>
//   AKI_LAUNCHER_TOKEN  32 hex digits
// A game without those variables (started by hand, or by an older launcher) runs as before. The pipe takes one
// client, local only (PIPE_REJECT_REMOTE_CLIENTS), with the launcher's default security (the same user).
//
// Messages: UTF-8 JSON objects, one per line ("\n"; compact, so no newline inside one), at most kIpcMaxLine bytes,
// each with a "type". Either side ignores types it doesn't know, and fields it doesn't know.
//   game -> launcher  {"type":"hello","protocol":1,"token":"<AKI_LAUNCHER_TOKEN>","game":"<mod game id>",
//                      "version":"<port version>","pid":<pid>}            first, within kIpcHelloMs of connecting
//   launcher -> game  {"type":"welcome","protocol":1}                       the hello was good
//                     {"type":"error","message":"..."} then the pipe closes   it wasn't (token, protocol, order)
//   game -> launcher  {"type":"state","state":"boot"|"menu"|"match"|"results"}   when that changes
//                     {"type":"match_end","result":{...}}                   a match is over (fields: the game's to add)
//                     {"type":"bye"}                                         the game is closing
//   launcher -> game  {"type":"quit"}                                       please close (saves first), like quick-back
//                     {"type":"ping","id":N}  /  game -> launcher {"type":"pong","id":N}
// Protocol 1 is this list. A launcher that gets a hello with another protocol answers error and closes; new message
// types don't need a new protocol number (they're ignored where unknown).

constexpr int kIpcProtocol = 1;
constexpr size_t kIpcMaxLine = 16 * 1024;
constexpr DWORD kIpcHelloMs = 10000;

// Complete lines taken off the front of buf (the rest stays for the next read). False when buf has more than
// kIpcMaxLine bytes without a newline (the peer is broken: close).
bool ipc_take_lines(std::string& buf, std::vector<std::string>* lines);

// A hello's problem ("" = fine): not a hello, wrong token, other protocol.
std::string ipc_check_hello(const JsonValue& msg, const std::string& token);

// {"type": type} plus fields, as one line (with its "\n").
std::string ipc_line(const char* type, const std::vector<std::pair<std::string, JsonValue>>& fields = {});

enum class LinkState { Closed, Listening, Connected, Ready };

// The launcher's end. Single-threaded: poll() from the frame loop never blocks (overlapped I/O).
class GameLink {
public:
    GameLink() = default;
    ~GameLink() { close(); }
    GameLink(const GameLink&) = delete;
    GameLink& operator=(const GameLink&) = delete;

    // A new pipe and token; false (with *err) if the pipe can't be made.
    bool open(std::wstring* err);
    const std::wstring& pipe_name() const { return name_; }
    const std::string& token() const { return token_; }
    // Accepts the game, reads what it sent, answers its hello. Messages after the hello come out of take().
    void poll();
    std::vector<JsonValue> take();
    // A message to the game (Ready only); false when it can't be written within a second.
    bool send(const std::string& line);
    void close();

    LinkState state() const { return state_; }
    const JsonValue& hello() const { return hello_; }  // the game's hello (Ready)
    const std::string& last_error() const { return error_; }
    void set_hello_timeout(DWORD ms) { helloMs_ = ms; }  // tests

private:
    void fail(const std::string& why, bool tellGame);
    bool start_read();

    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    OVERLAPPED connectOv_{}, readOv_{};
    bool reading_ = false;
    char readBuf_[4096];
    std::string buf_;
    std::wstring name_;
    std::string token_;
    LinkState state_ = LinkState::Closed;
    ULONGLONG connectedTick_ = 0;
    DWORD helloMs_ = kIpcHelloMs;
    JsonValue hello_;
    std::vector<JsonValue> inbox_;
    std::string error_;
};
