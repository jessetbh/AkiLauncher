#include "json.h"

#include <windows.h>

#include <cmath>
#include <cstdlib>

std::wstring json_utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n > 0 ? n : 0, 0);
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

const JsonValue* JsonValue::get(const char* key) const {
    if (type != Type::Object) return nullptr;
    for (size_t i = keys.size(); i-- > 0;)
        if (keys[i] == key) return &items[i];
    return nullptr;
}

std::wstring JsonValue::wstr(const char* key, const std::wstring& def) const {
    const JsonValue* v = get(key);
    return v && v->type == Type::String ? json_utf8_to_wide(v->s) : def;
}

bool JsonValue::boolean(const char* key, bool def) const {
    const JsonValue* v = get(key);
    return v && v->type == Type::Bool ? v->b : def;
}

int JsonValue::integer(const char* key, int def) const {
    const JsonValue* v = get(key);
    return v && v->type == Type::Number && std::floor(v->n) == v->n ? (int)v->n : def;
}

namespace {
struct Parser {
    const std::string& s;
    size_t i = 0;
    std::string err;

    bool fail(const char* what) {
        if (err.empty()) err = std::string(what) + " at byte " + std::to_string(i);
        return false;
    }
    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
    }
    bool lit(const char* word) {
        size_t n = strlen(word);
        if (s.compare(i, n, word) != 0) return false;
        i += n;
        return true;
    }
    static void put_utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F));
        } else {
            o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F));
            o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(unsigned* v) {
        if (i + 4 > s.size()) return fail("short \\u escape");
        *v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = s[i++];
            *v <<= 4;
            if (c >= '0' && c <= '9') *v |= c - '0';
            else if (c >= 'a' && c <= 'f') *v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') *v |= c - 'A' + 10;
            else return fail("bad \\u escape");
        }
        return true;
    }
    bool str(std::string* o) {
        ++i;  // opening quote
        while (true) {
            if (i >= s.size()) return fail("unterminated string");
            char c = s[i++];
            if (c == '"') return true;
            if ((unsigned char)c < 0x20) return fail("control character in a string");
            if (c != '\\') { *o += c; continue; }
            if (i >= s.size()) return fail("unterminated string");
            char e = s[i++];
            switch (e) {
                case '"': *o += '"'; break;
                case '\\': *o += '\\'; break;
                case '/': *o += '/'; break;
                case 'b': *o += '\b'; break;
                case 'f': *o += '\f'; break;
                case 'n': *o += '\n'; break;
                case 'r': *o += '\r'; break;
                case 't': *o += '\t'; break;
                case 'u': {
                    unsigned cp;
                    if (!hex4(&cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                        i += 2;
                        unsigned lo;
                        if (!hex4(&lo)) return false;
                        if (lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    put_utf8(*o, cp);
                    break;
                }
                default: return fail("bad escape");
            }
        }
    }
    bool value(JsonValue* v, int depth) {
        if (depth > 64) return fail("nested too deeply");
        ws();
        if (i >= s.size()) return fail("unexpected end");
        char c = s[i];
        if (c == '"') {
            v->type = JsonValue::Type::String;
            return str(&v->s);
        }
        if (c == '{' || c == '[') {
            bool obj = c == '{';
            v->type = obj ? JsonValue::Type::Object : JsonValue::Type::Array;
            ++i;
            ws();
            char close = obj ? '}' : ']';
            if (i < s.size() && s[i] == close) {
                ++i;
                return true;
            }
            while (true) {
                if (obj) {
                    ws();
                    if (i >= s.size() || s[i] != '"') return fail("expected a key");
                    std::string k;
                    if (!str(&k)) return false;
                    ws();
                    if (i >= s.size() || s[i] != ':') return fail("expected ':'");
                    ++i;
                    v->keys.push_back(std::move(k));
                }
                v->items.emplace_back();
                if (!value(&v->items.back(), depth + 1)) return false;
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == close) { ++i; return true; }
                return fail(obj ? "expected ',' or '}'" : "expected ',' or ']'");
            }
        }
        if (lit("true")) { v->type = JsonValue::Type::Bool; v->b = true; return true; }
        if (lit("false")) { v->type = JsonValue::Type::Bool; v->b = false; return true; }
        if (lit("null")) { v->type = JsonValue::Type::Null; return true; }
        if (c == '-' || (c >= '0' && c <= '9')) {
            const char* start = s.c_str() + i;
            char* end = nullptr;
            double d = strtod(start, &end);
            if (end == start) return fail("bad number");
            i += end - start;
            v->type = JsonValue::Type::Number;
            v->n = d;
            return true;
        }
        return fail("unexpected character");
    }
};
}  // namespace

bool json_parse(const std::string& text, JsonValue* out, std::string* err) {
    bool bom = text.rfind("\xEF\xBB\xBF", 0) == 0;
    std::string body = bom ? text.substr(3) : std::string();
    Parser p{bom ? body : text};
    *out = JsonValue{};
    bool ok = p.value(out, 0);
    if (ok) {
        p.ws();
        if (p.i != p.s.size()) ok = p.fail("text after the value");
    }
    if (!ok) {
        *out = JsonValue{};
        if (err) *err = p.err;
    }
    return ok;
}
