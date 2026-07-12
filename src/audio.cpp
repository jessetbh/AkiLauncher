#include "audio.h"

#include <windows.h>

#include <mmsystem.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include "games.h"  // app_root

// ---------------------------------------------------------------------------
// In-memory WAV synthesis (mono, 16-bit, 44.1kHz). PlaySound(SND_MEMORY |
// SND_ASYNC) needs the buffer to outlive the call, so buffers are static.
// ---------------------------------------------------------------------------

static const int kRate = 44100;

static std::vector<uint8_t> wrap_wav(const std::vector<int16_t>& samples) {
    uint32_t dataSize = (uint32_t)(samples.size() * 2);
    std::vector<uint8_t> w(44 + dataSize);
    auto put32 = [&](size_t at, uint32_t v) { memcpy(w.data() + at, &v, 4); };
    auto put16 = [&](size_t at, uint16_t v) { memcpy(w.data() + at, &v, 2); };
    memcpy(w.data(), "RIFF", 4);
    put32(4, 36 + dataSize);
    memcpy(w.data() + 8, "WAVEfmt ", 8);
    put32(16, 16);            // fmt chunk size
    put16(20, 1);             // PCM
    put16(22, 1);             // mono
    put32(24, kRate);
    put32(28, kRate * 2);     // byte rate
    put16(32, 2);             // block align
    put16(34, 16);            // bits per sample
    memcpy(w.data() + 36, "data", 4);
    put32(40, dataSize);
    memcpy(w.data() + 44, samples.data(), dataSize);
    return w;
}

// One tone segment: freq sweeps f0 -> f1 over ms, exponential decay, short
// attack ramp to avoid clicks. Appends to out.
static void tone(std::vector<int16_t>& out, float f0, float f1, int ms, float amp,
                 float decay = 6.0f) {
    int n = kRate * ms / 1000;
    float phase = 0.0f;
    for (int i = 0; i < n; ++i) {
        float t = (float)i / n;
        float f = f0 + (f1 - f0) * t;
        phase += 2.0f * 3.14159265f * f / kRate;
        float env = expf(-decay * t);
        float attack = (float)i / (0.002f * kRate);  // 2ms ramp
        if (attack > 1.0f) attack = 1.0f;
        float s = sinf(phase) + 0.25f * sinf(phase * 2.0f);  // a little body
        out.push_back((int16_t)(s * env * attack * amp * 32767.0f));
    }
}

static std::vector<uint8_t> g_navWav, g_flipWav, g_launchWav;

static void load_override(const std::filesystem::path& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
    if (bytes.size() > 44) out = std::move(bytes);
}

void audio_init() {
    std::vector<int16_t> s;
    tone(s, 1150.0f, 950.0f, 45, 0.17f, 8.0f);
    g_navWav = wrap_wav(s);

    s.clear();
    tone(s, 620.0f, 980.0f, 90, 0.19f, 5.0f);
    g_flipWav = wrap_wav(s);

    s.clear();
    tone(s, 523.25f, 523.25f, 105, 0.24f, 3.0f);   // C5
    tone(s, 783.99f, 783.99f, 240, 0.24f, 4.5f);   // G5
    g_launchWav = wrap_wav(s);

    // assets\sounds\*.wav under the app root replace the synthesized ones
    std::filesystem::path dir = app_root() / L"assets" / L"sounds";
    load_override(dir / L"nav.wav", g_navWav);
    load_override(dir / L"flip.wav", g_flipWav);
    load_override(dir / L"launch.wav", g_launchWav);
}

static void play(const std::vector<uint8_t>& wav) {
    if (wav.empty()) return;
    PlaySoundW((LPCWSTR)wav.data(), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

void play_nav() { play(g_navWav); }
void play_flip() { play(g_flipWav); }
void play_launch() { play(g_launchWav); }
