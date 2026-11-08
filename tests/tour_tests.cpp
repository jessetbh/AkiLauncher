// Checks for the History Tour's data side (src/tour.cpp): parsing tour.json, the built-in tour's stops against the
// No Mercy quickmatch mod's own option lists, and the tour.json next to the launcher replacing it.
#include "../src/quickmatch.h"
#include "../src/tour.h"
#include "check.h"

#include <fstream>
#include <random>
#include <string>

namespace fs = std::filesystem;

static void put(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
}

// The No Mercy quickmatch manifest's Venue and Match lists, as AkiMods mods/quickmatch generates them (generate.ps1:
// "Game's pick" / "Game's rotation", then mod.psd1's Venues and Matches names).
static std::string nomercy_manifest() {
    return R"J({"id":"aki_quickmatch","version":"0.1.0","config_schema":{"options":[
        {"id":"enabled","type":"Bool","default":false},
        {"id":"venue","name":"Venue","type":"Enum","default":"Game's pick","options":["Game's pick","Raw","No Mercy",
         "SmackDown","King of the Ring","SummerSlam","Survivor Series","Royal Rumble","WrestleMania","Backlash",
         "Armageddon","Raw stage","No Mercy stage","SmackDown stage","King of the Ring stage","SummerSlam stage",
         "Survivor Series stage","Royal Rumble stage","WrestleMania stage","Backlash stage","Armageddon stage",
         "Backstage hallway","Backstage bar","Parking garage","Boiler room","Locker room"]},
        {"id":"match","name":"Match","type":"Enum","default":"Game's rotation","options":["Game's rotation","Tag team",
         "Singles A","Singles B","Singles C","Three wrestlers A","Royal Rumble","Four wrestlers","Triple threat",
         "Three wrestlers B"]},
        {"id":"length","name":"Demo length","type":"Enum","default":"No time limit","options":["No time limit",
         "1 minute (game)","2 minutes","3 minutes","5 minutes","10 minutes","30 minutes"]}]}})J";
}

static const char* kStop = R"({"year":2000,"title":"T","game":"nomercy","venue":"Raw","blurb":["one","two"]})";

static std::string tour_with(const std::string& stop) { return std::string(R"({"name":"N","stops":[)") + stop + "]}"; }

static void test_builtin() {
    Tour t;
    std::wstring err;
    CHECK(parse_tour(embedded_tour_json(), &t, &err));
    CHECK(t.stops.size() == 5 && !t.name.empty());
    QuickMatchMod nm;
    CHECK(parse_quickmatch_manifest(nomercy_manifest(), &nm, &err));
    std::mt19937 rng(1);
    for (const auto& s : t.stops) {
        CHECK(s.game == "nomercy");
        CHECK(!s.match.empty());  // every example stop sets its match set
        std::map<std::string, std::string> values;
        bool ok = resolve_quickmatch(nm, tour_stop_picks(s), rng, &values, &err);
        CHECK(ok);
        if (!ok) printf("  stop %s: %ls\n", s.title.c_str(), err.c_str());
        CHECK(values["venue"] == s.venue && values["match"] == s.match);
        CHECK(values["length"] == "No time limit");  // left to the mod's default
        CHECK(s.blurb.size() == 2);
        for (const auto& l : s.blurb) CHECK(l.size() <= 100);  // a line fits the page
        CHECK(s.year >= 1995 && s.year <= 2001);
    }
}

static void test_parse() {
    Tour t;
    std::wstring err;
    CHECK(parse_tour(tour_with(kStop), &t, &err));
    CHECK(t.name == "N" && t.stops.size() == 1 && t.stops[0].venue == "Raw" && t.stops[0].match.empty());
    CHECK(tour_stop_picks(t.stops[0]).size() == 1);  // no match: the mod's default
    CHECK(parse_tour(tour_with(R"({"year":1999,"title":"T","game":"g","venue":"v","match":"m","blurb":"one line"})"), &t, &err));
    CHECK(t.stops[0].blurb.size() == 1 && tour_stop_picks(t.stops[0]).at("match") == "m");

    const char* bad[] = {
        R"({"stops":[{"year":2000,"title":"T","game":"g","venue":"v","blurb":["x"]}]})",  // no name
        R"({"name":"","stops":[{"year":2000,"title":"T","game":"g","venue":"v","blurb":["x"]}]})",
        R"({"name":"N","stops":[]})",                                                // no stops
        R"({"name":"N"})",
        "[1]",
        "not json",
    };
    for (const char* b : bad) CHECK(!parse_tour(b, &t, &err));
    const char* badStops[] = {
        R"({"year":2000,"game":"g","venue":"v","blurb":["x"]})",                    // no title
        R"({"year":"2000","title":"T","game":"g","venue":"v","blurb":["x"]})",      // year as text
        R"({"year":20,"title":"T","game":"g","venue":"v","blurb":["x"]})",
        R"({"year":2000,"title":"T","venue":"v","blurb":["x"]})",                   // no game
        R"({"year":2000,"title":"T","game":"g","blurb":["x"]})",                    // no venue
        R"({"year":2000,"title":"T","game":"g","venue":"v","match":5,"blurb":["x"]})",
        R"({"year":2000,"title":"T","game":"g","venue":"v"})",                      // no blurb
        R"({"year":2000,"title":"T","game":"g","venue":"v","blurb":["a","b","c"]})",
        R"({"year":2000,"title":"T","game":"g","venue":"v","blurb":["a",3]})",
        R"({"year":2000,"title":"T","game":"g","venue":"v","blurb":[]})",
    };
    for (const char* b : badStops) {
        bool ok = parse_tour(tour_with(std::string(kStop) + "," + b), &t, &err);
        CHECK(!ok);
        CHECK(err.find(L"stop 2") != std::wstring::npos);  // the message names the stop
    }
    // A failed parse leaves the earlier tour as it was.
    CHECK(parse_tour(tour_with(kStop), &t, &err));
    CHECK(!parse_tour(tour_with("{}"), &t, &err) && t.stops.size() == 1);
}

static void test_load(const fs::path& tmp) {
    Tour t;
    std::wstring source, err;
    CHECK(load_tour(tmp / L"none", &t, &source, &err) && source == L"built in" && t.stops.size() == 5);
    put(tmp / L"mine" / L"tour.json", tour_with(kStop));
    CHECK(load_tour(tmp / L"mine", &t, &source, &err) && t.stops.size() == 1 && source.find(L"tour.json") != std::wstring::npos);
    // A broken tour.json is an error, not silently the built-in tour.
    put(tmp / L"broken" / L"tour.json", R"({"name":"N","stops":[{}]})");
    CHECK(!load_tour(tmp / L"broken", &t, &source, &err) && !err.empty());
}

void run_tour_tests(const fs::path& tmp) {
    fs::create_directories(tmp);
    test_builtin();
    test_parse();
    test_load(tmp);
}
