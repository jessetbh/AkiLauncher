#include <windows.h>

#include <d3d11.h>

#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "games.h"
#include "launch.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ---------------------------------------------------------------------------
// D3D11 boilerplate
// ---------------------------------------------------------------------------

static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_context = nullptr;
static IDXGISwapChain* g_swapChain = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static UINT g_resizeW = 0, g_resizeH = 0;

static void create_render_target() {
    ID3D11Texture2D* back = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

static void destroy_render_target() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

static bool create_device(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                               levels, 2, D3D11_SDK_VERSION, &sd, &g_swapChain,
                                               &g_device, &got, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                           D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &got,
                                           &g_context);
    if (FAILED(hr)) return false;

    // Keep DXGI from hijacking Alt+Enter into exclusive fullscreen.
    IDXGIDevice* dxgiDev = nullptr;
    if (SUCCEEDED(g_device->QueryInterface(IID_PPV_ARGS(&dxgiDev)))) {
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(dxgiDev->GetAdapter(&adapter))) {
            IDXGIFactory* factory = nullptr;
            if (SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
                factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
                factory->Release();
            }
            adapter->Release();
        }
        dxgiDev->Release();
    }

    create_render_target();
    return true;
}

static void destroy_device() {
    destroy_render_target();
    if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
}

// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------

static GameSession g_session;
static bool g_quickBackRequested = false;
static constexpr int HOTKEY_QUICKBACK = 1;

static LRESULT WINAPI wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED) {
            g_resizeW = LOWORD(lparam);
            g_resizeH = HIWORD(lparam);
        }
        return 0;
    case WM_HOTKEY:
        if (wparam == HOTKEY_QUICKBACK) g_quickBackRequested = true;
        return 0;
    case WM_SYSCOMMAND:
        if ((wparam & 0xfff0) == SC_KEYMENU) return 0;  // no Alt menu beep
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

static std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, 0);
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

// Returns the index of a game to launch this frame, or -1.
static int draw_ui(std::vector<GameEntry>& games, const std::filesystem::path& depotRoot) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);

    int launchIndex = -1;

    if (g_session.active()) {
        // Normally hidden behind the fullscreen game; visible during the
        // launch/close transitions, which is exactly the seamless effect.
        ImVec2 center(vp->WorkSize.x * 0.5f, vp->WorkSize.y * 0.5f);
        const char* verb = g_session.state == SessionState::WaitingForWindow ? "Starting"
                           : g_session.state == SessionState::Closing        ? "Closing"
                                                                              : "Now playing";
        std::string line = std::string(verb) + "  -  " + narrow(g_session.title);
        ImVec2 sz = ImGui::CalcTextSize(line.c_str());
        ImGui::SetCursorPos(ImVec2(center.x - sz.x * 0.5f, center.y - sz.y * 0.5f));
        ImGui::TextUnformatted(line.c_str());
        std::string hint =
            "Shift+F12 or hold Back+Start for the quit prompt, confirm to return here";
        ImVec2 hsz = ImGui::CalcTextSize(hint.c_str());
        ImGui::SetCursorPos(ImVec2(center.x - hsz.x * 0.5f, center.y + sz.y * 2.0f));
        ImGui::TextDisabled("%s", hint.c_str());
        ImGui::End();
        return -1;
    }

    ImGui::Dummy(ImVec2(0, vp->WorkSize.y * 0.08f));
    {
        const char* title = "A K I   L A U N C H E R";
        ImVec2 sz = ImGui::CalcTextSize(title);
        ImGui::SetCursorPosX((vp->WorkSize.x - sz.x) * 0.5f);
        ImGui::TextUnformatted(title);
    }
    ImGui::Dummy(ImVec2(0, vp->WorkSize.y * 0.06f));

    float cardW = vp->WorkSize.x * 0.5f;
    for (int i = 0; i < (int)games.size(); ++i) {
        GameEntry& g = games[i];
        std::string label = narrow(g.title);
        std::string status = !g.exeFound      ? "Not built"
                             : !g.romFound    ? "ROM missing"
                             : g.experimental ? "Ready (experimental)"
                                              : "Ready";
        ImGui::SetCursorPosX((vp->WorkSize.x - cardW) * 0.5f);
        ImGui::BeginDisabled(!g.launchable());
        if (ImGui::Button((label + "##game" + std::to_string(i)).c_str(),
                          ImVec2(cardW, ImGui::GetFontSize() * 3.0f)))
            launchIndex = i;
        ImGui::EndDisabled();
        ImGui::SetCursorPosX((vp->WorkSize.x - cardW) * 0.5f);
        if (g.launchable() && !g.experimental)
            ImGui::TextDisabled("  %s", status.c_str());
        else
            ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f), "  %s", status.c_str());
        ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.8f));
    }

    if (!g_session.statusNote.empty()) {
        ImGui::Dummy(ImVec2(0, ImGui::GetFontSize()));
        ImGui::SetCursorPosX((vp->WorkSize.x - cardW) * 0.5f);
        ImGui::TextColored(ImVec4(0.9f, 0.5f, 0.4f, 1.0f), "%s",
                           narrow(g_session.statusNote).c_str());
    }

    // Footer
    ImGui::SetCursorPos(ImVec2(ImGui::GetFontSize(), vp->WorkSize.y - ImGui::GetFontSize() * 2.5f));
    if (depotRoot.empty())
        ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f),
                           "Depot root not found - set AKI_DEPOT_ROOT");
    else
        ImGui::TextDisabled("Enter/A: play    Esc: quit    In game: Shift+F12 or Back+Start to return");

    ImGui::End();
    return launchIndex;
}

// ---------------------------------------------------------------------------
// wWinMain
// ---------------------------------------------------------------------------

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR, int) {
    bool smoke = false;
    int testLaunch = -1;
    for (int i = 1; i < __argc; ++i) {
        std::wstring a = __wargv[i];
        if (a == L"--smoke") smoke = true;
        else if (a == L"--test-launch" && i + 1 < __argc) testLaunch = _wtoi(__wargv[++i]);
    }

    ImGui_ImplWin32_EnableDpiAwareness();

    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, wnd_proc, 0, 0, hinst, nullptr,
                   LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, L"AkiLauncher", nullptr};
    RegisterClassExW(&wc);

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    bool windowed = smoke;  // smoke test shouldn't take over the screen
    HWND hwnd;
    if (windowed) {
        hwnd = CreateWindowW(wc.lpszClassName, L"AKI Launcher", WS_OVERLAPPEDWINDOW, 100, 100,
                             1280, 720, nullptr, nullptr, hinst, nullptr);
    } else {
        hwnd = CreateWindowW(wc.lpszClassName, L"AKI Launcher", WS_POPUP, 0, 0, screenW, screenH,
                             nullptr, nullptr, hinst, nullptr);
    }

    if (!create_device(hwnd)) {
        destroy_device();
        UnregisterClassW(wc.lpszClassName, hinst);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    float uiScale = windowed ? 1.5f : (float)screenH / 720.0f;
    style.ScaleAllSizes(uiScale);
    style.FrameRounding = 6.0f * uiScale;
    io.FontGlobalScale = uiScale;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    RegisterHotKey(hwnd, HOTKEY_QUICKBACK, MOD_SHIFT | MOD_NOREPEAT, VK_F12);

    std::filesystem::path depotRoot = detect_depot_root();
    std::vector<GameEntry> games = build_game_list(depotRoot);
    logf(L"depot root: %s", depotRoot.empty() ? L"(NOT FOUND)" : depotRoot.c_str());
    for (auto& g : games)
        logf(L"game: %s exe=%d rom=%d (%s)", g.title.c_str(), g.exeFound, g.romFound,
             g.exePath.c_str());

    int frame = 0;
    bool testCloseSent = false;
    bool testConfirmSent = false;
    ULONGLONG testCloseTick = 0;
    int exitCodeOut = 0;
    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (g_resizeW && g_resizeH) {
            destroy_render_target();
            g_swapChain->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = g_resizeH = 0;
            create_render_target();
        }

        // --- session upkeep (non-blocking) ---
        bool wasActive = g_session.active();
        g_session.tick(hwnd);
        if (g_session.active() && poll_quickback_chord()) g_quickBackRequested = true;
        if (g_quickBackRequested) {
            g_quickBackRequested = false;
            g_session.requestQuickBack();
        }

        // --- automated test drivers ---
        frame++;
        if (smoke && frame > 60) break;
        if (testLaunch >= 0) {
            if (frame == 30 && testLaunch < (int)games.size()) {
                std::wstring err;
                if (!g_session.launch(games[testLaunch], &err)) {
                    logf(L"test-launch: launch failed: %s", err.c_str());
                    exitCodeOut = 2;
                    break;
                }
            }
            if (g_session.state == SessionState::Running && !testCloseSent &&
                GetTickCount64() - g_session.runningTick > 15000) {
                logf(L"test-launch: triggering quick-back");
                g_session.requestQuickBack();
                testCloseSent = true;
                testCloseTick = GetTickCount64();
            }
            if (testCloseSent && g_session.active()) {
                // The port's quit prompt autofocuses Cancel (focus_on_cancel in
                // ui_state.cpp), with Quit to its left in a horizontal nav row.
                // Stage inputs on separate frames to find what confirms it.
                ULONGLONG dt = GetTickCount64() - testCloseTick;
                auto postKey = [](HWND w, WPARAM vk, WORD scan, bool ext) {
                    LPARAM down = 1 | ((LPARAM)scan << 16) | (ext ? (1LL << 24) : 0);
                    PostMessageW(w, WM_KEYDOWN, vk, down);
                    PostMessageW(w, WM_KEYUP, vk, down | (1LL << 30) | (1LL << 31));
                };
                static int stage = 0;
                if (stage == 0 && dt > 3000) {
                    logf(L"test-launch: stage 0 - posting Left");
                    postKey(g_session.gameWnd, VK_LEFT, 0x4B, true);
                    stage = 1;
                } else if (stage == 1 && dt > 5000) {
                    logf(L"test-launch: stage 1 - posting Enter");
                    postKey(g_session.gameWnd, VK_RETURN, 0x1C, false);
                    stage = 2;
                } else if (stage == 2 && dt > 7000) {
                    logf(L"test-launch: stage 2 - posting Space");
                    postKey(g_session.gameWnd, VK_SPACE, 0x39, false);
                    stage = 3;
                } else if (stage == 3 && dt > 9000) {
                    logf(L"test-launch: stage 3 - SendInput Enter (needs game foreground)");
                    INPUT in[2]{};
                    in[0].type = INPUT_KEYBOARD;
                    in[0].ki.wVk = VK_RETURN;
                    in[1].type = INPUT_KEYBOARD;
                    in[1].ki.wVk = VK_RETURN;
                    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
                    SendInput(2, in, sizeof(INPUT));
                    stage = 4;
                }
                testConfirmSent = stage >= 4;
            }
            if (testCloseSent && g_session.active() &&
                GetTickCount64() - testCloseTick > 30000) {
                logf(L"test-launch: game never exited after quit prompt; giving up");
                TerminateProcess(g_session.pi.hProcess, 1);
                exitCodeOut = 5;
                break;
            }
            if (wasActive && !g_session.active()) {
                logf(L"test-launch: session ended (terminated=%d, note=%s)",
                     g_session.wasTerminated, g_session.statusNote.c_str());
                exitCodeOut = g_session.wasTerminated ? 3 : 0;
                break;
            }
            if (frame > 30 && !g_session.active() && !testCloseSent && frame > 40) {
                // launched but process ended before Running was ever reached
                logf(L"test-launch: game exited before window was found");
                exitCodeOut = 4;
                break;
            }
        }

        // --- render ---
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if (!g_session.active() && ImGui::IsKeyPressed(ImGuiKey_Escape) && testLaunch < 0) done = true;

        int launchIdx = draw_ui(games, depotRoot);
        if (launchIdx >= 0) {
            std::wstring err;
            if (!g_session.launch(games[launchIdx], &err))
                g_session.statusNote = games[launchIdx].title + L": " + err;
        }

        ImGui::Render();
        const float clear[4] = {0.02f, 0.02f, 0.03f, 1.0f};
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swapChain->Present(1, 0);

        // While the game owns the screen the launcher just idles behind it.
        if (g_session.state == SessionState::Running) Sleep(100);
    }

    if (g_session.active()) {
        logf(L"launcher exiting with game still running; leaving it alone");
    }

    UnregisterHotKey(hwnd, HOTKEY_QUICKBACK);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    destroy_device();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hinst);
    logf(L"exit code %d", exitCodeOut);
    return exitCodeOut;
}
