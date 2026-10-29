#include "shots.h"

#include <windows.h>
#include <wincodec.h>

#include <cstring>
#include <fstream>

#include "miniz.h"

namespace fs = std::filesystem;

const std::vector<std::pair<std::wstring, ShotPage>>& shot_pages() {
    static const std::vector<std::pair<std::wstring, ShotPage>> pages = {
        {L"home", ShotPage::Home},
        {L"settings", ShotPage::Settings},
        {L"mods", ShotPage::Mods},
        {L"model-packs", ShotPage::ModelPacks},
        {L"quick-match", ShotPage::QuickMatch},
        {L"history-tour", ShotPage::HistoryTour},
        {L"dev", ShotPage::Dev},
    };
    return pages;
}

static bool page_named(const std::wstring& name, ShotPage* out) {
    for (const auto& [n, p] : shot_pages())
        if (n == name) {
            *out = p;
            return true;
        }
    return false;
}

// A value after a flag: present, and not the next flag.
static bool value_at(const std::vector<std::wstring>& args, size_t i) {
    return i < args.size() && !args[i].empty() && args[i].rfind(L"--", 0) != 0;
}

bool parse_shot_args(const std::vector<std::wstring>& args, ShotRequest* out, std::wstring* err) {
    ShotRequest r;
    bool one = false, all = false, gameOrDemo = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::wstring& a = args[i];
        if (a == L"--shot") {
            if (!value_at(args, i + 1) || !value_at(args, i + 2)) {
                *err = L"--shot <page> <out.png>";
                return false;
            }
            ShotPage p;
            if (!page_named(args[i + 1], &p)) {
                std::wstring names;
                for (const auto& [n, _] : shot_pages()) names += (names.empty() ? L"" : L", ") + n;
                *err = L"--shot: no page \"" + args[i + 1] + L"\" (pages: " + names + L")";
                return false;
            }
            if (one || all) {
                *err = L"one --shot or one --shot-all per run";
                return false;
            }
            r.shots.emplace_back(p, fs::path(args[i + 2]));
            one = true;
            i += 2;
        } else if (a == L"--shot-all") {
            if (!value_at(args, i + 1)) {
                *err = L"--shot-all <dir>";
                return false;
            }
            if (one || all) {
                *err = L"one --shot or one --shot-all per run";
                return false;
            }
            fs::path dir = args[++i];
            for (const auto& [n, p] : shot_pages()) r.shots.emplace_back(p, dir / (n + L".png"));
            all = true;
        } else if (a == L"--game") {
            if (!value_at(args, i + 1)) {
                *err = L"--game <id>";
                return false;
            }
            r.game = args[++i];
            gameOrDemo = true;
        } else if (a == L"--demo") {
            r.demo = true;
            gameOrDemo = true;
        } else if (a == L"--size") {
            int w = 0, h = 0;
            wchar_t x = 0, extra = 0;
            if (!value_at(args, i + 1) || swscanf_s(args[i + 1].c_str(), L"%d%c%d%c", &w, &x, 1, &h, &extra, 1) != 3 ||
                (x != L'x' && x != L'X') || w < 320 || h < 240 || w > 7680 || h > 4320) {
                *err = L"--size WxH, e.g. 3840x2160 (320x240 to 7680x4320)";
                return false;
            }
            r.width = w;
            r.height = h;
            gameOrDemo = true;
            ++i;
        }
    }
    if (gameOrDemo && !one && !all) {
        *err = L"--game, --demo and --size go with --shot or --shot-all";
        return false;
    }
    r.active = one || all;
    *out = r;
    return true;
}

bool write_png(const fs::path& file, int width, int height, const uint8_t* rgba, size_t stride, std::wstring* err) {
    if (width <= 0 || height <= 0) {
        *err = L"empty image";
        return false;
    }
    // WIC's PNG encoder takes BGRA: swap, and make every pixel opaque (the swapchain's alpha means nothing here).
    std::vector<uint8_t> bgra((size_t)width * height * 4);
    for (int y = 0; y < height; ++y) {
        const uint8_t* s = rgba + (size_t)y * stride;
        uint8_t* d = bgra.data() + (size_t)y * width * 4;
        for (int x = 0; x < width; ++x, s += 4, d += 4) {
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
            d[3] = 255;
        }
    }
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::error_code ec;
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    IWICImagingFactory* f = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    if (SUCCEEDED(hr)) hr = f->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    if (SUCCEEDED(hr)) hr = enc->Initialize(stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = frame->SetSize((UINT)width, (UINT)height);
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&fmt);
    if (SUCCEEDED(hr) && fmt != GUID_WICPixelFormat32bppBGRA) hr = E_FAIL;
    if (SUCCEEDED(hr))
        hr = frame->WritePixels((UINT)height, (UINT)width * 4, (UINT)bgra.size(), bgra.data());
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = enc->Commit();
    if (frame) frame->Release();
    if (enc) enc->Release();
    if (stream) stream->Release();
    if (f) f->Release();
    if (SUCCEEDED(co)) CoUninitialize();
    if (FAILED(hr)) {
        wchar_t buf[16];
        swprintf(buf, 16, L"%08lx", (unsigned long)hr);
        *err = L"can't write " + file.wstring() + L" (WIC 0x" + buf + L")";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Demo data
// ---------------------------------------------------------------------------

static bool put(const fs::path& p, const std::string& s) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
    return (bool)f;
}

static bool put_zip(const fs::path& p, const std::vector<std::pair<std::string, std::string>>& entries) {
    mz_zip_archive za{};
    if (!mz_zip_writer_init_heap(&za, 0, 0)) return false;
    bool ok = true;
    for (const auto& [name, data] : entries)
        ok = ok && mz_zip_writer_add_mem(&za, name.c_str(), data.data(), data.size(), MZ_DEFAULT_COMPRESSION);
    void* buf = nullptr;
    size_t size = 0;
    ok = ok && mz_zip_writer_finalize_heap_archive(&za, &buf, &size);
    if (ok) ok = put(p, std::string((const char*)buf, size));
    mz_zip_writer_end(&za);
    return ok;
}

// The No Mercy quickmatch manifest's options (AkiMods mods/quickmatch: generate.ps1 and mod.psd1).
static const char* kDemoQuickMatch = R"J({"id":"aki_quickmatch","version":"0.1.0","display_name":"Quick Match (testing)",
"authors":["demo data"],"game_id":"wwfnomercy","short_description":"Demo data for a launcher screenshot.",
"config_schema":{"options":[
{"id":"enabled","name":"Enabled","type":"Bool","default":false},
{"id":"venue","name":"Venue","type":"Enum","default":"Game's pick","options":["Game's pick","Raw","No Mercy","SmackDown",
"King of the Ring","SummerSlam","Survivor Series","Royal Rumble","WrestleMania","Backlash","Armageddon","Backstage hallway"]},
{"id":"match","name":"Match","type":"Enum","default":"Game's rotation","options":["Game's rotation","Tag team","Singles A",
"Singles B","Singles C","Three wrestlers A","Royal Rumble","Four wrestlers","Triple threat","Three wrestlers B"]},
{"id":"length","name":"Demo length","type":"Enum","default":"No time limit","options":["No time limit","1 minute (game)",
"2 minutes","3 minutes","5 minutes","10 minutes","30 minutes"]}]}})J";

// A small preview: a lit figure on a dark ground, so thumbnails show something.
static bool put_preview(const fs::path& p, uint8_t r, uint8_t g, uint8_t b) {
    const int n = 64;
    std::vector<uint8_t> px((size_t)n * n * 4);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            uint8_t* d = &px[((size_t)y * n + x) * 4];
            int dx = x - n / 2;
            bool head = dx * dx + (y - 14) * (y - 14) < 36;
            bool body = y > 20 && y < 58 && dx > -10 + (y > 40 ? 3 : 0) && dx < 10 - (y > 40 ? 3 : 0);
            float shade = (body || head) ? 0.55f + 0.45f * (float)(n - y) / n : 0.12f;
            d[0] = (uint8_t)(((body || head) ? r : 40) * shade);
            d[1] = (uint8_t)(((body || head) ? g : 44) * shade);
            d[2] = (uint8_t)(((body || head) ? b : 60) * shade);
            d[3] = 255;
        }
    std::wstring err;
    return write_png(p, n, n, px.data(), (size_t)n * 4, &err);
}

static bool put_pack(const fs::path& models, const char* id, const char* name, const char* author, int triangles,
                     uint8_t r, uint8_t g, uint8_t b) {
    fs::path d = models / (std::wstring(id, id + strlen(id)) + L".akimodel");
    std::string pack = std::string(R"({"name":")") + name + R"(","author":")" + author +
                       R"(","license":"CC0-1.0","source":"demo data","profile":"nomercy"})";
    std::string man = R"({"profile":"nomercy","triangles":)" + std::to_string(triangles) + "}";
    return put(d / L"pack.json", pack) && put(d / L"manifest.json", man) && put(d / L"model.bin", "AKP1 demo") &&
           put_preview(d / L"preview.png", r, g, b);
}

bool make_demo_conversions(const fs::path& data, std::wstring* err) {
    std::error_code ec;
    fs::remove_all(data, ec);
    bool ok = put(data / L"conversions" / L"demo_plus" / L"aki.json",
                  R"({"format":"aki-conversion","id":"demo_plus","name":"Demo Mercy Plus","version":"1.2.0",)"
                  R"("authors":["demo data"]})") &&
              put(data / L"conversions" / L"demo_plus" / L"codes.txt", "# demo data\n") &&
              put(data / L"saves" / L"demo_plus" / L"demo.sav", "demo") &&
              put_zip(data / L"mods" / L"demo_plus_textures.rtz", {{"rt64.json", "{}"}}) &&
              put(data / L"packages.json",
                  R"({"format":1,"packages":[{"id":"demo_plus","kind":"conversion","version":"1.2.0","source":"launcher",)"
                  R"("installed":"2026-10-29T00:00:00Z","files":[{"path":"conversions/demo_plus","hash":""},)"
                  R"({"path":"mods/demo_plus_textures.rtz","hash":""}]}]})") &&
              put(data / L"conversions" / L"demo_hack" / L"aki.json",
                  R"({"format":"aki-conversion","id":"demo_hack","name":"Demo Hack for another ROM","version":"0.9.0",)"
                  R"("authors":["demo data"],"base_rom":{"sha1":"0000000000000000000000000000000000000000"}})");
    if (!ok) *err = L"can't make the demo conversions in " + data.wstring();
    return ok;
}

bool make_demo_mods_dir(const fs::path& dir, std::wstring* err) {
    std::error_code ec;
    fs::remove_all(dir, ec);
    const std::string cheats = R"({"id":"aki_cheats","version":"0.5.0","display_name":"Cheats","authors":["demo data"],)"
                               R"("game_id":"wwfnomercy","short_description":"Demo data for a launcher screenshot."})";
    const std::string rt64 = R"({"configuration":{"autoPath":"rt64"},"textures":[]})";
    bool ok = put_zip(dir / L"aki_quickmatch.nrm", {{"mod.json", kDemoQuickMatch}, {"mod_binary.bin", "demo"}}) &&
              put_zip(dir / L"aki_cheats.nrm", {{"mod.json", cheats}, {"mod_binary.bin", "demo"}}) &&
              put_zip(dir / L"demo_arena_textures.rtz", {{"rt64.json", rt64}}) &&
              put_pack(dir / L"models", "demo-hero", "Demo Hero", "demo data", 1840, 230, 170, 80) &&
              put_pack(dir / L"models", "demo-robot", "Demo Robot", "demo data", 2210, 120, 170, 230) &&
              put(dir / L"models" / L"models.txt", "# demo data for a launcher screenshot\ndemo-hero    slot 1\n");
    if (!ok) *err = L"can't make the demo mods folder " + dir.wstring();
    return ok;
}
