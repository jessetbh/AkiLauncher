#include <windows.h>

#include <d3d11.h>
#include <xinput.h>

#include <cmath>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "artwork.h"
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

struct BoxArt {
    Texture front;
    Texture back;
};

struct UiState {
    int selected = 0;
    bool showBack = false;
    float flipAnim = 0.0f;  // 0 = front, 1 = back
    ImFont* fontBase = nullptr;
    ImFont* fontTitle = nullptr;
    ImFont* fontSmall = nullptr;
    ImFont* fontHeader = nullptr;
};
static UiState g_ui;

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

// Newly-pressed XInput buttons this frame (any pad), for carousel navigation.
static WORD pad_pressed() {
    static WORD prev = 0;
    WORD cur = 0;
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        XINPUT_STATE st{};
        if (XInputGetState(i, &st) == ERROR_SUCCESS) cur |= st.Gamepad.wButtons;
    }
    WORD edge = cur & (WORD)~prev;
    prev = cur;
    return edge;
}

// ---------------------------------------------------------------------------
// UI helpers
// ---------------------------------------------------------------------------

static std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, 0);
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

static void text_centered(const std::string& s) {
    ImVec2 sz = ImGui::CalcTextSize(s.c_str());
    ImGui::SetCursorPosX((ImGui::GetWindowSize().x - sz.x) * 0.5f);
    ImGui::TextUnformatted(s.c_str());
}

static void text_centered_colored(ImVec4 col, const std::string& s) {
    ImVec2 sz = ImGui::CalcTextSize(s.c_str());
    ImGui::SetCursorPosX((ImGui::GetWindowSize().x - sz.x) * 0.5f);
    ImGui::TextColored(col, "%s", s.c_str());
}

namespace palette {
static const ImVec4 accent{0.98f, 0.78f, 0.28f, 1.0f};
static const ImVec4 ready{0.45f, 0.85f, 0.50f, 1.0f};
static const ImVec4 warn{0.95f, 0.70f, 0.30f, 1.0f};
static const ImVec4 dim{0.62f, 0.64f, 0.70f, 1.0f};
static const ImVec4 faint{0.42f, 0.44f, 0.50f, 1.0f};
static const ImVec4 error{0.92f, 0.45f, 0.40f, 1.0f};
static const ImU32 cardBg = IM_COL32(24, 26, 34, 255);
static const ImU32 cardBgSoon = IM_COL32(18, 19, 25, 255);
static const ImU32 cardBorder = IM_COL32(70, 74, 90, 255);
static const ImU32 cardBorderSel = IM_COL32(250, 199, 72, 255);
}  // namespace palette

// Draw a box-art face (texture or styled placeholder) into rect, rounded.
static void draw_box_face(ImDrawList* dl, const Texture& tex, ImVec2 p0, ImVec2 p1, bool isBack,
                          const std::string& title, float rounding, bool dimmed) {
    ImU32 tint = dimmed ? IM_COL32(140, 140, 140, 255) : IM_COL32_WHITE;
    if (tex.ok()) {
        dl->AddImageRounded((ImTextureID)tex.srv, p0, p1, ImVec2(0, 0), ImVec2(1, 1), tint,
                            rounding);
        return;
    }
    // Placeholder: dark card with the title and face label.
    dl->AddRectFilled(p0, p1, dimmed ? palette::cardBgSoon : palette::cardBg, rounding);
    float w = p1.x - p0.x, h = p1.y - p0.y;
    ImU32 txt = IM_COL32(200, 202, 210, dimmed ? 130 : 220);
    ImFont* f = g_ui.fontSmall;
    float fs = f->FontSize;
    // crude centered wrap: title split across lines by width
    ImVec2 tsz = f->CalcTextSizeA(fs, w - 16, w - 16, title.c_str());
    dl->AddText(f, fs, ImVec2(p0.x + (w - tsz.x) * 0.5f, p0.y + (h - tsz.y) * 0.5f - fs * 0.6f),
                txt, title.c_str(), nullptr, w - 16);
    const char* lbl = isBack ? "back cover" : "cover art";
    ImVec2 lsz = f->CalcTextSizeA(fs * 0.85f, FLT_MAX, 0, lbl);
    dl->AddText(f, fs * 0.85f, ImVec2(p0.x + (w - lsz.x) * 0.5f, p1.y - fs * 1.6f),
                IM_COL32(150, 152, 160, dimmed ? 100 : 170), lbl);
}

// Returns the index of a game to launch this frame, or -1.
static int draw_ui(std::vector<GameEntry>& games, std::vector<BoxArt>& art,
                   const std::filesystem::path& depotRoot) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float vw = vp->WorkSize.x, vh = vp->WorkSize.y;
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // ------------------------------------------------------------------ game running overlay
    if (g_session.active()) {
        const char* verb = g_session.state == SessionState::WaitingForWindow ? "Starting"
                           : g_session.state == SessionState::Closing        ? "Closing"
                                                                              : "Now playing";
        ImGui::PushFont(g_ui.fontTitle);
        ImGui::SetCursorPosY(vh * 0.44f);
        text_centered(std::string(verb) + "  -  " + narrow(g_session.title));
        ImGui::PopFont();
        ImGui::PushFont(g_ui.fontSmall);
        ImGui::SetCursorPosY(vh * 0.54f);
        text_centered_colored(palette::dim,
                              "Shift+F12  or hold  View+Menu  for the quit prompt, "
                              "confirm to return here");
        ImGui::PopFont();
        ImGui::End();
        return -1;
    }

    int launchIndex = -1;
    int n = (int)games.size();
    GameEntry& sel = games[g_ui.selected];

    // ------------------------------------------------------------------ input
    int move = 0;
    bool actLaunch = false, actFlip = false;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) move = -1;
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) move = +1;
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))
        actLaunch = true;
    if (ImGui::IsKeyPressed(ImGuiKey_F)) actFlip = true;
    WORD pad = pad_pressed();
    if (pad & XINPUT_GAMEPAD_DPAD_LEFT) move = -1;
    if (pad & XINPUT_GAMEPAD_DPAD_RIGHT) move = +1;
    if (pad & XINPUT_GAMEPAD_A) actLaunch = true;
    if (pad & XINPUT_GAMEPAD_X) actFlip = true;

    // ------------------------------------------------------------------ header
    ImGui::PushFont(g_ui.fontHeader);
    ImGui::SetCursorPosY(vh * 0.055f);
    text_centered("A K I   L A U N C H E R");
    ImGui::PopFont();
    ImGui::PushFont(g_ui.fontSmall);
    text_centered_colored(palette::faint, "N64 wrestling recompiled");
    ImGui::PopFont();

    // ------------------------------------------------------------------ carousel
    const float aspect = 0.72f;  // N64 boxes are landscape: h = w * aspect
    float baseW = vw * 0.115f;
    float gap = vw * 0.014f;
    float selScale = 1.38f;
    float rowMidY = vp->WorkPos.y + vh * 0.365f;

    float total = 0;
    for (int i = 0; i < n; ++i) total += (i == g_ui.selected ? baseW * selScale : baseW);
    total += gap * (n - 1);
    float x = vp->WorkPos.x + (vw - total) * 0.5f;

    ImGui::PushFont(g_ui.fontSmall);
    for (int i = 0; i < n; ++i) {
        GameEntry& g = games[i];
        bool isSel = i == g_ui.selected;
        float w = isSel ? baseW * selScale : baseW;
        float h = w * aspect;
        float drawW = w;

        // Flip animation squeezes the selected card horizontally.
        bool backFace = false;
        if (isSel) {
            float t = g_ui.flipAnim;
            t = t * t * (3.0f - 2.0f * t);  // smoothstep
            drawW = w * fabsf(1.0f - 2.0f * t);
            if (drawW < w * 0.02f) drawW = w * 0.02f;
            backFace = t > 0.5f;
        }

        ImVec2 p0(x + (w - drawW) * 0.5f, rowMidY - h * 0.5f);
        ImVec2 p1(p0.x + drawW, rowMidY + h * 0.5f);
        float rounding = w * 0.03f;
        bool soon = g.comingSoon && !g.launchable();

        // shadow + face + border
        dl->AddRectFilled(ImVec2(p0.x + 6, p0.y + 8), ImVec2(p1.x + 6, p1.y + 8),
                          IM_COL32(0, 0, 0, 110), rounding);
        const Texture& tex = backFace ? art[i].back : art[i].front;
        draw_box_face(dl, tex, p0, p1, backFace, narrow(g.title), rounding, soon);
        dl->AddRect(p0, p1, isSel ? palette::cardBorderSel : palette::cardBorder, rounding, 0,
                    isSel ? 3.0f : 1.5f);

        if (soon) {
            const char* ribbon = "COMING SOON";
            ImFont* f = g_ui.fontSmall;
            ImVec2 rsz = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, ribbon);
            ImVec2 rp(p0.x + (drawW - rsz.x) * 0.5f, p1.y - h * 0.5f - rsz.y * 0.5f);
            dl->AddRectFilled(ImVec2(rp.x - 10, rp.y - 4), ImVec2(rp.x + rsz.x + 10, rp.y + rsz.y + 4),
                              IM_COL32(10, 10, 14, 215), 4.0f);
            dl->AddText(f, f->FontSize, rp, IM_COL32(235, 200, 120, 235), ribbon);
        }

        // year caption under the card
        std::string dateStr = narrow(g.releaseDate);
        std::string year = dateStr.size() >= 4 ? dateStr.substr(dateStr.size() - 4) : dateStr;
        if (g.region == L"Japan") year += "  (JP)";
        ImFont* f = g_ui.fontSmall;
        ImVec2 ysz = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, year.c_str());
        dl->AddText(f, f->FontSize, ImVec2(x + (w - ysz.x) * 0.5f, rowMidY + h * 0.5f + 10),
                    isSel ? IM_COL32(250, 199, 72, 255) : IM_COL32(130, 132, 140, 200),
                    year.c_str());

        // hit test (full logical card area, not the squeezed width)
        ImGui::SetCursorScreenPos(ImVec2(x, rowMidY - h * 0.5f));
        ImGui::InvisibleButton(("card" + std::to_string(i)).c_str(), ImVec2(w, h));
        if (ImGui::IsItemClicked()) {
            if (isSel) actFlip = true;
            else { g_ui.selected = i; g_ui.showBack = false; g_ui.flipAnim = 0.0f; }
        }

        x += w + gap;
    }
    ImGui::PopFont();

    // ------------------------------------------------------------------ apply input
    if (move != 0) {
        g_ui.selected = (g_ui.selected + move + n) % n;
        g_ui.showBack = false;
        g_ui.flipAnim = 0.0f;
    }
    if (actFlip) g_ui.showBack = !g_ui.showBack;
    float target = g_ui.showBack ? 1.0f : 0.0f;
    float dt = ImGui::GetIO().DeltaTime;
    if (g_ui.flipAnim < target) g_ui.flipAnim = fminf(target, g_ui.flipAnim + dt / 0.30f);
    else if (g_ui.flipAnim > target) g_ui.flipAnim = fmaxf(target, g_ui.flipAnim - dt / 0.30f);

    // ------------------------------------------------------------------ info panel
    GameEntry& s = games[g_ui.selected];
    bool soon = s.comingSoon && !s.launchable();
    float infoY = vh * 0.60f;

    ImGui::PushFont(g_ui.fontTitle);
    ImGui::SetCursorPosY(infoY);
    text_centered(narrow(s.title));
    ImGui::PopFont();

    ImGui::PushFont(g_ui.fontBase);
    ImGui::Dummy(ImVec2(0, vh * 0.004f));
    text_centered_colored(palette::dim, "Released " + narrow(s.releaseDate) + "   -   " +
                                            narrow(s.region));

    std::string status;
    ImVec4 statusCol;
    if (soon) { status = "Recompilation not started yet"; statusCol = palette::faint; }
    else if (!s.exeFound) { status = "Not built - run the port build in " + narrow(s.repoDir); statusCol = palette::warn; }
    else if (!s.romFound) { status = "ROM missing - place " + narrow(s.romFile) + " in " + narrow(s.repoDir); statusCol = palette::warn; }
    else if (s.experimental) { status = "Playable - experimental (bring-up in progress)"; statusCol = palette::warn; }
    else { status = "Ready to play"; statusCol = palette::ready; }
    ImGui::Dummy(ImVec2(0, vh * 0.002f));
    text_centered_colored(statusCol, status);
    ImGui::PopFont();

    // Play button (mouse users; keyboard/pad use Enter/A)
    if (s.launchable()) {
        ImGui::PushFont(g_ui.fontBase);
        ImVec2 bsz(vw * 0.10f, vh * 0.048f);
        ImGui::SetCursorPos(ImVec2((vw - bsz.x) * 0.5f, infoY + vh * 0.115f));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.62f, 0.12f, 0.92f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.98f, 0.75f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.75f, 0.53f, 0.08f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.07f, 0.05f, 1.0f));
        if (ImGui::Button("PLAY", bsz)) launchIndex = g_ui.selected;
        ImGui::PopStyleColor(4);
        ImGui::PopFont();
        if (actLaunch) launchIndex = g_ui.selected;
    }

    // Mods section (tucks up when there's no PLAY button)
    float modsY = infoY + (s.launchable() ? vh * 0.185f : vh * 0.115f);
    ImGui::PushFont(g_ui.fontSmall);
    ImGui::SetCursorPosY(modsY);
    if (!s.mods.empty()) {
        text_centered_colored(palette::accent, "Full-conversion mods  -  not yet playable here");
        std::vector<std::string> lines;
        for (const auto& m : s.mods)
            lines.push_back(narrow(m.name) + (m.note.empty() ? "" : "  -  " + narrow(m.note)));
        if ((int)lines.size() <= 5) {
            for (const auto& l : lines) text_centered_colored(palette::faint, l);
        } else {
            // two centered columns for the big No Mercy scene
            int rows = ((int)lines.size() + 1) / 2;
            float y0 = ImGui::GetCursorPosY();
            float lh = ImGui::GetTextLineHeightWithSpacing();
            for (int i = 0; i < (int)lines.size(); ++i) {
                int col = i / rows, row = i % rows;
                float cx = vw * (col == 0 ? 0.27f : 0.73f);
                ImVec2 tsz = ImGui::CalcTextSize(lines[i].c_str());
                ImGui::SetCursorPos(ImVec2(cx - tsz.x * 0.5f, y0 + row * lh));
                ImGui::TextColored(palette::faint, "%s", lines[i].c_str());
            }
            ImGui::SetCursorPosY(y0 + rows * lh);
        }
        text_centered_colored(palette::faint,
                              "Full conversions need HD texture + GameShark support in the "
                              "ports - planned.");
    } else {
        text_centered_colored(palette::faint,
                              "Popular full-conversion mods for this game will appear here "
                              "once mod support lands.");
    }
    ImGui::PopFont();

    // Status note from last session (crash/force-close info)
    if (!g_session.statusNote.empty()) {
        ImGui::PushFont(g_ui.fontSmall);
        ImGui::SetCursorPosY(vh * 0.905f);
        text_centered_colored(palette::error, narrow(g_session.statusNote));
        ImGui::PopFont();
    }

    // Footer
    ImGui::PushFont(g_ui.fontSmall);
    {
        const char* credit = "created by: jessetbh";
        ImVec2 csz = ImGui::CalcTextSize(credit);
        ImGui::SetCursorPos(ImVec2(vw - csz.x - ImGui::GetFontSize(), vh * 0.945f));
        ImGui::TextColored(palette::faint, "%s", credit);
    }
    ImGui::SetCursorPosY(vh * 0.945f);
    if (depotRoot.empty())
        text_centered_colored(palette::error, "Depot root not found - set AKI_DEPOT_ROOT");
    else
        text_centered_colored(palette::faint,
                              "Left/Right browse      Enter / A  play      F / X  flip box      "
                              "Esc quit      In game:  Shift+F12 / View+Menu  to return");
    ImGui::PopFont();

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
        else if (a == L"--select" && i + 1 < __argc) g_ui.selected = _wtoi(__wargv[++i]);
        else if (a == L"--flip") g_ui.showBack = true;
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
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.FrameRounding = 8.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.028f, 0.030f, 0.045f, 1.0f);

    // Fonts: Segoe UI at sizes derived from the actual window height.
    RECT cr{};
    GetClientRect(hwnd, &cr);
    float uiH = (float)(cr.bottom - cr.top);
    if (uiH < 400) uiH = (float)screenH;
    const char* segoe = "C:\\Windows\\Fonts\\segoeui.ttf";
    const char* segoeBold = "C:\\Windows\\Fonts\\segoeuib.ttf";
    auto addFont = [&](const char* path, float size) -> ImFont* {
        ImFont* f = io.Fonts->AddFontFromFileTTF(path, size);
        return f ? f : io.Fonts->AddFontDefault();
    };
    g_ui.fontBase = addFont(segoe, uiH * 0.0215f);
    g_ui.fontSmall = addFont(segoe, uiH * 0.0165f);
    g_ui.fontTitle = addFont(segoeBold, uiH * 0.036f);
    g_ui.fontHeader = addFont(segoeBold, uiH * 0.030f);
    io.FontDefault = g_ui.fontBase;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    RegisterHotKey(hwnd, HOTKEY_QUICKBACK, MOD_SHIFT | MOD_NOREPEAT, VK_F12);

    std::filesystem::path depotRoot = detect_depot_root();
    std::vector<GameEntry> games = build_game_list(depotRoot);
    logf(L"depot root: %s", depotRoot.empty() ? L"(NOT FOUND)" : depotRoot.c_str());

    // Box art: assets\boxart\<key>_front.png / _back.png next to the repo root.
    wchar_t exeBuf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
    std::filesystem::path artDir =
        std::filesystem::path(exeBuf).parent_path().parent_path() / L"assets" / L"boxart";
    std::vector<BoxArt> art(games.size());
    for (size_t i = 0; i < games.size(); ++i) {
        art[i].front = load_texture(g_device, artDir / (games[i].artKey + L"_front.png"));
        art[i].back = load_texture(g_device, artDir / (games[i].artKey + L"_back.png"));
        logf(L"game: %s exe=%d rom=%d art(front=%d back=%d)", games[i].title.c_str(),
             games[i].exeFound, games[i].romFound, art[i].front.ok(), art[i].back.ok());
    }

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
                // The port's quit prompt autofocuses Cancel; Quit is one step
                // left. Keys must land on separate frames (same-batch posts
                // coalesce badly in SDL/RmlUi).
                ULONGLONG dt2 = GetTickCount64() - testCloseTick;
                auto postKey = [](HWND w, WPARAM vk, WORD scan, bool ext) {
                    LPARAM down = 1 | ((LPARAM)scan << 16) | (ext ? (1LL << 24) : 0);
                    PostMessageW(w, WM_KEYDOWN, vk, down);
                    PostMessageW(w, WM_KEYUP, vk, down | (1LL << 30) | (1LL << 31));
                };
                static int stage = 0;
                if (stage == 0 && dt2 > 3000) {
                    logf(L"test-launch: posting Left");
                    postKey(g_session.gameWnd, VK_LEFT, 0x4B, true);
                    stage = 1;
                } else if (stage == 1 && dt2 > 5000) {
                    logf(L"test-launch: posting Enter");
                    postKey(g_session.gameWnd, VK_RETURN, 0x1C, false);
                    stage = 2;
                }
                testConfirmSent = stage >= 2;
                if (dt2 > 30000) {
                    logf(L"test-launch: game never exited after quit prompt; giving up");
                    TerminateProcess(g_session.pi.hProcess, 1);
                    exitCodeOut = 5;
                    break;
                }
            }
            if (wasActive && !g_session.active()) {
                logf(L"test-launch: session ended (terminated=%d, note=%s)",
                     g_session.wasTerminated, g_session.statusNote.c_str());
                exitCodeOut = g_session.wasTerminated ? 3 : 0;
                break;
            }
        }

        // --- render ---
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if (!g_session.active() && ImGui::IsKeyPressed(ImGuiKey_Escape) && testLaunch < 0) done = true;

        int launchIdx = draw_ui(games, art, depotRoot);
        if (launchIdx >= 0) {
            std::wstring err;
            if (!g_session.launch(games[launchIdx], &err))
                g_session.statusNote = games[launchIdx].title + L": " + err;
        }

        ImGui::Render();
        const float clear[4] = {0.028f, 0.030f, 0.045f, 1.0f};
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
