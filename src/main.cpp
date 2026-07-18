#include <windows.h>

#include <d3d11.h>
#include <shobjidl.h>
#include <xinput.h>

#include <cmath>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "imgui_stdlib.h"

#include "artwork.h"
#include "audio.h"
#include "games.h"
#include "installer.h"
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
    WORD chordPending = 0;
    bool captureHotkey = false;
    std::vector<std::string> exeEdit, romEdit;  // per-game path field buffers
};
static UiState g_ui;
static HWND g_hwnd = nullptr;
static DWORD g_attractMs = 60000;  // idle before attract mode (--attract-ms)

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
        auto it = settings().overrides.find(g.artKey);
        if (it != settings().overrides.end()) {
            const GameOverride& o = it->second;
            if (!o.exePath.empty()) g.exePath = o.exePath;
            if (!o.romPath.empty()) g.romPath = o.romPath;
            if (o.forceBorderless != -1) g.forceBorderless = o.forceBorderless != 0;
        }
        g.exeFound = std::filesystem::is_regular_file(g.exePath, ec);
        g.romFound = std::filesystem::is_regular_file(g.romPath, ec);
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
static void build_fonts(float uiH) {
    ImGuiIO& io = ImGui::GetIO();
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

// Validate (SHA1 when known) and copy a user-picked ROM to the game's expected
// romPath; falls back to a settings override pointing at the original if the
// copy fails. errOut untouched on success.
static bool install_rom_file(GameEntry& g, const std::filesystem::path& src,
                             std::wstring* errOut) {
    if (!g.romSha1.empty()) {
        std::wstring h = sha1_file(src);
        if (h.empty()) {
            *errOut = L"Couldn't read " + src.wstring();
            return false;
        }
        if (h != g.romSha1) {
            *errOut = L"That file isn't the expected " + g.title +
                      L" ROM - need the big-endian (.z64) dump of the US release.";
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::create_directories(g.romPath.parent_path(), ec);
    ec.clear();
    std::filesystem::copy_file(src, g.romPath, std::filesystem::copy_options::overwrite_existing,
                               ec);
    if (ec) {
        settings().overrides[g.artKey].romPath = src.wstring();
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
    std::wstring title = L"Select your " + g.title + L" ROM (" + g.romFile + L")";
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
        std::wstring defRom = (g.repoPath / g.romFile).wstring();
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

static void draw_settings(std::vector<GameEntry>& games, WORD pad) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float vw = vp->WorkSize.x, vh = vp->WorkSize.y;
    ImGuiIO& io = ImGui::GetIO();

    // --- capture flows -----------------------------------------------------
    if (g_ui.captureChord) {
        WORD held = pad_held_mask();
        g_ui.chordPending |= held;
        if (g_ui.chordPending != 0 && held == 0) {
            settings().chordMask = g_ui.chordPending;
            settings_save();
            g_ui.captureChord = false;
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
        if (esc || (pad & XINPUT_GAMEPAD_B)) close_settings(games);
    }

    // --- layout ------------------------------------------------------------
    ImGui::PushFont(g_ui.fontHeader);
    ImGui::SetCursorPosY(vh * 0.055f);
    text_centered("S E T T I N G S");
    ImGui::PopFont();

    float panelW = vw * 0.62f;
    ImGui::SetCursorPos(ImVec2((vw - panelW) * 0.5f, vh * 0.14f));
    ImGui::BeginChild("##settings", ImVec2(panelW, vh * 0.72f), ImGuiChildFlags_None);

    ImGui::PushFont(g_ui.fontBase);
    ImGui::TextColored(palette::accent, "Quick-back");
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
            g_ui.chordPending = 0;
        }
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Hold time");
    ImGui::SameLine(panelW * 0.28f);
    ImGui::SetNextItemWidth(panelW * 0.40f);
    if (ImGui::SliderInt("##hold", &settings().chordHoldMs, 200, 5000, "%d ms"))
        settings_save();

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
                              hotkey_to_string(settings().hotkeyMods, settings().hotkeyVk) +
                                  "  or hold  " + chord_to_string(settings().chordMask) +
                                  "  for the quit prompt, confirm to return here");
        ImGui::PopFont();
        ImGui::End();
        return -1;
    }

    WORD pad = pad_pressed();

    // Which device is the user on? Pad wins ties; the state sticks until the
    // other device produces input.
    if (pad || pad_held_mask()) g_ui.padInput = true;
    else if (keyboard_mouse_activity()) g_ui.padInput = false;

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
        // dismiss the crash panel
        if (!g_session.logTail.empty() &&
            (ImGui::IsKeyPressed(ImGuiKey_Backspace) || (pad & XINPUT_GAMEPAD_B))) {
            g_session.logTail.clear();
            g_session.statusNote.clear();
        }
    }

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
        if (idle > g_attractMs && g_session.logTail.empty()) {
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

    // The crash panel replaces this whole zone until dismissed.
    if (g_session.logTail.empty()) {
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
        if (s.experimental) { status = "Playable - experimental (bring-up in progress)"; statusCol = palette::warn; }
        else { status = "Ready to play"; statusCol = palette::ready; }
        action = CardAction::Play;
    } else if (!s.exeFound) {
        if (haveRel) {
            status = "Not installed - " + narrow(rel.tag) + " is a free download from GitHub";
            statusCol = palette::dim;
            action = CardAction::Download;
        } else if (soon) { status = "Recompilation not started yet"; statusCol = palette::faint; }
        else if (devCheckout) { status = "Not built - run the port build in " + narrow(s.repoDir); statusCol = palette::warn; }
        else { status = "Not available for download yet"; statusCol = palette::faint; }
    } else {
        status = "Installed - now select your own " + narrow(s.romFile) + " ROM dump";
        statusCol = palette::warn;
        action = CardAction::PickRom;
    }
    ImGui::Dummy(ImVec2(0, vh * 0.002f));
    text_centered_colored(statusCol, status);
    ImGui::PopFont();

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

    // Action button (mouse users; keyboard/pad use Enter/A)
    if (action != CardAction::None) {
        const char* label = action == CardAction::Play       ? "PLAY"
                            : action == CardAction::Download ? "DOWNLOAD"
                                                             : "SELECT ROM...";
        ImGui::PushFont(g_ui.fontBase);
        ImVec2 bsz(vw * (action == CardAction::Play ? 0.10f : 0.13f), vh * 0.048f);
        ImGui::SetCursorPos(ImVec2((vw - bsz.x) * 0.5f, infoY + vh * 0.115f));
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

    }  // end if (no crash panel)

    // Crash panel: statusNote + tail of the port's own log, dismissible.
    if (!g_session.logTail.empty()) {
        float px0 = vw * 0.16f, px1 = vw * 0.84f;
        float py0 = vh * 0.615f, py1 = vh * 0.935f;
        ImVec2 p0(vp->WorkPos.x + px0, vp->WorkPos.y + py0);
        ImVec2 p1(vp->WorkPos.x + px1, vp->WorkPos.y + py1);
        dl->AddRectFilled(p0, p1, IM_COL32(16, 12, 14, 246), 10.0f);
        dl->AddRect(p0, p1, IM_COL32(180, 80, 70, 200), 10.0f, 0, 2.0f);
        ImGui::SetCursorPos(ImVec2(px0 + 24, py0 + 16));
        ImGui::PushFont(g_ui.fontBase);
        ImGui::TextColored(palette::error, "%s", narrow(g_session.statusNote).c_str());
        ImGui::PopFont();
        ImGui::PushFont(g_ui.fontSmall);
        ImGui::SetCursorPosX(px0 + 24);
        ImGui::TextColored(palette::faint, "%s",
                           narrow(g_session.portLogPath.wstring()).c_str());
        ImGui::Dummy(ImVec2(0, 6));
        for (const auto& line : g_session.logTail) {
            ImGui::SetCursorPosX(px0 + 24);
            ImGui::TextColored(palette::dim, "%s", line.c_str());
        }
        ImGui::SetCursorPos(ImVec2(px0 + 24, py1 - ImGui::GetFontSize() * 1.9f));
        ImGui::TextColored(palette::faint, "Backspace / B to dismiss");
        ImGui::PopFont();
    } else if (!g_session.statusNote.empty()) {
        ImGui::PushFont(g_ui.fontSmall);
        ImGui::SetCursorPosY(vh * 0.905f);
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
                              "D-Pad browse      A play      X flip box      "
                              "Y settings      In game:  hold " +
                                  chord_to_string(settings().chordMask) + "  to return");
    else
        text_centered_colored(palette::faint,
                              "Left/Right browse      Enter play      F flip box      "
                              "S settings      Esc quit      In game:  " +
                                  hotkey_to_string(settings().hotkeyMods, settings().hotkeyVk) +
                                  "  to return");
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

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR, int) {
    bool smoke = false;
    bool wantSettings = false;
    int testLaunch = -1;
    int playNow = -1;      // debug: launch game N at startup, keep normal UI
    int testInstall = -1;  // debug: download+install game N, exit 0/4
    int testRomIdx = -1;   // debug: validate+copy a ROM for game N, exit 0/4
    std::wstring testRomPath;
    for (int i = 1; i < __argc; ++i) {
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
        else if (a == L"--play" && i + 1 < __argc) playNow = _wtoi(__wargv[++i]);
    }

    ImGui_ImplWin32_EnableDpiAwareness();

    HICON appIcon = LoadIconW(hinst, MAKEINTRESOURCEW(1));  // icons/app.rc resource
    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, wnd_proc, 0, 0, hinst, appIcon,
                   LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, L"AkiLauncher", appIcon};
    RegisterClassExW(&wc);

    settings_load();  // needed before window creation for the windowed pref

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    bool windowed = smoke || settings().windowed;  // smoke shouldn't take over the screen
    HWND hwnd;
    if (smoke) {
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
    build_fonts(uiH);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    g_hwnd = hwnd;
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

    std::filesystem::path depotRoot = detect_depot_root();
    std::vector<GameEntry> games = build_game_list(depotRoot);
    apply_overrides(games);
    if (g_ui.selected < 0 || g_ui.selected >= (int)games.size()) g_ui.selected = 0;
    if (wantSettings) open_settings(games);
    logf(L"depot root: %s", depotRoot.empty() ? L"(NOT FOUND)" : depotRoot.c_str());

    releases_check_start(games);

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

        // Window-mode switches happen between frames: the font atlas rebuild
        // must not race an in-flight ImGui frame.
        if (g_pendingWindowMode >= 0 && !smoke) {
            set_launcher_windowed(g_pendingWindowMode == 1);
            g_pendingWindowMode = -1;
        }

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
                    apply_overrides(games);
                    play_flip();
                } else {
                    g_session.statusNote = L"Install failed - " + st.error;
                }
                install_ack();
            }
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
        if (playNow >= 0 && frame == 30 && playNow < (int)games.size()) {
            std::wstring err;
            g_session.launch(games[playNow], &err);
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
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if (!g_session.active() && !g_ui.showSettings && g_ui.insertIndex < 0 &&
            ImGui::IsKeyPressed(ImGuiKey_Escape) && testLaunch < 0)
            done = true;

        // F11 / Alt+Enter toggle windowed <-> fullscreen from any screen.
        if (!smoke && (ImGui::IsKeyPressed(ImGuiKey_F11, false) ||
                       (ImGui::GetIO().KeyAlt && ImGui::IsKeyPressed(ImGuiKey_Enter, false))))
            g_pendingWindowMode = settings().windowed ? 0 : 1;

        int launchIdx = draw_ui(games, art, depotRoot);
        if (launchIdx >= 0) {
            // play_launch() already fired when the cart-insert animation began.
            std::wstring err;
            if (!g_session.launch(games[launchIdx], &err)) {
                g_session.statusNote = games[launchIdx].title + L": " + err;
                if (testLaunch >= 0) {
                    logf(L"test-launch: launch failed: %s", err.c_str());
                    exitCodeOut = 2;
                    break;
                }
            }
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

    if (!smoke && settings().windowed) {
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
