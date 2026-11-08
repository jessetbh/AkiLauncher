#include <windows.h>

#include <d3d11.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <xinput.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "imgui_stdlib.h"

#include "artwork.h"
#include "audio.h"
#include "conversions.h"
#include "games.h"
#include "installer.h"
#include "rebuild.h"
#include "romscan.h"
#include "mods.h"
#include "modelpacks.h"
#include "packages.h"
#include "quickmatch.h"
#include "tour.h"
#include "shots.h"
#include "selfupdate.h"
#include "launch.h"
#include "settings.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ---------------------------------------------------------------------------
// D3D11 boilerplate
// ---------------------------------------------------------------------------

static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_context = nullptr;
static IDXGISwapChain* g_swapChain = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static UINT g_resizeW = 0, g_resizeH = 0;
// Screenshot mode draws into this texture instead of a swapchain: nothing is presented, and it works without a
// display (a server session, a locked screen).
static ID3D11Texture2D* g_offscreen = nullptr;

// The texture frames are drawn into (the swapchain's back buffer, or g_offscreen), with a reference to release.
static ID3D11Texture2D* frame_texture() {
    ID3D11Texture2D* t = nullptr;
    if (g_offscreen) {
        g_offscreen->AddRef();
        t = g_offscreen;
    } else if (g_swapChain) {
        g_swapChain->GetBuffer(0, IID_PPV_ARGS(&t));
    }
    return t;
}

static void create_render_target() {
    ID3D11Texture2D* back = frame_texture();
    if (back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

static void destroy_render_target() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

// A device and a w x h render texture, no swapchain (screenshot mode). Hardware first, else WARP (no GPU).
static bool create_offscreen_device(UINT w, UINT h) {
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                   &g_device, &got, &g_context);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &g_device,
                               &got, &g_context);
    if (FAILED(hr)) return false;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(g_device->CreateTexture2D(&d, nullptr, &g_offscreen))) return false;
    create_render_target();
    return g_rtv != nullptr;
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
    if (g_offscreen) { g_offscreen->Release(); g_offscreen = nullptr; }
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
}

// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------

static GameSession g_session;
// Screenshot mode (--shot / --shot-all, shots.h): no launches, no config writes, no carry_portable.
static bool g_shot = false;
static std::filesystem::path g_shotModsDir;  // the mods folder pages show instead of the game's (demo data)
static bool g_shotScrollPacks = false;       // the model-packs shot: the Mods page scrolled to Model packs
// Quick match (aki-roadmap#22): the data folder whose mod settings the running game's quick match changed, put back
// once the session ends (empty = none).
static std::filesystem::path g_qmRunDir;
static bool g_quickBackRequested = false;
static constexpr int HOTKEY_QUICKBACK = 1;

struct BoxArt {
    Texture front;
    Texture back;
    Texture cart;      // cartridge scan, third face in the flip cycle
    Texture backdrop;  // blurred front cover for the full-screen background
};

struct UiState {
    int selected = 0;
    int face = 0;           // 0 = front, 1 = back
    int facePrev = 0;       // face shown during the first half of a flip
    float flipAnim = 1.0f;  // 0 -> 1 flip progress (1 = settled)
    int insertIndex = -1;   // game whose cart is sliding into the console (-1 = idle)
    float insertAnim = 0.0f;  // 0 -> 1 insert progress; launch fires at 1
    int bdIndex = 0;        // backdrop currently fading in
    int bdPrev = -1;        // backdrop fading out (-1 = none)
    float bdFade = 1.0f;    // 0 -> 1 crossfade progress
    ULONGLONG attractTick = 0;  // last attract-mode carousel step
    ImFont* fontBase = nullptr;
    ImFont* fontTitle = nullptr;
    ImFont* fontSmall = nullptr;
    ImFont* fontHeader = nullptr;
    bool padInput = false;  // last-used input device drives the footer hints
    // settings screen
    bool showSettings = false;
    bool captureChord = false;
    bool chordArmed = false;  // wait for all buttons up before capturing (the
                              // A press that activated Rebind must not count)
    WORD chordPending = 0;
    bool captureHotkey = false;
    std::vector<std::string> exeEdit, romEdit;  // per-game path field buffers
    // conversions page (full-conversion mods of the selected game, every type side by side)
    bool showConversions = false;
    std::vector<ConversionInfo> convList;
    int convFor = -1;           // game the list was read for
    std::wstring convMessage;   // last add / remove result
    // mods page (aki-roadmap#96)
    bool showMods = false;
    int modsGame = -1;
    std::filesystem::path modsDir;
    std::vector<ModItem> modItems;
    std::vector<Texture> modThumbs;  // parallel to modItems (empty srv = none)
    std::string modsNote;
    bool modsNoteOk = true;
    int modsRemove = -1;  // Remove pressed: done at the start of the next frame, before any thumbnail is drawn
    int packRemove = -1;  // the same for a model pack
    // Model packs (aki-roadmap#130): <mods>\models\ and its models.txt, for games with model import
    std::vector<ModelPack> packs;
    std::vector<Texture> packThumbs;  // parallel to packs
    ModelsTxt modelsTxt;
    Roster roster;
    bool haveRoster = false;
    std::filesystem::path rosterFile;
    std::string packsNote;
    bool packsNoteOk = true;
    bool modelsTxtOk = true;  // read without errors: editable
    // Conversions (aki-roadmap#154): the Mods page's list, and each game's for Play as... (by game index)
    std::vector<bool> modIsHack;  // parallel to modItems: a patch-mod ROM hack (it belongs under Conversions)
    std::vector<pkg::ConversionInfo> convs;
    int convRemove = -1, convPlay = -1, convMove = -1;  // pressed: done at the start of the next frame
    std::map<std::wstring, std::vector<pkg::ConversionInfo>> convCache;  // by artKey
    // launcher self-update (aki-roadmap#117)
    bool showUpdate = false;
    bool updateRequested = false;  // "Update now": install as soon as the download is verified
    std::string updateNote;        // the last apply's outcome
    // quick match (aki-roadmap#22)
    bool showQuick = false;
    int quickGame = -1;
    bool qmFound = false;
    QuickMatchMod qm;
    std::string qmWhy;        // why there is no quick match for this game
    std::vector<int> qmPick;  // per qm.options: the choice (randomOk options: 0 = Random, then the choices)
    std::string quickNote;
    bool qmLaunch = false;    // the launch draw_ui returns is a quick match, with these values
    std::map<std::string, std::string> qmValues;
    // History Tour (aki-roadmap#23)
    bool showTour = false;
    Tour tour;
    bool tourOk = false;
    std::string tourErr, tourSource, tourNote;
    int tourSel = 0;
    int tourLaunch = -1;   // the stop the launch draw_ui returns is for
    int tourRunning = -1;  // the stop whose game is running
};
static UiState g_ui;
static HWND g_hwnd = nullptr;
// Files dropped on the window (conversion packages), handled by the render loop.
// Env for the next launch (a conversion's), set by the Conversions page or --conversion.
static std::vector<std::pair<std::wstring, std::wstring>> g_launchEnv;
static DWORD g_attractMs = 60000;  // idle before attract mode (--attract-ms)
static std::vector<std::filesystem::path> g_dropped;  // files / folders dropped on the window

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
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wparam;
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            UINT len = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring buf(len + 1, L'\0');
            if (len && DragQueryFileW(drop, i, buf.data(), len + 1)) {
                buf.resize(len);
                g_dropped.emplace_back(buf);
            }
        }
        DragFinish(drop);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// Any keyboard key, mouse button, wheel or mouse movement this frame — the
// signal that the user is NOT on the controller (for the footer hints).
static bool keyboard_mouse_activity() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f || io.MouseWheel != 0.0f) return true;
    for (int i = 0; i < IM_ARRAYSIZE(io.MouseDown); ++i)
        if (io.MouseDown[i]) return true;
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
        if (k >= ImGuiKey_GamepadStart && k <= ImGuiKey_GamepadRStickDown) continue;
        if (ImGui::IsKeyDown((ImGuiKey)k)) return true;
    }
    return false;
}

// Newly-pressed XInput buttons this frame (any pad), for carousel navigation.
static WORD pad_pressed() {
    if (g_shot) return 0;  // a pad someone is holding mustn't drive a screenshot run
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

static std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, 0);
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

// Overlay settings.ini overrides onto the registry defaults and re-stat.
static void apply_overrides(std::vector<GameEntry>& games) {
    std::error_code ec;
    for (auto& g : games) {
        std::wstring romOverride;
        auto it = settings().overrides.find(g.artKey);
        if (it != settings().overrides.end()) {
            const GameOverride& o = it->second;
            // A stale override must not shadow a healthy default: only apply
            // path overrides whose target still exists. (The vpw2 bug: a dead
            // exe= override from earlier testing pinned the card to "Not
            // installed" even after a successful, SHA-verified in-app install.)
            if (!o.exePath.empty()) {
                if (std::filesystem::is_regular_file(o.exePath, ec)) {
                    g.exePath = o.exePath;
                    // [fullconv] Run an overridden exe from its own checkout (build-msvc\..), so its portable.txt,
                    // mods, conversions and saves are the ones used, not the default repo's.
                    std::filesystem::path dir = g.exePath.parent_path();
                    g.repoPath = (dir.filename() == L"build-msvc") ? dir.parent_path() : dir;
                }
                else logf(L"override exe for %s ignored (missing): %s",
                          g.artKey.c_str(), o.exePath.c_str());
            }
            romOverride = o.romPath;
            if (o.forceBorderless != -1) g.forceBorderless = o.forceBorderless != 0;
        }
        g.exeFound = std::filesystem::is_regular_file(g.exePath, ec);
        // After the exe override: the ROM belongs with the checkout that runs (aki-roadmap#216).
        resolve_rom(g, romOverride);
    }
}

static void reregister_hotkey() {
    UnregisterHotKey(g_hwnd, HOTKEY_QUICKBACK);
    RegisterHotKey(g_hwnd, HOTKEY_QUICKBACK, settings().hotkeyMods | MOD_NOREPEAT,
                   settings().hotkeyVk);
}

// ---------------------------------------------------------------------------
// Window mode (fullscreen WS_POPUP <-> normal resizable window)
// ---------------------------------------------------------------------------

// -1 = no switch pending; 0/1 = switch to fullscreen/windowed before the next
// frame (outside NewFrame/Render, because the font atlas is rebuilt).
static int g_pendingWindowMode = -1;

// Fonts are sized from the window height; rebuilt on every mode switch.
// The style the launcher sets up (sizes for a 900-pixel-high UI), scaled by build_fonts.
static ImGuiStyle g_baseStyle;
static float g_fontsH = 0;  // the UI height the fonts and style were built for

// Fonts and the style's sizes (padding, spacing, frame heights, rounding, scrollbars) for a UI uiH pixels high, so
// a 4K window looks like a 1080p one, only sharper.
static void build_fonts(float uiH) {
    ImGuiIO& io = ImGui::GetIO();
    g_fontsH = uiH;
    ImGuiStyle& style = ImGui::GetStyle();
    style = g_baseStyle;
    style.ScaleAllSizes(uiH / 900.0f);
    io.Fonts->Clear();
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
}

// Saved windowed placement, validated against the current monitor layout;
// falls back to 1280x720 centered on the primary monitor.
static RECT windowed_rect() {
    Settings& s = settings();
    RECT r{s.winX, s.winY, s.winX + s.winW, s.winY + s.winH};
    if (s.winW < 320 || s.winH < 240 || !MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
        int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
        r = {(sw - 1280) / 2, (sh - 720) / 2, (sw - 1280) / 2 + 1280, (sh - 720) / 2 + 720};
    }
    return r;
}

static void remember_windowed_placement() {
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (!GetWindowPlacement(g_hwnd, &wp)) return;
    const RECT& r = wp.rcNormalPosition;
    if (r.right - r.left < 320 || r.bottom - r.top < 240) return;
    settings().winX = r.left;
    settings().winY = r.top;
    settings().winW = r.right - r.left;
    settings().winH = r.bottom - r.top;
}

// Runtime switch. A running game follows automatically: GameSession::tick
// re-forces the game window whenever the launcher rect changes.
static void set_launcher_windowed(bool windowed) {
    if (g_shot) return;  // the screenshot window is never shown
    if (windowed) {
        RECT r = windowed_rect();
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(g_hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
    } else {
        remember_windowed_placement();
        HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{sizeof(mi)};
        if (!GetMonitorInfoW(mon, &mi)) return;
        const RECT& r = mi.rcMonitor;
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    settings().windowed = windowed;
    settings_save();
    RECT cr{};
    GetClientRect(g_hwnd, &cr);
    float uiH = (float)(cr.bottom - cr.top);
    if (uiH < 400) uiH = (float)GetSystemMetrics(SM_CYSCREEN);
    build_fonts(uiH);
    ImGui_ImplDX11_InvalidateDeviceObjects();  // recreated (with the atlas) on next NewFrame
    logf(L"window mode -> %s", windowed ? L"windowed" : L"fullscreen");
}

// Sizes given in pixels for a 900-pixel-high UI, at the current one (fonts and style follow the same scale).
static float ui_px(float px) { return px * (g_fontsH > 0 ? g_fontsH / 900.0f : 1.0f); }

// A page's content column width: a share of the window, but no wider than maxAspect x its height, so forms stay
// a readable width on wide and ultrawide screens. Pages centre it.
static float page_column(float vw, float vh, float share, float maxAspect) {
    return fminf(vw * share, vh * maxAspect);
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
// face: 0 = front cover, 1 = back cover, 2 = cartridge.
static void draw_box_face(ImDrawList* dl, const Texture& tex, ImVec2 p0, ImVec2 p1, int face,
                          const std::string& title, float rounding, bool dimmed) {
    ImU32 tint = dimmed ? IM_COL32(140, 140, 140, 255) : IM_COL32_WHITE;
    if (tex.ok()) {
        if (face == 2) {
            // Cart scans have their own shape/alpha: aspect-fit on a dark card
            // instead of stretching to the box aspect.
            dl->AddRectFilled(p0, p1, dimmed ? palette::cardBgSoon : palette::cardBg, rounding);
            float w = p1.x - p0.x, h = p1.y - p0.y;
            float aw = w * 0.88f, ah = h * 0.88f;
            float ta = (float)tex.w / (float)tex.h;
            float dw = aw, dh = dw / ta;
            if (dh > ah) { dh = ah; dw = dh * ta; }
            ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
            dl->AddImage((ImTextureID)tex.srv, ImVec2(c.x - dw * 0.5f, c.y - dh * 0.5f),
                         ImVec2(c.x + dw * 0.5f, c.y + dh * 0.5f), ImVec2(0, 0), ImVec2(1, 1),
                         tint);
        } else {
            dl->AddImageRounded((ImTextureID)tex.srv, p0, p1, ImVec2(0, 0), ImVec2(1, 1), tint,
                                rounding);
        }
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
    const char* lbl = face == 2 ? "cartridge" : face == 1 ? "back cover" : "cover art";
    ImVec2 lsz = f->CalcTextSizeA(fs * 0.85f, FLT_MAX, 0, lbl);
    dl->AddText(f, fs * 0.85f, ImVec2(p0.x + (w - lsz.x) * 0.5f, p1.y - fs * 1.6f),
                IM_COL32(150, 152, 160, dimmed ? 100 : 170), lbl);
}

// Validate (SHA1 when known) and copy a user-picked ROM to the checkout that runs
// (romInstallPath, never over the file a rom= override names, and never over another
// checkout's ROM); the override is dropped since the picked ROM replaces it. Falls back
// to an override pointing at the original if the copy fails. errOut untouched on success.
static bool install_rom_file(GameEntry& g, const std::filesystem::path& src,
                             std::wstring* errOut) {
    if (!g.romSha1.empty()) {
        std::wstring h = sha1_file(src);
        if (h.empty()) {
            *errOut = L"Couldn't read " + src.wstring();
            return false;
        }
        if (h != g.romSha1) {
            *errOut = L"That file isn't the accepted " + g.title +
                      L" ROM. This port needs exactly one dump: the " + g.romNote +
                      L", as an unmodified big-endian .z64 file. Patched/translated"
                      L" or byte-swapped (.n64/.v64) dumps won't match.\n"
                      L"Required SHA1: " + g.romSha1 + L"\n"
                      L"Your file's SHA1: " + h;
            if (!g.romHint.empty()) *errOut += L"\n" + g.romHint;
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::create_directories(g.romInstallPath.parent_path(), ec);
    ec.clear();
    std::filesystem::copy_file(src, g.romInstallPath, std::filesystem::copy_options::overwrite_existing,
                               ec);
    auto ov = settings().overrides.find(g.artKey);
    if (ec) {
        settings().overrides[g.artKey].romPath = src.wstring();
        settings_save();
    } else if (ov != settings().overrides.end() && !ov->second.romPath.empty()) {
        ov->second.romPath.clear();
        settings_save();
    }
    logf(L"ROM set for %s from %s%s", g.title.c_str(), src.c_str(),
         ec ? L" (copy failed, using override)" : L" (copied)");
    return true;
}

// Modal system file picker; returns false on cancel (errOut empty) or failure.
static bool pick_rom(GameEntry& g, std::wstring* errOut) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg)))) {
        *errOut = L"File dialog unavailable";
        return false;
    }
    COMDLG_FILTERSPEC types[] = {{L"N64 ROM (*.z64)", L"*.z64"}, {L"All files", L"*.*"}};
    dlg->SetFileTypes(2, types);
    std::wstring title = L"Select your " + g.title + L" ROM - " + g.romNote + L" (.z64)";
    dlg->SetTitle(title.c_str());
    bool ok = false;
    if (SUCCEEDED(dlg->Show(g_hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR psz = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &psz))) {
                ok = install_rom_file(g, psz, errOut);
                CoTaskMemFree(psz);
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

// Modal folder picker for the ROM scan; empty on cancel.
static std::filesystem::path pick_folder(const std::wstring& start) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg))))
        return {};
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Scan a folder for N64 ROMs (subfolders too)");
    if (!start.empty()) {
        IShellItem* si = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&si)))) {
            dlg->SetFolder(si);
            si->Release();
        }
    }
    std::filesystem::path out;
    if (SUCCEEDED(dlg->Show(g_hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR psz = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &psz))) {
                out = psz;
                CoTaskMemFree(psz);
            }
            item->Release();
        }
    }
    dlg->Release();
    return out;
}

// Blurred cover of the selected game behind the whole UI, crossfading on
// selection change, under a dark scrim so the art tints rather than competes.
static void draw_backdrop(ImDrawList* dl, ImGuiViewport* vp, const std::vector<BoxArt>& art) {
    if (art.empty()) return;
    if (g_ui.selected != g_ui.bdIndex) {
        g_ui.bdPrev = g_ui.bdIndex;
        g_ui.bdIndex = g_ui.selected;
        g_ui.bdFade = 0.0f;
    }
    g_ui.bdFade = fminf(1.0f, g_ui.bdFade + ImGui::GetIO().DeltaTime / 0.40f);
    if (g_ui.bdFade >= 1.0f) g_ui.bdPrev = -1;

    ImVec2 p0 = vp->WorkPos;
    ImVec2 p1(vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y);
    float va = vp->WorkSize.x / vp->WorkSize.y;

    auto drawOne = [&](const Texture& t, float alpha) {
        if (!t.ok() || alpha <= 0.0f) return;
        // aspect-fill: crop the texture to the viewport's aspect via UVs
        float ta = (float)t.w / (float)t.h;
        ImVec2 uv0(0, 0), uv1(1, 1);
        if (ta > va) {
            float u = va / ta;
            uv0.x = 0.5f - u * 0.5f;
            uv1.x = 0.5f + u * 0.5f;
        } else {
            float v = ta / va;
            uv0.y = 0.5f - v * 0.5f;
            uv1.y = 0.5f + v * 0.5f;
        }
        dl->AddImage((ImTextureID)t.srv, p0, p1, uv0, uv1,
                     IM_COL32(255, 255, 255, (int)(alpha * 255.0f)));
    };
    if (g_ui.bdPrev >= 0 && g_ui.bdPrev < (int)art.size())
        drawOne(art[g_ui.bdPrev].backdrop, 1.0f);
    if (g_ui.bdIndex >= 0 && g_ui.bdIndex < (int)art.size())
        drawOne(art[g_ui.bdIndex].backdrop, g_ui.bdFade);

    // scrim: keep the UI readable; heavier toward the bottom (text zone)
    dl->AddRectFilled(p0, p1, IM_COL32(7, 8, 12, 168));
    ImVec2 mid(p0.x, p0.y + vp->WorkSize.y * 0.45f);
    dl->AddRectFilledMultiColor(mid, p1, IM_COL32(7, 8, 12, 0), IM_COL32(7, 8, 12, 0),
                                IM_COL32(7, 8, 12, 150), IM_COL32(7, 8, 12, 150));
}

static void open_settings(std::vector<GameEntry>& games) {
    g_ui.exeEdit.resize(games.size());
    g_ui.romEdit.resize(games.size());
    for (size_t i = 0; i < games.size(); ++i) {
        g_ui.exeEdit[i] = narrow(games[i].exePath.wstring());
        g_ui.romEdit[i] = narrow(games[i].romPath.wstring());
    }
    g_ui.showSettings = true;
}

static void close_settings(std::vector<GameEntry>& games) {
    // Path fields become overrides when they differ from the registry default.
    for (size_t i = 0; i < games.size(); ++i) {
        GameEntry& g = games[i];
        std::wstring defExe = (g.repoPath / g.exeRel).wstring();
        std::wstring defRom = g.romAutoPath.wstring();
        std::wstring edExe = widen(g_ui.exeEdit[i]);
        std::wstring edRom = widen(g_ui.romEdit[i]);
        GameOverride& o = settings().overrides[g.artKey];
        o.exePath = (!edExe.empty() && edExe != defExe) ? edExe : L"";
        o.romPath = (!edRom.empty() && edRom != defRom) ? edRom : L"";
    }
    settings_save();
    apply_overrides(games);
    g_ui.showSettings = false;
    g_ui.captureChord = false;
    g_ui.captureHotkey = false;
    logf(L"settings saved to %s", settings_path().c_str());
}

// --- Mods page (aki-roadmap#96): a game's mods folder ---------------------------

static void release_mod_thumbs() {
    for (auto& t : g_ui.modThumbs)
        if (t.srv) t.srv->Release();
    g_ui.modThumbs.clear();
    for (auto& t : g_ui.packThumbs)
        if (t.srv) t.srv->Release();
    g_ui.packThumbs.clear();
}

// Model packs: the packs, models.txt and (for picking wrestlers by name) the game's roster.
static void refresh_model_packs(const GameEntry& g) {
    g_ui.packs.clear();
    g_ui.modelsTxt = ModelsTxt{};
    g_ui.haveRoster = false;
    if (g.modelProfile.empty()) return;
    std::filesystem::path models = g_ui.modsDir / L"models";
    g_ui.packs = list_model_packs(models);
    for (const auto& p : g_ui.packs)
        g_ui.packThumbs.push_back(p.preview.empty() ? Texture{} : load_texture(g_device, p.preview));
    std::wstring err;
    g_ui.modelsTxtOk = read_models_txt(models / L"models.txt", &g_ui.modelsTxt, &err);
    if (!g_ui.modelsTxtOk) {
        g_ui.packsNote = narrow(err) + " (fix it, then Refresh: the controls are off until it reads)";
        g_ui.packsNoteOk = false;
    }
    g_ui.rosterFile = find_roster(g.modelProfile, app_root(), detect_depot_root());
    if (!g_ui.rosterFile.empty()) g_ui.haveRoster = load_roster_file(g_ui.rosterFile, &g_ui.roster, &err);
}

// One change from the page, applied to models.txt as it is on disk now (it may have been edited by hand since the page
// was drawn), then written back.
struct PackEdit {
    std::string pack;
    bool setOff = false, off = false;
    ModelsTxtLine::Kind kind = ModelsTxtLine::Kind::Blank;
    int slot = 0, wrestler = -1, costume = -1;
    std::string comment;
};

static void apply_pack_edit(const PackEdit& e) {
    std::filesystem::path file = g_ui.modsDir / L"models" / L"models.txt";
    ModelsTxt fresh;
    std::wstring err;
    if (!read_models_txt(file, &fresh, &err)) {
        g_ui.packsNote = narrow(err) + ": not changed";
        g_ui.packsNoteOk = false;
        return;
    }
    if (e.setOff) fresh.set_off(e.pack, e.off);
    else fresh.set_assignment(e.pack, e.kind, e.slot, e.wrestler, e.costume, e.comment);
    if (write_models_txt(file, fresh, &err)) {
        g_ui.modelsTxt = std::move(fresh);
        g_ui.packsNote = "Saved models.txt: the game picks it up at the next match (or flip Reload in its Mods menu).";
        g_ui.packsNoteOk = true;
    } else {
        g_ui.packsNote = narrow(err);
        g_ui.packsNoteOk = false;
    }
}

static std::string assignment_text(const ModelsTxtLine* a) {
    if (!a) return "nobody (not assigned)";
    if (a->kind == ModelsTxtLine::Kind::Slot) return "slot " + std::to_string(a->slot);
    char buf[48];
    snprintf(buf, sizeof(buf), "wrestler 0x%04x", a->wrestler);
    std::string s = buf;
    if (g_ui.haveRoster)
        if (const RosterEntry* e = g_ui.roster.find(a->wrestler)) s = e->name + " (" + s.substr(9) + ")";
    if (a->costume >= 0) s += ", costume " + std::to_string(a->costume);
    return s;
}

static void draw_model_packs(const GameEntry& g, float panelW, float thumb) {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(palette::accent, "Model packs");
    ImGui::PushFont(g_ui.fontSmall);
    ImGui::TextColored(palette::faint, "%s   -   who each pack replaces is in models.txt; the aki_modelimport mod reads it every match",
                       narrow((g_ui.modsDir / L"models").wstring()).c_str());
    if (ImGui::SmallButton("Open models.txt")) {
        std::filesystem::path mt = g_ui.modsDir / L"models" / L"models.txt";
        std::error_code ec;
        std::wstring err;
        if (!std::filesystem::exists(mt, ec)) write_models_txt(mt, ModelsTxt::parse("# pack       replaces\n"), &err);
        ShellExecuteW(nullptr, L"open", mt.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::SameLine();
    ImGui::TextColored(palette::faint, "%s", g_ui.haveRoster ? ("wrestler names: " + narrow(g_ui.rosterFile.filename().wstring())).c_str()
                                                             : "no wrestler roster for this game yet: wrestler lines are read-only here");
    if (!g_ui.packsNote.empty())
        ImGui::TextColored(g_ui.packsNoteOk ? palette::ready : palette::error, "%s", g_ui.packsNote.c_str());
    ImGui::PopFont();
    if (g_ui.packs.empty())
        ImGui::TextColored(palette::dim, "No model packs yet: AkiTools `aki-model.py quick` builds and installs one.");
    bool changed = false;
    PackEdit pe;
    for (int i = 0; i < (int)g_ui.packs.size(); ++i) {
        const ModelPack& p = g_ui.packs[i];
        ImGui::PushID(1000 + i);
        ImVec2 top = ImGui::GetCursorPos();
        if (i < (int)g_ui.packThumbs.size() && g_ui.packThumbs[i].srv) {
            const Texture& tx = g_ui.packThumbs[i];
            float w = thumb * (float)tx.w / (float)(tx.h ? tx.h : 1);
            ImGui::Image((ImTextureID)tx.srv, ImVec2(w > thumb * 2 ? thumb * 2 : w, thumb));
        } else {
            ImGui::Dummy(ImVec2(thumb, thumb));
        }
        ImGui::SetCursorPos(ImVec2(top.x + thumb * 2 + ui_px(14), top.y));
        ImGui::BeginGroup();
        ImGui::TextUnformatted((p.name.empty() ? p.id : p.name).c_str());
        ImGui::PushFont(g_ui.fontSmall);
        std::string line = p.id;
        if (!p.author.empty()) line += "  -  by " + p.author;
        if (p.triangles) line += "  -  " + std::to_string(p.triangles) + " triangles";
        ImGui::TextColored(palette::dim, "%s", line.c_str());
        if (p.license.empty()) ImGui::TextColored(palette::warn, "no license in pack.json: don't share it as it is");
        else ImGui::TextColored(palette::faint, "license: %s", p.license.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) g_ui.packRemove = i;  // next frame, before the thumbnails are drawn
        // The pack's current lines, copied (an edit replaces the lines, so no pointer into them is kept).
        const ModelsTxtLine* ap = g_ui.modelsTxt.assignment(p.id);
        const bool has = ap != nullptr;
        const ModelsTxtLine::Kind akind = has ? ap->kind : ModelsTxtLine::Kind::Blank;
        const int aslot = has ? ap->slot : 0, awr = has ? ap->wrestler : -1, acos = has ? ap->costume : -1;
        const std::string atext = assignment_text(ap);
        const int lineCount = g_ui.modelsTxt.assignment_count(p.id);
        // Read-only when the launcher can't edit it safely: models.txt didn't read, the folder isn't a valid pack id,
        // the pack has several lines (only models.txt shows them all), or a wrestler line with no roster.
        std::string lock;
        if (!g_ui.modelsTxtOk) lock = "models.txt didn't read";
        else if (!valid_pack_id(p.id)) lock = "rename the folder: pack ids are a-z 0-9 _ - (at most 31)";
        else if (lineCount > 1) lock = "+" + std::to_string(lineCount - 1) + " more line(s) in models.txt: edit it there";
        else if (akind == ModelsTxtLine::Kind::Wrestler && !g_ui.haveRoster) lock = "no roster: edit models.txt to change it";
        // On / off: the "off <pack>" line.
        if (!lock.empty() && (!g_ui.modelsTxtOk || !valid_pack_id(p.id))) {
            ImGui::TextColored(palette::warn, "%s", lock.c_str());
        } else {
            bool on = !g_ui.modelsTxt.is_off(p.id);
            if (ImGui::Checkbox("On", &on) && !changed) {
                pe.pack = p.id;
                pe.setOff = true;
                pe.off = !on;
                changed = true;
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(panelW * 0.32f);
            if (!lock.empty()) {
                ImGui::TextColored(palette::dim, "replaces %s (%s)", atext.c_str(), lock.c_str());
            } else if (ImGui::BeginCombo("##who", ("replaces " + atext).c_str())) {
                if (ImGui::Selectable("nobody (not assigned)", !has) && !changed) {
                    pe.pack = p.id;
                    pe.kind = ModelsTxtLine::Kind::Blank;
                    changed = true;
                }
                for (int s = 1; s <= 4; ++s) {
                    bool sel = akind == ModelsTxtLine::Kind::Slot && aslot == s;
                    if (ImGui::Selectable(("slot " + std::to_string(s) + " of the match").c_str(), sel) && !changed) {
                        pe.pack = p.id;
                        pe.kind = ModelsTxtLine::Kind::Slot;
                        pe.slot = s;
                        changed = true;
                    }
                }
                if (g_ui.haveRoster) {
                    for (const auto& w : g_ui.roster.wrestlers) {
                        bool sel = akind == ModelsTxtLine::Kind::Wrestler && awr == w.id;
                        char lbl[160];
                        snprintf(lbl, sizeof(lbl), "%s (0x%04x)##w%d", w.name.c_str(), w.id, w.id);
                        if (ImGui::Selectable(lbl, sel) && !changed) {
                            pe.pack = p.id;
                            pe.kind = ModelsTxtLine::Kind::Wrestler;
                            pe.wrestler = w.id;
                            pe.comment = w.name;
                            changed = true;
                        }
                    }
                }
                ImGui::EndCombo();
            }
            // A wrestler's costume, when the roster names them.
            const RosterEntry* re = lock.empty() && akind == ModelsTxtLine::Kind::Wrestler && g_ui.haveRoster
                                        ? g_ui.roster.find(awr) : nullptr;
            if (re && !re->costumes.empty()) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(panelW * 0.18f);
                std::string cur = acos < 0 ? "any costume" : "costume " + std::to_string(acos);
                for (const auto& c : re->costumes)
                    if (c.first == acos && !c.second.empty()) cur = c.second;
                if (ImGui::BeginCombo("##costume", cur.c_str())) {
                    if (ImGui::Selectable("any costume", acos < 0) && !changed) {
                        pe.pack = p.id;
                        pe.kind = ModelsTxtLine::Kind::Wrestler;
                        pe.wrestler = awr;
                        pe.comment = re->name;
                        changed = true;
                    }
                    for (const auto& c : re->costumes) {
                        std::string lbl = std::to_string(c.first) + (c.second.empty() ? "" : "  " + c.second) + "##c" +
                                          std::to_string(c.first);
                        if (ImGui::Selectable(lbl.c_str(), acos == c.first) && !changed) {
                            pe.pack = p.id;
                            pe.kind = ModelsTxtLine::Kind::Wrestler;
                            pe.wrestler = awr;
                            pe.costume = c.first;
                            pe.comment = re->name;
                            changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
            }
        }
        ImGui::PopFont();
        ImGui::EndGroup();
        float bottom = top.y + thumb;
        if (ImGui::GetCursorPosY() < bottom) ImGui::SetCursorPosY(bottom);
        ImGui::Spacing();
        ImGui::PopID();
    }
    if (changed) apply_pack_edit(pe);  // after drawing: nothing above still looks at the old lines
    std::vector<std::string> missing = g_ui.modelsTxt.missing_packs(g_ui.packs);
    if (!missing.empty()) {
        std::string m;
        for (const auto& x : missing) m += (m.empty() ? "" : ", ") + x;
        ImGui::PushFont(g_ui.fontSmall);
        ImGui::TextColored(palette::faint, "models.txt also names %s (not installed here)", m.c_str());
        ImGui::PopFont();
    }
}

static std::filesystem::path conversions_data_dir(const GameEntry& g);

static void refresh_mods(const std::vector<GameEntry>& games) {
    release_mod_thumbs();
    const GameEntry& g = games[g_ui.modsGame];
    g_ui.modsDir = g_shotModsDir.empty() ? game_mods_dir(g) : g_shotModsDir;
    // Bundled mods sit in assets\mods next to the exe (none known without one).
    std::filesystem::path bundled = g.exeFound && g_shotModsDir.empty() ? g.exePath.parent_path() / L"assets" / L"mods"
                                                                         : std::filesystem::path();
    g_ui.modItems = list_mods(g_ui.modsDir, bundled);
    // Thumbnails go through a temp file: the texture loader reads files.
    wchar_t tmpBuf[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmpBuf);
    std::filesystem::path tmp = std::filesystem::path(tmpBuf) / L"akilauncher-thumb.png";
    for (const auto& m : g_ui.modItems) {
        Texture t;
        std::vector<uint8_t> png = read_mod_thumb(m);
        if (!png.empty()) {
            {
                std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                f.write((const char*)png.data(), (std::streamsize)png.size());
            }
            t = load_texture(g_device, tmp);
        }
        g_ui.modThumbs.push_back(t);
    }
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    refresh_model_packs(g);
    g_ui.modIsHack.clear();
    for (const auto& m : g_ui.modItems)
        g_ui.modIsHack.push_back(m.kind == ModKind::Nrm && !m.code && !m.bundled && pkg::is_patch_mod(m.path));
    g_ui.convs = pkg::list_conversions(conversions_data_dir(g));
    g_ui.convCache.clear();
}

// aki-roadmap#14: a portable launcher makes the games it installed portable
// (once: portable.txt + a copy of their AppData data) before they start or
// their mods folder is shown.
static void carry_portable(const GameEntry& g) {
    if (g_shot) return;  // screenshots change nothing on disk
    if (!wants_portable_game(g, launcher_portable(), settings().portableGames)) return;
    wchar_t lad[MAX_PATH]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH);
    std::filesystem::path appData = std::filesystem::path(lad) / std::filesystem::path(g.exeRel).stem();
    // Synchronous: a first-time copy (config, saves, mods) before the game reads them.
    ULONGLONG t0 = GetTickCount64();
    int n = make_game_portable(g, appData);
    if (n < 0)
        logf(L"portable: %s stays on %s for now (a copy failed; retried next start)", g.title.c_str(),
             appData.c_str());
    else
        logf(L"portable: %s now keeps its data in %s (%d file(s) copied from %s, %llu ms)", g.title.c_str(),
             g.repoPath.c_str(), n, appData.c_str(), GetTickCount64() - t0);
}

// --- Conversions and packages (aki-roadmap#154) ---------------------------------

// Where a game's conversions are: its data folder, or in screenshot mode the demo one beside the demo mods folder.
static std::filesystem::path conversions_data_dir(const GameEntry& g) {
    return g_shotModsDir.empty() ? game_data_dir(g) : std::filesystem::path(g_shotModsDir.wstring() + L"-data");
}

// The game's conversions (cached per game; dropped when the Mods page closes or a game ends).
static const std::vector<pkg::ConversionInfo>& game_conversions(const std::vector<GameEntry>& games, int i) {
    auto it = g_ui.convCache.find(games[i].artKey);
    if (it == g_ui.convCache.end()) {
        std::vector<pkg::ConversionInfo> v;
        if (g_shot ? !g_shotModsDir.empty() : games[i].exeFound) v = pkg::list_conversions(conversions_data_dir(games[i]));
        it = g_ui.convCache.emplace(games[i].artKey, std::move(v)).first;
    }
    return it->second;
}

// The conversion Play starts game i as: the remembered pick while it is still installed, else nullptr (the original).
static const pkg::ConversionInfo* play_as(const std::vector<GameEntry>& games, int i) {
    auto o = settings().overrides.find(games[i].artKey);
    if (o == settings().overrides.end() || o->second.playAs.empty()) return nullptr;
    for (const auto& c : game_conversions(games, i))
        if (widen(c.id) == o->second.playAs) return &c;
    return nullptr;
}

static void set_play_as(const GameEntry& g, const std::string& id) {
    settings().overrides[g.artKey].playAs = widen(id);
    settings_save();
}

static std::vector<std::string> rom_sha1_of(const GameEntry& g) {
    std::vector<std::string> v;
    if (!g.romSha1.empty()) v.push_back(narrow(g.romSha1));
    return v;
}

static pkg::InstallTarget install_target(const GameEntry& g) {
    pkg::InstallTarget t;
    t.dataDir = game_data_dir(g);
    t.gameTitle = g.title;
    t.gameId = narrow(g.modGameId);
    t.romSha1 = rom_sha1_of(g);  // the dump the game accepts: the player's ROM was checked against it
    std::filesystem::path rom = g.romPath;
    t.hashRom = [rom] { return narrow(sha1_file(rom)); };
    return t;
}

static bool is_package_source(const std::filesystem::path& p) {
    pkg::Package pk(p);
    return pk.opened() && pk.has("aki-package.json");
}

// A package (or a patch-mod ROM hack, which becomes a conversion), with Yes/No for a reinstall or a downgrade.
static pkg::PackageInstall install_one_package(const GameEntry& g, const std::filesystem::path& src, bool patchMod) {
    pkg::InstallTarget t = install_target(g);
    pkg::InstallOptions o;
    o.gameRunning = g_session.active() && g_session.artKey == g.artKey;
    auto run = [&] { return patchMod ? pkg::install_patch_mod(src, t, o) : pkg::install_package(src, t, o); };
    pkg::PackageInstall r = run();
    using O = pkg::PackageInstall::Outcome;
    if (r.outcome == O::AlreadyInstalled || r.outcome == O::NeedsDowngradeConfirm) {
        bool down = r.outcome == O::NeedsDowngradeConfirm;
        std::wstring ask = r.message + (down ? L".\n\nInstall the older version anyway? Files you changed are kept as .edited copies."
                                             : L".\n\nInstall it again? Files you changed are kept as .edited copies.");
        if (MessageBoxW(g_hwnd, ask.c_str(), L"AKI Launcher", MB_YESNO | MB_ICONQUESTION) == IDYES) {
            (down ? o.allowDowngrade : o.reinstall) = true;
            r = run();
        }
    }
    for (const auto& n : r.notes) r.message += L"  (" + n + L")";
    return r;
}

static void open_mods(const std::vector<GameEntry>& games, int index) {
    carry_portable(games[index]);
    g_ui.modsGame = index;
    g_ui.modsNote.clear();
    g_ui.packsNote.clear();
    g_ui.modsRemove = -1;
    refresh_mods(games);
    g_ui.showMods = true;
}

static void close_mods() {
    release_mod_thumbs();
    g_ui.modItems.clear();
    g_ui.convs.clear();
    g_ui.convCache.clear();
    g_ui.showMods = false;
}

static void mods_note(const std::wstring& msg, bool ok) {
    g_ui.modsNote = narrow(msg);
    g_ui.modsNoteOk = ok;
}

static void install_into_mods(const std::vector<GameEntry>& games, const std::vector<std::filesystem::path>& srcs) {
    const GameEntry& g = games[g_ui.modsGame];
    std::wstring all;
    bool allOk = true;
    for (const auto& src : srcs) {
        // aki-roadmap#154: a package (aki-package.json) or a patch-mod ROM hack goes through the shared installer;
        // everything else (a plain .nrm, .rtz, zip of them, model-pack folder) installs as before.
        std::wstring ext = src.extension().wstring();
        bool hack = _wcsicmp(ext.c_str(), L".nrm") == 0 && pkg::is_patch_mod(src);
        bool ok;
        std::wstring msg;
        if (hack || is_package_source(src)) {
            pkg::PackageInstall r = install_one_package(g, src, hack);
            ok = r.ok();
            msg = r.message;
        } else {
            InstallResult r = install_mod(src, g_ui.modsDir, g.modGameId, g.title, g.modelProfile);
            ok = r.ok;
            msg = r.message;
        }
        allOk = allOk && ok;
        if (!all.empty()) all += L"   |   ";
        all += msg;
    }
    refresh_mods(games);
    mods_note(all + (allOk ? L". Start (or restart) the game to use it; turn mods on and off in its Mods menu." : L""),
              allOk);
    if (allOk) play_flip();
}

// Multi-select picker for mod files; empty on cancel.
static std::vector<std::filesystem::path> pick_mod_files() {
    std::vector<std::filesystem::path> out;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return out;
    COMDLG_FILTERSPEC types[] = {{L"Mods and texture packs (*.nrm, *.rtz, *.zip)", L"*.nrm;*.rtz;*.zip"},
                                 {L"All files", L"*.*"}};
    dlg->SetFileTypes(2, types);
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Install mods or texture packs");
    if (SUCCEEDED(dlg->Show(g_hwnd))) {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dlg->GetResults(&items))) {
            DWORD n = 0;
            items->GetCount(&n);
            for (DWORD i = 0; i < n; ++i) {
                IShellItem* it = nullptr;
                if (SUCCEEDED(items->GetItemAt(i, &it))) {
                    PWSTR psz = nullptr;
                    if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &psz))) {
                        out.emplace_back(psz);
                        CoTaskMemFree(psz);
                    }
                    it->Release();
                }
            }
            items->Release();
        }
    }
    dlg->Release();
    return out;
}

static std::filesystem::path pick_folder(const std::wstring& start);

// The game index to launch (a conversion's Play), else -1.
static int draw_mods(std::vector<GameEntry>& games, WORD pad) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float vw = vp->WorkSize.x, vh = vp->WorkSize.y;
    ImGuiIO& io = ImGui::GetIO();
    const GameEntry& g = games[g_ui.modsGame];
    if ((ImGui::IsKeyPressed(ImGuiKey_Escape) || (pad & XINPUT_GAMEPAD_B)) && !io.WantTextInput) {
        close_mods();
        return -1;
    }
    // A conversion's Play, Remove, or a ROM hack's Move to Conversions from last frame (before any thumbnail is drawn).
    if (g_ui.convPlay >= 0 && g_ui.convPlay < (int)g_ui.convs.size()) {
        set_play_as(g, g_ui.convs[g_ui.convPlay].id);
        int idx = g_ui.modsGame;
        g_ui.convPlay = -1;
        close_mods();
        return idx;
    }
    g_ui.convPlay = -1;
    if (g_ui.convRemove >= 0 && g_ui.convRemove < (int)g_ui.convs.size()) {
        std::wstring msg;
        bool running = g_session.active() && g_session.artKey == g.artKey;
        bool ok = pkg::remove_package(g_ui.convs[g_ui.convRemove].id, conversions_data_dir(g), running, &msg);
        mods_note(ok ? msg + L" (in the Recycle Bin)" : L"Not removed: " + msg, ok);
        refresh_mods(games);
    }
    g_ui.convRemove = -1;
    if (g_ui.convMove >= 0 && g_ui.convMove < (int)g_ui.modItems.size()) {
        std::filesystem::path from = g_ui.modItems[g_ui.convMove].path;
        pkg::PackageInstall r = install_one_package(g, from, true);  // moves it, and any copy with its mod id, out of the mods folder
        mods_note(r.message, r.ok());
        refresh_mods(games);
    }
    g_ui.convMove = -1;
    // A Remove from last frame: now, before this frame draws (and so holds) any thumbnail.
    if (g_ui.modsRemove >= 0 && g_ui.modsRemove < (int)g_ui.modItems.size()) {
        std::wstring err;
        const ModItem& m = g_ui.modItems[g_ui.modsRemove];
        std::wstring name = m.display_name();
        if (remove_mod(m, &err)) mods_note(L"Removed " + name + L" (it is in the Recycle Bin)", true);
        else mods_note(err, false);
        refresh_mods(games);
    }
    g_ui.modsRemove = -1;
    if (g_ui.packRemove >= 0 && g_ui.packRemove < (int)g_ui.packs.size()) {
        std::wstring err;
        const ModelPack& p = g_ui.packs[g_ui.packRemove];
        std::string name = p.name.empty() ? p.id : p.name;
        if (remove_model_pack(p.dir, g_ui.modsDir, &err))
            mods_note(L"Removed model pack " + widen(name) + L" (it is in the Recycle Bin; models.txt keeps its lines)", true);
        else
            mods_note(err, false);
        refresh_mods(games);
    }
    g_ui.packRemove = -1;

    ImGui::PushFont(g_ui.fontHeader);
    ImGui::SetCursorPosY(vh * 0.055f);
    text_centered("M O D S");
    ImGui::PopFont();
    ImGui::PushFont(g_ui.fontBase);
    text_centered_colored(palette::dim, narrow(g.title));
    ImGui::PopFont();

    float panelW = page_column(vw, vh, 0.70f, 1.45f);
    ImGui::SetCursorPos(ImVec2((vw - panelW) * 0.5f, vh * 0.15f));
    ImGui::BeginChild("##mods", ImVec2(panelW, vh * 0.74f), ImGuiChildFlags_None);
    ImGui::PushFont(g_ui.fontBase);
    if (ImGui::Button("Install files...")) {
        auto files = pick_mod_files();
        if (!files.empty()) install_into_mods(games, files);
    }
    ImGui::SameLine();
    if (ImGui::Button("Install a pack folder...")) {
        std::filesystem::path dir = pick_folder(L"");
        if (!dir.empty()) install_into_mods(games, {dir});
    }
    ImGui::SameLine();
    if (ImGui::Button("Open mods folder")) {
        std::error_code ec;
        std::filesystem::create_directories(g_ui.modsDir, ec);
        ShellExecuteW(nullptr, L"explore", g_ui.modsDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) refresh_mods(games);
    ImGui::PushFont(g_ui.fontSmall);
    ImGui::TextColored(palette::faint, "%s   -   or drop .nrm / .rtz / .zip files or pack folders on this window",
                       narrow(g_ui.modsDir.wstring()).c_str());
    if (!g_ui.modsNote.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(g_ui.modsNoteOk ? palette::ready : palette::error, "%s", g_ui.modsNote.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::PopFont();
    ImGui::Separator();

    if (g_ui.modItems.empty()) {
        ImGui::TextColored(palette::dim, "No mods or texture packs installed for this game yet.");
    }
    float thumb = vh * 0.07f;
    // The right-hand column (Remove, or "comes with the game"): as wide as its text at this size, plus the scrollbar.
    ImGui::PushFont(g_ui.fontSmall);
    const ImGuiStyle& st = ImGui::GetStyle();
    float rightW = fmaxf(ImGui::CalcTextSize("comes with").x, ImGui::CalcTextSize("Remove").x + st.FramePadding.x * 2) +
                   st.ScrollbarSize + st.ItemSpacing.x;
    ImGui::PopFont();
    int removeIndex = -1, moveIndex = -1;
    for (int i = 0; i < (int)g_ui.modItems.size(); ++i) {
        const ModItem& m = g_ui.modItems[i];
        ImGui::PushID(i);
        ImVec2 top = ImGui::GetCursorPos();
        if (i < (int)g_ui.modThumbs.size() && g_ui.modThumbs[i].srv)
            ImGui::Image((ImTextureID)g_ui.modThumbs[i].srv, ImVec2(thumb, thumb));
        else
            ImGui::Dummy(ImVec2(thumb, thumb));
        ImGui::SetCursorPos(ImVec2(top.x + thumb + ui_px(14), top.y));
        ImGui::BeginGroup();
        std::string head = narrow(m.display_name());
        if (!m.manifest.version.empty()) head += "  " + narrow(m.manifest.version);
        ImGui::TextUnformatted(head.c_str());
        ImGui::PushFont(g_ui.fontSmall);
        std::string line = narrow(m.kind_text());
        if (!m.manifest.authors.empty()) {
            line += "  -  by ";
            for (size_t a = 0; a < m.manifest.authors.size(); ++a)
                line += (a ? ", " : "") + narrow(m.manifest.authors[a]);
        }
        if (m.manifest.present && m.manifest.gameId.empty() && m.textures) line += "  -  any game";
        char size[48];
        snprintf(size, sizeof(size), "  -  %.1f MB", m.bytes / 1048576.0);
        line += size;
        line += "  -  " + narrow(m.path.filename().wstring()) + (m.kind == ModKind::Folder ? "\\" : "");
        ImGui::TextColored(palette::dim, "%s", line.c_str());
        if (!m.manifest.description.empty()) {
            ImGui::PushTextWrapPos(panelW - rightW - ui_px(20));
            ImGui::TextColored(palette::faint, "%s", narrow(m.manifest.description).c_str());
            ImGui::PopTextWrapPos();
        }
        if (!m.problem.empty()) ImGui::TextColored(palette::warn, "%s", narrow(m.problem).c_str());
        if (i < (int)g_ui.modIsHack.size() && g_ui.modIsHack[i]) {
            ImGui::TextColored(palette::warn, "A ROM hack: it belongs under Conversions (one at a time, its own saves)");
            if (ImGui::SmallButton("Move to Conversions")) moveIndex = i;
        }
        if (!m.manifest.gameId.empty() && !g.modGameId.empty() && m.manifest.gameId != g.modGameId)
            ImGui::TextColored(palette::warn, "made for %s, not this game", narrow(m.manifest.gameId).c_str());
        ImGui::PopFont();
        ImGui::EndGroup();
        ImGui::SameLine(panelW - rightW);
        ImGui::PushFont(g_ui.fontSmall);
        if (m.bundled) ImGui::TextColored(palette::faint, "comes with\nthe game");
        else if (ImGui::SmallButton("Remove")) removeIndex = i;
        ImGui::PopFont();
        float bottom = top.y + thumb;
        if (ImGui::GetCursorPosY() < bottom) ImGui::SetCursorPosY(bottom);
        ImGui::Spacing();
        ImGui::PopID();
    }
    g_ui.modsRemove = removeIndex;  // the thumbnails drawn above stay alive this frame
    g_ui.convMove = moveIndex;
    // Conversions (aki-roadmap#154): one at a time, each with its own saves. Play starts the game as it (and Play as...
    // on the home page remembers it); Remove takes what it installed, never its saves.
    {
        int playConv = -1, removeConv = -1;
        ImGui::Spacing();
        ImGui::TextColored(palette::accent, "Conversions");
        ImGui::PushFont(g_ui.fontSmall);
        if (g_ui.convs.empty())
            ImGui::TextColored(palette::dim, "None installed. A conversion package, or a ROM-hack .nrm, installs here through Install files...");
        std::vector<std::string> rom = rom_sha1_of(g);
        for (int i = 0; i < (int)g_ui.convs.size(); ++i) {
            const pkg::ConversionInfo& c = g_ui.convs[i];
            ImGui::PushID(10000 + i);
            ImGui::PushFont(g_ui.fontBase);
            ImGui::TextUnformatted((narrow(c.name) + (c.version.empty() ? "" : "  " + narrow(c.version))).c_str());
            ImGui::PopFont();
            std::string line = c.authors.empty() ? std::string() : "by " + narrow(c.authors) + "  -  ";
            line += c.recorded ? "installed as a package" : "added by hand";
            line += "  -  saves in saves\\" + c.id + "\\";
            for (const auto& a : c.addons) line += "  -  with " + a;
            ImGui::PushTextWrapPos(panelW - ui_px(20));
            ImGui::TextColored(palette::dim, "%s", line.c_str());
            ImGui::PopTextWrapPos();
            bool romOk = pkg::conversion_rom_matches(c, rom);
            ImGui::TextColored(romOk ? palette::ready : palette::warn, "%s", romOk ? "Your ROM matches" : "Made for another ROM than yours");
            if (g.launchable() && ImGui::SmallButton("Play")) playConv = i;
            std::error_code ec;
            if (std::filesystem::is_directory(c.saves, ec)) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Open saves folder")) ShellExecuteW(nullptr, L"explore", c.saves.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            if (c.recorded) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove")) removeConv = i;
            }
            ImGui::Spacing();
            ImGui::PopID();
        }
        ImGui::PopFont();
        g_ui.convPlay = playConv;
        g_ui.convRemove = removeConv;
        ImGui::Spacing();
    }
    if (!g.modelProfile.empty()) {
        // The model-packs shot: the section scrolled to the top (when the list is long enough to scroll) and outlined,
        // so it isn't the mods shot again.
        if (g_shotScrollPacks) ImGui::SetScrollHereY(0.0f);
        ImVec2 top = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->ChannelsSplit(2);
        ImGui::GetWindowDrawList()->ChannelsSetCurrent(1);
        draw_model_packs(g, panelW, thumb);
        ImGui::GetWindowDrawList()->ChannelsSetCurrent(0);
        if (g_shotScrollPacks) {
            ImVec2 pad(ImGui::GetStyle().ItemSpacing.x, ImGui::GetStyle().ItemSpacing.y);
            ImVec2 a(top.x - pad.x * 0.5f, top.y), b(top.x + ImGui::GetContentRegionAvail().x + pad.x * 0.5f,
                                                      ImGui::GetCursorScreenPos().y + pad.y);
            ImGui::GetWindowDrawList()->AddRectFilled(a, b, IM_COL32(250, 199, 72, 18), ImGui::GetStyle().FrameRounding);
            ImGui::GetWindowDrawList()->AddRect(a, b, IM_COL32(250, 199, 72, 170), ImGui::GetStyle().FrameRounding, 0, 2.0f);
        }
        ImGui::GetWindowDrawList()->ChannelsMerge();
    }
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PushFont(g_ui.fontSmall);
    ImGui::SetCursorPosY(vh * 0.92f);
    text_centered_colored(palette::faint, g_ui.padInput ? "B Back      The game turns mods on and off and sets their order in its own Mods menu"
                                                        : "Esc Back      The game turns mods on and off and sets their order in its own Mods menu");
    ImGui::PopFont();
    return -1;
}

// --- Quick match (aki-roadmap#22) --------------------------------------------

// A process running this exe (any instance, e.g. one an earlier launcher started).
static bool exe_running(const std::filesystem::path& exe) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    std::wstring name = exe.filename().wstring();
    for (BOOL more = Process32FirstW(snap, &pe); more && !found; more = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, name.c_str()) != 0) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) {
            found = true;  // same name, can't look closer: assume it's the game
            continue;
        }
        wchar_t path[MAX_PATH * 2]{};
        DWORD n = MAX_PATH * 2;
        if (QueryFullProcessImageNameW(h, 0, path, &n)) {
            std::error_code ec;
            found = std::filesystem::equivalent(std::filesystem::path(path), exe, ec);
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    return found;
}

// The game's Quick Match mod (bundled with the game, else in its mods folder).
static bool game_quickmatch(const GameEntry& g, QuickMatchMod* mod, std::wstring* why) {
    std::filesystem::path bundled = g.exeFound && g_shotModsDir.empty() ? g.exePath.parent_path() / L"assets" / L"mods"
                                                                         : std::filesystem::path();
    return find_quickmatch(bundled, g_shotModsDir.empty() ? game_mods_dir(g) : g_shotModsDir, mod, why);
}

// An install or rebuild that a launch of this game must wait for.
static bool game_busy(int index) {
    InstallStatus inst = install_status();
    return rebuild_running() ||
           ((inst.phase == InstallPhase::Downloading || inst.phase == InstallPhase::Extracting) && inst.gameIndex == index);
}

static void open_tour();

static void open_quick(const std::vector<GameEntry>& games, int index) {
    const GameEntry& g = games[index];
    carry_portable(g);  // the data folder the run's settings go into
    g_ui.quickGame = index;
    g_ui.quickNote.clear();
    std::wstring why;
    g_ui.qmFound = game_quickmatch(g, &g_ui.qm, &why);
    g_ui.qmWhy = narrow(why);
    g_ui.qmPick.assign(g_ui.qm.options.size(), 0);
    for (size_t i = 0; g_ui.qmFound && i < g_ui.qm.options.size(); ++i) {
        const QmOption& o = g_ui.qm.options[i];
        if (o.randomOk && o.id == "venue") continue;  // Random venue by default
        int base = o.randomOk ? 1 : 0;
        for (size_t c = 0; c < o.choices.size(); ++c)
            if (o.choices[c] == o.def) g_ui.qmPick[i] = base + (int)c;
    }
    g_ui.showQuick = true;
}

// The launch index once Start is chosen (with g_ui.qmLaunch and qmValues set), else -1.
static int draw_quick(std::vector<GameEntry>& games, WORD pad) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float vw = vp->WorkSize.x, vh = vp->WorkSize.y;
    const GameEntry& g = games[g_ui.quickGame];
    // B / Esc in an open list closes the list (ImGui), not the page.
    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
        (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || (pad & XINPUT_GAMEPAD_B))) {
        g_ui.showQuick = false;
        return -1;
    }
    bool busy = game_busy(g_ui.quickGame);
    ImGui::PushFont(g_ui.fontHeader);
    ImGui::SetCursorPosY(vh * 0.055f);
    text_centered("Q U I C K   M A T C H");
    ImGui::PopFont();
    ImGui::PushFont(g_ui.fontBase);
    text_centered_colored(palette::dim, narrow(g.title));
    ImGui::PopFont();

    float panelW = page_column(vw, vh, 0.50f, 1.0f);
    ImGui::SetCursorPos(ImVec2((vw - panelW) * 0.5f, vh * 0.20f));
    ImGui::BeginChild("##quick", ImVec2(panelW, vh * 0.70f), ImGuiChildFlags_None);
    ImGui::PushFont(g_ui.fontBase);
    int launch = -1;
    if (!g_ui.qmFound) {
        ImGui::TextColored(palette::warn, "No quick match for this game");
        ImGui::PushFont(g_ui.fontSmall);
        ImGui::TextWrapped("%s", g_ui.qmWhy.c_str());
        ImGui::TextColored(palette::faint, "Quick match uses the Quick Match mod that the game releases bundle.");
        ImGui::PopFont();
    } else {
        ImGui::PushFont(g_ui.fontSmall);
        ImGui::TextWrapped("The game starts straight into a CPU-vs-CPU match through its Quick Match mod. These settings "
                           "are for this run only: your own mod settings come back when the game closes.");
        ImGui::PopFont();
        ImGui::Spacing();
        float labelW = panelW * 0.28f;
        for (size_t i = 0; i < g_ui.qm.options.size(); ++i) {
            const QmOption& o = g_ui.qm.options[i];
            ImGui::PushID((int)i);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(o.name.c_str());
            ImGui::SameLine(labelW);
            ImGui::SetNextItemWidth(panelW - labelW - ImGui::GetStyle().ScrollbarSize);
            int base = o.randomOk ? 1 : 0;
            int& pick = g_ui.qmPick[i];
            auto label = [&](int k) { return k < base ? std::string(kQmRandom) : o.choices[(size_t)(k - base)]; };
            if (ImGui::BeginCombo("##opt", label(pick).c_str())) {
                for (int k = 0; k < base + (int)o.choices.size(); ++k) {
                    if (ImGui::Selectable(label(k).c_str(), k == pick)) pick = k;
                    if (k == pick) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();
        }
        ImGui::Spacing();
        ImGui::Spacing();
        if (!g.launchable()) {
            ImGui::TextColored(palette::warn, "Set the game up first (it needs its exe and ROM, like Play)");
        } else if (busy) {
            ImGui::TextColored(palette::warn, "Wait for the install or rebuild to finish");
        } else if (ImGui::Button("Start match") || (pad & XINPUT_GAMEPAD_START)) {
            std::map<std::string, std::string> picks, values;
            for (size_t i = 0; i < g_ui.qm.options.size(); ++i) {
                const QmOption& o = g_ui.qm.options[i];
                int base = o.randomOk ? 1 : 0;
                int k = g_ui.qmPick[i];
                picks[o.id] = k < base ? std::string(kQmRandom) : o.choices[(size_t)(k - base)];
            }
            static std::mt19937 rng{std::random_device{}()};
            std::wstring err;
            if (resolve_quickmatch(g_ui.qm, picks, rng, &values, &err)) {
                std::wstring summary;
                for (const auto& o : g_ui.qm.options) summary += L" " + widen(o.id) + L"=" + widen(values[o.id]);
                logf(L"quick match: %s%s", g.title.c_str(), summary.c_str());
                // Written by the launch, after carry_portable: into the data folder the game will use.
                g_ui.qmValues = values;
                g_ui.qmLaunch = true;
                g_ui.showQuick = false;
                play_launch();
                launch = g_ui.quickGame;
            } else {
                g_ui.quickNote = narrow(err);
            }
        }
        if (!g_ui.quickNote.empty()) {
            ImGui::PushFont(g_ui.fontSmall);
            ImGui::TextColored(palette::error, "%s", g_ui.quickNote.c_str());
            ImGui::PopFont();
        }
    }
    ImGui::Spacing();
    if (launch < 0 && ImGui::Button("History Tour...")) {
        g_ui.showQuick = false;
        open_tour();
    }
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PushFont(g_ui.fontSmall);
    ImGui::SetCursorPosY(vh * 0.92f);
    text_centered_colored(palette::faint, g_ui.padInput ? "Start  Start Match      B Back" : "Esc Back");
    ImGui::PopFont();
    return launch;
}

// --- History Tour (aki-roadmap#23) -------------------------------------------

// The page on the tour's next stop (the last one once it's complete).
static void open_tour() {
    std::wstring source, err;
    g_ui.tourOk = load_tour(app_root(), &g_ui.tour, &source, &err);
    g_ui.tourErr = narrow(err);
    g_ui.tourSource = narrow(source);
    g_ui.tourNote.clear();
    int n = g_ui.tourOk ? (int)g_ui.tour.stops.size() : 0;
    g_ui.tourSel = n ? (settings().tourStop < n ? settings().tourStop : n - 1) : 0;
    g_ui.showTour = true;
}

// The launch index once a stop is started (with g_ui.qmLaunch, qmValues and tourLaunch set), else -1.
static int draw_tour(std::vector<GameEntry>& games, WORD pad) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float vw = vp->WorkSize.x, vh = vp->WorkSize.y;
    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
        (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || (pad & XINPUT_GAMEPAD_B))) {
        g_ui.showTour = false;
        return -1;
    }
    ImGui::PushFont(g_ui.fontHeader);
    ImGui::SetCursorPosY(vh * 0.055f);
    text_centered("H I S T O R Y   T O U R");
    ImGui::PopFont();
    int launch = -1;
    float panelW = page_column(vw, vh, 0.56f, 1.1f);
    if (!g_ui.tourOk) {
        ImGui::SetCursorPos(ImVec2((vw - panelW) * 0.5f, vh * 0.18f));
        ImGui::BeginChild("##tour", ImVec2(panelW, vh * 0.70f), ImGuiChildFlags_None);
        ImGui::PushFont(g_ui.fontBase);
        ImGui::TextColored(palette::error, "The tour couldn't be read");
        ImGui::PopFont();
        ImGui::PushFont(g_ui.fontSmall);
        ImGui::TextWrapped("%s", g_ui.tourErr.c_str());
        ImGui::TextColored(palette::faint, "Fix or remove %s to get the built-in tour back.", g_ui.tourSource.c_str());
        ImGui::PopFont();
        ImGui::EndChild();
        return -1;
    }
    const Tour& tour = g_ui.tour;
    int n = (int)tour.stops.size();
    int next = settings().tourStop;
    ImGui::PushFont(g_ui.fontBase);
    text_centered_colored(palette::dim, tour.name + (next >= n ? "  -  complete" : "  -  stop " + std::to_string(next + 1) +
                                                                                  " of " + std::to_string(n)));
    ImGui::PopFont();

    ImGui::SetCursorPos(ImVec2((vw - panelW) * 0.5f, vh * 0.17f));
    ImGui::BeginChild("##tour", ImVec2(panelW, vh * 0.72f), ImGuiChildFlags_None);
    ImGui::PushFont(g_ui.fontBase);
    for (int i = 0; i < n; ++i) {
        const TourStop& s = tour.stops[i];
        const GameEntry* gm = nullptr;
        for (const auto& g : games)
            if (narrow(g.artKey) == s.game) gm = &g;
        std::string line = std::to_string(s.year) + "   " + s.title + "   -   " + (gm ? narrow(gm->title) : s.game);
        ImGui::PushID(i);
        // The marker in a column of its own (spaces don't line up in a proportional font).
        ImGui::TextColored(i < next ? palette::ready : palette::accent, "%s", i < next ? "done" : i == next ? "next" : "");
        ImGui::SameLine(ImGui::CalcTextSize("done").x + ImGui::GetFontSize() * 1.5f);
        if (ImGui::Selectable(line.c_str(), i == g_ui.tourSel)) g_ui.tourSel = i;
        if (i == g_ui.tourSel && ImGui::IsWindowAppearing()) ImGui::SetItemDefaultFocus();
        else if (ImGui::IsItemFocused()) g_ui.tourSel = i;  // the D-pad moves the selection, so Start plays what's shown
        ImGui::PopID();
    }
    ImGui::Separator();
    ImGui::Spacing();
    const TourStop& s = tour.stops[(size_t)g_ui.tourSel];
    int gameIndex = -1;
    for (int i = 0; i < (int)games.size(); ++i)
        if (narrow(games[i].artKey) == s.game) gameIndex = i;
    ImGui::TextColored(palette::accent, "%d   %s", s.year, s.title.c_str());
    ImGui::PopFont();
    ImGui::PushFont(g_ui.fontSmall);
    for (const auto& l : s.blurb) ImGui::TextWrapped("%s", l.c_str());
    ImGui::TextColored(palette::faint, "%s, %s%s%s", gameIndex >= 0 ? narrow(games[gameIndex].title).c_str() : s.game.c_str(),
                       s.venue.c_str(), s.match.empty() ? "" : ", ", s.match.c_str());
    ImGui::TextColored(palette::faint, "A CPU-vs-CPU match through the game's Quick Match mod; close the game for the next stop.");
    ImGui::PopFont();
    ImGui::Spacing();
    ImGui::PushFont(g_ui.fontBase);
    if (ImGui::Button("Play this stop") || (pad & XINPUT_GAMEPAD_START)) {
        std::wstring why;
        QuickMatchMod mod;
        std::map<std::string, std::string> values;
        static std::mt19937 rng{std::random_device{}()};
        if (g_ui.tourSel > next)
            why = L"the tour goes in order: play stop " + std::to_wstring(next + 1) + L" first (done stops can be replayed)";
        else if (gameIndex < 0) why = widen(s.game) + L" isn't one of this launcher's games";
        else if (!games[gameIndex].launchable()) why = games[gameIndex].title + L" isn't set up yet (it needs its exe and ROM)";
        else if (game_busy(gameIndex)) why = L"wait for the install or rebuild to finish";
        else if (carry_portable(games[gameIndex]);  // first: the mods folder the mod lookup reads
                 game_quickmatch(games[gameIndex], &mod, &why) &&
                 resolve_quickmatch(mod, tour_stop_picks(s), rng, &values, &why)) {
            logf(L"history tour: stop %d (%s) in %s", g_ui.tourSel + 1, widen(s.title).c_str(), games[gameIndex].title.c_str());
            g_ui.qm = mod;
            g_ui.qmValues = values;
            g_ui.qmLaunch = true;
            g_ui.tourLaunch = g_ui.tourSel;
            g_ui.showTour = false;
            play_launch();
            launch = gameIndex;
            why.clear();
        }
        g_ui.tourNote = narrow(why);
    }
    ImGui::SameLine();
    if (next > 0 && ImGui::Button("Start the tour over")) {
        settings().tourStop = 0;
        settings_save();
        g_ui.tourSel = 0;
    }
    ImGui::PopFont();
    if (!g_ui.tourNote.empty()) {
        ImGui::PushFont(g_ui.fontSmall);
        ImGui::TextColored(palette::error, "%s", g_ui.tourNote.c_str());
        ImGui::PopFont();
    }
    ImGui::EndChild();
    ImGui::PushFont(g_ui.fontSmall);
    ImGui::SetCursorPosY(vh * 0.92f);
    text_centered_colored(palette::faint, g_ui.padInput ? "Start  Play This Stop      B Back" : "Esc Back");
    ImGui::PopFont();
    return launch;
}

// --- Launcher self-update (aki-roadmap#117) -----------------------------------

static std::string update_line(const UpdateStatus& u) {
    switch (u.phase) {
        case UpdatePhase::Off: return "update checks are off";
        case UpdatePhase::DevBuild: return "dev build, updates off";
        case UpdatePhase::Idle:
        case UpdatePhase::Checking: return "checking for updates...";
        case UpdatePhase::UpToDate: return "up to date" + (u.checkedAt.empty() ? std::string() : " (checked " + narrow(u.checkedAt) + ")");
        case UpdatePhase::Available: return narrow(u.release.tag) + " is available";
        case UpdatePhase::Downloading: return "downloading " + narrow(u.release.tag) + "...";
        case UpdatePhase::Staged: return narrow(u.release.tag) + " downloaded and verified";
        case UpdatePhase::Failed: return narrow(u.error);
    }
    return "";
}

static void draw_update_page(WORD pad) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float vw = vp->WorkSize.x, vh = vp->WorkSize.y;
    UpdateStatus u = update_status();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || (pad & XINPUT_GAMEPAD_B)) {
        g_ui.showUpdate = false;
        return;
    }
    ImGui::PushFont(g_ui.fontHeader);
    ImGui::SetCursorPosY(vh * 0.055f);
    text_centered("U P D A T E");
    ImGui::PopFont();
    float panelW = page_column(vw, vh, 0.62f, 1.2f);
    ImGui::SetCursorPos(ImVec2((vw - panelW) * 0.5f, vh * 0.15f));
    ImGui::BeginChild("##update", ImVec2(panelW, vh * 0.74f), ImGuiChildFlags_None);
    ImGui::PushFont(g_ui.fontBase);
    ImGui::TextColored(palette::accent, "AKI Launcher %s", narrow(u.release.name.empty() ? u.release.tag : u.release.name).c_str());
    ImGui::TextColored(palette::dim, "You have %s.  %s", narrow(version_text(u.current)).c_str(), update_line(u).c_str());
    ImGui::Spacing();
    bool busy = u.phase == UpdatePhase::Downloading || u.phase == UpdatePhase::Staged;
    bool blocked = g_session.active() || rebuild_running() || install_status().phase == InstallPhase::Downloading ||
                   install_status().phase == InstallPhase::Extracting;
    if (u.phase == UpdatePhase::Failed && u.release.zipUrl.empty()) {
        if (ImGui::Button("Check again")) update_check_start();  // the check failed: nothing to download yet
    } else if (u.phase == UpdatePhase::Available || u.phase == UpdatePhase::Failed) {
        if (blocked) {
            ImGui::TextColored(palette::warn, "Finish the running game, build or install first.");
        } else if (ImGui::Button(u.phase == UpdatePhase::Failed ? "Try again" : "Update now")) {
            g_ui.updateRequested = true;
            g_ui.updateNote.clear();
            update_download_start();
        }
        ImGui::SameLine();
        if (ImGui::Button("Later")) {
            update_dismiss();
            g_ui.showUpdate = false;
        }
        if (u.phase == UpdatePhase::Available) {
            ImGui::SameLine();
            if (ImGui::Button("Skip this version")) {
                update_skip_version();
                g_ui.showUpdate = false;
            }
        }
    } else if (busy) {
        if (u.total > 0)
            ImGui::ProgressBar((float)u.got / (float)u.total, ImVec2(panelW * 0.6f, 0));
        ImGui::TextColored(palette::dim, "%s", u.phase == UpdatePhase::Staged ? "Installing and restarting..."
                                                                             : "Downloading; it is checked against the release's SHA256SUMS");
    }
    if (!g_ui.updateNote.empty()) ImGui::TextColored(palette::error, "%s", g_ui.updateNote.c_str());
    ImGui::Separator();
    ImGui::PushFont(g_ui.fontSmall);
    ImGui::BeginChild("##notes", ImVec2(0, 0), ImGuiChildFlags_None);
    std::string notes = narrow(u.release.notes);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(notes.empty() ? "(no release notes)" : notes.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::PopFont();
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PushFont(g_ui.fontSmall);
    ImGui::SetCursorPosY(vh * 0.92f);
    text_centered_colored(palette::faint, std::string(g_ui.padInput ? "B" : "Esc") +
                                              " Back      Settings, saves, games and mods stay as they are; "
                                              "the previous version is kept for a rollback");
    ImGui::PopFont();
}

// Settings section: the launcher's version and its update options.
static void draw_update_settings() {
    UpdateStatus u = update_status();
    ImGui::TextColored(palette::accent, "AKI Launcher");
    ImGui::Spacing();
    ImGui::TextColored(palette::dim, "Version %s  -  %s", narrow(version_text(u.current)).c_str(), update_line(u).c_str());
    if (u.phase == UpdatePhase::DevBuild) return;
    bool checks = settings().updateChecks;
    if (ImGui::Checkbox("Check for launcher updates", &checks)) {
        settings().updateChecks = checks;
        settings_save();
    }
    ImGui::SameLine();
    bool pre = settings().updatePrerelease;
    if (ImGui::Checkbox("Include pre-releases", &pre)) {
        settings().updatePrerelease = pre;
        settings_save();
    }
    if (u.phase != UpdatePhase::Off) {
        ImGui::SameLine();
        if (ImGui::Button("Check now")) update_check_start();
        if (u.phase == UpdatePhase::Available) {
            ImGui::SameLine();
            if (ImGui::Button("Details...")) {
                g_ui.showSettings = false;
                g_ui.showUpdate = true;
            }
        }
    } else {
        ImGui::PushFont(g_ui.fontSmall);
        ImGui::TextColored(palette::faint, "(turning checks back on takes effect on the next start)");
        ImGui::PopFont();
    }
}

// The verified download goes in: files swapped (the old ones kept), the new
// launcher started, and this one waits for it to come up. If it doesn't
// (exits, or no sign within 30 s), everything is put back and this one goes on.
// Returns true when this process should now exit.
static bool apply_update_and_restart(HWND hwnd) {
    UpdateStatus u = update_status();
    wchar_t exeBuf[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, exeBuf, (DWORD)std::size(exeBuf));
    std::filesystem::path exe(exeBuf), appDir = exe.parent_path();
    std::filesystem::path staging = appDir / L".update" / L"staged", previous = appDir / L".update" / L"previous";
    if (g_session.active() || rebuild_running()) {
        update_mark_failed(L"a game or build was running: try again when it is done");
        return false;
    }
    settings_save();  // before the new launcher reads it
    std::wstring err;
    if (!apply_staged_update(appDir, staging, previous, &err)) {
        update_mark_failed(L"Update not installed: " + err);
        logf(L"update: %s", err.c_str());
        return false;
    }
    // The release's exe is AkiLauncher.exe, whatever this copy is called.
    std::filesystem::path newExe = appDir / L"AkiLauncher.exe";
    std::wstring evName = L"Local\\AkiLauncherUpdate-" + std::to_wstring(GetCurrentProcessId());
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, evName.c_str());
    UnregisterHotKey(hwnd, HOTKEY_QUICKBACK);  // the new launcher registers it
    std::wstring cmd = L"\"" + newExe.wstring() + L"\" --update-confirm " + evName + L" --updated-from " +
                       version_text(u.current);
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    bool up = false;
    if (ev && CreateProcessW(newExe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        // Wait for its "up" event (or its exit), pumping messages so this window stays responsive.
        HANDLE waits[2] = {ev, pi.hProcess};
        ULONGLONG deadline = GetTickCount64() + 30000;
        DWORD r = WAIT_TIMEOUT;
        while (GetTickCount64() < deadline) {
            r = MsgWaitForMultipleObjects(2, waits, FALSE, (DWORD)(deadline - GetTickCount64()), QS_ALLINPUT);
            if (r != WAIT_OBJECT_0 + 2) break;
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            r = WAIT_TIMEOUT;
        }
        up = r == WAIT_OBJECT_0;
        if (!up && r == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
        if (!up) WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    if (ev) CloseHandle(ev);
    if (up) {
        logf(L"update: %s is up; this launcher exits", u.release.tag.c_str());
        return true;
    }
    std::wstring rerr;
    restore_previous(appDir, staging, previous, &rerr);
    reregister_hotkey();
    update_mark_failed(L"The new version didn't start, so " + version_text(u.current) + L" was put back" +
                       (rerr.empty() ? L"" : L" (" + rerr + L")"));
    logf(L"update: %s did not come up; rolled back", u.release.tag.c_str());
    return false;
}

// Settings section: scan a ROM folder (aki-roadmap#26), results grouped by game.
static void draw_rom_scan(std::vector<GameEntry>& games) {
    ImGui::TextColored(palette::accent, "ROM Folder");
    ImGui::Spacing();
    // Copy the results only when the scan changed (they can be thousands).
    static RomScanStatus st;
    static unsigned stGen = ~0u;
    static std::string note;  // last set-up result
    if (romscan_generation() != stGen) {
        st = romscan_status();
        stGen = st.generation;
    }
    if (st.phase == RomScanPhase::Scanning) {
        if (st.total == 0)
            ImGui::TextColored(palette::warn, "Listing the files in %s...", narrow(st.dir.wstring()).c_str());
        else
            ImGui::TextColored(palette::warn, "Scanning %s  -  %zu / %zu files",
                               narrow(st.dir.wstring()).c_str(), st.done, st.total);
        return;
    }
    if (ImGui::Button("Scan a folder for ROMs...")) {
        std::filesystem::path dir = pick_folder(settings().romScanDir);
        if (!dir.empty()) {
            settings().romScanDir = dir.wstring();
            settings_save();
            note.clear();
            romscan_start(dir, games);
        }
    }
    ImGui::SameLine();
    ImGui::PushFont(g_ui.fontSmall);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(palette::faint, "Finds each game's accepted dump (and ROM hacks of it) by its header and SHA1");
    ImGui::PopFont();
    if (st.phase != RomScanPhase::Done) return;

    ImGui::PushFont(g_ui.fontSmall);
    ImGui::TextColored(palette::dim, "%zu files in %s", st.results.size(), narrow(st.dir.wstring()).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear##romscan")) {
        romscan_clear();
        note.clear();
    }
    if (st.truncated)
        ImGui::TextColored(palette::warn, "Stopped at %zu files: scan a smaller folder for the rest", st.results.size());
    if (st.skippedDirs)
        ImGui::TextColored(palette::warn, "%zu folders couldn't be read (see aki-launcher.log)", st.skippedDirs);
    for (int gi = 0; gi < (int)games.size(); ++gi) {
        GameEntry& g = games[gi];
        bool any = false;
        for (const auto& r : st.results) any = any || r.gameIndex == gi;
        if (!any) continue;
        ImGui::PushID(gi);
        ImGui::TextUnformatted(narrow(g.title).c_str());
        int row = 0;
        for (const auto& r : st.results) {
            if (r.gameIndex != gi) continue;
            ImGui::PushID(row++);
            ImGui::Indent();
            ImGui::TextColored(r.verdict == RomVerdict::BaseDump ? palette::ready : palette::warn, "%s",
                               narrow(r.file.filename().wstring()).c_str());
            if (r.verdict == RomVerdict::BaseDump) {
                ImGui::SameLine();
                if (g.romFound) ImGui::TextColored(palette::faint, "- set up already");
                else if (!g.exeFound) ImGui::TextColored(palette::faint, "- install the game first, then use it");
                else if (ImGui::SmallButton("Use this ROM")) {
                    std::wstring err;
                    if (rom_provision(g, r, &err)) {
                        apply_overrides(games);
                        note = narrow(g.title) + " is ready to play.";
                        play_flip();
                    } else {
                        note = narrow(err);
                    }
                }
            }
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(palette::faint, "%s", narrow(r.note).c_str());
            ImGui::PopTextWrapPos();
            ImGui::Unindent();
            ImGui::PopID();
        }
        ImGui::PopID();
    }
    size_t other = 0;
    for (const auto& r : st.results) other += r.gameIndex < 0;
    if (other && ImGui::TreeNode("##other", "%zu other files: not supported ROMs", other)) {
        for (const auto& r : st.results) {
            if (r.gameIndex >= 0) continue;
            ImGui::TextColored(palette::faint, "%s  -  %s", narrow(r.file.filename().wstring()).c_str(),
                               narrow(r.note).c_str());
        }
        ImGui::TreePop();
    }
    if (!note.empty()) ImGui::TextColored(palette::accent, "%s", note.c_str());
    ImGui::PopFont();
}

static void draw_settings(std::vector<GameEntry>& games, WORD pad) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float vw = vp->WorkSize.x, vh = vp->WorkSize.y;
    ImGuiIO& io = ImGui::GetIO();

    // --- capture flows -----------------------------------------------------
    if (g_ui.captureChord) {
        WORD held = pad_held_mask();
        if (!g_ui.chordArmed) {
            if (held == 0) g_ui.chordArmed = true;  // buttons from the Rebind press released
        } else {
            g_ui.chordPending |= held;
            if (g_ui.chordPending != 0 && held == 0) {
                settings().chordMask = g_ui.chordPending;
                settings_save();
                g_ui.captureChord = false;
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) g_ui.captureChord = false;
    } else if (g_ui.captureHotkey) {
        for (int vk = 0x08; vk <= 0xFE; ++vk) {
            if (vk <= 0x06) continue;
            if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN ||
                vk == VK_RWIN || (vk >= VK_LSHIFT && vk <= VK_RMENU))
                continue;
            if (!(GetAsyncKeyState(vk) & 0x8000)) continue;
            if (vk == VK_ESCAPE) {
                g_ui.captureHotkey = false;
                break;
            }
            UINT mods = 0;
            if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
            if (GetAsyncKeyState(VK_MENU) & 0x8000) mods |= MOD_ALT;
            if (GetAsyncKeyState(VK_SHIFT) & 0x8000) mods |= MOD_SHIFT;
            if ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) mods |= MOD_WIN;
            settings().hotkeyMods = mods;
            settings().hotkeyVk = (UINT)vk;
            settings_save();
            reregister_hotkey();
            g_ui.captureHotkey = false;
            break;
        }
    } else {
        bool esc = ImGui::IsKeyPressed(ImGuiKey_Escape) && !io.WantTextInput;
        // With gamepad nav on, B is also ImGui's cancel — while a text field
        // is active let it just leave the field instead of closing the page.
        if (esc || ((pad & XINPUT_GAMEPAD_B) && !io.WantTextInput)) close_settings(games);
    }

    // --- layout ------------------------------------------------------------
    ImGui::PushFont(g_ui.fontHeader);
    ImGui::SetCursorPosY(vh * 0.055f);
    text_centered("S E T T I N G S");
    ImGui::PopFont();

    float panelW = page_column(vw, vh, 0.62f, 1.2f);
    ImGui::SetCursorPos(ImVec2((vw - panelW) * 0.5f, vh * 0.14f));
    ImGui::BeginChild("##settings", ImVec2(panelW, vh * 0.72f), ImGuiChildFlags_None);

    ImGui::PushFont(g_ui.fontBase);
    ImGui::TextColored(palette::accent, "Return To Launcher");
    ImGui::Spacing();

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Controller chord");
    ImGui::SameLine(panelW * 0.28f);
    if (g_ui.captureChord)
        ImGui::TextColored(palette::warn, "Hold the buttons together, then release... (Esc cancels)");
    else {
        ImGui::TextColored(palette::dim, "%s", chord_to_string(settings().chordMask).c_str());
        ImGui::SameLine(panelW * 0.72f);
        if (ImGui::Button("Rebind##chord")) {
            g_ui.captureChord = true;
            g_ui.chordArmed = false;
            g_ui.chordPending = 0;
        }
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Keyboard hotkey");
    ImGui::SameLine(panelW * 0.28f);
    if (g_ui.captureHotkey)
        ImGui::TextColored(palette::warn, "Press the desired key combo... (Esc cancels)");
    else {
        ImGui::TextColored(palette::dim, "%s",
                           hotkey_to_string(settings().hotkeyMods, settings().hotkeyVk).c_str());
        ImGui::SameLine(panelW * 0.72f);
        if (ImGui::Button("Rebind##hotkey")) g_ui.captureHotkey = true;
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextColored(palette::accent, "Display");
    ImGui::Spacing();
    bool winMode = settings().windowed;
    if (ImGui::Checkbox("Windowed launcher", &winMode)) g_pendingWindowMode = winMode ? 1 : 0;
    ImGui::SameLine();
    ImGui::PushFont(g_ui.fontSmall);
    ImGui::TextColored(palette::faint, "(F11 or Alt+Enter anywhere; games follow the window)");
    ImGui::PopFont();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    draw_update_settings();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    draw_rom_scan(games);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextColored(palette::accent, "Games");
    ImGui::PushFont(g_ui.fontSmall);
    ImGui::TextColored(palette::faint,
                       "Path fields override the depot defaults; clear a field or restore the "
                       "default text to remove the override.");
    ImGui::PopFont();
    ImGui::Spacing();

    ImGui::BeginChild("##games", ImVec2(panelW, vh * 0.40f));
    for (int i = 0; i < (int)games.size(); ++i) {
        GameEntry& g = games[i];
        ImGui::PushID(i);
        ImGui::TextUnformatted(narrow(g.title).c_str());
        ImGui::PushFont(g_ui.fontSmall);
        bool borderless = g.forceBorderless;
        if (ImGui::Checkbox("Force borderless fullscreen", &borderless)) {
            g.forceBorderless = borderless;
            settings().overrides[g.artKey].forceBorderless = borderless ? 1 : 0;
            settings_save();
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(palette::dim, "EXE");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(panelW * 0.78f);
        ImGui::InputText("##exe", &g_ui.exeEdit[i]);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(palette::dim, "ROM");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(panelW * 0.78f);
        ImGui::InputText("##rom", &g_ui.romEdit[i]);
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset")) {
            g_ui.exeEdit[i] = narrow((g.repoPath / g.exeRel).wstring());
            g_ui.romEdit[i] = narrow((g.repoPath / g.romFile).wstring());
        }
        ImGui::PopFont();
        ImGui::Spacing();
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::Spacing();
    if (ImGui::Button("Save & Close", ImVec2(panelW * 0.22f, 0))) close_settings(games);
    ImGui::SameLine();
    ImGui::PushFont(g_ui.fontSmall);
    ImGui::TextColored(palette::faint, "%s also saves and closes.  Stored at %s",
                       g_ui.padInput ? "B" : "Esc", narrow(settings_path().wstring()).c_str());
    ImGui::PopFont();
    ImGui::PopFont();
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------------------- conversions page
// Full-conversion mods for the selected game: ROM patches, code mods, save packs and texture packs, side by side.

static void refresh_conversions(const std::vector<GameEntry>& games) {
    g_ui.convFor = g_ui.selected;
    g_ui.convList = (g_ui.selected >= 0 && g_ui.selected < (int)games.size())
                        ? list_conversions(games[g_ui.selected])
                        : std::vector<ConversionInfo>{};
}

static void add_conversion_from(std::vector<GameEntry>& games, const std::filesystem::path& p) {
    AddResult r = add_conversion(games, p);
    g_ui.convMessage = r.message;
    if (r.ok && r.gameIndex >= 0) g_ui.selected = r.gameIndex;
    g_ui.showConversions = true;
    refresh_conversions(games);
}

static bool pick_package(std::filesystem::path* out) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
    COMDLG_FILTERSPEC types[] = {{L"Conversion package (*.zip)", L"*.zip"}, {L"All files", L"*.*"}};
    dlg->SetFileTypes(2, types);
    dlg->SetTitle(L"Add a conversion package (or its texture package)");
    bool ok = false;
    if (SUCCEEDED(dlg->Show(g_hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR psz = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &psz))) {
                *out = psz;
                ok = true;
                CoTaskMemFree(psz);
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

static void badge(const char* label, bool on, ImVec4 col) {
    if (!on) return;
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(col.x * 0.35f, col.y * 0.35f, col.z * 0.35f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(col.x * 0.35f, col.y * 0.35f, col.z * 0.35f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(col.x * 0.35f, col.y * 0.35f, col.z * 0.35f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::SmallButton(label);
    ImGui::PopStyleColor(4);
}

// Returns a game index to launch (with g_launchEnv set), or -1.
static int draw_conversions(std::vector<GameEntry>& games, WORD pad) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float vw = vp->WorkSize.x, vh = vp->WorkSize.y;
    if (g_ui.convFor != g_ui.selected) refresh_conversions(games);
    GameEntry& g = games[g_ui.selected];
    int launch = -1;

    ImGui::PushFont(g_ui.fontHeader);
    ImGui::SetCursorPosY(vh * 0.055f);
    text_centered("C O N V E R S I O N S");
    ImGui::PopFont();
    ImGui::PushFont(g_ui.fontSmall);
    text_centered_colored(palette::dim, narrow(g.title) + "  -  full-conversion mods: ROM patches, code mods, save packs, texture packs");
    ImGui::PopFont();

    float panelW = vw * 0.70f;
    ImGui::SetCursorPos(ImVec2((vw - panelW) * 0.5f, vh * 0.15f));
    ImGui::BeginChild("##conversions", ImVec2(panelW, vh * 0.62f), ImGuiChildFlags_None);
    ImGui::PushFont(g_ui.fontBase);
    if (g_ui.convList.empty()) {
        ImGui::TextColored(palette::dim, "No conversions yet.");
        ImGui::PushFont(g_ui.fontSmall);
        ImGui::TextColored(palette::faint, "Add a conversion package (.zip) with the button below, or drop it on this window.");
        ImGui::PopFont();
    }
    std::wstring removeId;
    for (size_t i = 0; i < g_ui.convList.size(); i++) {
        const ConversionInfo& c = g_ui.convList[i];
        ImGui::PushID((int)i);
        ImGui::Separator();
        ImGui::TextColored(palette::accent, "%s", narrow(c.name).c_str());
        badge("Patch", c.patch, ImVec4(0.55f, 0.75f, 1.0f, 1.0f));
        badge("Codes", c.codes, ImVec4(1.0f, 0.80f, 0.40f, 1.0f));
        badge("Save", c.saves, ImVec4(0.55f, 0.95f, 0.60f, 1.0f));
        badge("Textures", c.textures, ImVec4(0.95f, 0.60f, 0.95f, 1.0f));
        badge("RT", c.rt, ImVec4(0.70f, 0.90f, 0.95f, 1.0f));
        ImGui::PushFont(g_ui.fontSmall);
        std::string line = (c.version.empty() ? std::string() : "v" + narrow(c.version)) +
                           (c.authors.empty() ? std::string() : "   by " + narrow(c.authors));
        if (!line.empty()) ImGui::TextColored(palette::dim, "%s", line.c_str());
        if (!c.description.empty()) ImGui::TextWrapped("%s", narrow(c.description).c_str());
        for (const auto& n : c.notes) ImGui::TextColored(palette::warn, "%s", narrow(n).c_str());
        ImGui::PopFont();
        if (ImGui::Button("Play")) {
            if (g.launchable()) {
                g_launchEnv = conversion_env(c);
                launch = g_ui.selected;
                g_ui.showConversions = false;
                play_launch();
            } else {
                g_ui.convMessage = L"Install the game and select its ROM first.";
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Remove")) removeId = c.id;
        ImGui::PopID();
    }
    ImGui::PopFont();
    ImGui::EndChild();

    if (!removeId.empty()) {
        std::wstring err;
        g_ui.convMessage = remove_conversion(g, removeId, &err) ? (L"Removed " + removeId + L" (its saves are kept).")
                                                                 : (L"Couldn't remove " + removeId + L": " + err);
        refresh_conversions(games);
    }

    ImGui::SetCursorPos(ImVec2((vw - panelW) * 0.5f, vh * 0.79f));
    ImGui::PushFont(g_ui.fontBase);
    if (ImGui::Button("Add conversion...")) {
        std::filesystem::path p;
        if (pick_package(&p)) add_conversion_from(games, p);
    }
    ImGui::SameLine();
    if (ImGui::Button("Back") || ImGui::IsKeyPressed(ImGuiKey_Escape) || (pad & XINPUT_GAMEPAD_B)) g_ui.showConversions = false;
    ImGui::PopFont();
    if (!g_ui.convMessage.empty()) {
        ImGui::PushFont(g_ui.fontSmall);
        ImGui::SetCursorPosX((vw - panelW) * 0.5f);
        ImGui::PushTextWrapPos((vw + panelW) * 0.5f);
        ImGui::TextColored(palette::dim, "%s", narrow(g_ui.convMessage).c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
    }
    return launch;
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

    draw_backdrop(dl, vp, art);

    // A game that ended may have changed its conversions (the in-game list, its drop folder): read them again.
    static bool s_wasActive = false;
    if (s_wasActive && !g_session.active()) g_ui.convCache.clear();
    s_wasActive = g_session.active();

    // ------------------------------------------------------------------ game running overlay
    if (g_session.active()) {
        // Keeps pad_pressed's last state current: a button held when the game closes (the quit chord, Back by
        // default) is not a new press on the launcher.
        (void)pad_pressed();
        const char* verb = g_session.state == SessionState::WaitingForWindow ? "Starting"
                           : g_session.state == SessionState::Closing        ? "Closing"
                                                                              : "Now playing";
        ImGui::PushFont(g_ui.fontTitle);
        ImGui::SetCursorPosY(vh * 0.44f);
        text_centered(std::string(verb) + "  -  " + narrow(g_session.title) +
                      (g_session.conversionName.empty() ? "" : "  -  " + narrow(g_session.conversionName)));
        ImGui::PopFont();
        ImGui::PushFont(g_ui.fontSmall);
        ImGui::SetCursorPosY(vh * 0.54f);
        text_centered_colored(palette::dim,
                              hotkey_to_string(settings().hotkeyMods, settings().hotkeyVk) +
                                  "  Or Hold  " + chord_to_string(settings().chordMask) +
                                  "  For The Quit Prompt, Confirm To Return Here");
        ImGui::PopFont();
        ImGui::End();
        return -1;
    }

    WORD pad = pad_pressed();

    // Which device is the user on? Pad wins ties; the state sticks until the
    // other device produces input.
    if (pad || pad_held_mask()) g_ui.padInput = true;
    else if (keyboard_mouse_activity()) g_ui.padInput = false;

    // ------------------------------------------------------------------ update page
    if (g_ui.showUpdate) {
        draw_update_page(pad_pressed());
        ImGui::End();
        return -1;
    }

    // ------------------------------------------------------------------ history tour page
    if (g_ui.showTour) {
        int launch = draw_tour(games, pad);
        ImGui::End();
        return launch;
    }

    // ------------------------------------------------------------------ quick match page
    if (g_ui.showQuick) {
        int launch = draw_quick(games, pad);
        ImGui::End();
        return launch;
    }

    // ------------------------------------------------------------------ mods page
    if (g_ui.showMods) {
        int launch = draw_mods(games, pad);
        ImGui::End();
        return launch;
    }

    // ------------------------------------------------------------------ settings screen
    if (g_ui.showSettings) {
        draw_settings(games, pad);
        ImGui::End();
        return -1;
    }

    // ------------------------------------------------------------------ conversions page
    // While this page is open, dropped packages go to the game their aki.json names; on any other page a drop
    // goes to the Mods page's installer (main loop).
    while (g_ui.showConversions && !g_dropped.empty()) {
        std::filesystem::path p = g_dropped.front();
        g_dropped.erase(g_dropped.begin());
        add_conversion_from(games, p);
    }
    if (g_ui.showConversions) {
        int li = draw_conversions(games, pad);
        ImGui::End();
        return li;
    }

    int launchIndex = -1;
    int n = (int)games.size();

    // ------------------------------------------------------------------ input
    int move = 0;
    bool actLaunch = false, actFlip = false;
    const bool inserting = g_ui.insertIndex >= 0;  // cart-insert animation owns the screen
    if (!inserting) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) move = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) move = +1;
        if (!ImGui::GetIO().KeyAlt &&
            (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)))
            actLaunch = true;
        if (ImGui::IsKeyPressed(ImGuiKey_F)) actFlip = true;
        if (pad & XINPUT_GAMEPAD_DPAD_LEFT) move = -1;
        if (pad & XINPUT_GAMEPAD_DPAD_RIGHT) move = +1;
        if (pad & XINPUT_GAMEPAD_A) actLaunch = true;
        if (pad & XINPUT_GAMEPAD_X) actFlip = true;
        if (ImGui::IsKeyPressed(ImGuiKey_S) || (pad & XINPUT_GAMEPAD_Y)) {
            play_flip();
            open_settings(games);
            ImGui::End();
            return -1;
        }
        UpdatePhase up = update_status().phase;
        if (ImGui::IsKeyPressed(ImGuiKey_L, false) && (up == UpdatePhase::Available || up == UpdatePhase::Failed)) {
            play_flip();
            g_ui.showUpdate = true;
            ImGui::End();
            return -1;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_M, false) || (pad & XINPUT_GAMEPAD_START)) {
            play_flip();
            open_mods(games, g_ui.selected);
            ImGui::End();
            return -1;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_H, false)) {
            play_flip();
            open_tour();
            ImGui::End();
            return -1;
        }
        if ((ImGui::IsKeyPressed(ImGuiKey_Q, false) || (pad & XINPUT_GAMEPAD_BACK)) && !game_busy(g_ui.selected)) {
            play_flip();
            open_quick(games, g_ui.selected);
            ImGui::End();
            return -1;
        }
        // dismiss the crash panel; else cancel or dismiss the rebuild panel
        // No key repeat: a held Backspace must not cancel and then dismiss.
        bool back = ImGui::IsKeyPressed(ImGuiKey_Backspace, false) || (pad & XINPUT_GAMEPAD_B);
        if (back && !g_session.logTail.empty()) {
            g_session.logTail.clear();
            g_session.statusNote.clear();
        } else if (back && rebuild_running()) {
            rebuild_cancel();
        } else if (back && rebuild_status().phase != RebuildPhase::Idle) {
            rebuild_ack();
        }
    }
    const bool rebuildPanel = g_session.logTail.empty() && rebuild_status().phase != RebuildPhase::Idle;

    // ------------------------------------------------------------------ header
    ImGui::PushFont(g_ui.fontHeader);
    ImGui::SetCursorPosY(vh * 0.055f);
    text_centered("A K I   L A U N C H E R");
    ImGui::PopFont();
    ImGui::PushFont(g_ui.fontSmall);
    text_centered_colored(palette::faint, "N64 Wrestling Recompiled");
    // A new AKI Launcher (aki-roadmap#117): a quiet notice, top right.
    {
        UpdateStatus us = update_status();
        if (us.phase == UpdatePhase::Available || (us.phase == UpdatePhase::Failed && g_ui.updateRequested)) {
            std::string label = us.phase == UpdatePhase::Available
                                    ? "AKI Launcher " + narrow(us.release.tag) + " is available  -  L"
                                    : "Launcher update failed  -  L";
            ImVec2 sz = ImGui::CalcTextSize(label.c_str());
            ImGui::SetCursorPos(ImVec2(vw - sz.x - ImGui::GetFontSize() * 2.0f, vh * 0.03f));
            ImGui::PushStyleColor(ImGuiCol_Text, us.phase == UpdatePhase::Available ? palette::accent : palette::error);
            if (ImGui::SmallButton(label.c_str())) g_ui.showUpdate = true;
            ImGui::PopStyleColor();
        }
    }
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

        // Flip animation squeezes the selected card horizontally; the new face
        // appears past the halfway point.
        int face = 0;
        if (isSel) {
            float t = g_ui.flipAnim;
            t = t * t * (3.0f - 2.0f * t);  // smoothstep
            drawW = w * fabsf(1.0f - 2.0f * t);
            if (drawW < w * 0.02f) drawW = w * 0.02f;
            face = t > 0.5f ? g_ui.face : g_ui.facePrev;
        }

        ImVec2 p0(x + (w - drawW) * 0.5f, rowMidY - h * 0.5f);
        ImVec2 p1(p0.x + drawW, rowMidY + h * 0.5f);
        float rounding = w * 0.03f;
        // A game with a live GitHub release is downloadable, never "coming soon".
        ReleaseInfo relProbe;
        bool soon = g.comingSoon && !g.launchable() && !release_for(g.artKey, &relProbe);

        // shadow + face + border
        dl->AddRectFilled(ImVec2(p0.x + 6, p0.y + 8), ImVec2(p1.x + 6, p1.y + 8),
                          IM_COL32(0, 0, 0, 110), rounding);
        const Texture& tex = face == 1 ? art[i].back : art[i].front;
        draw_box_face(dl, tex, p0, p1, face, narrow(g.title), rounding, soon);
        dl->AddRect(p0, p1, isSel ? palette::cardBorderSel : palette::cardBorder, rounding, 0,
                    isSel ? 3.0f : 1.5f);

        if (soon) {
            const char* ribbon = "COMING SOON";
            ImFont* f = g_ui.fontSmall;
            ImVec2 rsz = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, ribbon);
            ImVec2 rp(p0.x + (drawW - rsz.x) * 0.5f, p1.y - h * 0.5f - rsz.y * 0.5f);
            dl->AddRectFilled(ImVec2(rp.x - 10, rp.y - 4), ImVec2(rp.x + rsz.x + 10, rp.y + rsz.y + 4),
                              IM_COL32(10, 10, 14, 215), ui_px(4));
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
        if (ImGui::IsItemClicked() && !inserting) {
            if (isSel) actFlip = true;
            else {
                g_ui.selected = i;
                g_ui.face = g_ui.facePrev = 0;
                g_ui.flipAnim = 1.0f;
                play_nav();
            }
        }

        x += w + gap;
    }
    ImGui::PopFont();

    // ------------------------------------------------------------------ apply input
    if (move != 0) {
        g_ui.selected = (g_ui.selected + move + n) % n;
        g_ui.face = g_ui.facePrev = 0;
        g_ui.flipAnim = 1.0f;
        play_nav();
    }
    if (actFlip) {
        g_ui.facePrev = g_ui.face;
        g_ui.face = (g_ui.face + 1) % 2;
        g_ui.flipAnim = 0.0f;
        play_flip();
    }
    g_ui.flipAnim = fminf(1.0f, g_ui.flipAnim + ImGui::GetIO().DeltaTime / 0.30f);

    // ------------------------------------------------------------------ attract idle
    // After a minute with no input anywhere (keyboard/mouse via
    // GetLastInputInfo, pads polled here), silently drift the carousel.
    {
        DWORD nowTick = GetTickCount();
        static DWORD lastPadTick = nowTick;
        if (pad_held_mask() != 0) lastPadTick = nowTick;
        LASTINPUTINFO lii{sizeof(lii), 0};
        DWORD idle = GetLastInputInfo(&lii) ? nowTick - lii.dwTime : 0;
        if (nowTick - lastPadTick < idle) idle = nowTick - lastPadTick;
        ULONGLONG now64 = GetTickCount64();
        if (idle > g_attractMs && g_session.logTail.empty() && !rebuildPanel) {
            if (now64 - g_ui.attractTick > 5000) {
                g_ui.attractTick = now64;
                g_ui.selected = (g_ui.selected + 1) % n;
                g_ui.face = g_ui.facePrev = 0;
                g_ui.flipAnim = 1.0f;
            }
        } else {
            g_ui.attractTick = now64;  // next step no sooner than 5s into attract
        }
    }

    // ------------------------------------------------------------------ info panel
    GameEntry& s = games[g_ui.selected];
    ReleaseInfo selRelProbe;
    bool soon = s.comingSoon && !s.launchable() && !release_for(s.artKey, &selRelProbe);
    float infoY = vh * 0.60f;

    // The crash panel, or the dev-mode rebuild panel, replaces this whole zone
    // until dismissed.
    if (g_session.logTail.empty() && !rebuildPanel) {
    ImGui::PushFont(g_ui.fontTitle);
    ImGui::SetCursorPosY(infoY);
    text_centered(narrow(s.title));
    ImGui::PopFont();

    ImGui::PushFont(g_ui.fontBase);
    ImGui::Dummy(ImVec2(0, vh * 0.004f));
    text_centered_colored(palette::dim, "Released " + narrow(s.releaseDate) + "   -   " +
                                            narrow(s.region));

    // Remote release + install-job state for the selected game
    ReleaseInfo rel;
    bool haveRel = release_for(s.artKey, &rel);
    InstallStatus inst = install_status();
    bool installingSel = (inst.phase == InstallPhase::Downloading ||
                          inst.phase == InstallPhase::Extracting) &&
                         inst.gameIndex == g_ui.selected;
    std::error_code fec;
    bool devCheckout =
        s.repoPath != s.installDir && std::filesystem::is_directory(s.repoPath, fec);

    enum class CardAction { None, Play, Download, PickRom };
    CardAction action = CardAction::None;

    std::string status;
    ImVec4 statusCol;
    if (installingSel) {
        if (inst.phase == InstallPhase::Downloading) {
            char buf[96];
            if (inst.total > 0)
                snprintf(buf, sizeof(buf), "Downloading %s - %d%%  (%.1f / %.1f MB)",
                         narrow(inst.tag).c_str(), (int)(inst.got * 100 / inst.total),
                         inst.got / 1048576.0, inst.total / 1048576.0);
            else
                snprintf(buf, sizeof(buf), "Downloading %s - %.1f MB", narrow(inst.tag).c_str(),
                         inst.got / 1048576.0);
            status = buf;
        } else {
            status = "Installing...";
        }
        statusCol = palette::accent;
    } else if (s.launchable()) {
        status = "Ready To Play";
        statusCol = palette::ready;
        action = CardAction::Play;
    } else if (!s.exeFound) {
        if (haveRel) {
            status = "Not installed - " + narrow(rel.tag) + " is a free download from GitHub";
            statusCol = palette::dim;
            action = CardAction::Download;
        } else if (soon) { status = "Recompilation not started yet"; statusCol = palette::faint; }
        else if (devCheckout) { status = "Not built - run the port build in " + narrow(s.repoDir); statusCol = palette::warn; }
        else { status = "Not available for download yet"; statusCol = palette::faint; }
    } else if (s.romWrong) {
        status = "Wrong ROM - " + narrow(s.romPath.filename().wstring()) + " isn't the dump this port needs. Select your own";
        statusCol = palette::warn;
        action = CardAction::PickRom;
    } else {
        status = "Installed - Select your own ROM dump: " + narrow(s.romNote) + " (.z64)";
        statusCol = palette::warn;
        action = CardAction::PickRom;
    }
    ImGui::Dummy(ImVec2(0, vh * 0.002f));
    text_centered_colored(statusCol, status);
    ImGui::PopFont();

    // Exact dump fingerprint, so users can verify their ROM before picking it
    if (action == CardAction::PickRom && !s.romSha1.empty()) {
        ImGui::PushFont(g_ui.fontSmall);
        if (s.romWrong) {
            text_centered_colored(palette::faint, "Needed: " + narrow(s.romNote));
            text_centered_colored(palette::faint, "Needed SHA1: " + narrow(s.romSha1));
            text_centered_colored(palette::faint, "This file's SHA1: " +
                                  (s.romHaveSha1.empty() ? std::string("(couldn't read it)") : narrow(s.romHaveSha1)));
        } else {
            text_centered_colored(palette::faint, "SHA1: " + narrow(s.romSha1));
        }
        ImGui::PopFont();
    }
    if (!s.romNotice.empty()) {
        ImGui::PushFont(g_ui.fontSmall);
        text_centered_colored(palette::warn, narrow(s.romNotice));
        ImGui::PopFont();
    }

    // Play stats ("Last played Tuesday  -  4.2 hours total")
    {
        auto it = settings().stats.find(s.artKey);
        if (it != settings().stats.end() && it->second.lastPlayedUnix > 0) {
            std::string line = "Last played " + format_last_played(it->second.lastPlayedUnix);
            if (it->second.playSeconds >= 60)
                line += "   -   " + format_play_time(it->second.playSeconds) + " total";
            ImGui::PushFont(g_ui.fontSmall);
            text_centered_colored(palette::faint, line);
            ImGui::PopFont();
        }
    }

    // Play as... (aki-roadmap#154): the original game or one of its conversions, remembered per game. A launch
    // choice: nothing in the game's files changes.
    bool lbPlayAs = false;
    if ((s.launchable() || !g_shotModsDir.empty()) && !installingSel) {
        const auto& convs = game_conversions(games, g_ui.selected);
        if (!convs.empty()) {
            lbPlayAs = true;
            const pkg::ConversionInfo* pick = play_as(games, g_ui.selected);
            if (!inserting && (ImGui::IsKeyPressed(ImGuiKey_C, false) || (pad & XINPUT_GAMEPAD_LEFT_SHOULDER))) {
                int next = (pick ? (int)(pick - convs.data()) : -1) + 1;  // the original, then each conversion
                set_play_as(s, next < (int)convs.size() ? convs[next].id : std::string());
                pick = play_as(games, g_ui.selected);
                play_nav();
            }
            ImGui::PushFont(g_ui.fontSmall);
            text_centered_colored(palette::accent, std::to_string(convs.size()) +
                                                       (convs.size() == 1 ? " conversion" : " conversions") +
                                                       " installed   -   Play as:  " +
                                                       (pick ? narrow(pick->name) : std::string("the original game")) +
                                                       "   -   C / LB changes it");
            if (pick && !pkg::conversion_rom_matches(*pick, rom_sha1_of(s)))
                text_centered_colored(palette::warn, narrow(pick->name) + " is made for another ROM than yours");
            ImGui::PopFont();
        }
    }

    // Update-available line (installed via the launcher, newer tag on GitHub)
    bool updateAvail = s.exeFound && !s.installedTag.empty() && haveRel &&
                       rel.tag != s.installedTag && !installingSel &&
                       inst.phase == InstallPhase::Idle;
    if (updateAvail) {
        ImGui::PushFont(g_ui.fontSmall);
        text_centered_colored(palette::accent,
                              "Update " + narrow(rel.tag) + " available  -  U / RB installs it");
        ImGui::PopFont();
        if (!inserting &&
            (ImGui::IsKeyPressed(ImGuiKey_U) || (pad & XINPUT_GAMEPAD_RIGHT_SHOULDER))) {
            if (install_start(g_ui.selected, s, rel)) play_flip();
        }
    }

    // Dev mode (aki-roadmap#25): rebuild the selected game's depot checkout
    // through AkiTools' aki-build.ps1. Players never see this line.
    if (dev_mode() && !installingSel) {
        std::wstring why = rebuild_unavailable_reason(s, depotRoot);
        ImGui::PushFont(g_ui.fontSmall);
        if (why.empty()) {
            text_centered_colored(palette::faint, std::string(lbPlayAs ? "Dev:  R Rebuilds " : "Dev:  R / LB Rebuilds ") +
                                                      narrow(s.repoDir) + " (AkiTools aki-build)");
            if (!inserting &&
                (ImGui::IsKeyPressed(ImGuiKey_R, false) || (!lbPlayAs && (pad & XINPUT_GAMEPAD_LEFT_SHOULDER)))) {
                std::wstring err;
                if (rebuild_start(g_ui.selected, s, depotRoot, &err)) {
                    play_flip();
                    g_session.statusNote.clear();
                    // Not this frame's Play too: the exe is about to be relinked.
                    actLaunch = false;
                    action = CardAction::None;
                } else {
                    g_session.statusNote = err;
                }
            }
        } else {
            text_centered_colored(palette::faint, "Dev:  Rebuild unavailable - " + narrow(why));
        }
        ImGui::PopFont();
    }

    // Action button (mouse users; keyboard/pad use Enter/A)
    if (action != CardAction::None) {
        const char* label = action == CardAction::Play       ? "PLAY"
                            : action == CardAction::Download ? "DOWNLOAD"
                                                             : "SELECT ROM...";
        ImGui::PushFont(g_ui.fontBase);
        ImVec2 bsz(vw * (action == CardAction::Play ? 0.10f : 0.13f), vh * 0.048f);
        // Keep the button at a stable anchor, but never let it ride up over the
        // stacked info lines above it (status + optional last-played + update-
        // available line). When those flow past the anchor, drop below them so the
        // button can't paint over the "Update available" text.
        float btnY = infoY + vh * 0.115f;
        float belowText = ImGui::GetCursorPosY() + vh * 0.012f;
        if (belowText > btnY) btnY = belowText;
        ImGui::SetCursorPos(ImVec2((vw - bsz.x) * 0.5f, btnY));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.62f, 0.12f, 0.92f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.98f, 0.75f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.75f, 0.53f, 0.08f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.07f, 0.05f, 1.0f));
        bool trigger = ImGui::Button(label, bsz);
        ImGui::PopStyleColor(4);
        ImGui::PopFont();
        if (actLaunch) trigger = true;
        if (trigger && !inserting) {
            if (action == CardAction::Play) {
                // Launch happens when the cart-insert animation finishes;
                // without cart art just launch immediately.
                play_launch();
                if (art[g_ui.selected].cart.ok()) {
                    g_ui.insertIndex = g_ui.selected;
                    g_ui.insertAnim = 0.0f;
                } else {
                    launchIndex = g_ui.selected;
                }
            } else if (action == CardAction::Download) {
                if (install_start(g_ui.selected, s, rel)) play_flip();
            } else {
                std::wstring err;
                if (pick_rom(s, &err)) {
                    apply_overrides(games);
                    play_flip();
                } else if (!err.empty()) {
                    g_session.statusNote = err;
                }
            }
        }
    }

    // [fullconv] The selected game's conversions (full-conversion mods of every kind): C / LB.
    if (s.exeFound && !inserting) {
        ImGui::PushFont(g_ui.fontSmall);
        const float cw = vw * 0.16f;
        ImGui::SetCursorPosX((vw - cw) * 0.5f);
        const std::string label = g_ui.padInput ? "CONVERSIONS  (LB)" : "CONVERSIONS  (C)";
        if (ImGui::Button(label.c_str(), ImVec2(cw, 0)) || ImGui::IsKeyPressed(ImGuiKey_C) ||
            (pad & XINPUT_GAMEPAD_LEFT_SHOULDER)) {
            g_ui.showConversions = true;
            g_ui.convMessage.clear();
            refresh_conversions(games);
        }
        ImGui::PopFont();
    }

    }  // end if (no crash panel)

    // Crash panel: statusNote + tail of the port's own log, dismissible.
    if (!g_session.logTail.empty()) {
        float px0 = vw * 0.16f, px1 = vw * 0.84f;
        float py0 = vh * 0.615f, py1 = vh * 0.935f;
        ImVec2 p0(vp->WorkPos.x + px0, vp->WorkPos.y + py0);
        ImVec2 p1(vp->WorkPos.x + px1, vp->WorkPos.y + py1);
        dl->AddRectFilled(p0, p1, IM_COL32(16, 12, 14, 246), ui_px(10));
        dl->AddRect(p0, p1, IM_COL32(180, 80, 70, 200), ui_px(10), 0, ui_px(2));
        ImGui::SetCursorPos(ImVec2(px0 + 24, py0 + 16));
        ImGui::PushFont(g_ui.fontBase);
        ImGui::TextColored(palette::error, "%s", narrow(g_session.statusNote).c_str());
        ImGui::PopFont();
        ImGui::PushFont(g_ui.fontSmall);
        ImGui::SetCursorPosX(px0 + 24);
        ImGui::TextColored(palette::faint, "%s",
                           narrow(g_session.portLogPath.wstring()).c_str());
        ImGui::Dummy(ImVec2(0, ui_px(6)));
        for (const auto& line : g_session.logTail) {
            ImGui::SetCursorPosX(px0 + 24);
            ImGui::TextColored(palette::dim, "%s", line.c_str());
        }
        ImGui::SetCursorPos(ImVec2(px0 + 24, py1 - ImGui::GetFontSize() * 1.9f));
        ImGui::TextColored(palette::faint, "Backspace / B to dismiss");
        ImGui::PopFont();
    } else if (rebuildPanel) {
        // Rebuild panel: progress line + the tail of aki-build's output.
        const RebuildStatus& rb = rebuild_status();
        float px0 = vw * 0.16f, px1 = vw * 0.84f;
        float py0 = vh * 0.615f, py1 = vh * 0.935f;
        ImVec2 p0(vp->WorkPos.x + px0, vp->WorkPos.y + py0);
        ImVec2 p1(vp->WorkPos.x + px1, vp->WorkPos.y + py1);
        bool running = rb.phase == RebuildPhase::Running;
        bool good = rb.phase == RebuildPhase::Done;
        dl->AddRectFilled(p0, p1, IM_COL32(12, 14, 18, 246), ui_px(10));
        dl->AddRect(p0, p1, running ? IM_COL32(250, 199, 72, 160)
                            : good  ? IM_COL32(110, 200, 120, 200)
                                    : IM_COL32(180, 80, 70, 200),
                    10.0f, 0, 2.0f);
        ULONGLONG ms = (running ? GetTickCount64() : rb.endTick) - rb.startTick;
        char head[256];
        if (running && rb.stepTotal > 0)
            snprintf(head, sizeof(head), "Rebuilding %s  -  %d / %d  (%d%%)  -  %llum %02llus",
                     narrow(rb.title).c_str(), rb.stepDone, rb.stepTotal,
                     rb.stepDone * 100 / rb.stepTotal, ms / 60000, ms / 1000 % 60);
        else if (running)
            snprintf(head, sizeof(head), "Rebuilding %s  -  %llum %02llus", narrow(rb.title).c_str(),
                     ms / 60000, ms / 1000 % 60);
        else if (good)
            snprintf(head, sizeof(head), "Rebuilt %s in %llum %02llus", narrow(rb.title).c_str(),
                     ms / 60000, ms / 1000 % 60);
        else if (rb.cancelled)
            snprintf(head, sizeof(head), "Rebuild of %s cancelled", narrow(rb.title).c_str());
        else
            snprintf(head, sizeof(head), "Rebuild of %s failed (exit %lu)", narrow(rb.title).c_str(),
                     rb.exitCode);
        ImGui::SetCursorPos(ImVec2(px0 + 24, py0 + 16));
        ImGui::PushFont(g_ui.fontBase);
        ImGui::TextColored(running ? palette::accent : good ? palette::ready : palette::error, "%s", head);
        ImGui::PopFont();
        ImGui::PushFont(g_ui.fontSmall);
        ImGui::Dummy(ImVec2(0, ui_px(6)));
        // As many of the last lines as fit above the hint line.
        float lineH = ImGui::GetTextLineHeightWithSpacing();
        float room = (py1 - ImGui::GetFontSize() * 2.4f) - ImGui::GetCursorPosY();
        size_t fit = room > 0 ? (size_t)(room / lineH) : 0;
        size_t first = rb.tail.size() > fit ? rb.tail.size() - fit : 0;
        float textW = px1 - px0 - 48;
        for (size_t i = first; i < rb.tail.size(); ++i) {
            ImGui::SetCursorPosX(px0 + 24);
            ImGui::PushClipRect(ImVec2(p0.x + 24, p0.y), ImVec2(p0.x + 24 + textW, p1.y), true);
            ImGui::TextColored(palette::dim, "%s", rb.tail[i].c_str());
            ImGui::PopClipRect();
        }
        ImGui::SetCursorPos(ImVec2(px0 + 24, py1 - ImGui::GetFontSize() * 1.9f));
        ImGui::TextColored(palette::faint, running ? "Backspace / B cancels the build"
                                                   : "Backspace / B to dismiss");
        ImGui::PopFont();
    } else if (!g_session.statusNote.empty()) {
        ImGui::PushFont(g_ui.fontSmall);
        // Multi-line notes (the wrong-ROM error) grow upward, clear of the footer.
        std::string note = narrow(g_session.statusNote);
        int lines = 1 + (int)std::count(note.begin(), note.end(), '\n');
        float noteY = vh * 0.935f - lines * ImGui::GetTextLineHeightWithSpacing();
        ImGui::SetCursorPosY(noteY < vh * 0.905f ? noteY : vh * 0.905f);
        text_centered_colored(palette::error, note);
        ImGui::PopFont();
    }

    // Footer
    ImGui::PushFont(g_ui.fontSmall);
    {
        const char* credit = "Made by jessetbh";
        ImVec2 csz = ImGui::CalcTextSize(credit);
        ImGui::SetCursorPos(ImVec2(vw - csz.x - ImGui::GetFontSize(), vh * 0.945f));
        ImGui::TextColored(palette::faint, "%s", credit);
    }
    ImGui::SetCursorPosY(vh * 0.945f);
    // No depot is the normal end-user state (games install via download);
    // the per-card status already says what each game needs.
    if (g_ui.padInput)
        text_centered_colored(palette::faint,
                              "D-Pad Browse      A Play      X Flip Box      "
                              "Y Settings      Start Mods      Back Quick Match      In Game:  Hold " +
                                  chord_to_string(settings().chordMask) + "  To Return");
    else
        text_centered_colored(palette::faint,
                              "Left/Right Browse      Enter Play      F Flip Box      "
                              "S Settings      M Mods      Q Quick Match      H History Tour      Esc Quit      In Game:  " +
                                  hotkey_to_string(settings().hotkeyMods, settings().hotkeyVk) +
                                  "  To Return");
    ImGui::PopFont();

    // ------------------------------------------------------------------ cart insert
    // PLAY slides the cart art down off the bottom edge - into an N64 sitting
    // just out of view - then the actual launch fires.
    if (g_ui.insertIndex >= 0) {
        float tPrev = g_ui.insertAnim;
        g_ui.insertAnim += ImGui::GetIO().DeltaTime / 1.15f;        // ~1.15s total
        if (tPrev < 0.80f && g_ui.insertAnim >= 0.80f) play_nav();  // seat "click"
        if (g_ui.insertAnim >= 1.0f) {
            launchIndex = g_ui.insertIndex;
            g_ui.insertIndex = -1;
        } else {
            float t = g_ui.insertAnim;
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            ImVec2 wp = vp->WorkPos;
            float dim = fminf(t / 0.30f, 1.0f);
            fg->AddRectFilled(wp, ImVec2(wp.x + vw, wp.y + vh),
                              IM_COL32(8, 8, 12, (int)(dim * 190.0f)));
            const Texture& cart = art[g_ui.insertIndex].cart;
            float appear = fminf(t / 0.10f, 1.0f);  // quick fade/scale in
            float cw = vw * 0.16f * (0.92f + 0.08f * appear);
            float ch = cw * (float)cart.h / (float)cart.w;
            float cx = wp.x + vw * 0.5f;
            // Timeline: drop onto the slot, rest a beat, click-in push, hold
            // seated with the top half of the cart peeking over the edge.
            float y0 = wp.y + vh * 0.30f;            // hovering at the card row
            float yRest = wp.y + vh - ch * 0.24f;    // dropped in, not yet seated
            float ySeat = wp.y + vh;                 // seated: 50% below the edge
            float cy;
            if (t < 0.55f) {
                float u = t / 0.55f;
                cy = y0 + (yRest - y0) * u * u;      // gravity: accelerate down
            } else if (t < 0.70f) {
                cy = yRest;                          // resting on the slot
            } else if (t < 0.80f) {
                float u = (t - 0.70f) / 0.10f;
                cy = yRest + (ySeat - yRest) * u * u * (3.0f - 2.0f * u);  // push in
            } else {
                cy = ySeat;                          // seated until launch
            }
            fg->AddImage((ImTextureID)cart.srv, ImVec2(cx - cw * 0.5f, cy - ch * 0.5f),
                         ImVec2(cx + cw * 0.5f, cy + ch * 0.5f), ImVec2(0, 0), ImVec2(1, 1),
                         IM_COL32(255, 255, 255, (int)(appear * 255.0f)));
        }
    }

    ImGui::End();
    return launchIndex;
}

// ---------------------------------------------------------------------------
// wWinMain
// ---------------------------------------------------------------------------

// --- Screenshots (--shot / --shot-all) ------------------------------------------

// The frame just drawn (before any Present) as a PNG.
static bool capture_png(const std::filesystem::path& out, std::wstring* err) {
    ID3D11Texture2D* back = frame_texture();
    if (!back) {
        *err = L"no frame texture";
        return false;
    }
    D3D11_TEXTURE2D_DESC d{};
    back->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    bool ok = SUCCEEDED(g_device->CreateTexture2D(&d, nullptr, &staging));
    if (ok) {
        g_context->CopyResource(staging, back);
        D3D11_MAPPED_SUBRESOURCE m{};
        ok = SUCCEEDED(g_context->Map(staging, 0, D3D11_MAP_READ, 0, &m));
        if (ok) {
            ok = write_png(out, (int)d.Width, (int)d.Height, (const uint8_t*)m.pData, m.RowPitch, err);
            g_context->Unmap(staging, 0);
        } else {
            *err = L"can't read the back buffer";
        }
        staging->Release();
    } else {
        *err = L"can't make a staging texture";
    }
    back->Release();
    return ok;
}

// Puts the UI on a shot's page. Pages with nothing to show get the demo mods folder. False (with *why) when the page
// can't be shown as asked: the shot is still saved, and the run fails.
static bool shot_setup(ShotPage page, std::vector<GameEntry>& games, int gameIndex, bool demo,
                       const std::filesystem::path& demoDir, bool demoOk, std::wstring* why) {
    if (g_ui.showMods) close_mods();
    g_ui.showSettings = g_ui.showQuick = g_ui.showTour = g_ui.showUpdate = false;
    g_shotModsDir.clear();
    g_shotScrollPacks = false;
    set_dev_mode_from_args(page == ShotPage::Dev);
    if (gameIndex >= 0) g_ui.selected = gameIndex;
    bool needsGame = page == ShotPage::Mods || page == ShotPage::ModelPacks || page == ShotPage::QuickMatch;
    if (gameIndex < 0) {
        *why = L"no game to show";
        return !needsGame;
    }
    if (page == ShotPage::ModelPacks && games[gameIndex].modelProfile.empty()) {
        *why = games[gameIndex].title + L" has no model import (no model packs section)";
        return false;
    }
    switch (page) {
        case ShotPage::Settings: open_settings(games); break;
        case ShotPage::Mods:
        case ShotPage::ModelPacks:
            if (demo) g_shotModsDir = demoDir;
            open_mods(games, gameIndex);
            if (g_shotModsDir.empty() &&
                (page == ShotPage::ModelPacks ? g_ui.packs.empty() : g_ui.modItems.empty() && g_ui.packs.empty())) {
                close_mods();
                g_shotModsDir = demoDir;
                open_mods(games, gameIndex);
            }
            g_shotScrollPacks = page == ShotPage::ModelPacks;
            break;
        case ShotPage::QuickMatch:
            if (demo) g_shotModsDir = demoDir;
            open_quick(games, gameIndex);
            if (!g_ui.qmFound && g_shotModsDir.empty()) {
                g_shotModsDir = demoDir;
                open_quick(games, gameIndex);
            }
            break;
        case ShotPage::HistoryTour: open_tour(); break;
        case ShotPage::Home:
            if (demo) g_shotModsDir = demoDir;  // its conversions: Play as... (aki-roadmap#154)
            break;
        default: break;
    }
    if (!g_shotModsDir.empty() && !demoOk) {
        *why = L"the demo mods folder couldn't be made";
        return false;
    }
    return true;
}

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR, int) {
    bool smoke = false;
    bool wantSettings = false;
    int testLaunch = -1;
    int playNow = -1;      // debug: launch game N at startup, keep normal UI
    std::wstring addConversionPath;  // --add-conversion <zip|folder>: install it, exit 0/6
    std::wstring playConversion;     // --conversion <id> (with --play N): launch that conversion
    int listConversions = -1;        // --list-conversions N: log game N's conversions, exit 0
    bool listRoms = false;           // --list-roms: log the ROM each game would get, exit 0 (opens no window)
    int testInstall = -1;  // debug: download+install game N, exit 0/4
    int testRomIdx = -1;   // debug: validate+copy a ROM for game N, exit 0/4
    int testRebuild = -1;  // debug: dev-mode rebuild of game N through aki-build, exit 0/4
    std::wstring updateConfirm;  // self-update: the event the previous launcher waits on (set once running)
    std::wstring updatedFrom;    // self-update: the version this one replaced
    std::wstring testRomPath;
    ShotRequest shot;
    {
        std::vector<std::wstring> args(__wargv + 1, __wargv + __argc);
        for (const auto& a : args)  // a screenshot run keeps the last real session's log
            if (a == L"--shot" || a == L"--shot-all") log_set_name(L"aki-launcher-shot.log");
        std::wstring err;
        if (!parse_shot_args(args, &shot, &err)) {
            logf(L"shot: %s", err.c_str());
            return 2;
        }
        g_shot = shot.active;
    }
    for (int i = 1; i < __argc && !g_shot; ++i) {
        std::wstring a = __wargv[i];
        if (a == L"--smoke") smoke = true;
        else if (a == L"--test-launch" && i + 1 < __argc) testLaunch = _wtoi(__wargv[++i]);
        else if (a == L"--select" && i + 1 < __argc) g_ui.selected = _wtoi(__wargv[++i]);
        else if (a == L"--flip") g_ui.face = 1;
        else if (a == L"--face" && i + 1 < __argc) g_ui.face = _wtoi(__wargv[++i]) % 2;
        else if (a == L"--attract-ms" && i + 1 < __argc) g_attractMs = (DWORD)_wtoi(__wargv[++i]);
        else if (a == L"--install" && i + 1 < __argc) testInstall = _wtoi(__wargv[++i]);
        else if (a == L"--set-rom" && i + 2 < __argc) {
            testRomIdx = _wtoi(__wargv[++i]);
            testRomPath = __wargv[++i];
        }
        else if (a == L"--settings") wantSettings = true;
        else if (a == L"--add-conversion" && i + 1 < __argc) addConversionPath = __wargv[++i];
        else if (a == L"--conversion" && i + 1 < __argc) playConversion = __wargv[++i];
        else if (a == L"--list-conversions" && i + 1 < __argc) listConversions = _wtoi(__wargv[++i]);
        else if (a == L"--list-roms") listRoms = true;
        else if (a == L"--play" && i + 1 < __argc) playNow = _wtoi(__wargv[++i]);
        else if (a == L"--dev") set_dev_mode_from_args(true);
        else if (a == L"--dev-ipc") launch_set_ipc(true);  // aki-roadmap#21: offer games the launcher link (gameipc.h)
        else if (a == L"--test-rebuild" && i + 1 < __argc) testRebuild = _wtoi(__wargv[++i]);
        else if (a == L"--update-confirm" && i + 1 < __argc) updateConfirm = __wargv[++i];
        else if (a == L"--updated-from" && i + 1 < __argc) updatedFrom = __wargv[++i];
    }

    {
        wchar_t ipcEnv[4]{};
        if (!g_shot && GetEnvironmentVariableW(L"AKI_DEV_IPC", ipcEnv, 4) && ipcEnv[0] == L'1') launch_set_ipc(true);
    }

    ImGui_ImplWin32_EnableDpiAwareness();

    HICON appIcon = LoadIconW(hinst, MAKEINTRESOURCEW(1));  // icons/app.rc resource
    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, wnd_proc, 0, 0, hinst, appIcon,
                   LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, L"AkiLauncher", appIcon};
    RegisterClassExW(&wc);

    settings_load();  // needed before window creation for the windowed pref
    logf(L"settings file: %s", settings_path().c_str());
    if (g_shot) {
        // Nothing a screenshot run does may reach the config or start a game.
        settings_set_read_only(true);
        launch_set_blocked(true);
        // Dev mode is the dev shot's alone (settings or AKI_DEV would put its line on home too); in memory only.
        settings().devMode = false;
        SetEnvironmentVariableW(L"AKI_DEV", nullptr);
        logf(L"shot: %d page(s), no launches, settings read-only", (int)shot.shots.size());
    }

    if (listRoms) {
        std::vector<GameEntry> romGames = build_game_list(detect_depot_root());
        apply_overrides(romGames);
        for (const GameEntry& g : romGames) {
            const std::wstring problem = rom_problem(g);
            if (!problem.empty()) logf(L"   %s", problem.c_str());
            if (!g.romNotice.empty()) logf(L"   %s", g.romNotice.c_str());
        }
        return 0;
    }

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    bool windowed = smoke || settings().windowed;  // smoke shouldn't take over the screen
    HWND hwnd;
    if (g_shot) {
        // Never shown: no focus taken, nothing on the screen. The swapchain renders all the same.
        hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"AKI Launcher (screenshot)", WS_POPUP,
                               -32000, -32000, shot.width, shot.height, nullptr, nullptr, hinst, nullptr);
    } else if (smoke) {
        hwnd = CreateWindowW(wc.lpszClassName, L"AKI Launcher", WS_OVERLAPPEDWINDOW, 100, 100,
                             1280, 720, nullptr, nullptr, hinst, nullptr);
    } else if (windowed) {
        RECT r = windowed_rect();
        hwnd = CreateWindowW(wc.lpszClassName, L"AKI Launcher", WS_OVERLAPPEDWINDOW, r.left, r.top,
                             r.right - r.left, r.bottom - r.top, nullptr, nullptr, hinst, nullptr);
    } else {
        hwnd = CreateWindowW(wc.lpszClassName, L"AKI Launcher", WS_POPUP, 0, 0, screenW, screenH,
                             nullptr, nullptr, hinst, nullptr);
    }

    if (g_shot ? !create_offscreen_device((UINT)shot.width, (UINT)shot.height) : !create_device(hwnd)) {
        logf(L"no D3D11 device");
        destroy_device();
        UnregisterClassW(wc.lpszClassName, hinst);
        return 1;
    }

    if (!g_shot) {
        ShowWindow(hwnd, SW_SHOWDEFAULT);
        UpdateWindow(hwnd);
        DragAcceptFiles(hwnd, TRUE);  // drop mods / texture packs on the window (aki-roadmap#96)
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.FrameRounding = 8.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.028f, 0.030f, 0.045f, 1.0f);
    g_baseStyle = style;

    // Fonts: Segoe UI at sizes derived from the actual window height.
    RECT cr{};
    GetClientRect(hwnd, &cr);
    float uiH = (float)(cr.bottom - cr.top);
    if (uiH < 400 && !g_shot) uiH = (float)screenH;
    build_fonts(uiH);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    g_hwnd = hwnd;
    DragAcceptFiles(hwnd, TRUE);  // conversion packages can be dropped on the window
    // Register the Return To Launcher keyboard hotkey at startup. This was
    // previously only done from the Settings rebind flow, so Shift+F12 was
    // dead on a fresh install until the user rebound it once.
    if (!g_shot) {  // a screenshot run takes no hotkeys and makes no sound
        reregister_hotkey();
        // Footer hints start in pad mode when a controller is already connected.
        for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
            XINPUT_STATE xs{};
            if (XInputGetState(i, &xs) == ERROR_SUCCESS) {
                g_ui.padInput = true;
                break;
            }
        }
        audio_init();
        RegisterHotKey(hwnd, HOTKEY_QUICKBACK, settings().hotkeyMods | MOD_NOREPEAT,
                       settings().hotkeyVk);
    }

    std::filesystem::path depotRoot = detect_depot_root();
    std::vector<GameEntry> games = build_game_list(depotRoot);
    apply_overrides(games);
    if (g_ui.selected < 0 || g_ui.selected >= (int)games.size()) g_ui.selected = 0;
    // A quick match whose launcher stopped before the game did: its settings are undone now, unless that game is
    // still running (it could still save them; then the game's next launch does it).
    for (const auto& g : games) {
        std::filesystem::path data = g.exeFound && !g_shot ? game_data_dir(g) : std::filesystem::path();
        if (data.empty() || !quickmatch_pending(data)) continue;
        if (exe_running(g.exePath)) {
            logf(L"quick match: %s is still running; its run settings in %s are undone at its next launch",
                 g.title.c_str(), data.c_str());
            continue;
        }
        std::wstring err;
        bool ok = quickmatch_restore(data, &err);
        logf(L"quick match: left-over run settings in %s %s%s", data.c_str(), ok ? L"restored" : L"NOT restored: ",
             err.c_str());
    }
    if (wantSettings) open_settings(games);
    if (listConversions >= 0 && listConversions < (int)games.size()) {
        for (const ConversionInfo& c : list_conversions(games[listConversions])) {
            logf(L"conversion %s \"%s\" v%s:%s%s%s%s%s", c.id.c_str(), c.name.c_str(), c.version.c_str(),
                 c.patch ? L" [Patch]" : L"", c.codes ? L" [Codes]" : L"", c.saves ? L" [Save]" : L"",
                 c.textures ? L" [Textures]" : L"", c.rt ? L" [RT]" : L"");
            for (const auto& n : c.notes) logf(L"   note: %s", n.c_str());
        }
        return 0;
    }
    if (!addConversionPath.empty()) {
        AddResult r = add_conversion(games, addConversionPath);
        logf(L"add-conversion %s: %s", r.ok ? L"OK" : L"FAILED", r.message.c_str());
        return r.ok ? 0 : 6;
    }
    logf(L"depot root: %s", depotRoot.empty() ? L"(NOT FOUND)" : depotRoot.c_str());

    if (!g_shot) releases_check_start(games);
    if (!smoke && !g_shot && testInstall < 0 && testRomIdx < 0 && testRebuild < 0 && testLaunch < 0) update_tick();
    if (!updatedFrom.empty()) logf(L"update: running after an update from %s", updatedFrom.c_str());

    // Box art: assets\boxart\<key>_front.png / _back.png under the app root.
    std::filesystem::path artDir = app_root() / L"assets" / L"boxart";
    std::vector<BoxArt> art(games.size());
    for (size_t i = 0; i < games.size(); ++i) {
        art[i].front = load_texture(g_device, artDir / (games[i].artKey + L"_front.png"));
        art[i].back = load_texture(g_device, artDir / (games[i].artKey + L"_back.png"));
        art[i].cart = load_texture(g_device, artDir / (games[i].artKey + L"_cart.png"));
        art[i].backdrop = load_texture_blurred(g_device, artDir / (games[i].artKey + L"_front.png"));
        logf(L"game: %s exe=%d rom=%d art(front=%d back=%d)", games[i].title.c_str(),
             games[i].exeFound, games[i].romFound, art[i].front.ok(), art[i].back.ok());
    }

    // Screenshots: which shot, how many frames of it so far, the game, the demo folder (a temp folder, removed at exit).
    size_t shotIndex = 0;
    int shotFrame = 0, shotFailed = 0;
    bool shotPageOk = true, shotDemoOk = false;
    std::wstring shotWhy;
    int shotGame = -1;
    std::filesystem::path shotDemo;
    if (g_shot) {
        std::wstring want = shot.game.empty() ? L"nomercy" : shot.game;
        for (int i = 0; i < (int)games.size(); ++i)
            if (games[i].artKey == want) shotGame = i;
        if (shotGame < 0 && !shot.game.empty()) {
            logf(L"shot: no game \"%s\" in the registry", shot.game.c_str());
            return 2;
        }
        if (shotGame < 0 && !games.empty()) shotGame = 0;
        wchar_t tmpBuf[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tmpBuf);
        shotDemo = std::filesystem::path(tmpBuf) / (L"akilauncher-shot-demo-" + std::to_wstring(GetCurrentProcessId()));
        std::wstring err;
        shotDemoOk = make_demo_mods_dir(shotDemo, &err) &&
                     make_demo_conversions(std::filesystem::path(shotDemo.wstring() + L"-data"), &err);
        if (!shotDemoOk) logf(L"shot: %s", err.c_str());
    }

    int frame = 0;
    bool testCloseSent = false;
    bool testConfirmSent = false;
    ULONGLONG testCloseTick = 0;
    int exitCodeOut = 0;
    bool done = false;
    bool exitForUpdate = false;  // the updated launcher took over: don't save over its settings
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
        if (g_shot && shotFrame == 0) {
            shotWhy.clear();
            shotPageOk = shot_setup(shot.shots[shotIndex].first, games, shotGame, shot.demo, shotDemo, shotDemoOk, &shotWhy);
        }

        // Window-mode switches happen between frames: the font atlas rebuild
        // must not race an in-flight ImGui frame.
        if (g_pendingWindowMode >= 0 && !smoke) {
            set_launcher_windowed(g_pendingWindowMode == 1);
            g_pendingWindowMode = -1;
        }

        if (g_resizeW && g_resizeH && g_swapChain) {
            destroy_render_target();
            g_swapChain->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            // A window resized (or moved to a screen with another scale) a good deal gets fonts and sizes for its
            // new height; small drags keep the atlas.
            float h = (float)g_resizeH;
            if (h >= 400 && g_fontsH > 0 && fabsf(h - g_fontsH) > g_fontsH * 0.08f) {
                build_fonts(h);
                ImGui_ImplDX11_InvalidateDeviceObjects();  // recreated (with the atlas) on next NewFrame
            }
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

        // --- install completion (poll; worker thread does the heavy lifting) ---
        {
            InstallStatus st = install_status();
            if (st.phase == InstallPhase::Done || st.phase == InstallPhase::Error) {
                if (testInstall >= 0) {  // test driver consumes the result
                    logf(L"test-install: %s (%s)",
                         st.phase == InstallPhase::Done ? L"done" : L"FAILED", st.error.c_str());
                    exitCodeOut = st.phase == InstallPhase::Done ? 0 : 4;
                    break;
                }
                if (st.phase == InstallPhase::Done) {
                    games = build_game_list(depotRoot);
                    g_ui.convCache.clear();  // a game installed or rebuilt: its conversions read again
                    apply_overrides(games);
                    play_flip();
                } else {
                    g_session.statusNote = L"Install failed - " + st.error;
                }
                install_ack();
            }
        }

        // --- files dropped on the window: install into the mods page's game (or the selected one) ---
        if (!g_dropped.empty()) {
            std::vector<std::filesystem::path> drops;
            drops.swap(g_dropped);
            if (g_session.active() || g_ui.showSettings || g_ui.insertIndex >= 0) {
                logf(L"drop ignored: %zu item(s) while %s", drops.size(),
                     g_session.active() ? L"a game runs" : g_ui.showSettings ? L"in Settings" : L"a game starts");
            } else {
                if (!g_ui.showMods) open_mods(games, g_ui.selected);
                install_into_mods(games, drops);
            }
        }

        // --- launcher self-update: periodic check; install once the download is verified ---
        if (!smoke && !g_shot && testInstall < 0 && testRomIdx < 0 && testRebuild < 0 && testLaunch < 0) {
            update_tick();
            if (g_ui.updateRequested && update_status().phase == UpdatePhase::Staged) {
                g_ui.updateRequested = false;
                if (apply_update_and_restart(hwnd)) {
                    exitForUpdate = true;
                    break;
                }
            }
        }
        // The new launcher is up (a few frames drawn): tell the previous one, which then exits.
        if (!updateConfirm.empty() && frame == 30) {
            if (HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, updateConfirm.c_str())) {
                SetEvent(ev);
                CloseHandle(ev);
            }
            logf(L"update: confirmed start (replacing %s)", updatedFrom.c_str());
            updateConfirm.clear();
            // The staged copy is done with; .update\previous stays for a manual rollback.
            wchar_t exeBuf[MAX_PATH * 2]{};
            GetModuleFileNameW(nullptr, exeBuf, (DWORD)std::size(exeBuf));
            std::error_code ec;
            std::filesystem::remove_all(std::filesystem::path(exeBuf).parent_path() / L".update" / L"staged", ec);
        }

        // --- dev-mode rebuild (poll the pipe; the panel stays until dismissed) ---
        {
            static RebuildPhase lastPhase = RebuildPhase::Idle;
            rebuild_tick();
            RebuildPhase ph = rebuild_status().phase;
            if (lastPhase == RebuildPhase::Running && ph != RebuildPhase::Running) {
                if (testRebuild >= 0) {
                    exitCodeOut = ph == RebuildPhase::Done ? 0 : 4;
                    break;
                }
                // A first build makes the exe appear: re-scan like after an install.
                games = build_game_list(depotRoot);
                g_ui.convCache.clear();  // a game installed or rebuilt: its conversions read again
                apply_overrides(games);
                if (ph == RebuildPhase::Done) play_flip();
            }
            lastPhase = ph;
        }

        // --- automated test drivers ---
        frame++;
        if (smoke && frame > 60) break;
        if (testRomIdx >= 0 && frame == 5) {
            std::wstring err;
            bool ok = testRomIdx < (int)games.size() &&
                      install_rom_file(games[testRomIdx], testRomPath, &err);
            logf(L"test-set-rom: %s (%s)", ok ? L"ok" : L"FAILED", err.c_str());
            exitCodeOut = ok ? 0 : 4;
            break;
        }
        if (testInstall >= 0 && frame > 30) {
            static bool started = false;
            ReleaseInfo rel;
            if (!started && testInstall < (int)games.size() &&
                release_for(games[testInstall].artKey, &rel)) {
                started = install_start(testInstall, games[testInstall], rel);
                logf(L"test-install: started %s %s", games[testInstall].title.c_str(),
                     rel.tag.c_str());
            }
            if (!started && frame > 1800) {  // ~30s without release info
                logf(L"test-install: release check never produced a result");
                exitCodeOut = 4;
                break;
            }
        }
        if (testRebuild >= 0 && frame == 5) {
            std::wstring err;
            if (testRebuild >= (int)games.size() ||
                !rebuild_start(testRebuild, games[testRebuild], depotRoot, &err)) {
                logf(L"test-rebuild: FAILED to start (%s)", err.c_str());
                exitCodeOut = 4;
                break;
            }
        }
        if (playNow >= 0 && frame == 30 && playNow < (int)games.size()) {
            std::wstring err;
            std::vector<std::pair<std::wstring, std::wstring>> env;
            if (!playConversion.empty()) {
                for (const ConversionInfo& c : list_conversions(games[playNow])) {
                    if (c.id == playConversion) env = conversion_env(c);
                }
                if (env.empty()) logf(L"play: conversion %s isn't installed for %s", playConversion.c_str(), games[playNow].title.c_str());
            }
            if (!g_session.launch(games[playNow], &err, env.empty() ? nullptr : &env)) logf(L"play: launch failed: %s", err.c_str());
        }
        if (testLaunch >= 0) {
            if (frame == 30 && testLaunch < (int)games.size()) {
                // Go through the real UI path, including the cart-insert
                // animation, when cart art exists.
                if (art[testLaunch].cart.ok()) {
                    logf(L"test-launch: starting cart-insert animation");
                    g_ui.insertIndex = testLaunch;
                    g_ui.insertAnim = 0.0f;
                } else {
                    std::wstring err;
                    if (!g_session.launch(games[testLaunch], &err)) {
                        logf(L"test-launch: launch failed: %s", err.c_str());
                        exitCodeOut = 2;
                        break;
                    }
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
        // Settings page is controller-navigable: enable ImGui gamepad (+
        // keyboard) nav while it's open — the Win32 backend polls XInput for
        // nav when the flag is set at NewFrame. Off everywhere else (the
        // carousel reads the pad directly), and off during the bind-capture
        // flows so held buttons don't drive focus mid-capture.
        {
            ImGuiIO& nio = ImGui::GetIO();
            if (!g_shot && ((g_ui.showSettings && !g_ui.captureChord && !g_ui.captureHotkey) || g_ui.showMods ||
                            g_ui.showUpdate || g_ui.showQuick || g_ui.showTour))
                nio.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
            else
                nio.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard);
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if (!g_session.active() && !g_ui.showSettings && !g_ui.showMods && !g_ui.showUpdate && !g_ui.showQuick && !g_ui.showTour &&
            g_ui.insertIndex < 0 &&
            ImGui::IsKeyPressed(ImGuiKey_Escape) && testLaunch < 0)
            done = true;

        // F11 / Alt+Enter toggle windowed <-> fullscreen from any screen.
        if (!smoke && (ImGui::IsKeyPressed(ImGuiKey_F11, false) ||
                       (ImGui::GetIO().KeyAlt && ImGui::IsKeyPressed(ImGuiKey_Enter, false))))
            g_pendingWindowMode = settings().windowed ? 0 : 1;

        int launchIdx = draw_ui(games, art, depotRoot);
        if (g_shot) {
            launchIdx = -1;  // nothing starts from a screenshot run
            g_ui.qmLaunch = false;
            if (!g_shotModsDir.empty()) {
                ImGui::GetForegroundDrawList()->AddText(ImVec2(ImGui::GetMainViewport()->WorkPos.x + 12,
                                                               ImGui::GetMainViewport()->WorkPos.y + 8),
                                                        IM_COL32(250, 199, 72, 255), "demo data");
            }
        }
        if (launchIdx >= 0) {
            // play_launch() already fired when the cart-insert animation began.
            std::wstring err;
            carry_portable(games[launchIdx]);
            // Quick match settings go into the data folder the game uses (after carry_portable). An earlier run's
            // left over (its undo failed) are undone first, or a plain Play would start a quick match.
            std::filesystem::path data = game_data_dir(games[launchIdx]);
            bool quick = g_ui.qmLaunch, ready = true;
            int tourStop = g_ui.tourLaunch;
            g_ui.qmLaunch = false;
            g_ui.tourLaunch = -1;
            if (quickmatch_pending(data) && !quickmatch_restore(data, &err)) {
                ready = false;
                g_session.statusNote = games[launchIdx].title + L": an earlier quick match's mod settings couldn't be "
                                       L"put back (" + err + L")";
            } else if (quick) {
                if (quickmatch_apply(data, g_ui.qm, g_ui.qmValues, &err)) g_qmRunDir = data;
                else {
                    ready = false;
                    g_session.statusNote = L"Quick match: " + err;
                }
            }
            // Play as... (aki-roadmap#154): a plain Play starts the remembered conversion; quick match and the
            // History Tour start the original game.
            g_session.conversion.clear();
            if (quick || tourStop >= 0) g_launchEnv.clear();
            g_session.conversionName.clear();
            if (!quick && tourStop < 0) {
                if (const pkg::ConversionInfo* c = play_as(games, launchIdx)) {
                    g_session.conversion = widen(c->id);
                    g_session.conversionName = c->name;
                    // Its RT rig and materials, as the conversions picker passes them (aki-roadmap#57).
                    if (g_launchEnv.empty()) {
                        for (const ConversionInfo& fc : list_conversions(games[launchIdx]))
                            if (fc.id == g_session.conversion) { g_launchEnv = conversion_env(fc); break; }
                    }
                }
            }
            // A pick from the conversions picker (C / LB) wins over Play as... for this launch.
            bool launched = ready && g_session.launch(games[launchIdx], &err, g_launchEnv.empty() ? nullptr : &g_launchEnv);
            g_launchEnv.clear();
            if (launched) g_ui.tourRunning = tourStop;
            if (ready && !launched) {
                g_session.statusNote = games[launchIdx].title + L": " + err;
                if (testLaunch >= 0) {
                    logf(L"test-launch: launch failed: %s", err.c_str());
                    exitCodeOut = 2;
                    break;
                }
            }
        }
        // A History Tour stop's game closed: the tour moves on and its page comes back.
        // Only a stop that ran and ended normally counts: a crash (its panel stays in front), a forced close or a
        // game whose window never came up leaves the tour where it was. Replaying a done stop doesn't move it.
        if (g_ui.tourRunning >= 0 && !g_session.active()) {
            bool played = g_session.runningTick != 0 && !g_session.wasTerminated && g_session.logTail.empty();
            if (played && g_ui.tourRunning == settings().tourStop) {
                settings().tourStop = g_ui.tourRunning + 1;
                settings_save();
            }
            g_ui.tourRunning = -1;
            if (played) open_tour();  // on the next stop
        }
        // A quick match's run is over (or never started): the player's mod settings come back.
        if (!g_qmRunDir.empty() && !g_session.active()) {
            std::wstring err;
            if (quickmatch_restore(g_qmRunDir, &err)) logf(L"quick match: mod settings restored in %s", g_qmRunDir.c_str());
            else {
                // The undo record stays: the next launch of this game (or launcher start) tries again first.
                logf(L"quick match: restore FAILED in %s: %s", g_qmRunDir.c_str(), err.c_str());
                g_session.statusNote = L"Quick match: your mod settings couldn't be put back yet (" + err +
                                       L"); the next launch tries again";
            }
            g_qmRunDir.clear();
        }

        ImGui::Render();
        const float clear[4] = {0.028f, 0.030f, 0.045f, 1.0f};
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        // A shot is taken once the page has had a few frames to lay out and load (fonts, thumbnails).
        if (g_shot && ++shotFrame >= 20) {
            const auto& [page, out] = shot.shots[shotIndex];
            std::wstring err;
            if (!shotPageOk) {
                ++shotFailed;
                logf(L"shot FAILED: %s: %s (saved, but not the page asked for)", out.c_str(), shotWhy.c_str());
                capture_png(out, &err);
            } else if (capture_png(out, &err)) logf(L"shot: %s", out.c_str());
            else {
                ++shotFailed;
                logf(L"shot FAILED: %s: %s", out.c_str(), err.c_str());
            }
            shotFrame = 0;
            if (++shotIndex >= shot.shots.size()) {
                exitCodeOut = shotFailed ? 1 : 0;
                done = true;
            }
        }
        if (g_swapChain) g_swapChain->Present(1, 0);

        // While the game owns the screen the launcher just idles behind it.
        if (g_session.state == SessionState::Running) Sleep(100);
    }

    if (g_session.active()) {
        logf(L"launcher exiting with game still running; leaving it alone");
    }
    romscan_shutdown();  // a scan thread must not outlive its globals
    release_mod_thumbs();
    // A build left behind would write into a pipe nobody reads: end it (its
    // build slot frees itself).
    if (rebuild_running()) {
        logf(L"launcher exiting during a rebuild; cancelling it");
        rebuild_cancel();
    }

    if (g_shot) {
        if (shotIndex < shot.shots.size()) {  // the loop ended early (WM_QUIT, a session end)
            logf(L"shot: stopped after %d of %d", (int)shotIndex, (int)shot.shots.size());
            exitCodeOut = 1;
        }
        if (g_ui.showMods) close_mods();  // its thumbnails may hold demo files open
        std::error_code ec;
        std::filesystem::remove_all(shotDemo, ec);
    std::filesystem::remove_all(std::filesystem::path(shotDemo.wstring() + L"-data"), ec);
    }
    if (!smoke && !g_shot && !exitForUpdate && settings().windowed) {
        remember_windowed_placement();
        settings_save();
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
