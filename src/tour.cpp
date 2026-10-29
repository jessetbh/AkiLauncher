#include "tour.h"

#include "json.h"

#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

// assets/tour.json, built in: CMake writes its bytes (and a closing 0) to tour_json.inc.
static const char kEmbeddedTourJson[] = {
#include "tour_json.inc"
};

const char* embedded_tour_json() { return kEmbeddedTourJson; }

static bool text(const JsonValue& v, const char* key, std::string* out) {
    const JsonValue* m = v.get(key);
    if (!m || !m->is(JsonValue::Type::String) || m->s.empty()) return false;
    *out = m->s;
    return true;
}

bool parse_tour(const std::string& json, Tour* out, std::wstring* err) {
    JsonValue root;
    std::string perr;
    if (!json_parse(json, &root, &perr) || !root.is(JsonValue::Type::Object)) {
        *err = L"tour.json: " + (perr.empty() ? std::wstring(L"not an object") : json_utf8_to_wide(perr));
        return false;
    }
    Tour t;
    if (!text(root, "name", &t.name)) {
        *err = L"tour.json: no \"name\"";
        return false;
    }
    const JsonValue* stops = root.get("stops");
    if (!stops || !stops->is(JsonValue::Type::Array) || stops->items.empty()) {
        *err = L"tour.json: no \"stops\"";
        return false;
    }
    for (size_t i = 0; i < stops->items.size(); ++i) {
        const JsonValue& v = stops->items[i];
        std::wstring where = L"tour.json stop " + std::to_wstring(i + 1);
        TourStop s;
        if (!v.is(JsonValue::Type::Object) || !text(v, "title", &s.title)) {
            *err = where + L": no \"title\"";
            return false;
        }
        where += L" (" + json_utf8_to_wide(s.title) + L")";
        s.year = v.integer("year", 0);
        if (s.year < 1900 || s.year > 2100) {
            *err = where + L": \"year\" must be a year";
            return false;
        }
        if (!text(v, "game", &s.game) || !text(v, "venue", &s.venue)) {
            *err = where + L": needs a \"game\" and a \"venue\"";
            return false;
        }
        if (v.get("match") && !text(v, "match", &s.match)) {
            *err = where + L": \"match\" must be a match set's name";
            return false;
        }
        const JsonValue* blurb = v.get("blurb");
        if (blurb && blurb->is(JsonValue::Type::String)) s.blurb.push_back(blurb->s);
        else if (blurb && blurb->is(JsonValue::Type::Array))
            for (const auto& l : blurb->items)
                if (l.is(JsonValue::Type::String)) s.blurb.push_back(l.s);
        if (s.blurb.empty() || s.blurb.size() > 2 || (blurb->is(JsonValue::Type::Array) && blurb->items.size() != s.blurb.size())) {
            *err = where + L": \"blurb\" is one or two lines of text";
            return false;
        }
        t.stops.push_back(std::move(s));
    }
    *out = std::move(t);
    return true;
}

bool load_tour(const fs::path& appRoot, Tour* out, std::wstring* source, std::wstring* err) {
    fs::path file = appRoot / L"tour.json";
    std::error_code ec;
    if (!appRoot.empty() && fs::is_regular_file(file, ec)) {
        std::ifstream f(file, std::ios::binary);
        *source = file.wstring();
        if (!f) {
            *err = L"can't read " + file.wstring();
            return false;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        return parse_tour(ss.str(), out, err);
    }
    *source = L"built in";
    return parse_tour(embedded_tour_json(), out, err);
}

std::map<std::string, std::string> tour_stop_picks(const TourStop& s) {
    std::map<std::string, std::string> p{{"venue", s.venue}};
    if (!s.match.empty()) p["match"] = s.match;
    return p;
}
