// AkiLauncherTests: conversions.cpp's installer (add_conversion) on bad manifests (aki-roadmap#243).
#include "../src/conversions.h"
#include "../src/games.h"
#include "check.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
    // A conversion folder holding only this aki.json; add_conversion refuses it before reading any game.
    std::wstring add_with(const fs::path& tmp, const char* name, const std::string& json) {
        const fs::path dir = tmp / name;
        fs::create_directories(dir);
        std::ofstream(dir / "aki.json", std::ios::binary) << json;
        const std::vector<GameEntry> games;
        try {
            return add_conversion(games, dir, -1).message;
        } catch (...) {
            return L"<threw>";
        }
    }
}

void run_conversions_tests(const fs::path& tmp) {
    const std::wstring invalid = L"aki.json isn't a valid v1 manifest.";
    // format_version as a string threw nlohmann::json::type_error and ended the launcher.
    CHECK(add_with(tmp, "fv_string", R"({ "format": "aki-conversion", "format_version": "1", "id": "a" })") == invalid);
    CHECK(add_with(tmp, "fv_missing", R"({ "format": "aki-conversion", "id": "a" })") == invalid);
    CHECK(add_with(tmp, "fv_two", R"({ "format": "aki-conversion", "format_version": 2, "id": "a" })") == invalid);
    CHECK(add_with(tmp, "fv_float", R"({ "format": "aki-conversion", "format_version": 1.5, "id": "a" })") == invalid);
    CHECK(add_with(tmp, "not_object", R"([1])") == invalid);
    // A v1 manifest gets past that check (and is refused later for its id).
    const std::wstring v1 = add_with(tmp, "fv_one", R"({ "format": "aki-conversion", "format_version": 1, "id": "Bad Id" })");
    CHECK(v1 != invalid && v1 != L"<threw>");
}
