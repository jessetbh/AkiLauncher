// Checks for screenshot mode (src/shots.cpp and its guards): the --shot / --shot-all arguments, the PNG writer, the
// demo mods folder as the launcher's own readers see it, and the no-side-effects rule (settings_save writes nothing,
// GameSession::launch starts nothing).
#include "../src/games.h"
#include "../src/installer.h"
#include "../src/launch.h"
#include "../src/rebuild.h"
#include "../src/selfupdate.h"
#include "../src/modelpacks.h"
#include "../src/mods.h"
#include "../src/quickmatch.h"
#include "../src/settings.h"
#include "../src/shots.h"
#include "check.h"

#include <windows.h>
#include <wincodec.h>

#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

static void test_args() {
    ShotRequest r;
    std::wstring err;
    CHECK(parse_shot_args({L"--select", L"2"}, &r, &err) && !r.active);  // no shot: a normal start
    CHECK(parse_shot_args({L"--shot", L"quick-match", L"C:\\out\\q.png", L"--game", L"vpw2"}, &r, &err));
    CHECK(r.active && r.shots.size() == 1 && r.shots[0].first == ShotPage::QuickMatch &&
          r.shots[0].second == fs::path(L"C:\\out\\q.png") && r.game == L"vpw2" && !r.demo);
    CHECK(parse_shot_args({L"--demo", L"--shot-all", L"D:\\shots"}, &r, &err));
    CHECK(r.active && r.demo && r.shots.size() == shot_pages().size() && r.shots.size() == 7);
    CHECK(r.shots[3].first == ShotPage::ModelPacks && r.shots[3].second == fs::path(L"D:\\shots\\model-packs.png"));
    CHECK(r.shots[6].first == ShotPage::Dev && r.shots[6].second.filename() == L"dev.png");
    // every page name parses, and only those
    for (const auto& [name, page] : shot_pages()) {
        CHECK(parse_shot_args({L"--shot", name, L"x.png"}, &r, &err) && r.shots[0].first == page);
    }
    CHECK(!parse_shot_args({L"--shot", L"credits", L"x.png"}, &r, &err) && err.find(L"history-tour") != std::wstring::npos);
    CHECK(!parse_shot_args({L"--shot", L"home"}, &r, &err));  // no path
    CHECK(!parse_shot_args({L"--shot-all"}, &r, &err));
    CHECK(!parse_shot_args({L"--shot", L"home", L"a.png", L"--shot", L"mods", L"b.png"}, &r, &err));
    CHECK(!parse_shot_args({L"--shot", L"home", L"a.png", L"--shot-all", L"d"}, &r, &err));
    CHECK(!parse_shot_args({L"--game", L"nomercy"}, &r, &err));  // --game without a shot
    CHECK(!parse_shot_args({L"--demo"}, &r, &err));
    CHECK(!parse_shot_args({L"--shot", L"home", L"a.png", L"--game"}, &r, &err));
    CHECK(r.width == 1600 && r.height == 900);  // the default frame
    CHECK(parse_shot_args({L"--shot-all", L"d", L"--size", L"3840x2160"}, &r, &err) && r.width == 3840 && r.height == 2160);
    CHECK(!parse_shot_args({L"--shot-all", L"d", L"--size", L"3840"}, &r, &err));
    CHECK(!parse_shot_args({L"--shot-all", L"d", L"--size", L"3840x2160x2"}, &r, &err));
    CHECK(!parse_shot_args({L"--shot-all", L"d", L"--size", L"100x100"}, &r, &err));
    CHECK(!parse_shot_args({L"--size", L"1920x1080"}, &r, &err));  // without a shot
    // a flag is never taken as a value
    CHECK(!parse_shot_args({L"--shot", L"home", L"--game", L"nomercy"}, &r, &err));
    CHECK(!parse_shot_args({L"--shot-all", L"--demo"}, &r, &err));
    CHECK(!parse_shot_args({L"--shot", L"home", L"a.png", L"--game", L"--demo"}, &r, &err));
}

// Width, height and one pixel (BGRA) of a PNG, through WIC.
static bool read_png(const fs::path& p, UINT* w, UINT* h, int x, int y, uint8_t px[4]) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IWICImagingFactory* f = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
              SUCCEEDED(f->CreateDecoderFromFilename(p.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec)) &&
              SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(w, h)) &&
              SUCCEEDED(f->CreateFormatConverter(&conv)) &&
              SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                         WICBitmapPaletteTypeCustom));
    WICRect r{x, y, 1, 1};
    ok = ok && SUCCEEDED(conv->CopyPixels(&r, 4, 4, px));
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    if (f) f->Release();
    return ok;
}

static void test_png(const fs::path& tmp) {
    // 3x2 RGBA with a row stride wider than the row (as a mapped texture has); alpha is dropped (opaque output).
    const size_t stride = 16;
    std::vector<uint8_t> img(stride * 2, 0);
    uint8_t* p = &img[stride + 2 * 4];  // pixel (2, 1)
    p[0] = 200, p[1] = 100, p[2] = 50, p[3] = 7;
    std::wstring err;
    fs::path out = tmp / L"sub" / L"t.png";
    CHECK(write_png(out, 3, 2, img.data(), stride, &err));
    UINT w = 0, h = 0;
    uint8_t px[4]{};
    CHECK(read_png(out, &w, &h, 2, 1, px));
    CHECK(w == 3 && h == 2);
    CHECK(px[2] == 200 && px[1] == 100 && px[0] == 50 && px[3] == 255);  // BGRA back: red, green, blue, opaque
    CHECK(!write_png(tmp / L"e.png", 0, 0, img.data(), stride, &err));
}

static void test_demo(const fs::path& tmp) {
    fs::path d = tmp / L"demo";
    std::wstring err;
    CHECK(make_demo_mods_dir(d, &err));
    std::vector<ModItem> mods = list_mods(d);
    CHECK(mods.size() == 3);
    int code = 0, textures = 0;
    for (const auto& m : mods) {
        code += m.code;
        textures += m.textures;
        CHECK(m.problem.empty());
    }
    CHECK(code == 2 && textures == 1);
    std::vector<ModelPack> packs = list_model_packs(d / L"models");
    CHECK(packs.size() == 2 && packs[0].id == "demo-hero" && !packs[0].preview.empty() && packs[0].triangles > 0);
    CHECK(model_pack_profile(packs[1].dir) == "nomercy");
    ModelsTxt mt;
    CHECK(read_models_txt(d / L"models" / L"models.txt", &mt, &err));
    QuickMatchMod qm;
    CHECK(find_quickmatch({}, d, &qm, &err) && qm.option("venue") && qm.option("match") && qm.option("length"));
    // Made again over an old one: the same result (no leftovers).
    CHECK(make_demo_mods_dir(d, &err) && list_mods(d).size() == 3);
}

static std::string slurp(const fs::path& p, bool* exists) {
    std::ifstream f(p, std::ios::binary);
    *exists = (bool)f;
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void test_no_side_effects(const fs::path& tmp) {
    // settings_save in read-only mode leaves the settings file exactly as it was (or absent).
    fs::path sp = settings_path();
    bool existed = false;
    std::string before = slurp(sp, &existed);
    settings_set_read_only(true);
    settings().tourStop = 12345;  // a change that would be written
    settings_save();
    bool existsAfter = false;
    std::string after = slurp(sp, &existsAfter);
    CHECK(existed == existsAfter && before == after);
    if (existed != existsAfter || before != after) {  // the guard failed: put the file back
        std::error_code ec;
        if (existed) {
            std::ofstream f(sp, std::ios::binary | std::ios::trunc);
            f << before;
        } else {
            fs::remove(sp, ec);
        }
    }
    settings_set_read_only(false);
    settings().tourStop = 0;

    // A blocked launch starts nothing: it fails before CreateProcess, with its own message, and the session stays idle.
    GameEntry g;
    g.title = L"Blocked";
    g.exePath = tmp / L"no-such-game.exe";
    g.repoPath = tmp;
    GameSession s;
    std::wstring err;
    launch_set_blocked(true);
    CHECK(!s.launch(g, &err) && !s.active());
    CHECK(err.find(L"screenshot") != std::wstring::npos);
    // ... and so do a rebuild, a game install and the launcher's update check and download.
    err.clear();
    CHECK(!rebuild_start(0, g, tmp, &err) && err.find(L"screenshot") != std::wstring::npos);
    CHECK(!install_start(0, g, ReleaseInfo{}) && install_status().phase == InstallPhase::Idle);
    update_check_start();
    update_tick();
    UpdatePhase ph = update_status().phase;  // the other phases only follow a check (a dev build never checks, so this is no proof on its own)
    CHECK(ph != UpdatePhase::Checking && ph != UpdatePhase::UpToDate && ph != UpdatePhase::Available &&
          ph != UpdatePhase::Failed);
    CHECK(!update_download_start());
    launch_set_blocked(false);
    CHECK(!s.launch(g, &err) && err.find(L"CreateProcess") != std::wstring::npos);  // unblocked: it really tries
}

void run_shots_tests(const fs::path& tmp) {
    fs::create_directories(tmp);
    test_args();
    test_png(tmp);
    test_demo(tmp);
    test_no_side_effects(tmp);
}
