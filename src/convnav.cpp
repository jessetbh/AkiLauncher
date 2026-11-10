#include "convnav.h"

#include <cwctype>

namespace fs = std::filesystem;

namespace convnav {

int play_as_step(int current, int count, int dir) {
    if (count <= 0) return -1;
    if (current < -1 || current >= count) current = -1;
    int slots = count + 1;               // the original + each conversion
    int slot = current + 1;              // 0 = the original
    slot = ((slot + (dir < 0 ? -1 : 1)) % slots + slots) % slots;
    return slot - 1;
}

int conversions_focus(const std::vector<std::string>& ids, const std::string& pickedId) {
    if (ids.empty()) return -1;
    if (!pickedId.empty())
        for (size_t i = 0; i < ids.size(); ++i)
            if (ids[i] == pickedId) return (int)i;
    return 0;
}

InsertArt insert_art(bool haveCart, bool haveCover) {
    return haveCart ? InsertArt::Cart : haveCover ? InsertArt::Cover : InsertArt::Drawn;
}

TierBadge tier_badge(const std::wstring& tier, bool untested) {
    TierBadge b;
    if (untested) {
        b.text = L"Untested";
        b.warn = true;
        return b;
    }
    if (tier.empty()) return b;
    b.text = tier;
    b.warn = tier != L"Verified";
    return b;
}

static std::wstring dir_key(const fs::path& p) {
    std::wstring s = p.wstring();
    while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    for (auto& c : s) c = (wchar_t)std::towlower(c == L'/' ? L'\\' : c);
    return s;
}

std::vector<fs::path> rom_search_dirs(const std::vector<fs::path>& candidates, const std::function<bool(const fs::path&)>& isDir) {
    std::vector<fs::path> out;
    std::vector<std::wstring> keys;
    for (const auto& c : candidates) {
        if (c.empty()) continue;
        std::wstring k = dir_key(c);
        bool seen = false;
        for (const auto& s : keys) seen = seen || s == k;
        if (seen || !isDir(c)) continue;
        keys.push_back(k);
        out.push_back(c);
    }
    return out;
}

static bool button_named(const std::wstring& n, uint16_t* bit) {
    static const struct {
        const wchar_t* name;
        uint16_t bit;
    } names[] = {{L"up", 0x0001},    {L"down", 0x0002}, {L"left", 0x0004}, {L"right", 0x0008}, {L"start", 0x0010},
                 {L"back", 0x0020},  {L"lb", 0x0100},   {L"rb", 0x0200},   {L"a", 0x1000},     {L"b", 0x2000},
                 {L"x", 0x4000},     {L"y", 0x8000}};
    for (const auto& e : names)
        if (n == e.name) {
            *bit = e.bit;
            return true;
        }
    return false;
}

bool parse_pad_script(const std::wstring& text, std::vector<PadStep>* out, std::wstring* err) {
    std::vector<PadStep> steps;
    std::wstring tok;
    auto flush = [&]() -> bool {
        if (tok.empty()) return true;
        std::wstring t = tok;
        tok.clear();
        for (auto& c : t) c = (wchar_t)std::towlower(c);
        PadStep s;
        if (t == L"shot") {
            s.shot = true;
        } else if (t.compare(0, 4, L"wait") == 0) {
            std::wstring n = t.substr(4);
            if (n.empty() || n.size() > 5 || n.find_first_not_of(L"0123456789") != std::wstring::npos || std::stoi(n) <= 0) {
                *err = L"pad script: \"" + t + L"\" needs a frame count, e.g. wait30";
                return false;
            }
            s.wait = std::stoi(n);
        } else {
            size_t start = 0;
            while (start <= t.size()) {
                size_t plus = t.find(L'+', start);
                std::wstring part = t.substr(start, plus == std::wstring::npos ? std::wstring::npos : plus - start);
                uint16_t bit = 0;
                if (!button_named(part, &bit)) {
                    *err = L"pad script: no button \"" + part + L"\" (A B X Y LB RB START BACK UP DOWN LEFT RIGHT, wait<N>, shot)";
                    return false;
                }
                s.buttons |= bit;
                if (plus == std::wstring::npos) break;
                start = plus + 1;
            }
        }
        steps.push_back(s);
        return true;
    };
    for (wchar_t c : text) {
        if (c == L',' || c == L' ' || c == L'\t' || c == L'\n' || c == L'\r' || c == L';') {
            if (!flush()) return false;
        } else {
            tok += c;
        }
    }
    if (!flush()) return false;
    if (steps.empty()) {
        *err = L"pad script: no steps";
        return false;
    }
    *out = std::move(steps);
    return true;
}

PadFrame PadScriptPlayer::next() {
    PadFrame f;
    if (at_ >= steps_.size()) {
        f.done = true;
        return f;
    }
    const PadStep& s = steps_[at_];
    if (s.shot) {
        f.shot = true;
        ++at_;
    } else if (s.wait > 0) {
        if (++phase_ >= s.wait) {
            phase_ = 0;
            ++at_;
        }
    } else {
        // A press: down for one frame, up for one frame (so the next press of the same button is a new edge).
        if (phase_ == 0) {
            f.down = s.buttons;
            phase_ = 1;
        } else {
            phase_ = 0;
            ++at_;
        }
    }
    return f;
}

CardOpen card_open(const CardKeys& k, int conversions) {
    if (k.keyM || k.padStart) return conversions > 0 ? CardOpen::ModsOnConversions : CardOpen::Mods;
    return CardOpen::None;
}

std::string home_hint_line(bool padInput, bool quickMatch, const std::string& returnKey) {
    if (padInput)
        return std::string("D-Pad Browse      A Play      X Flip Box      Y Settings      ") +
               (quickMatch ? "Back Quick Match      " : "") + "In Game:  Hold " + returnKey + "  To Return";
    return std::string("Left/Right Browse      Enter Play      F Flip Box      S / Esc Settings      ") +
           (quickMatch ? "Q Quick Match      H History Tour      " : "") + "In Game:  " + returnKey + "  To Return";
}

std::string mods_hint_line(bool padInput, const std::string& title) {
    return std::string(padInput ? "Start" : "M") + "  Manage " + title + " Mods";
}

std::string play_as_hint(bool padInput) {
    return padInput ? "LB Changes It" : "C Changes It (Shift+C Back)";  // #289 #295 #297: M / Start is on the Manage line
}

}  // namespace convnav
