#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <vector>

struct QuickMatchMod;

// Wrestling History Tour (aki-roadmap#23), first slice: a list of stops walked in order, each one launched through
// Quick match (quickmatch.h) with its venue and match set. The tour is data: assets/tour.json is built in, and a
// tour.json next to the launcher replaces it.
//
//   {"name": "...", "stops": [{"year": 2000, "title": "...", "game": "<games.json id>",
//                              "venue": "<quickmatch Venue choice>", "match": "<quickmatch Match choice>",
//                              "blurb": ["line one", "line two"]}, ...]}
//
// "match" may be left out (the game's rotation), and "blurb" holds one or two lines.

struct TourStop {
    int year = 0;
    std::string title, game, venue, match;  // UTF-8; match "" = the mod's default
    std::vector<std::string> blurb;         // 1-2 lines
};

struct Tour {
    std::string name;
    std::vector<TourStop> stops;
};

// False (with *err naming the stop) for a tour that isn't valid; nothing is half-read.
bool parse_tour(const std::string& json, Tour* out, std::wstring* err);

// The built-in tour (assets/tour.json).
const char* embedded_tour_json();

// <appRoot>\tour.json when there is one (*source = its path), else the built-in tour (*source = L"built in").
// False (with *err) when the file is there but isn't a valid tour: it is not silently replaced by the built-in one.
bool load_tour(const std::filesystem::path& appRoot, Tour* out, std::wstring* source, std::wstring* err);

// The stop's Quick match picks (venue, and match when given), for resolve_quickmatch.
std::map<std::string, std::string> tour_stop_picks(const TourStop& s);
