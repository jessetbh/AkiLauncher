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
#include "imgui_internal.h"  // SetFocusID: the Conversions page lands the controller on a conversion
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "imgui_stdlib.h"

#include "artwork.h"
#include "audio.h"
#include "conversions.h"
#include "games.h"
#include "importbatch.h"
#include "importer.h"
#include "installer.h"
#include "rebuild.h"
#include "rom_rules.h"
#include "romscan.h"
#include "mods.h"
#include "modelpacks.h"
#include "packages.h"
#include "cardlayout.h"
#include "convnav.h"
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
// A shot run driven by a pad script (--pad-script, aki-roadmap#232): the buttons down this frame, fed to the UI as a
// controller would be (pad_pressed and ImGui's gamepad nav).
static convnav::PadScriptPlayer g_padScript;
static WORD g_padScriptDown = 0;
static std::filesystem::path g_shotModsDir;  // the mods folder pages show instead of the game's (demo data)
// Bulk import (aki-roadmap#228): the batch, and a sandbox folder that stands in for the launcher's data folder and every
// game's data folder (--import-sandbox: headless tests and owner shots never touch real installs).
static ImportBatch g_import;
static std::filesystem::path g_importSandbox;
static std::vector<std::filesystem::path> g_importRomDirs;  // folders picked with Find it... this session
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
    // mods page (aki-roadmap#96)
    bool showMods = false;
    int modsGame = -1;
    std::filesystem::path modsDir;
    std::vector<ModItem> modItems;
    std::vector<Texture> modThumbs;  // parallel to modItems (empty srv = none)
    std::string modsNote;
    bool modsNoteOk = true;
    std::vector<ImportLine> modsResults;  // aki-roadmap#228: one line per item of the last multi-item install
    bool showImport = false;              // the Import results screen (over the Mods page)
    int importNew = 0;                    // entries waiting in the Import folder
    std::string importLog;                // the [Log] popup's text
    bool importLogOpen = false;
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
    // aki-roadmap#232: opened from the card's Play as... line (Down): the controller lands on this conversion's row
    // (-1 = the original game's) for the first frames, after ImGui's own first focus.
    int convFocus = -1;
    int convFocusFrames = 0;
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

// The controllers this frame (aki-roadmap#232), read once per frame before ImGui's: every XInput pad merged (buttons
// OR'd, the stick and triggers pushed furthest), or a shot run's pad script. pad_pressed() and ImGui's gamepad nav
// both read it, so any pad drives every page (ImGui's Win32 backend reads only the first; it is built without that).
static XINPUT_GAMEPAD g_padFrame{};
static bool g_padFrameAny = false;
static void poll_pads() {
    g_padFrame = {};
    g_padFrameAny = false;
    if (g_shot) {  // a pad someone is holding mustn't drive a screenshot run; a script may
        g_padFrame.wButtons = g_padScriptDown;
        g_padFrameAny = g_padScript.active();
        return;
    }
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        XINPUT_STATE st{};
        if (XInputGetState(i, &st) != ERROR_SUCCESS) continue;
        const XINPUT_GAMEPAD& p = st.Gamepad;
        g_padFrameAny = true;
        g_padFrame.wButtons |= p.wButtons;
        if (p.bLeftTrigger > g_padFrame.bLeftTrigger) g_padFrame.bLeftTrigger = p.bLeftTrigger;
        if (p.bRightTrigger > g_padFrame.bRightTrigger) g_padFrame.bRightTrigger = p.bRightTrigger;
        if (abs(p.sThumbLX) > abs(g_padFrame.sThumbLX)) g_padFrame.sThumbLX = p.sThumbLX;
        if (abs(p.sThumbLY) > abs(g_padFrame.sThumbLY)) g_padFrame.sThumbLY = p.sThumbLY;
        if (abs(p.sThumbRX) > abs(g_padFrame.sThumbRX)) g_padFrame.sThumbRX = p.sThumbRX;
        if (abs(p.sThumbRY) > abs(g_padFrame.sThumbRY)) g_padFrame.sThumbRY = p.sThumbRY;
    }
}

// g_padFrame as ImGui gamepad input (the mapping imgui_impl_win32 uses). Between ImGui_ImplWin32_NewFrame and
// ImGui::NewFrame.
static void feed_imgui_gamepad() {
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    if (!g_padFrameAny) return;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    const XINPUT_GAMEPAD& g = g_padFrame;
    auto button = [&](ImGuiKey key, WORD bit) { io.AddKeyEvent(key, (g.wButtons & bit) != 0); };
    auto analog = [&](ImGuiKey key, float v, float v0, float v1) {
        float t = (v - v0) / (v1 - v0);
        t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
        io.AddKeyAnalogEvent(key, t > 0.10f, t);
    };
    button(ImGuiKey_GamepadStart, XINPUT_GAMEPAD_START);
    button(ImGuiKey_GamepadBack, XINPUT_GAMEPAD_BACK);
    button(ImGuiKey_GamepadFaceLeft, XINPUT_GAMEPAD_X);
    button(ImGuiKey_GamepadFaceRight, XINPUT_GAMEPAD_B);
    button(ImGuiKey_GamepadFaceUp, XINPUT_GAMEPAD_Y);
    button(ImGuiKey_GamepadFaceDown, XINPUT_GAMEPAD_A);
    button(ImGuiKey_GamepadDpadLeft, XINPUT_GAMEPAD_DPAD_LEFT);
    button(ImGuiKey_GamepadDpadRight, XINPUT_GAMEPAD_DPAD_RIGHT);
    button(ImGuiKey_GamepadDpadUp, XINPUT_GAMEPAD_DPAD_UP);
    button(ImGuiKey_GamepadDpadDown, XINPUT_GAMEPAD_DPAD_DOWN);
    button(ImGuiKey_GamepadL1, XINPUT_GAMEPAD_LEFT_SHOULDER);
    button(ImGuiKey_GamepadR1, XINPUT_GAMEPAD_RIGHT_SHOULDER);
    button(ImGuiKey_GamepadL3, XINPUT_GAMEPAD_LEFT_THUMB);
    button(ImGuiKey_GamepadR3, XINPUT_GAMEPAD_RIGHT_THUMB);
    analog(ImGuiKey_GamepadL2, g.bLeftTrigger, XINPUT_GAMEPAD_TRIGGER_THRESHOLD, 255);
    analog(ImGuiKey_GamepadR2, g.bRightTrigger, XINPUT_GAMEPAD_TRIGGER_THRESHOLD, 255);
    analog(ImGuiKey_GamepadLStickLeft, g.sThumbLX, -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, -32768);
    analog(ImGuiKey_GamepadLStickRight, g.sThumbLX, +XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, +32767);
    analog(ImGuiKey_GamepadLStickUp, g.sThumbLY, +XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, +32767);
    analog(ImGuiKey_GamepadLStickDown, g.sThumbLY, -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, -32768);
    analog(ImGuiKey_GamepadRStickLeft, g.sThumbRX, -XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE, -32768);
    analog(ImGuiKey_GamepadRStickRight, g.sThumbRX, +XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE, +32767);
    analog(ImGuiKey_GamepadRStickUp, g.sThumbRY, +XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE, +32767);
    analog(ImGuiKey_GamepadRStickDown, g.sThumbRY, -XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE, -32768);
}

// Newly-pressed buttons this frame (any pad, or the shot run's pad script), for carousel navigation.
static WORD pad_pressed() {
    static WORD prev = 0;
    WORD cur = g_padFrame.wButtons;
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
            GameOverride& o = it->second;
            // An exe= naming the default exe is no override (aki-roadmap#263: closing Settings wrote one for every
            // installed game); dropped here, so the next save leaves it out.
            o.exePath = rom_rules::exe_override_from_field(o.exePath, g.exeRegistryPath.wstring());
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
            // A byte-swapped (.v64 / .n64) copy of the accepted dump is set up as .z64, as the ROM scan does (#274).
            std::vector<unsigned char> z64;
            if (rom_read_matching_z64(src, g.romSha1, &z64)) {
                std::error_code wec;
                std::filesystem::create_directories(g.romInstallPath.parent_path(), wec);
                std::filesystem::path tmp = g.romInstallPath;
                tmp += L".new";
                bool wrote = false;
                {
                    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                    f.write((const char*)z64.data(), (std::streamsize)z64.size());
                    f.close();
                    wrote = !f.fail() && std::filesystem::file_size(tmp, wec) == z64.size() && !wec;
                }
                if (!wrote) {
                    std::filesystem::remove(tmp, wec);
                    *errOut = L"Couldn't write " + g.romInstallPath.wstring() + L" (disk full?)";
                    return false;
                }
                std::filesystem::rename(tmp, g.romInstallPath, wec);
                if (wec) {
                    std::error_code rec;
                    std::filesystem::remove(tmp, rec);
                    *errOut = L"Couldn't replace " + g.romInstallPath.wstring() + L" (is the game running?)";
                    return false;
                }
                auto ov = settings().overrides.find(g.artKey);
                if (ov != settings().overrides.end() && !ov->second.romPath.empty()) {
                    ov->second.romPath.clear();
                    settings_save();
                }
                logf(L"ROM set for %s from %s (%s, stored as .z64)", g.title.c_str(), src.c_str(),
                     rom_is_zip(src) ? L"zipped" : L"byte-swapped");
                return true;
            }
            *errOut = L"That file isn't the accepted " + g.title +
                      L" ROM. This port needs exactly one dump: the " + g.romNote +
                      L" (.z64, or the same dump byte-swapped as .v64 / .n64). Patched or translated"
                      L" dumps won't match: those go in through Import.\n"
                      L"Required SHA1: " + g.romSha1 + L"\n"
                      L"Your file's SHA1: " + h;
            if (rom_is_zip(src)) {  // the zip's own SHA1 says nothing: name the ROM inside (aki-roadmap#280)
                std::wstring in = rom_z64_sha1(src);
                *errOut += in.empty() ? L"\nThat zip doesn't hold exactly one N64 ROM: extract it and select the ROM itself."
                                      : L"\nThe ROM inside the zip: SHA1 " + in;
            }
            if (!g.romHint.empty()) *errOut += L"\n" + g.romHint;
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::create_directories(g.romInstallPath.parent_path(), ec);
    ec.clear();
    if (g.romSha1.empty() && rom_is_zip(src)) {
        // No SHA1 to check (a custom game): the ROM inside the zip, never the zip itself (aki-roadmap#280).
        std::vector<unsigned char> z64;
        if (!rom_read_any_z64(src, &z64)) {
            *errOut = L"That zip doesn't hold exactly one N64 ROM: extract it and select the ROM itself.";
            return false;
        }
        std::ofstream f(g.romInstallPath, std::ios::binary | std::ios::trunc);
        f.write((const char*)z64.data(), (std::streamsize)z64.size());
        f.close();
        if (f.fail()) {
            *errOut = L"Couldn't write " + g.romInstallPath.wstring() + L" (disk full?)";
            return false;
        }
        logf(L"ROM set for %s from the zip %s (stored as .z64)", g.title.c_str(), src.c_str());
        return true;
    }
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
    // .v64 / .n64 copies of the dump are set up as .z64 too (aki-roadmap#274).
    // A zip holding the dump works too (aki-roadmap#280).
    COMDLG_FILTERSPEC types[] = {{L"N64 ROM (*.z64, *.v64, *.n64, *.zip)", L"*.z64;*.v64;*.n64;*.zip"}, {L"All files", L"*.*"}};
    dlg->SetFileTypes(2, types);
    std::wstring title = L"Select your " + g.title + L" ROM - " + g.romNote + L" (.z64 / .v64 / .n64)";
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
        GameOverride& o = settings().overrides[g.artKey];
        o.exePath = rom_rules::exe_override_from_field(widen(g_ui.exeEdit[i]), g.exeRegistryPath.wstring());
        o.romPath = rom_rules::rom_override_from_field(widen(g_ui.romEdit[i]), g.romAutoPath.wstring());
    }
    g_ui.convCache.clear();  // the card's Play-as warnings follow an exe= change at once (aki-roadmap#264)
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

// A game's mods folder as the Mods page uses it (the import sandbox and the screenshot tool have their own).
static std::filesystem::path mods_dir_for(const GameEntry& g) {
    return !g_importSandbox.empty() ? g_importSandbox / L"data" / g.artKey / L"mods"
           : g_shotModsDir.empty() ? game_mods_dir(g) : g_shotModsDir;
}

static void refresh_mods(const std::vector<GameEntry>& games) {
    release_mod_thumbs();
    const GameEntry& g = games[g_ui.modsGame];
    g_ui.modsDir = mods_dir_for(g);
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
    g_ui.convs = pkg::list_conversions(conversions_data_dir(g), narrow(port_rom_tag(g)), game_run_version(g), g.title);
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
    if (!g_importSandbox.empty()) return g_importSandbox / L"data" / g.artKey;
    return g_shotModsDir.empty() ? game_data_dir(g) : std::filesystem::path(g_shotModsDir.wstring() + L"-data");
}

// The game's conversions (cached per game; dropped when the Mods page closes or a game ends).
static const std::vector<pkg::ConversionInfo>& game_conversions(const std::vector<GameEntry>& games, int i) {
    auto it = g_ui.convCache.find(games[i].artKey);
    if (it == g_ui.convCache.end()) {
        std::vector<pkg::ConversionInfo> v;
        if (g_shot ? !g_shotModsDir.empty() : games[i].exeFound) v = pkg::list_conversions(conversions_data_dir(games[i]), narrow(port_rom_tag(games[i])),
                                                                                         game_run_version(games[i]), games[i].title);
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
    t.dataDir = conversions_data_dir(g);
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

// A package (or a patch-mod ROM hack, which becomes a conversion). ask: Yes/No for a reinstall or a downgrade (a
// batch doesn't ask: the same version stays, and says so).
static pkg::PackageInstall install_one_package(const GameEntry& g, const std::filesystem::path& src, bool patchMod,
                                               bool ask = true) {
    // aki-roadmap#276: an import for a game that hasn't started yet (Starrcade from another game's Mods page) carries it
    // to portable first, so the package goes into games\<id>\, not %LOCALAPPDATA%.
    if (carry_before_install(g, launcher_portable(), settings().portableGames, !g_importSandbox.empty()))
        carry_portable(g);
    pkg::InstallTarget t = install_target(g);
    pkg::InstallOptions o;
    o.gameRunning = g_session.active() && g_session.artKey == g.artKey;
    auto run = [&] { return patchMod ? pkg::install_patch_mod(src, t, o) : pkg::install_package(src, t, o); };
    pkg::PackageInstall r = run();
    using O = pkg::PackageInstall::Outcome;
    if (ask && (r.outcome == O::AlreadyInstalled || r.outcome == O::NeedsDowngradeConfirm)) {
        bool down = r.outcome == O::NeedsDowngradeConfirm;
        std::wstring q = r.message + (down ? L".\n\nInstall the older version anyway? Files you changed are kept as .edited copies."
                                           : L".\n\nInstall it again? Files you changed are kept as .edited copies.");
        if (MessageBoxW(g_hwnd, q.c_str(), L"AKI Launcher", MB_YESNO | MB_ICONQUESTION) == IDYES) {
            (down ? o.allowDowngrade : o.reinstall) = true;
            r = run();
        }
    }
    for (const auto& n : r.notes) r.message += L"  (" + n + L")";
    return r;
}

static void refresh_import_count();

static const pkg::ConversionInfo* play_as(const std::vector<GameEntry>& games, int i);

// focusConversions (aki-roadmap#232): the controller starts on the Conversions section, on the Play as... pick.
static void open_mods(const std::vector<GameEntry>& games, int index, bool focusConversions = false) {
    carry_portable(games[index]);
    g_ui.modsGame = index;
    g_ui.modsNote.clear();
    g_ui.modsResults.clear();
    g_ui.packsNote.clear();
    g_ui.modsRemove = -1;
    refresh_mods(games);
    refresh_import_count();
    g_ui.showMods = true;
    g_ui.convFocusFrames = 0;
    if (focusConversions) {
        std::vector<std::string> ids;
        for (const auto& c : g_ui.convs) ids.push_back(c.id);
        const pkg::ConversionInfo* pick = play_as(games, index);
        g_ui.convFocus = pick ? convnav::conversions_focus(ids, pick->id) : -1;
        g_ui.convFocusFrames = 3;
    }
}

static void close_mods() {
    g_ui.showImport = false;
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

// One picked or dropped item, installed by the launcher itself when it can (aki-roadmap#228): a ROM is the stock
// game, another region, another game, or a hack for the converter (viaWorker); everything else goes through the
// installers as before (packages, patch-mod hacks, aki.json conversions, mods, texture packs, Rice/Jabo packs, model
// packs). What none of them recognises is for the converter too.
static ImportLine install_local(const std::vector<GameEntry>& games, const std::filesystem::path& src, bool ask,
                                bool* anyConversion) {
    const GameEntry& g = games[g_ui.modsGame];
    ImportLine l;
    l.item = src.wstring();
    l.source = src;
    l.name = src.filename().wstring();
    l.gameKey = engine_game_key(g.modGameId);
    std::error_code ec;
    // A zip goes on to the installers and the converter as before (packages, hacks in zips), not as a ROM (#280).
    if (!std::filesystem::is_directory(src, ec) && !rom_is_zip(src)) {
        RomScanEntry e = rom_classify(src, games);  // 64 bytes for a non-ROM; hashes only ROMs of the six games
        if (e.header.order != RomByteOrder::NotARom) {
            l.kind = L"ROM";
            int gi = e.gameIndex >= 0 ? e.gameIndex : e.otherRegionGame;
            l.gameKey = gi >= 0 ? engine_game_key(games[gi].modGameId) : L"";
            switch (import_rom_route(e.verdict == RomVerdict::BaseDump, e.verdict == RomVerdict::OtherBuild,
                                     e.otherRegionGame >= 0, e.verdict == RomVerdict::Unreadable)) {
            case ImportRomRoute::Stock:
                l.outcome = ImportOutcome::Stock;
                l.detail = L"This is the stock game (" + games[gi].title +
                           L"): nothing to install. Set it as the game's ROM on its card if it needs one.";
                break;
            case ImportRomRoute::Unsupported:
                l.outcome = ImportOutcome::Unsupported;
                l.detail = e.note;
                break;
            case ImportRomRoute::Failed:
                l.outcome = ImportOutcome::Failed;
                l.detail = e.note;
                break;
            case ImportRomRoute::Converter:
                // A hack or other revision of one of the six, or an unknown cartridge code the converter may know (a
                // catalog hack with its own code, VPW2 Freem; a renamed No Mercy hack): it decides (aki-roadmap#277).
                l.viaWorker = true;
                l.detail = e.note;
                break;
            }
            return l;
        }
    }
    std::wstring ext = src.extension().wstring();
    bool hack = _wcsicmp(ext.c_str(), L".nrm") == 0 && pkg::is_patch_mod(src);
    bool package = !hack && is_package_source(src);
    if (hack || package) {
        pkg::PackageInstall r = install_one_package(g, src, hack, ask);
        l.kind = hack || r.kind == "conversion" ? L"conversion" : r.kind == "texture-pack" ? L"texture pack" : L"package";
        if (!r.name.empty()) l.name = r.name;
        if (r.ok()) {
            l.outcome = ImportOutcome::Installed;
            l.installed.push_back({g.artKey, r.id, {}});
            if (l.kind == L"conversion") *anyConversion = true;
        } else {
            l.outcome = r.outcome == pkg::PackageInstall::Outcome::AlreadyInstalled ? ImportOutcome::HaveIt : ImportOutcome::Failed;
        }
        l.detail = r.message;
    } else if (is_conversion_source(src)) {
        // A conversion or texture package with only an aki.json (aki-roadmap#57): its own checks and long-path
        // extraction, into the game the aki.json names.
        AddResult r;
        if (g_session.active()) r.message = L"Close the game first, then add " + src.filename().wstring() + L".";
        else r = add_conversion(games, src, g_ui.modsGame);
        l.kind = L"conversion";
        l.outcome = r.ok ? ImportOutcome::Installed : ImportOutcome::Failed;
        if (r.ok) *anyConversion = true;
        l.detail = r.message;
        for (wchar_t& ch : l.detail) if (ch == L'\n') ch = L' ';
    } else {
        InstallResult r = install_mod(src, g_ui.modsDir, g.modGameId, g.title, g.modelProfile);
        const GameEntry* into = &g;
        if (!r.ok && !r.otherGameId.empty()) {
            // A Rice texture pack named for another of the games: an import is one way in for every game, so it goes
            // to that game's mods folder, like the hacks of a mixed batch (aki-roadmap#279).
            int og = game_index_for_mod_id(games, r.otherGameId);
            if (og >= 0 && games[og].exeFound) {
                into = &games[og];
                r = install_mod(src, mods_dir_for(*into), into->modGameId, into->title, into->modelProfile);
                l.gameKey = engine_game_key(into->modGameId);
                if (r.ok) r.message += L" (installed for " + into->title + L")";  // not the page it was imported from
            } else if (og >= 0) {
                l.gameKey = engine_game_key(games[og].modGameId);
                r.message = l.name + L" is a texture pack for " + games[og].title + L", which isn't installed: install " +
                            games[og].title + L", then import the pack again";
            }
        }
        l.detail = r.message;
        if (r.ok) {
            l.outcome = ImportOutcome::Installed;
            l.kind = L"mod";
            for (const auto& p : r.installed) {
                l.installed.push_back({into->artKey, {}, p});
                if (inspect_mod(p).kind_text() == L"Texture pack") l.kind = L"texture pack";
            }
        } else if (r.already) {
            l.outcome = ImportOutcome::HaveIt;
            l.kind = L"texture pack";
        } else if (r.unrecognized) {
            l.viaWorker = true;  // a patch, bundle, 7z, an archive of hacks...: the converter knows more
        } else {
            l.outcome = ImportOutcome::Failed;
        }
    }
    return l;
}

// --- Bulk import (aki-roadmap#228, phase 2) ----------------------------------------------------------------

// The launcher's data folder (settings.ini's folder: AppData, or beside the launcher when portable).
static std::filesystem::path launcher_data_dir() {
    return g_importSandbox.empty() ? settings_path().parent_path() : g_importSandbox;
}
static std::filesystem::path import_dir() { return launcher_data_dir() / L"Import"; }
static std::filesystem::path converted_dir() { return launcher_data_dir() / L"Converted"; }

// The converter (AkiMods tools/romhack): AKI_CONVERTER_TOOLS, else tools\romhack next to the launcher (0.3.0 ships it
// there), else the depot's AkiMods checkout (developers).
static std::filesystem::path converter_tools_dir() {
    wchar_t env[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"AKI_CONVERTER_TOOLS", env, MAX_PATH)) return env;
    std::error_code ec;
    std::filesystem::path beside = app_root() / L"tools" / L"romhack";
    if (std::filesystem::is_regular_file(beside / L"aki_import.py", ec)) return beside;
    std::filesystem::path depot = detect_depot_root();
    if (!depot.empty() && std::filesystem::is_regular_file(depot / L"AkiMods" / L"tools" / L"romhack" / L"aki_import.py", ec))
        return depot / L"AkiMods" / L"tools" / L"romhack";
    return beside;
}

static int game_by_engine_key(const std::vector<GameEntry>& games, const std::wstring& key) {
    for (int i = 0; i < (int)games.size(); ++i)
        if (!key.empty() && engine_game_key(games[i].modGameId) == key) return i;
    return -1;
}

static void refresh_import_count() {
    g_ui.importNew = (int)import_folder_items(import_dir()).size();
}

// A Converted line's package zip(s) installed through the #154 installer into the game it was built for.
static void install_converted(const std::vector<GameEntry>& games, ImportLine& l) {
    int gi = game_by_engine_key(games, l.gameKey);
    if (gi < 0) {
        l.outcome = ImportOutcome::Failed;
        l.detail = L"built for a game this launcher doesn't know (" + l.gameKey + L")";
        return;
    }
    const GameEntry& g = games[gi];
    std::wstring problems;
    bool have = false;
    for (const auto& pk : l.packages) {
        pkg::PackageInstall r = install_one_package(g, pk, false, false);
        if (r.ok()) l.installed.push_back({g.artKey, r.id, {}});
        else if (r.outcome == pkg::PackageInstall::Outcome::AlreadyInstalled) have = true;
        else problems += (problems.empty() ? L"" : L"; ") + r.message;
    }
    if (!problems.empty()) {
        l.outcome = ImportOutcome::Failed;
        l.detail = L"built (kept in Converted), but installing it failed: " + problems;
        l.log = problems + L"\n\n" + l.log;
    } else if (l.installed.empty() && have) {
        l.outcome = ImportOutcome::HaveIt;
        l.detail = L"built the same version you have installed already";
    } else {
        l.detail += L"; installed for " + g.title;
    }
}

static bool uninstall_imported(const std::vector<GameEntry>& games, const ImportLine::Installed& in, std::wstring* err) {
    for (const auto& g : games) {
        if (g.artKey != in.gameArtKey) continue;
        if (!in.packageId.empty()) {
            bool running = g_session.active() && g_session.artKey == g.artKey;
            return pkg::remove_package(in.packageId, conversions_data_dir(g), running, err);
        }
        return recycle_path(in.path, err);
    }
    if (err) *err = L"no game " + in.gameArtKey;
    return false;
}

static ImportEnv make_import_env(const std::vector<GameEntry>& games);
static void finish_start_import(const std::vector<GameEntry>& games, bool fromImportFolder);

// Every import starts here: picked files, a dropped mix, or the Import folder. Never while a game runs.
static void start_import(const std::vector<GameEntry>& games, const std::vector<std::filesystem::path>& srcs,
                         bool fromImportFolder) {
    if (srcs.empty()) return;
    if (g_session.active()) {
        mods_note(L"Close the game first: imports never run while a game is running.", false);
        return;
    }
    if (g_import.busy()) {
        mods_note(L"An import is still running: wait for it (or Stop it) first.", false);
        g_ui.showImport = true;
        return;
    }
    g_ui.modsResults.clear();
    ImportEnv env = make_import_env(games);
    logf(L"import: %zu item(s)%s", srcs.size(), fromImportFolder ? L" from the Import folder" : L"");
    g_import.start(env, srcs, fromImportFolder);
    finish_start_import(games, fromImportFolder);
}

static ImportEnv make_import_env(const std::vector<GameEntry>& games) {
    ImportEnv env;
    env.importDir = import_dir();
    env.convertedDir = converted_dir();
    env.toolsDir = converter_tools_dir();
    env.logDir = converted_dir() / L".logs";
    wchar_t py[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"AKI_PYTHON", py, MAX_PATH)) env.python = py;  // developers: skip pyfetch
    env.job.game = engine_game_key(games[g_ui.modsGame].modGameId);
    for (const auto& g : games) {
        std::wstring key = engine_game_key(g.modGameId);
        if (!key.empty() && g.romFound && !g.romWrong) env.job.roms[key] = g.romPath;  // conversions are built against it
    }
    env.job.romDirs = g_importRomDirs;
    env.installLocal = [&games](const std::filesystem::path& p) {
        bool anyConversion = false;
        return install_local(games, p, false, &anyConversion);
    };
    env.installPackages = [&games](ImportLine& l) { install_converted(games, l); };
    env.uninstall = [&games](const ImportLine::Installed& in, std::wstring* err) { return uninstall_imported(games, in, err); };
    return env;
}

static void finish_start_import(const std::vector<GameEntry>& games, bool fromImportFolder) {
    // One item the launcher installed itself: the Mods page's note, as before. Anything else: the results screen.
    if (!g_import.busy() && g_import.lines.size() == 1 && !g_import.lines[0].viaWorker && !fromImportFolder) {
        const ImportLine& l = g_import.lines[0];
        bool ok = l.outcome == ImportOutcome::Installed;
        mods_note(l.detail + (!ok ? L"" : l.kind == L"conversion" ? L" Pick it with Play as... on the game's card."
                                                                 : L". Start (or restart) the game to use it; turn mods on and off in its Mods menu."),
                  ok || l.outcome == ImportOutcome::Stock);
        if (ok) play_flip();
    } else {
        g_ui.showImport = true;
    }
    refresh_mods(games);
    refresh_import_count();
}

static void install_into_mods(const std::vector<GameEntry>& games, const std::vector<std::filesystem::path>& srcs) {
    start_import(games, srcs, false);
}

// Multi-select picker for mod files; empty on cancel.
static std::vector<std::filesystem::path> pick_mod_files() {
    std::vector<std::filesystem::path> out;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return out;
    COMDLG_FILTERSPEC types[] = {{L"Anything to import", L"*.*"},
                                 {L"Mods and texture packs (*.nrm, *.rtz, *.zip)", L"*.nrm;*.rtz;*.zip"}};
    dlg->SetFileTypes(2, types);
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Import mods, texture packs, ROM hacks or patches");
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

static std::wstring game_title_for_key(const std::vector<GameEntry>& games, const std::wstring& key) {
    int gi = game_by_engine_key(games, key);
    return gi >= 0 ? games[gi].title : key;
}

static ImVec4 outcome_color(ImportOutcome o) {
    switch (o) {
    case ImportOutcome::Converted:
    case ImportOutcome::Installed: return palette::ready;
    case ImportOutcome::Failed: return palette::error;
    case ImportOutcome::HaveIt:
    case ImportOutcome::Stock:
    case ImportOutcome::Waiting:
    case ImportOutcome::Stopped: return palette::dim;
    case ImportOutcome::Working: return palette::accent;
    default: return palette::warn;
    }
}

// The Import results screen (aki-roadmap#228, design 2.2): one line per item. The game index to launch, else -1.
static int draw_import(std::vector<GameEntry>& games, WORD pad) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float vw = vp->WorkSize.x, vh = vp->WorkSize.y;
    ImGuiIO& io = ImGui::GetIO();
    if ((ImGui::IsKeyPressed(ImGuiKey_Escape) || (pad & XINPUT_GAMEPAD_B)) && !io.WantTextInput && !g_ui.importLogOpen &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        g_ui.showImport = false;  // a running batch keeps going (the main loop ticks it)
        return -1;
    }
    ImGui::PushFont(g_ui.fontHeader);
    ImGui::SetCursorPosY(vh * 0.055f);
    text_centered("I M P O R T");
    ImGui::PopFont();
    ImportCounts c = count_import(g_import.lines);
    ImGui::PushFont(g_ui.fontBase);
    text_centered_colored(palette::dim, g_import.busy() ? "Importing " + std::to_string(c.total) + " item(s)   [ " +
                                                              narrow(import_progress(c)) + " ]"
                                                        : std::to_string(c.total) + " item(s)");
    ImGui::PopFont();

    float panelW = page_column(vw, vh, 0.86f, 1.75f);
    ImGui::SetCursorPos(ImVec2((vw - panelW) * 0.5f, vh * 0.15f));
    ImGui::BeginChild("##import", ImVec2(panelW, vh * 0.74f), ImGuiChildFlags_NavFlattened);
    ImGui::PushFont(g_ui.fontBase);
    const bool appearing = ImGui::IsWindowAppearing();
    if (g_import.busy()) {
        if (ImGui::Button("Stop")) g_import.stop();
        if (appearing) ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
    } else {
        bool anyInstalled = false;
        for (const auto& l : g_import.lines) anyInstalled = anyInstalled || !l.installed.empty() || !l.packages.empty();
        ImGui::BeginDisabled(!anyInstalled || g_import.undone() || g_session.active());
        if (ImGui::Button("Undo this import")) {
            std::wstring msg;
            g_import.undo(&msg);
            logf(L"import: %s", msg.c_str());
            refresh_mods(games);
            refresh_import_count();
            g_ui.convCache.clear();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
    }
    if (ImGui::Button("Back to Mods")) g_ui.showImport = false;
    if (appearing && !g_import.busy()) ImGui::SetItemDefaultFocus();
    ImGui::SameLine();
    if (ImGui::Button("Open Converted folder")) {
        std::error_code ec;
        std::filesystem::create_directories(converted_dir(), ec);
        ShellExecuteW(nullptr, L"explore", converted_dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::PushFont(g_ui.fontSmall);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(g_import.busy() ? palette::accent : (c.failed ? palette::warn : palette::ready), "%s",
                       narrow(g_import.status).c_str());
    if (c.untested)
        ImGui::TextColored(palette::warn, "Untested: built on this PC by the converter, not tested by the AKI team. It may crash or "
                                          "look wrong; your other saves are safe.");
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    ImGui::Separator();

    int launch = -1;
    std::filesystem::path findRomFor;
    int removeLine = -1;
    ImGui::PushFont(g_ui.fontSmall);
    ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX;
    if (ImGui::BeginTable("##importlines", 6, tf)) {
        ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Unsupported").x + ui_px(12));
        ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch, 2.2f);
        ImGui::TableSetupColumn("Game", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthStretch, 0.9f);
        ImGui::TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthStretch, 3.4f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Find it...  Remove  Log").x + ui_px(24));
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)g_import.lines.size(); ++i) {
            const ImportLine& l = g_import.lines[i];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            // A row-wide selectable: the D-pad visits every line (also those without buttons) and scrolls the list.
            ImGui::PushStyleColor(ImGuiCol_Text, outcome_color(l.outcome));
            bool rowPicked = ImGui::Selectable(narrow(import_outcome_word(l.outcome)).c_str(), false,
                                               ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap);
            ImGui::PopStyleColor();
            if (rowPicked && (!l.log.empty() || !l.detail.empty())) {
                g_ui.importLog = narrow(l.name + L"\n" + l.detail + (l.log.empty() ? L"" : L"\n\n" + l.log));
                g_ui.importLogOpen = true;
            }
            ImGui::TableNextColumn();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(narrow(l.name).c_str());
            ImGui::PopTextWrapPos();
            ImGui::TableNextColumn();
            ImGui::TextColored(palette::dim, "%s", l.gameKey.empty() ? "-" : narrow(game_title_for_key(games, l.gameKey)).c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(palette::dim, "%s", l.kind.empty() ? "-" : narrow(l.kind).c_str());
            if (!l.tier.empty()) ImGui::TextColored(l.untested ? palette::warn : palette::ready, "%s%s", narrow(l.tier).c_str(),
                                                    l.untested ? " (!)" : "");
            ImGui::TableNextColumn();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(palette::faint, "%s", narrow(l.detail).c_str());
            ImGui::PopTextWrapPos();
            ImGui::TableNextColumn();
            if (l.outcome == ImportOutcome::NeedsRom && !g_import.busy()) {
                if (ImGui::SmallButton("Find it...")) findRomFor = l.source;
                ImGui::SameLine();
            }
            if (!l.installed.empty() && !g_import.busy()) {
                const ImportLine::Installed& in = l.installed[0];
                if (l.kind == L"conversion" && !in.packageId.empty()) {
                    for (int gi = 0; gi < (int)games.size(); ++gi)
                        if (games[gi].artKey == in.gameArtKey && games[gi].launchable()) {
                            if (ImGui::SmallButton("Play")) {
                                set_play_as(games[gi], in.packageId);
                                launch = gi;
                            }
                            ImGui::SameLine();
                        }
                }
                if (ImGui::SmallButton("Remove")) removeLine = i;
                ImGui::SameLine();
            }
            if (!l.log.empty() || (l.outcome == ImportOutcome::Failed && !g_import.workerLog.empty())) {
                if (ImGui::SmallButton("Log")) {
                    g_ui.importLog = narrow(l.log.empty() ? g_import.workerLog : l.log);
                    g_ui.importLogOpen = true;
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopFont();
    if (g_ui.importLogOpen) ImGui::OpenPopup("Import log");
    ImGui::SetNextWindowSize(ImVec2(panelW * 0.8f, vh * 0.6f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Import log", &g_ui.importLogOpen)) {
        if ((pad & XINPUT_GAMEPAD_B) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            g_ui.importLogOpen = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::PushFont(g_ui.fontSmall);
        ImGui::InputTextMultiline("##log", &g_ui.importLog, ImVec2(-1, -ImGui::GetFrameHeightWithSpacing() * 1.4f),
                                  ImGuiInputTextFlags_ReadOnly);
        ImGui::PopFont();
        if (ImGui::Button("Copy")) ImGui::SetClipboardText(g_ui.importLog.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Close")) {
            g_ui.importLogOpen = false;
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsWindowAppearing()) ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }
    ImGui::PopFont();
    ImGui::EndChild();
    if (removeLine >= 0) {
        ImportLine& l = g_import.lines[removeLine];
        std::wstring problems;
        for (const auto& in : l.installed) {
            std::wstring err;
            if (!uninstall_imported(games, in, &err)) problems += err;
        }
        l.installed.clear();
        l.detail = problems.empty() ? L"removed (saves stay; the converted copy stays in Converted)" : L"not removed: " + problems;
        refresh_mods(games);
        g_ui.convCache.clear();
    }
    if (!findRomFor.empty()) {
        std::vector<std::filesystem::path> dirs;
        if (g_ui.padInput) {
            // The ROM scan's folder, every game's ROM folder, the folders picked before.
            auto add = [&](const std::filesystem::path& d) {
                std::error_code ec;
                if (!d.empty() && std::filesystem::is_directory(d, ec) && std::find(dirs.begin(), dirs.end(), d) == dirs.end())
                    dirs.push_back(d);
            };
            add(settings().romScanDir);
            for (const auto& g : games)
                if (g.romFound) add(g.romPath.parent_path());
            for (const auto& d : g_importRomDirs) add(d);
        } else {
            std::filesystem::path dir = pick_folder(settings().romScanDir);
            if (!dir.empty()) dirs.push_back(dir);
        }
        for (const auto& d : dirs)
            if (std::find(g_importRomDirs.begin(), g_importRomDirs.end(), d) == g_importRomDirs.end()) g_importRomDirs.push_back(d);
        std::wstring err;
        if (!dirs.empty() && !g_import.retry_needs_rom(dirs, &err)) logf(L"import: Find it...: %s", err.c_str());
    }
    if (launch >= 0) {
        g_ui.showImport = false;
        close_mods();
    }
    ImGui::PushFont(g_ui.fontSmall);
    ImGui::SetCursorPosY(vh * 0.92f);
    text_centered_colored(palette::faint, g_ui.padInput ? "A Select (a line opens its details)      B Back to Mods      "
                                                          "Find it... looks in your ROM folders"
                                                        : "Esc Back to Mods      Your originals are never changed");
    ImGui::PopFont();
    return launch;
}

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
    ImGui::BeginChild("##mods", ImVec2(panelW, vh * 0.74f), ImGuiChildFlags_NavFlattened);
    ImGui::PushFont(g_ui.fontBase);
    // aki-roadmap#228: one way in for everything (mods, texture packs, packages, ROM hacks, patches, Project64 bundles).
    bool gameRuns = g_session.active();
    ImGui::BeginDisabled(gameRuns || g_import.busy());
    // A file dialog is a dead end for a controller (aki-roadmap#232): with a pad these two are off, and the Import
    // folder (Convert my Import folder) is the way in.
    ImGui::BeginDisabled(g_ui.padInput);
    if (ImGui::Button("Import...")) {
        auto files = pick_mod_files();
        if (!files.empty()) start_import(games, files, false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Import a folder...")) {
        std::filesystem::path dir = pick_folder(L"");
        if (!dir.empty()) start_import(games, {dir}, false);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    std::string convLabel = "Convert my Import folder (" + std::to_string(g_ui.importNew) + " new)";
    ImGui::BeginDisabled(g_ui.importNew == 0);
    if (ImGui::Button(convLabel.c_str())) start_import(games, import_folder_items(import_dir()), true);
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Open Import folder")) {
        std::error_code ec;
        std::filesystem::create_directories(import_dir(), ec);
        ShellExecuteW(nullptr, L"explore", import_dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    if (!g_import.lines.empty()) {
        ImGui::SameLine();
        if (ImGui::Button(g_import.busy() ? "Import running..." : "Last import")) g_ui.showImport = true;
    }
    if (ImGui::Button("Open mods folder")) {
        std::error_code ec;
        std::filesystem::create_directories(g_ui.modsDir, ec);
        ShellExecuteW(nullptr, L"explore", g_ui.modsDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        refresh_mods(games);
        refresh_import_count();
    }
    ImGui::PushFont(g_ui.fontSmall);
    if (gameRuns) ImGui::TextColored(palette::warn, "Import is off while a game runs: close the game first.");
    else if (g_ui.padInput)
        ImGui::TextColored(palette::faint, "With a controller: put files in the Import folder, then Convert my Import folder.");
    ImGui::TextColored(palette::faint, "%s   -   or drop any mix of mods, texture packs, ROM hacks, patches and folders on this window",
                       narrow(g_ui.modsDir.wstring()).c_str());
    if (!g_ui.modsNote.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(g_ui.modsNoteOk ? palette::ready : palette::error, "%s", g_ui.modsNote.c_str());
        ImGui::PopTextWrapPos();
    }
    // aki-roadmap#228 phase 0: a multi-item install lists every item on its own line.
    if (!g_ui.modsResults.empty()) {
        float wordW = ImGui::CalcTextSize("Unsupported  ").x;
        for (const auto& l : g_ui.modsResults) {
            ImVec4 col = l.outcome == ImportOutcome::Installed || l.outcome == ImportOutcome::Converted ? palette::ready
                         : l.outcome == ImportOutcome::Failed ? palette::error
                         : l.outcome == ImportOutcome::HaveIt || l.outcome == ImportOutcome::Stock ? palette::dim
                                                                                                      : palette::warn;
            ImGui::TextColored(col, "%s", narrow(import_outcome_word(l.outcome)).c_str());
            ImGui::SameLine(wordW);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(palette::dim, "%s  -  %s", narrow(l.name).c_str(), narrow(l.detail).c_str());
            ImGui::PopTextWrapPos();
        }
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
    // aki-roadmap#232: every row is a stop for the D-pad (the original game first): A on a row picks it for Play as...,
    // Play starts it with the cart insert; its tier, Untested label and known issues are on the row.
    {
        int playConv = -1, removeConv = -1;
        ImGui::Spacing();
        ImGui::TextColored(palette::accent, "Conversions");
        ImGui::PushFont(g_ui.fontSmall);
        if (g_ui.convs.empty())
            ImGui::TextColored(palette::dim, "None installed. A conversion package, or a ROM-hack .nrm, installs here through Import...");
        const pkg::ConversionInfo* pick = play_as(games, g_ui.modsGame);
        bool focusNow = g_ui.convFocusFrames > 0;
        if (focusNow) --g_ui.convFocusFrames;
        auto land = [&](int row) {  // the controller starts here (opened from the card's Play as... line)
            if (!focusNow || row != g_ui.convFocus) return;
            ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
            ImGui::SetNavCursorVisible(true);
            ImGui::SetScrollHereY(0.25f);
        };
        if (!g_ui.convs.empty()) {
            ImGui::PushFont(g_ui.fontBase);
            if (ImGui::Selectable(pick ? "The original game" : "The original game   (Play as)", !pick, ImGuiSelectableFlags_AllowOverlap)) {
                set_play_as(g, std::string());
                mods_note(L"Play on the card starts the original " + g.title, true);
            }
            land(-1);
            ImGui::PopFont();
            ImGui::Spacing();
        }
        if (!g.launchable() && !g_ui.convs.empty())
            ImGui::TextColored(palette::warn, "%s isn't ready to play yet: B goes back to its card, which says what it needs.",
                               narrow(g.title).c_str());
        std::vector<std::string> rom = rom_sha1_of(g);
        for (int i = 0; i < (int)g_ui.convs.size(); ++i) {
            const pkg::ConversionInfo& c = g_ui.convs[i];
            bool picked = pick && pick->id == c.id;
            ImGui::PushID(10000 + i);
            ImGui::PushFont(g_ui.fontBase);
            std::string head = narrow(c.name) + (c.version.empty() ? "" : "  " + narrow(c.version)) + (picked ? "   (Play as)" : "");
            if (ImGui::Selectable(head.c_str(), picked, ImGuiSelectableFlags_AllowOverlap)) {
                set_play_as(g, c.id);
                mods_note(L"Picked " + c.name + L": Play here, or Play on the card, starts it", true);
            }
            land(i);
            ImGui::PopFont();
            ImGui::PushTextWrapPos(panelW - ui_px(20));
            convnav::TierBadge badge = convnav::tier_badge(c.tier, c.untested);
            if (c.untested)
                ImGui::TextColored(palette::warn, "Untested: built on this PC by the converter, not tested by the AKI team. It may crash "
                                                  "or look wrong; your other saves are safe.");
            else if (!badge.text.empty())
                ImGui::TextColored(badge.warn ? palette::warn : palette::ready, "%s", narrow(badge.text).c_str());
            if (!c.description.empty()) ImGui::TextColored(palette::faint, "%s", narrow(c.description).c_str());
            if (!c.knownIssues.empty()) {
                ImGui::TextColored(palette::dim, "Known issues:");
                for (const auto& k : c.knownIssues) ImGui::TextColored(palette::dim, "  -  %s", narrow(k).c_str());
            }
            if (!c.note.empty()) ImGui::TextColored(palette::faint, "%s", narrow(c.note).c_str());
            std::string line = c.authors.empty() ? std::string() : "by " + narrow(c.authors) + "  -  ";
            line += c.recorded ? "installed as a package" : "added by hand";
            line += "  -  saves in saves\\" + c.id + "\\";
            for (const auto& a : c.addons) line += "  -  with " + a;
            ImGui::TextColored(palette::dim, "%s", line.c_str());
            ImGui::PopTextWrapPos();
            if (!c.models.empty()) {
                ImGui::PushTextWrapPos(panelW - ui_px(20));
                ImGui::TextColored(c.modelsOff ? palette::warn : palette::dim, "%s", narrow(c.models).c_str());
                ImGui::PopTextWrapPos();
            }
            // aki-roadmap#243: missing texture package, ROM patch left out, codes the game won't run.
            if (!c.notes.empty()) {
                ImGui::PushTextWrapPos(panelW - ui_px(20));
                for (const auto& n : c.notes) ImGui::TextColored(palette::warn, "%s", narrow(n).c_str());
                ImGui::PopTextWrapPos();
            }
            bool romOk = pkg::conversion_rom_matches(c, rom);
            ImGui::TextColored(romOk ? palette::ready : palette::warn, "%s", romOk ? "Your ROM matches" : "Made for another ROM than yours");
            bool first = true;
            auto gap = [&]() {
                if (!first) ImGui::SameLine();
                first = false;
            };
            if (g.launchable()) {
                gap();
                if (ImGui::SmallButton("Play")) playConv = i;
            }
            std::error_code ec;
            if (std::filesystem::is_directory(c.saves, ec)) {
                gap();
                if (ImGui::SmallButton("Open saves folder")) ShellExecuteW(nullptr, L"explore", c.saves.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            if (c.recorded) {
                gap();
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
    text_centered_colored(palette::faint, g_ui.padInput ? "D-Pad Move      A Pick / Press      B Back      The game turns mods on and off in its own Mods menu"
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
    ImGui::BeginChild("##quick", ImVec2(panelW, vh * 0.70f), ImGuiChildFlags_NavFlattened);
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
        ImGui::BeginChild("##tour", ImVec2(panelW, vh * 0.70f), ImGuiChildFlags_NavFlattened);
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
    ImGui::BeginChild("##tour", ImVec2(panelW, vh * 0.72f), ImGuiChildFlags_NavFlattened);
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
            g_ui.exeEdit[i] = narrow(rom_rules::exe_field_reset(g.exeRegistryPath.wstring()));  // aki-roadmap#263
            g_ui.romEdit[i] = narrow(rom_rules::rom_field_reset(g.romAutoPath.wstring()));  // aki-roadmap#219
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

// Returns the index of a game to launch this frame, or -1.
// The Needs ROM prompt by controller (aki-roadmap#232): no file dialog, the game's accepted dump is looked for in the
// ROM scan's folder, the other games' ROM folders, the folders Find it... used, the Import folder and the usual
// Windows folders (Downloads, Desktop, Documents; 3 folders deep), and set up when found. False with *err naming
// where it looked.
static bool pad_find_rom(std::vector<GameEntry>& games, int index, std::wstring* err) {
    GameEntry& g = games[index];
    std::vector<std::filesystem::path> cands;
    cands.push_back(settings().romScanDir);
    for (const auto& o : games)
        if (o.romFound) cands.push_back(o.romPath.parent_path());
    for (const auto& d : g_importRomDirs) cands.push_back(d);
    cands.push_back(import_dir());
    wchar_t home[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH))
        for (const wchar_t* sub : {L"Downloads", L"Desktop", L"Documents"}) cands.push_back(std::filesystem::path(home) / sub);
    std::vector<std::filesystem::path> dirs = convnav::rom_search_dirs(cands, [](const std::filesystem::path& d) {
        std::error_code ec;
        return std::filesystem::is_directory(d, ec);
    });
    std::wstring where, provisionErr;
    for (const auto& d : dirs) {
        where += (where.empty() ? L"" : L", ") + d.wstring();
        for (const auto& f : rom_candidates(d, 2000).files) {
            RomScanEntry e = rom_classify(f, games);
            if (e.verdict != RomVerdict::BaseDump || e.gameIndex != index) continue;
            if (rom_provision(g, e, &provisionErr)) {
                logf(L"pad ROM search: %s set up from %s", g.title.c_str(), f.c_str());
                return true;
            }
            logf(L"pad ROM search: %s: %s", f.c_str(), provisionErr.c_str());
        }
    }
    *err = L"Couldn't find your " + g.title + L" ROM (" + g.romNote + L")" +
           (provisionErr.empty() ? L"" : L": " + provisionErr) + L".\nLooked in " + (where.empty() ? L"no folders" : where) +
           L".\nPut the .z64 in one of them and press A again, or pick it with the mouse.   B dismisses this.";
    return false;
}

// Every launch from the UI starts here (aki-roadmap#232): the home card's Play, a conversion's Play on the Conversions
// page and the Import screen's Play. The card comes back on that game and the cart-insert animation plays; the game
// starts when it ends (draw_ui returns the index then), so a conversion launch looks exactly like a stock one.
static void begin_launch(int index) {
    play_launch();
    g_ui.selected = index;
    g_ui.face = g_ui.facePrev = 0;
    g_ui.flipAnim = 1.0f;
    g_ui.insertIndex = index;
    g_ui.insertAnim = 0.0f;
}

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
    if (pad || g_padFrame.wButtons) g_ui.padInput = true;
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
        int launch = g_ui.showImport ? draw_import(games, pad) : draw_mods(games, pad);
        if (launch >= 0) begin_launch(launch);  // the pages closed themselves: the card plays the cart insert
        ImGui::End();
        return -1;
    }

    // ------------------------------------------------------------------ settings screen
    if (g_ui.showSettings) {
        draw_settings(games, pad);
        ImGui::End();
        return -1;
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
        } else if (back && !g_session.statusNote.empty()) {
            g_session.statusNote.clear();  // a launch error or Needs ROM note (aki-roadmap#232): B dismisses it too
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

    // The status note's top line (drawn over the footer, below): multi-line notes (the wrong-ROM error) grow upward.
    auto statusNoteY = [&]() {
        std::string note = narrow(g_session.statusNote);
        int lines = 1 + (int)std::count(note.begin(), note.end(), '\n');
        ImGui::PushFont(g_ui.fontSmall);
        float y = vh * 0.935f - lines * ImGui::GetTextLineHeightWithSpacing();
        ImGui::PopFont();
        return y < vh * 0.905f ? y : vh * 0.905f;
    };

    // aki-roadmap#240: the title, the info lines and the action button as one block (cardlayout::place). The button
    // goes under the lines, never over them (the update-available line was the one covered); a tall block (wrong ROM
    // + SHA1 lines + Play as... + update + dev) moves up clear of the status note and the footer, no higher than the
    // carousel's year captions. The lines' height is last frame's, per game: the block settles in one frame.
    static std::vector<cardlayout::Input> s_cardLast;
    if ((int)s_cardLast.size() != n) s_cardLast.assign(n, cardlayout::Input{});
    cardlayout::Input cardIn = s_cardLast[g_ui.selected];
    cardIn.infoY = infoY;
    cardIn.btnAnchor = vh * 0.115f;
    cardIn.gap = vh * 0.012f;
    ImGui::PushFont(g_ui.fontSmall);
    cardIn.ceilY = vh * 0.365f + baseW * selScale * aspect * 0.5f + 10 + ImGui::GetFontSize() + vh * 0.012f;
    ImGui::PopFont();
    cardIn.floorY = (g_session.statusNote.empty() ? vh * 0.945f : statusNoteY()) - vh * 0.012f;
    const cardlayout::Result card = cardlayout::place(cardIn);
    infoY = card.infoY;

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
    // aki-roadmap#232: all of it by controller: LB (C, Shift+C back) changes it, Down (V) opens the Conversions page
    // with its details (tier, known issues, Untested) on the picked one. Shown also while the game needs its ROM.
    bool lbPlayAs = false;
    if ((s.exeFound || !g_shotModsDir.empty()) && !installingSel) {
        const auto& convs = game_conversions(games, g_ui.selected);
        if (!convs.empty()) {
            lbPlayAs = true;
            const pkg::ConversionInfo* pick = play_as(games, g_ui.selected);
            bool keyC = ImGui::IsKeyPressed(ImGuiKey_C, false);
            if (!inserting && (keyC || (pad & XINPUT_GAMEPAD_LEFT_SHOULDER))) {
                int next = convnav::play_as_step(pick ? (int)(pick - convs.data()) : -1, (int)convs.size(),
                                                 keyC && ImGui::GetIO().KeyShift ? -1 : +1);
                set_play_as(s, next >= 0 ? convs[next].id : std::string());
                pick = play_as(games, g_ui.selected);
                play_nav();
            }
            if (!inserting && (ImGui::IsKeyPressed(ImGuiKey_V, false) || ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) ||
                               (pad & XINPUT_GAMEPAD_DPAD_DOWN))) {
                play_flip();
                open_mods(games, g_ui.selected, true);
                ImGui::End();
                return -1;
            }
            ImGui::PushFont(g_ui.fontSmall);
            std::string line = std::to_string(convs.size()) + (convs.size() == 1 ? " conversion" : " conversions") +
                               " installed   -   Play as:  " + (pick ? narrow(pick->name) : std::string("the original game"));
            convnav::TierBadge badge = pick ? convnav::tier_badge(pick->tier, pick->untested) : convnav::TierBadge{};
            if (!badge.text.empty()) line += "  [" + narrow(badge.text) + "]";
            text_centered_colored(badge.warn ? palette::warn : palette::accent, line);
            text_centered_colored(palette::dim, g_ui.padInput ? "LB Changes It      Down Details And Known Issues"
                                                              : "C Changes It (Shift+C Back)      V Or Down Details And Known Issues");
            if (pick && !pkg::conversion_rom_matches(*pick, rom_sha1_of(s)))
                text_centered_colored(palette::warn, narrow(pick->name) + " is made for another ROM than yours");
            if (pick && !pick->needsGame.empty())  // aki-roadmap#264: said before Play, not only after the cart goes in
                text_centered_colored(palette::warn, narrow(pick->name) + " needs " + narrow(s.title) + ": Recompiled " +
                                                         narrow(pick->needsGame) + " or newer: update the game, or play the original");
            ImGui::PopFont();
        }
    }

    // Update-available line (installed via the launcher, a strictly newer tag on GitHub: aki-roadmap#262)
    bool updateAvail = s.exeFound && !s.installedTag.empty() && haveRel &&
                       game_update_offered(s.installedTag, rel.tag) && !installingSel &&
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

    // This frame's lines and button, for next frame's placement.
    s_cardLast[g_ui.selected].stackH = ImGui::GetCursorPosY() - infoY;
    s_cardLast[g_ui.selected].btnH = action != CardAction::None ? vh * 0.048f : 0.0f;

    // Action button (mouse users; keyboard/pad use Enter/A)
    if (action != CardAction::None) {
        const char* label = action == CardAction::Play       ? "PLAY"
                            : action == CardAction::Download ? "DOWNLOAD"
                                                             : g_ui.padInput ? "FIND MY ROM" : "SELECT ROM...";
        ImGui::PushFont(g_ui.fontBase);
        ImVec2 bsz(vw * (action == CardAction::Play ? 0.10f : 0.13f), vh * 0.048f);
        // At the block's place (above), and in any case under this frame's lines: a line that appeared this
        // frame (a pick, the update check coming in) is never covered for the one frame before the block settles.
        float btnY = card.btnY;
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
                begin_launch(g_ui.selected);  // the launch happens when the cart-insert animation finishes
            } else if (action == CardAction::Download) {
                if (install_start(g_ui.selected, s, rel)) play_flip();
            } else {
                std::wstring err;
                // A controller can't drive the file dialog: it searches the known ROM folders instead.
                if (g_ui.padInput ? pad_find_rom(games, g_ui.selected, &err) : pick_rom(s, &err)) {
                    apply_overrides(games);
                    g_session.statusNote.clear();
                    play_flip();
                } else if (!err.empty()) {
                    g_session.statusNote = err;
                }
            }
        }
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
        // Multi-line notes (the wrong-ROM error) grow upward, clear of the footer.
        ImGui::SetCursorPosY(statusNoteY());
        ImGui::PushFont(g_ui.fontSmall);
        text_centered_colored(palette::error, narrow(g_session.statusNote));
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
            // The cartridge scan, else the front cover, else a drawn cartridge: the animation always plays.
            const BoxArt& ba = art[g_ui.insertIndex];
            convnav::InsertArt kind = convnav::insert_art(ba.cart.ok(), ba.front.ok());
            const Texture& cart = kind == convnav::InsertArt::Cover ? ba.front : ba.cart;
            float appear = fminf(t / 0.10f, 1.0f);  // quick fade/scale in
            float cw = vw * 0.16f * (0.92f + 0.08f * appear);
            float ch = kind == convnav::InsertArt::Drawn ? cw * 0.72f : cw * (float)cart.h / (float)cart.w;
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
            ImVec2 c0(cx - cw * 0.5f, cy - ch * 0.5f), c1(cx + cw * 0.5f, cy + ch * 0.5f);
            if (kind == convnav::InsertArt::Drawn) {
                int a = (int)(appear * 255.0f);
                fg->AddRectFilled(c0, c1, IM_COL32(58, 60, 70, a), cw * 0.04f);
                fg->AddRectFilled(ImVec2(c0.x + cw * 0.10f, c0.y + ch * 0.10f), ImVec2(c1.x - cw * 0.10f, c0.y + ch * 0.55f),
                                  IM_COL32(250, 199, 72, a), cw * 0.02f);
                fg->AddRect(c0, c1, IM_COL32(20, 20, 26, a), cw * 0.04f, 0, 2.0f);
            } else {
                fg->AddImage((ImTextureID)cart.srv, c0, c1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, (int)(appear * 255.0f)));
            }
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
    g_ui.showImport = false;
    bool needsGame = page == ShotPage::Mods || page == ShotPage::ModelPacks || page == ShotPage::QuickMatch ||
                     page == ShotPage::Import;
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
        case ShotPage::Import: {
            // The results of a real --import-batch run (AKI_SHOT_IMPORT_RESULTS = its import-results.json), else demo lines.
            if (g_importSandbox.empty()) g_shotModsDir = demoDir;  // a sandbox shows its own data
            open_mods(games, gameIndex);
            std::vector<ImportLine> lines;
            std::wstring status;
            wchar_t file[MAX_PATH]{};
            bool loaded = false;
            if (GetEnvironmentVariableW(L"AKI_SHOT_IMPORT_RESULTS", file, MAX_PATH)) {
                std::ifstream f(file, std::ios::binary);
                std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                loaded = import_lines_from_json(text, &lines, &status);
            }
            if (!loaded) {
                auto demo = [&](ImportOutcome o, const wchar_t* name, const wchar_t* game, const wchar_t* kind,
                                const wchar_t* tier, const wchar_t* detail) {
                    ImportLine l;
                    l.outcome = o;
                    l.name = name;
                    l.gameKey = game;
                    l.kind = kind;
                    l.tier = tier;
                    l.untested = std::wstring(tier) == L"Untested";
                    l.detail = detail;
                    lines.push_back(l);
                };
                demo(ImportOutcome::Converted, L"Demo Hack", L"nomercy", L"conversion", L"Untested", L"built with make_patch_mod");
                demo(ImportOutcome::Installed, L"Demo Rice Pack", L"nomercy", L"texture pack", L"", L"a Rice/GLideN64 texture pack");
                demo(ImportOutcome::NeedsRom, L"demo.bps", L"nomercy", L"patch", L"", L"needs WWF No Mercy (USA) 1.0");
                demo(ImportOutcome::Unsupported, L"other.z64", L"", L"ROM", L"", L"not one of the AKI games");
                status.clear();
            }
            g_import.show(std::move(lines), status);
            g_ui.showImport = true;
            break;
        }
        case ShotPage::HistoryTour: open_tour(); break;
        case ShotPage::Home:
        case ShotPage::Dev:  // home with the dev line
            if (demo) g_shotModsDir = demoDir;  // its conversions: Play as... (aki-roadmap#154)
            break;
        default: break;
    }
    // The home card's optional lines, for layout checks: AKI_SHOT_CARD = a comma list of update (a newer release than
    // the installed one), played (a last-played line), note (a status note) and wrongrom (the wrong-ROM status with
    // its SHA1 lines). A shot run has none of them on its own: it checks no releases, plays nothing and sets no note.
    // In memory only, like the rest of a shot run.
    wchar_t card[256]{};
    if ((page == ShotPage::Home || page == ShotPage::Dev) && GetEnvironmentVariableW(L"AKI_SHOT_CARD", card, 256)) {
        std::wstring want = L"," + std::wstring(card) + L",";
        GameEntry& g = games[gameIndex];
        if (want.find(L",update,") != std::wstring::npos) {
            if (!g.exeFound) {
                *why = g.title + L" isn't installed here: no update line to show";
                return false;
            }
            if (g.installedTag.empty()) g.installedTag = L"v0.1.0";
            release_set(g.artKey, ReleaseInfo{L"v9.9.9", L"", L""});
        }
        if (want.find(L",played,") != std::wstring::npos)
            settings().stats[g.artKey] = GameStats{(long long)time(nullptr) - 2 * 86400, 4 * 3600 + 12 * 60};
        if (want.find(L",note,") != std::wstring::npos)
            g_session.statusNote = L"Demo status note: the last launch didn't start";
        if (want.find(L",wrongrom,") != std::wstring::npos) {
            g.romWrong = true;
            if (g.romSha1.empty()) g.romSha1 = L"0123456789ABCDEF0123456789ABCDEF01234567";
            g.romHaveSha1 = L"FEDCBA9876543210FEDCBA9876543210FEDCBA98";
        }
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
    std::filesystem::path importSandbox;            // --import-batch
    std::wstring importGame;
    std::vector<std::filesystem::path> importPaths;
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
        if (!shot.padScript.empty()) {
            std::vector<convnav::PadStep> steps;
            if (!convnav::parse_pad_script(shot.padScript, &steps, &err)) {
                logf(L"shot: %s", err.c_str());
                return 2;
            }
            g_padScript = convnav::PadScriptPlayer(std::move(steps));
        }
        // Shots of the import pages (aki-roadmap#228): AKI_IMPORT_SANDBOX = an --import-batch sandbox, whose data folders
        // and Import folder the pages then show instead of the real ones.
        wchar_t sb[MAX_PATH]{};
        if (g_shot && GetEnvironmentVariableW(L"AKI_IMPORT_SANDBOX", sb, MAX_PATH)) g_importSandbox = sb;
    }
    for (int i = 1; i < __argc && !g_shot; ++i) {
        std::wstring a = __wargv[i];
        // Dev/test: --install-mod <mods folder> <mod game id> <file or folder> installs as the Mods page does (Rice/Jabo
        // packs wrapped, aki-roadmap#228) without a window; exit code 0 = installed. The result is in the log.
        if (a == L"--install-mod" && i + 3 < __argc) {
            InstallResult r = install_mod(__wargv[i + 3], __wargv[i + 1], __wargv[i + 2], __wargv[i + 2]);
            logf(L"install-mod: %s: %s", r.ok ? L"ok" : L"failed", r.message.c_str());
            return r.ok ? 0 : 1;
        }
        // Dev/test (aki-roadmap#228): --import-batch <sandbox dir> <game id> <path>... runs a whole import batch without a
        // window, with the sandbox as the launcher's and every game's data folder (Import, Converted, data\<game>\),
        // writes <sandbox>\import-results.json (for --shot import) and exits 0.
        if (a == L"--import-batch" && i + 3 < __argc) {
            importSandbox = __wargv[i + 1];
            importGame = __wargv[i + 2];
            for (int k = i + 3; k < __argc; ++k) importPaths.emplace_back(__wargv[k]);
            break;
        }
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

    if (!importSandbox.empty()) {
        std::vector<GameEntry> ig = build_game_list(detect_depot_root());
        apply_overrides(ig);
        int gi = -1;
        for (int k = 0; k < (int)ig.size(); ++k)
            if (ig[k].artKey == importGame || engine_game_key(ig[k].modGameId) == importGame) gi = k;
        if (gi < 0) {
            logf(L"import-batch: no game %s", importGame.c_str());
            return 2;
        }
        g_importSandbox = std::filesystem::absolute(importSandbox);
        g_ui.modsGame = gi;
        g_ui.modsDir = g_importSandbox / L"data" / ig[gi].artKey / L"mods";
        bool fromFolder = importPaths.size() == 1 && importPaths[0] == L"--import-folder";
        if (fromFolder) importPaths = import_folder_items(import_dir());
        g_import.start(make_import_env(ig), importPaths, fromFolder);
        while (g_import.busy()) {
            g_import.tick();
            Sleep(100);
        }
        for (const auto& l : g_import.lines)
            logf(L"import-batch: %-11s %s | %s | %s", import_outcome_word(l.outcome), l.name.c_str(), l.gameKey.c_str(),
                 l.detail.c_str());
        logf(L"import-batch: %s", g_import.status.c_str());
        std::ofstream f(g_importSandbox / L"import-results.json", std::ios::binary | std::ios::trunc);
        f << import_lines_json(g_import.lines, g_import.status);
        return 0;
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
            logf(L"conversion %s \"%s\" v%s:%s%s%s%s%s%s", c.id.c_str(), c.name.c_str(), c.version.c_str(),
                 c.patch ? L" [Patch]" : L"", c.codes ? L" [Codes]" : L"", c.saves ? L" [Save]" : L"",
                 c.textures ? L" [Textures]" : L"", c.rt ? L" [RT]" : L"", c.models ? L" [Models]" : L"");
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
                start_import(games, drops, false);
            }
        }

        // --- a running import (aki-roadmap#228): its worker's lines, then the Mods page and Import count again ---
        if (g_import.busy()) {
            g_import.tick();
            if (!g_import.busy()) {
                logf(L"import: %s", g_import.status.c_str());
                if (g_ui.showMods) refresh_mods(games);
                refresh_import_count();
                g_ui.convCache.clear();
                play_flip();
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
                // Go through the real UI path, including the cart-insert animation (it always plays).
                logf(L"test-launch: starting cart-insert animation");
                begin_launch(testLaunch);
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
            if ((!g_shot || g_padScript.active()) && ((g_ui.showSettings && !g_ui.captureChord && !g_ui.captureHotkey) || g_ui.showMods ||
                            g_ui.showUpdate || g_ui.showQuick || g_ui.showTour))
                nio.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
            else
                nio.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard);
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        // A pad script (aki-roadmap#232) starts once the page has laid out, like a plain shot's wait.
        bool padShot = false, padDone = false;
        if (g_shot && g_padScript.active()) {
            g_padScriptDown = 0;
            if (shotFrame >= 20) {
                convnav::PadFrame pf = g_padScript.next();
                g_padScriptDown = pf.down;
                padShot = pf.shot;
                padDone = pf.done;
            }
        }
        poll_pads();
        feed_imgui_gamepad();
        if (g_shot && g_padScript.active()) ImGui::GetIO().DeltaTime = 1.0f / 60.0f;  // script frames are 1/60 s
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
            if (launchIdx >= 0) {
                const pkg::ConversionInfo* c = play_as(games, launchIdx);
                logf(L"shot: launch blocked: %s%s%s (the cart insert ran)", games[launchIdx].title.c_str(),
                     c ? L" as " : L"", c ? c->name.c_str() : L"");
            }
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
            g_session.conversionName.clear();
            std::vector<std::pair<std::wstring, std::wstring>> convEnv;
            bool convRefused = false;
            if (!quick && tourStop < 0) {
                if (const pkg::ConversionInfo* c = play_as(games, launchIdx)) {
                    g_session.conversion = widen(c->id);
                    g_session.conversionName = c->name;
                    // Its RT rig file from its aki.json (aki-roadmap#57); the game reads no materials file yet (#281).
                    for (const ConversionInfo& fc : list_conversions(games[launchIdx])) {
                        if (fc.id != g_session.conversion) continue;
                        // aki-roadmap#264: an older game ignores AKI_CONVERSION and plays the original: say so instead.
                        if (!fc.needsGame.empty()) {
                            ready = false;
                            convRefused = true;
                            const std::wstring have = game_run_version(games[launchIdx]);
                            g_session.statusNote = fc.name + L" needs " + games[launchIdx].title + L": Recompiled " +
                                                   fc.needsGame + L" or newer; this one is " + have +
                                                   L". Update the game, or switch Play as to the original game (" +
                                                   (g_ui.padInput ? L"LB" : L"C") + L").";
                            logf(L"play: %s refused: the game is %s, the conversion needs %s", fc.id.c_str(),
                                 have.c_str(), fc.needsGame.c_str());
                            g_session.conversion.clear();
                            g_session.conversionName.clear();
                        } else {
                            convEnv = conversion_env(fc);
                        }
                        break;
                    }
                }
            }
            if (convRefused && testLaunch >= 0) {  // --test-launch: a refused conversion is a failed launch
                logf(L"test-launch: refused: %s", g_session.statusNote.c_str());
                exitCodeOut = 2;
                break;
            }
            bool launched = ready && g_session.launch(games[launchIdx], &err, convEnv.empty() ? nullptr : &convEnv);
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
        if (g_shot && g_padScript.active()) {
            // Pad script: a numbered frame at each "shot" step, the last frame as the shot's own file, then exit.
            static int padShotNo = 0;
            ++shotFrame;
            const std::filesystem::path& out = shot.shots[0].second;
            std::wstring err;
            if (padShot) {
                wchar_t num[16];
                swprintf_s(num, L"-%02d", ++padShotNo);
                std::filesystem::path f = out.parent_path() / (out.stem().wstring() + num + out.extension().wstring());
                if (capture_png(f, &err)) logf(L"shot: %s", f.c_str());
                else {
                    ++shotFailed;
                    logf(L"shot FAILED: %s: %s", f.c_str(), err.c_str());
                }
            }
            if (padDone) {
                if (capture_png(out, &err)) logf(L"shot: %s (pad script done)", out.c_str());
                else {
                    ++shotFailed;
                    logf(L"shot FAILED: %s: %s", out.c_str(), err.c_str());
                }
                shotIndex = shot.shots.size();
                exitCodeOut = shotFailed || !shotPageOk ? 1 : 0;
                done = true;
            }
        } else if (g_shot && ++shotFrame >= 20) {
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
