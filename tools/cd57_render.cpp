// cd57_render -- offline renderer: WAV in -> WAV out through the amplifier model (same engine as the plugin).
//   cd57_render in.wav out.wav [--inst 0..12] [--mic 0..12] [--tone 0..12] [--jack 1..4] [--os 0..3]
//               [--output terminals|mic] [--trim dB] [--level dB] [--cal volts] [--load 0..4] [--market 0..8] [--manifest FILE.json]
// Output is 32-bit float WAV at the input sample rate. Mono-mixes multichannel input (as the plugin does).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

#include "CD57Engine.h"
#include "EmbeddedData.h"

static bool readWav(const char* path, std::vector<float>& mono, double& sr)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 44 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4)) return false;
    auto u16 = [&](size_t o) { uint16_t v; std::memcpy(&v, &d[o], 2); return v; };
    auto u32 = [&](size_t o) { uint32_t v; std::memcpy(&v, &d[o], 4); return v; };
    size_t p = 12; int fmt = 0, ch = 1, bits = 16; sr = 48000;
    while (p + 8 <= d.size()) {
        const uint32_t sz = u32(p + 4);
        if (!std::memcmp(&d[p], "fmt ", 4)) { fmt = u16(p + 8); ch = u16(p + 10); sr = u32(p + 12); bits = u16(p + 22); if (fmt == 0xFFFE && sz >= 26) fmt = u16(p + 8 + 24); }
        else if (!std::memcmp(&d[p], "data", 4)) {
            const size_t n = sz / (bits / 8) / ch; mono.assign(n, 0.f);
            for (size_t i = 0; i < n; ++i) { double s = 0; for (int c = 0; c < ch; ++c) { const size_t o = p + 8 + (i * ch + c) * (bits / 8); double v = 0;
                if (fmt == 3 && bits == 32) { float x; std::memcpy(&x, &d[o], 4); v = x; }
                else if (bits == 16) { int16_t x; std::memcpy(&x, &d[o], 2); v = x / 32768.0; }
                else if (bits == 24) { int32_t x = (int32_t)((uint8_t)d[o] << 8 | (uint8_t)d[o + 1] << 16 | (uint8_t)d[o + 2] << 24) >> 8; v = x / 8388608.0; }
                else if (bits == 32) { int32_t x; std::memcpy(&x, &d[o], 4); v = x / 2147483648.0; }
                s += v; } mono[i] = (float)(s / ch); }
            return true;
        }
        p += 8 + sz + (sz & 1);
    }
    return false;
}
static void writeWav(const char* path, const std::vector<float>& x, double sr)
{
    std::ofstream f(path, std::ios::binary);
    auto w32 = [&](uint32_t v) { f.write((const char*)&v, 4); }; auto w16 = [&](uint16_t v) { f.write((const char*)&v, 2); };
    const uint32_t bytes = (uint32_t)(x.size() * 4);
    f.write("RIFF", 4); w32(36 + bytes); f.write("WAVEfmt ", 8); w32(16); w16(3); w16(1); w32((uint32_t)sr); w32((uint32_t)sr * 4); w16(4); w16(32);
    f.write("data", 4); w32(bytes); f.write((const char*)x.data(), bytes);
}

int main(int argc, char** argv)
{
    if (argc < 3) { std::cerr << "usage: cd57_render in.wav out.wav [--inst v] [--mic v] [--tone v] [--jack n] [--os n] [--output terminals|mic] [--trim dB] [--level dB] [--cal V] [--load n] [--market n]\n"; return 2; }
    cd57::EngineParams p; p.instVol = 0.5f; p.micVol = 0.f; p.tone = 0.6f; p.oversampleLog2 = 1;
    bool terminals = false; std::string manifestText = cd57::embeddedManifest();
    for (int i = 3; i + 1 < argc; i += 2) {
        const std::string k = argv[i]; const double v = std::atof(argv[i + 1]);
        if (k == "--inst") p.instVol = (float)(v / 12.0); else if (k == "--mic") p.micVol = (float)(v / 12.0); else if (k == "--tone") p.tone = (float)(v / 12.0);
        else if (k == "--jack") p.jack = (int)v; else if (k == "--os") p.oversampleLog2 = (int)v; else if (k == "--trim") p.inputTrimDb = (float)v;
        else if (k == "--level") p.outputDb = (float)v; else if (k == "--cal") p.inputCalVolts = (float)v; else if (k == "--load") p.load = (int)v;
        else if (k == "--market") p.market = (int)v; else if (k == "--output") terminals = std::string(argv[i + 1]) == "terminals";
        else if (k == "--manifest") { std::ifstream mf(argv[i + 1]); manifestText.assign((std::istreambuf_iterator<char>(mf)), std::istreambuf_iterator<char>()); if (manifestText.empty()) { std::cerr << "cannot read manifest " << argv[i + 1] << "\n"; return 2; } }
        else { std::cerr << "unknown option " << k << "\n"; return 2; }
    }
    std::vector<float> in; double sr;
    if (!readWav(argv[1], in, sr)) { std::cerr << "cannot read " << argv[1] << " (PCM16/24/32 or float WAV expected)\n"; return 1; }
    cd57::CD57Engine e;
    e.prepare(sr, 512, cd57::embeddedNetlist(), p, manifestText);
    if (!e.manifestError().empty()) std::cerr << "manifest warning: " << e.manifestError() << "\n";
    const int lat = e.latencySamples();
    std::vector<float> out(in.size()), ot(512), oa(512);
    in.resize(in.size() + lat, 0.f); out.resize(in.size());               // flush the oversampler delay
    for (size_t i = 0; i < in.size(); i += 512) {
        const int n = (int)std::min<size_t>(512, in.size() - i);
        e.process(&in[i], ot.data(), oa.data(), n);
        for (int k = 0; k < n; ++k) out[i + k] = terminals ? ot[k] : oa[k];
    }
    out.erase(out.begin(), out.begin() + lat);                              // remove the fixed, documented latency
    writeWav(argv[2], out, sr);
    const auto d = e.diag();
    std::cerr << "rendered " << out.size() << " samples at " << sr << " Hz; latency removed: " << lat << " samples; solver " << d.meanIterations() << " it/step, "
              << d.substepped << " fallbacks, " << d.failures << " unrecovered\n";
    return 0;
}
