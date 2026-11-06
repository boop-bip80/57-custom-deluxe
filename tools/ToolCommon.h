// ToolCommon.h -- shared plumbing of the offline tools (capture matrix, comparison, conduction, sensitivity, diagnostics).
//
//   Rig        : engine wrapper working in PHYSICAL units (volts at the jack in, volts and amps at the speaker terminals out),
//                with the oversampler's fixed, documented latency removed (Sec. 23/26: align only a fixed, documented delay)
//   stimuli    : the Sec. 25 composite capture stimulus (noise floor, log sweep, stepped tone, two-tone, burst + quiet probe) and a
//                deterministic synthetic guitar passage for held-out tests
//   WAV / JSON : minimal float32 WAV reader/writer and JSON emitters (no dependencies)
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "CD57Engine.h"
#include "EmbeddedData.h"
#include "Metrics.h"

namespace cd57tool {

using cd57::CD57Engine;
using cd57::EngineParams;
using Vec = std::vector<double>;
namespace M = cd57::metrics;

// ------------------------------------------------------------------------------------------------ WAV
inline bool readWav(const std::string& path, Vec& mono, double& sr)
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
            const size_t n = std::min<size_t>(sz, d.size() - p - 8) / (bits / 8) / ch; mono.assign(n, 0.0);
            for (size_t i = 0; i < n; ++i) {
                double s = 0;
                for (int c = 0; c < ch; ++c) {
                    const size_t o = p + 8 + (i * ch + c) * (bits / 8); double v = 0;
                    if (fmt == 3 && bits == 32) { float x; std::memcpy(&x, &d[o], 4); v = x; }
                    else if (fmt == 3 && bits == 64) { double x; std::memcpy(&x, &d[o], 8); v = x; }
                    else if (bits == 16) { int16_t x; std::memcpy(&x, &d[o], 2); v = x / 32768.0; }
                    else if (bits == 24) { int32_t x = (int32_t)((uint8_t)d[o] << 8 | (uint8_t)d[o + 1] << 16 | (uint8_t)d[o + 2] << 24) >> 8; v = x / 8388608.0; }
                    else if (bits == 32) { int32_t x; std::memcpy(&x, &d[o], 4); v = x / 2147483648.0; }
                    s += v;
                }
                mono[i] = s / ch;
            }
            return true;
        }
        p += 8 + sz + (sz & 1);
    }
    return false;
}
// 32-bit float WAV.  The samples carry PHYSICAL units (volts / amps), not full-scale fractions: the metadata states the unit.
inline bool writeWavF32(const std::string& path, const Vec& x, double sr)
{
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    auto w32 = [&](uint32_t v) { f.write((const char*)&v, 4); }; auto w16 = [&](uint16_t v) { f.write((const char*)&v, 2); };
    const uint32_t bytes = (uint32_t)(x.size() * 4);
    f.write("RIFF", 4); w32(36 + bytes); f.write("WAVEfmt ", 8); w32(16); w16(3); w16(1); w32((uint32_t)sr); w32((uint32_t)sr * 4); w16(4); w16(32);
    f.write("data", 4); w32(bytes);
    for (double v : x) { const float q = (float)v; f.write((const char*)&q, 4); }
    return (bool)f;
}

// ------------------------------------------------------------------------------------------------ JSON emitters
inline std::string jstr(const std::string& s)
{
    std::string o = "\"";
    for (char c : s) { if (c == '"' || c == '\\') { o += '\\'; o += c; } else if (c == '\n') o += "\\n"; else if ((unsigned char)c < 0x20) o += ' '; else o += c; }
    return o + "\"";
}
inline std::string jnum(double v, int digits = 9)
{
    if (!std::isfinite(v)) return "null";
    char b[64]; std::snprintf(b, sizeof b, "%.*g", digits, v); return b;
}
inline std::string jbool(bool b) { return b ? "true" : "false"; }
inline uint64_t fnv1a(const Vec& x)
{
    uint64_t h = 1469598103934665603ull;
    for (double v : x) { const float q = (float)v; uint32_t u; std::memcpy(&u, &q, 4); for (int i = 0; i < 4; ++i) { h ^= (u >> (8 * i)) & 0xff; h *= 1099511628211ull; } }
    return h;
}
inline std::string hex64(uint64_t h) { char b[32]; std::snprintf(b, sizeof b, "%016llx", (unsigned long long)h); return b; }

// ------------------------------------------------------------------------------------------------ rig
struct Rig {
    CD57Engine e;
    double sr;
    int latency = 0;
    int block;
    std::vector<float> in, in2, ot, oa;

    explicit Rig(const EngineParams& p, double sampleRate = 48000.0, int maxBlock = 512, const std::string& manifestOverride = std::string())
        : sr(sampleRate), block(maxBlock), in((size_t)maxBlock), ot((size_t)maxBlock), oa((size_t)maxBlock)
    {
        e.prepare(sampleRate, maxBlock, cd57::embeddedNetlist(), p, manifestOverride.empty() ? cd57::embeddedManifest() : manifestOverride);
        e.setCaptureTerminalCurrent(true);
        latency = e.latencySamples();
    }
    double kIn() const { return (double)e.params().inputCalVolts * std::pow(10.0, (double)e.params().inputTrimDb / 20.0); }
    double kOutVolts() const { return (double)e.params().outputRefVolts; }       // sample -> terminal volts

    // Jack volts in -> terminal volts (and optionally amps) out, the SAME length as the input, with the fixed oversampler latency removed.
    // onBlock(endIndex) is called after every block with the number of input samples consumed so far (probing of internal nodes).
    // jackVolts2 (optional, same length) drives the SECOND plug (EngineParams::jack2); null = the second plug, if any, is a terminated cable.
    Vec run(const Vec& jackVolts, Vec* current = nullptr, const std::function<void(size_t)>& onBlock = nullptr, const Vec* jackVolts2 = nullptr)
    {
        const size_t n = jackVolts.size(), total = n + (size_t)latency;
        Vec out(total, 0.0), cur(current ? total : 0, 0.0);
        const double ki = kIn(), ko = kOutVolts();
        if (jackVolts2 && in2.size() != in.size()) in2.assign(in.size(), 0.f);
        for (size_t i = 0; i < total; i += (size_t)block) {
            const int m = (int)std::min<size_t>((size_t)block, total - i);
            for (int k = 0; k < m; ++k) {
                const size_t j = i + (size_t)k;
                in[(size_t)k] = j < n ? (float)(jackVolts[j] / ki) : 0.f;
                if (jackVolts2) in2[(size_t)k] = j < n && j < jackVolts2->size() ? (float)((*jackVolts2)[j] / ki) : 0.f;
            }
            e.process(in.data(), jackVolts2 ? in2.data() : nullptr, ot.data(), oa.data(), m);
            for (int k = 0; k < m; ++k) out[i + (size_t)k] = (double)ot[(size_t)k] * ko;
            if (current) { const auto& ib = e.terminalCurrentBlock(); for (int k = 0; k < m; ++k) cur[i + (size_t)k] = ib[(size_t)k]; }
            if (onBlock) onBlock(std::min(n, i + (size_t)m));
        }
        out.erase(out.begin(), out.begin() + latency);
        if (current) { cur.erase(cur.begin(), cur.begin() + latency); *current = std::move(cur); }
        return out;
    }
    // restore circuit state from a template engine (same configuration), then flush the resampler history with silence
    void restoreFrom(const Rig& tmpl)
    {
        e.adoptStateFrom(tmpl.e);
        Vec z((size_t)block, 0.0);
        for (int i = 0; i < 3; ++i) run(z);
    }
};

// ------------------------------------------------------------------------------------------------ stimuli
inline Vec sineBurst(double sr, double f, double amp, double seconds, double ramp = 0.02)
{
    Vec x((size_t)std::llround(seconds * sr));
    for (size_t i = 0; i < x.size(); ++i) { const double t = (double)i / sr; x[i] = amp * std::sin(2 * M::kPi * f * t) * std::min(1.0, t / ramp); }
    return x;
}

struct Segment { std::string name; size_t begin = 0, end = 0; double f1 = 0, f2 = 0, level = 0; };
struct Composite { Vec jackVolts; std::vector<Segment> seg; uint64_t hash = 0; const Segment* find(const std::string& n) const { for (auto& s : seg) if (s.name == n) return &s; return nullptr; } };

// The Sec. 25 composite stimulus at peak level `level` volts at the jack (the sweep, stepped tone, burst all use this peak; the two tones
// share it).  Frequencies are exact bins of the analysis windows at 44.1/48/96 kHz (0.2 s windows).
inline Composite makeComposite(double sr, double level)
{
    Composite c;
    auto add = [&](const std::string& name, const Vec& v, double f1 = 0, double f2 = 0, double lev = 0) {
        Segment s; s.name = name; s.begin = c.jackVolts.size(); s.f1 = f1; s.f2 = f2; s.level = lev;
        c.jackVolts.insert(c.jackVolts.end(), v.begin(), v.end()); s.end = c.jackVolts.size(); c.seg.push_back(s);
    };
    auto gap = [&](double sec) { return Vec((size_t)std::llround(sec * sr), 0.0); };
    add("silence", gap(0.25));
    add("sweep", M::makeLogSweep(20.0, 20000.0, 2.0, sr, level).x, 20.0, 20000.0, level);
    add("gap1", gap(0.3));
    add("tone1k", sineBurst(sr, 1000.0, level, 0.3), 1000.0, 0.0, level);
    add("gap2", gap(0.2));
    {
        Vec t((size_t)std::llround(0.3 * sr));
        for (size_t i = 0; i < t.size(); ++i) { const double tt = (double)i / sr; t[i] = 0.5 * level * (std::sin(2 * M::kPi * 700.0 * tt) + std::sin(2 * M::kPi * 1000.0 * tt)) * std::min(1.0, tt / 0.02); }
        add("twotone", t, 700.0, 1000.0, level);
    }
    add("gap3", gap(0.2));
    add("burst", sineBurst(sr, 1000.0, level, 0.15, 0.005), 1000.0, 0.0, level);
    add("probe", sineBurst(sr, 1000.0, 0.01 * level, 0.7, 0.002), 1000.0, 0.0, 0.01 * level);     // quiet passage after drive (-40 dB)
    add("tail", gap(0.2));
    c.hash = fnv1a(c.jackVolts);
    return c;
}

// Deterministic synthetic guitar passage (plucked strings: inharmonic additive partials, pluck-position comb, per-partial decay), used as the
// HELD-OUT input (Sec. 22/25: held-out DI passages).  Not a recording: it only has to be fixed, wide-band and played in musical gestures:
// single notes, a strummed chord, a palm-muted low note and a volume-swell.  `peakVolts` = largest sample value.
inline Vec guitarPassage(double sr, double peakVolts, uint32_t seed = 1)
{
    uint32_t s = seed;
    auto rnd = [&]() { s = s * 1664525u + 1013904223u; return (double)(s >> 8) / 16777216.0; };
    const double dur = 6.0;
    Vec x((size_t)std::llround(dur * sr), 0.0);
    auto pluck = [&](Vec& dst, double t0, double f0, double amp, double decay, double mute) {
        const double B = 1.2e-4, pos = 0.17;
        for (int h = 1; h <= 14; ++h) {
            const double fh = f0 * h * std::sqrt(1.0 + B * h * h);
            if (fh > 0.45 * sr) break;
            const double a = amp * std::sin(M::kPi * h * pos) / (double)h;
            const double tau = decay / (1.0 + 0.5 * (h - 1)) / (1.0 + mute * 6.0);
            const double ph = 2 * M::kPi * rnd();
            const size_t i0 = (size_t)(t0 * sr);
            for (size_t i = i0; i < dst.size(); ++i) {
                const double t = (double)(i - i0) / sr;
                if (t > 8 * tau) break;
                dst[i] += a * std::exp(-t / tau) * std::sin(2 * M::kPi * fh * t + ph) * std::min(1.0, t / 0.001);
            }
        }
    };
    pluck(x, 0.05, 164.81, 1.0, 0.9, 0);            // E3
    pluck(x, 0.80, 220.00, 0.8, 0.9, 0);            // A3
    pluck(x, 1.40, 246.94, 0.9, 0.9, 0);            // B3
    const double chord[4] = {110.00, 164.81, 220.00, 277.18};    // A-power-ish strum, 12 ms between strings
    for (int k = 0; k < 4; ++k) pluck(x, 2.20 + 0.012 * k, chord[k], 0.7, 1.4, 0);
    for (int k = 0; k < 6; ++k) pluck(x, 3.60 + 0.25 * k, 82.41, 1.0, 0.8, 1.0);   // palm-muted low E, six chops
    {   // G3 with a volume swell: the knob is rolled up over 0.8 s, so the note's own envelope is scaled by a rising ramp
        Vec sw(x.size(), 0.0);
        pluck(sw, 5.00, 196.00, 0.9, 1.6, 0);
        for (size_t i = (size_t)(5.0 * sr); i < x.size(); ++i) x[i] += sw[i] * std::min(1.0, ((double)i / sr - 5.0) / 0.8) * 0.6;
    }
    double pk = 0; for (double v : x) pk = std::max(pk, std::fabs(v));
    for (auto& v : x) v *= peakVolts / (pk > 0 ? pk : 1.0);
    return x;
}

} // namespace cd57tool
