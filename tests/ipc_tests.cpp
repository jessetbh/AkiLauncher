// Checks for the launcher <-> game link (src/gameipc.cpp, aki-roadmap#21): the line framing, the hello check, and a
// GameLink against a fake game, a client thread that does what a port's side is meant to (README "Game link").
#include "../src/gameipc.h"
#include "../src/launch.h"
#include "check.h"

#include <windows.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// The fake game: connects, writes lines, and reads what the launcher answers (until it hangs up or quit arrives).
struct FakeGame {
    HANDLE h = INVALID_HANDLE_VALUE;
    std::string got;  // everything the launcher sent

    bool connect(const std::wstring& name) {
        for (int i = 0; i < 50 && h == INVALID_HANDLE_VALUE; ++i) {
            h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE) Sleep(20);
        }
        return h != INVALID_HANDLE_VALUE;
    }
    bool write(const std::string& s) {
        DWORD n = 0;
        return WriteFile(h, s.data(), (DWORD)s.size(), &n, nullptr) && n == s.size();
    }
    // Reads until `want` shows up in what arrived, or the pipe closes. True if it showed up.
    bool read_until(const std::string& want) {
        char buf[512];
        while (got.find(want) == std::string::npos) {
            DWORD n = 0;
            if (!ReadFile(h, buf, sizeof(buf), &n, nullptr) || n == 0) return false;
            got.append(buf, n);
        }
        return true;
    }
    void close() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
    }
    ~FakeGame() { close(); }
};

static std::string hello(const std::string& token, int protocol = kIpcProtocol) {
    return std::string(R"({"type":"hello","protocol":)") + std::to_string(protocol) + R"(,"token":")" + token +
           R"(","game":"wwfnomercy","version":"0.3.0","pid":1234})" + "\n";
}

// Polls the link (as the frame loop does) until done() or 5 s.
static bool pump(GameLink& link, const std::function<bool()>& done) {
    for (int i = 0; i < 500; ++i) {
        link.poll();
        if (done()) return true;
        Sleep(10);
    }
    return false;
}

static void test_framing() {
    std::string buf = "{\"type\":\"a\"}\n\r\n{\"type\":\"b\"}\r\n{\"type\":";
    std::vector<std::string> lines;
    CHECK(ipc_take_lines(buf, &lines));
    CHECK(lines.size() == 2 && lines[0] == "{\"type\":\"a\"}" && lines[1] == "{\"type\":\"b\"}");  // CRLF, blank lines
    CHECK(buf == "{\"type\":");  // the rest waits for its newline
    std::string big(kIpcMaxLine + 1, 'x');
    CHECK(!ipc_take_lines(big, &lines));
    std::string line = ipc_line("error", {{"message", json_string("two\nlines")}, {"n", json_number(3)}});
    CHECK(line == "{\"type\":\"error\",\"message\":\"two\\nlines\",\"n\":3}\n");  // one line, newline escaped
}

static void test_hello_check() {
    JsonValue v;
    json_parse(hello("abc"), &v);
    CHECK(ipc_check_hello(v, "abc").empty());
    CHECK(ipc_check_hello(v, "abd") == "wrong token");
    json_parse(hello("abc", 2), &v);
    CHECK(ipc_check_hello(v, "abc").find("protocol 2") == 0);
    json_parse(R"({"type":"state","state":"menu"})", &v);
    CHECK(!ipc_check_hello(v, "abc").empty());
}

static void test_session() {
    GameLink link;
    std::wstring err;
    CHECK(link.open(&err) && link.state() == LinkState::Listening);
    link.set_expected_pid(GetCurrentProcessId());  // the fake game is this process
    CHECK(link.pipe_name().rfind(L"\\\\.\\pipe\\aki-launcher-", 0) == 0 && link.token().size() == 32);
    link.poll();
    CHECK(link.state() == LinkState::Listening);  // nobody yet: poll returns at once

    std::atomic<bool> gotWelcome{false}, gotQuit{false}, secondRefused{false};
    std::wstring name = link.pipe_name();
    std::string token = link.token();
    std::thread game([&] {
        FakeGame g;
        if (!g.connect(name)) return;
        // One client only: a second one is turned away while the first is connected.
        HANDLE second = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        secondRefused = second == INVALID_HANDLE_VALUE;
        if (second != INVALID_HANDLE_VALUE) CloseHandle(second);
        g.write(hello(token));
        gotWelcome = g.read_until("\"type\":\"welcome\"");
        // Two messages in one write, one split over two writes, and a line that isn't JSON (dropped).
        g.write("{\"type\":\"state\",\"state\":\"menu\"}\n{\"type\":\"state\",\"state\":\"match\"}\nnot json\n{\"type\":\"match_");
        Sleep(30);
        g.write("end\",\"result\":{\"winner\":1}}\n{\"type\":\"some_future_thing\"}\n");
        gotQuit = g.read_until("\"type\":\"quit\"");
        g.write("{\"type\":\"bye\"}\n");
        g.close();
    });
    CHECK(pump(link, [&] { return link.state() == LinkState::Ready; }));
    CHECK(link.hello().wstr("game") == L"wwfnomercy" && link.hello().integer("pid", 0) == 1234);
    std::vector<JsonValue> msgs;
    CHECK(pump(link, [&] {
        for (auto& m : link.take()) msgs.push_back(m);
        return msgs.size() >= 4;
    }));
    CHECK(msgs.size() == 4);
    if (msgs.size() == 4) {
        CHECK(msgs[0].wstr("state") == L"menu" && msgs[1].wstr("state") == L"match");
        CHECK(msgs[2].wstr("type") == L"match_end" && msgs[2].get("result") && msgs[2].get("result")->integer("winner", 0) == 1);
        CHECK(msgs[3].wstr("type") == L"some_future_thing");  // unknown types reach the caller, who ignores them
    }
    CHECK(link.send(ipc_line("quit")));
    CHECK(pump(link, [&] {
        for (auto& m : link.take()) msgs.push_back(m);
        return link.state() == LinkState::Closed;
    }));
    game.join();
    CHECK(gotWelcome && gotQuit && secondRefused);
    CHECK(msgs.size() == 5 && msgs.back().wstr("type") == L"bye");
    CHECK(link.last_error() == "the game closed the link");
    CHECK(!link.send(ipc_line("ping")));  // closed: nothing to send to
}

// A client that gets the hello wrong is told why and hung up on.
static void test_refused(const std::string& first, const std::string& expectError, DWORD helloMs = kIpcHelloMs) {
    GameLink link;
    std::wstring err;
    CHECK(link.open(&err));
    link.set_hello_timeout(helloMs);
    std::atomic<bool> told{false};
    std::wstring name = link.pipe_name();
    std::string tok = link.token();
    std::thread game([&] {
        FakeGame g;
        if (!g.connect(name)) return;
        std::string s = first;
        size_t at = s.find("%TOKEN%");
        if (at != std::string::npos) s.replace(at, 7, tok);
        if (!s.empty()) g.write(s);
        told = g.read_until("\"type\":\"error\"") && g.got.find(expectError) != std::string::npos;
        g.read_until("never");  // then the launcher hangs up
    });
    CHECK(pump(link, [&] { return link.state() == LinkState::Closed; }));
    game.join();
    CHECK(told);
    CHECK(link.last_error().find(expectError) != std::string::npos);
    if (link.last_error().find(expectError) == std::string::npos) printf("  got: %s\n", link.last_error().c_str());
}

// Clients that would have hung the launcher's frame loop (a flush that waits for a reader): each poll returns, and
// the link closes in time.
static void test_never_stuck() {
    {  // connects, then neither writes nor reads
        GameLink link;
        std::wstring err;
        CHECK(link.open(&err));
        link.set_hello_timeout(300);
        std::atomic<bool> done{false};
        std::wstring name = link.pipe_name();
        std::thread game([&] {
            FakeGame g;
            g.connect(name);
            while (!done) Sleep(10);
        });
        ULONGLONG t0 = GetTickCount64();
        CHECK(pump(link, [&] { return link.state() == LinkState::Closed; }));
        CHECK(GetTickCount64() - t0 < 3000);
        done = true;
        game.join();
    }
    {  // one blocking write far over the line limit (and the pipe buffer), never reading
        GameLink link;
        std::wstring err;
        CHECK(link.open(&err));
        std::wstring name = link.pipe_name();
        std::thread game([&] {
            FakeGame g;
            if (!g.connect(name)) return;
            g.write(std::string(kIpcMaxLine * 4, 'x'));  // returns when the launcher hangs up
        });
        CHECK(pump(link, [&] { return link.state() == LinkState::Closed; }));
        CHECK(link.last_error().find("longer than") != std::string::npos);
        game.join();
    }
}

// Another process of the same user that finds the pipe's name isn't the launched game: refused at connect.
static void test_other_process() {
    GameLink link;
    std::wstring err;
    CHECK(link.open(&err));
    link.set_expected_pid(GetCurrentProcessId() + 4);
    std::wstring name = link.pipe_name();
    std::thread game([&] {
        FakeGame g;
        if (!g.connect(name)) return;
        g.read_until("never");  // hung up on
    });
    CHECK(pump(link, [&] { return link.state() == LinkState::Closed; }));
    CHECK(link.last_error().find("isn't the launched game") != std::string::npos);
    game.join();
}

static void test_launch_env() {
    // The dev flag is off unless asked for (a plain launcher offers no pipe).
    CHECK(!launch_ipc());
}

void run_ipc_tests(const fs::path&) {
    test_framing();
    test_hello_check();
    test_session();
    test_refused("{\"type\":\"hello\",\"protocol\":1,\"token\":\"nope\"}\n", "wrong token");
    test_refused("{\"type\":\"hello\",\"protocol\":9,\"token\":\"%TOKEN%\"}\n", "protocol 9");
    test_refused("{\"type\":\"state\",\"state\":\"menu\"}\n", "must be a hello");
    test_refused("hello there\n", "isn't JSON");
    test_refused(std::string(kIpcMaxLine + 10, 'x'), "longer than");
    test_refused("", "no hello within", 300);
    test_never_stuck();
    test_other_process();
    test_launch_env();
}
