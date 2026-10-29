#pragma once
#include <string>
#include <vector>

// A small JSON reader for the launcher's data files (games.json, aki-game.json,
// mod.json). UTF-8 in, UTF-8 strings out; nesting is capped at 64 levels.
struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool b = false;
    double n = 0;
    std::string s;                 // String
    std::vector<JsonValue> items;  // Array elements, or Object values (in order)
    std::vector<std::string> keys; // Object keys, parallel to items

    bool is(Type t) const { return type == t; }
    // The member's value, or nullptr (a key that appears twice: the last one).
    const JsonValue* get(const char* key) const;
    // Helpers for optional members: the value when present with that type, else the default.
    std::wstring wstr(const char* key, const std::wstring& def = L"") const;
    bool boolean(const char* key, bool def) const;
    int integer(const char* key, int def) const;
};

// Parses text (a leading UTF-8 BOM is skipped). False with *err on malformed input.
bool json_parse(const std::string& text, JsonValue* out, std::string* err = nullptr);
std::wstring json_utf8_to_wide(const std::string& s);
