// validate_realtime -- real-time safety and solver-robustness checks (dossier Sec. 21, 23 'Realtime engineering', 27 exp. 8).
//
//  1. no heap allocation inside CD57Engine::process(), measured by replacing operator new in this executable -- in steady state,
//     under heavy drive that engages the sub-step fallback, with every kind of control and configuration change;
//  2. non-finite and over-range host samples neither poison the circuit state nor cost an emergency re-initialisation;
//  3. the Newton acceptance test cannot accept an unbalanced circuit (silicon-rectifier mode used to report a 52 A KCL error);
//  4. dummy-load modes: the voice coil is disconnected, so no coil current and no heating; terminal current is I = V / R;
//  5. the output transformer core is linear by default (dossier Sec. 12/29) and the optional saturating law is switchable
//     without a flux discontinuity;
//  6. per-block processing time: mean, 99th, 99.9th percentile and worst case under demanding drive with automation.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

#include "CD57Engine.h"
#include "EmbeddedData.h"

// ------------------------------------------------------------------------------------------------ allocation counting
static std::atomic<long> gAllocs{0};
static std::atomic<bool> gCount{false};
void* operator new(std::size_t n)
{
    if (gCount.load(std::memory_order_relaxed)) gAllocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void* operator new(std::size_t n, std::align_val_t a)
{
    if (gCount.load(std::memory_order_relaxed)) gAllocs.fetch_add(1, std::memory_order_relaxed);
    void* p = nullptr;
    if (posix_memalign(&p, std::max<std::size_t>((std::size_t)a, sizeof(void*)), n ? n : 1) != 0) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }

using namespace cd57;

namespace {

int nPass = 0, nFail = 0;
void check(bool ok, const std::string& what, const std::string& detail = "")
{
    (ok ? nPass : nFail)++;
    std::printf("  %s  %s%s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.empty() ? "" : "   [", detail.c_str(), detail.empty() ? "" : "]");
}
void info(const std::string& s) { std::printf("  INFO  %s\n", s.c_str()); }
std::string f(double v, int p = 3) { char b[64]; std::snprintf(b, sizeof b, "%.*f", p, v); return b; }

EngineParams base()
{
    EngineParams p; p.oversampleLog2 = 1; p.instVol = 0.7f; p.micVol = 0.0f; p.tone = 0.7f; p.jack = 1; p.load = 0;
    p.speakerHeating = true; p.inputCalVolts = 1.0f; return p;
}

struct Rig {
    CD57Engine e;
    std::vector<float> in, ot, oa;
    double sr;
    explicit Rig(const EngineParams& p, double rate = 48000.0, int block = 512) : in((size_t)block), ot((size_t)block), oa((size_t)block), sr(rate)
    {
        e.prepare(rate, block, embeddedNetlist(), p, embeddedManifest());
    }
    // one block of a 1 kHz sine of peak 'amp' (digital) starting at phase index t0
    void run(double amp, long& t0, double f0 = 1000.0)
    {
        for (size_t i = 0; i < in.size(); ++i) in[i] = (float)(amp * std::sin(2 * M_PI * f0 * (double)(t0 + (long)i) / sr));
        t0 += (long)in.size();
        e.process(in.data(), ot.data(), oa.data(), (int)in.size());
    }
};

// ------------------------------------------------------------------------------------------------ 1. allocations
void allocations()
{
    std::printf("\n== 1. No heap allocation inside process() (operator new replaced and counted)\n");
    for (int os : {0, 1, 2}) {
        EngineParams p = base(); p.oversampleLog2 = os;
        Rig r(p);
        long t0 = 0;
        for (int i = 0; i < 60; ++i) r.run(0.5, t0);                       // warm-up outside the count: heavy drive, sub-stepping engaged
        gAllocs = 0; gCount = true;
        for (int i = 0; i < 200; ++i) r.run(0.5, t0);
        gCount = false;
        const auto d = r.e.diag();
        check(gAllocs.load() == 0, "steady state, heavy drive, " + std::to_string(1 << os) + "x oversampling: 0 allocations in 200 blocks", std::to_string(gAllocs.load()) + " allocations, " + std::to_string(d.substepped) + " sub-stepped steps in the run");
    }
    {   // every kind of control / configuration change, applied between blocks
        EngineParams p = base();
        Rig r(p);
        long t0 = 0;
        for (int i = 0; i < 40; ++i) r.run(0.4, t0);
        struct Change { const char* name; void (*apply)(EngineParams&, int); };
        const Change changes[] = {
            {"instrument volume", [](EngineParams& q, int k) { q.instVol = 0.2f + 0.05f * (float)(k % 12); }},
            {"mic volume", [](EngineParams& q, int k) { q.micVol = 0.05f * (float)(k % 12); }},
            {"tone", [](EngineParams& q, int k) { q.tone = 0.1f * (float)(k % 10); }},
            {"jack", [](EngineParams& q, int k) { q.jack = 1 + (k % 4); }},
            {"patch cable", [](EngineParams& q, int k) { q.patch = k % 7; }},
            {"speaker load", [](EngineParams& q, int k) { q.load = k % 5; }},
            {"power switch", [](EngineParams& q, int k) { q.power = (k % 2) == 0; }},
            {"market variant (50/60 Hz, nominal volts)", [](EngineParams& q, int k) { q.market = k % kMarketCount; }},
            {"line voltage", [](EngineParams& q, int k) { q.linePercent = 85.0f + (float)(k % 10); }},
            {"V1 / V2 tube swap", [](EngineParams& q, int k) { q.v1 = k % 2; q.v2 = k % 2; }},
            {"rectifier (5Y3 / silicon)", [](EngineParams& q, int k) { q.rectifier = k % 2; }},
            {"source model and resistance", [](EngineParams& q, int k) { q.sourceMode = k % 2; q.sourceR = 100.0f + 50.0f * (float)k; }},
            {"volume taper", [](EngineParams& q, int k) { q.taper = k % 3; }},
            {"specimen variation + seed", [](EngineParams& q, int k) { q.variation = 0.5f * (float)(k % 2); q.variationSeed = 1 + k; }},
            {"hum and noise", [](EngineParams& q, int k) { q.humDb = (k % 2) ? -60.0f : -200.0f; q.noise = 0.2f * (float)(k % 2); q.noiseSeed = 1 + k; }},
            {"OT saturation option", [](EngineParams& q, int k) { q.otSaturation = (k % 2) == 1; }},
            {"microphone position", [](EngineParams& q, int k) { q.micRadial = 0.1f * (float)(k % 10); q.micDistance = 0.1f * (float)((k + 3) % 10); }},
            {"speaker heating on/off", [](EngineParams& q, int k) { q.speakerHeating = (k % 2) == 0; }},
            {"input trim / calibration", [](EngineParams& q, int k) { q.inputTrimDb = (float)(k % 5); q.inputCalVolts = 0.5f + 0.1f * (float)(k % 6); }},
        };
        for (const Change& c : changes) {
            gAllocs = 0; gCount = true;
            for (int k = 1; k <= 8; ++k) {
                EngineParams q = r.e.params();
                c.apply(q, k);
                r.e.setParams(q);
                r.run(0.4, t0);
            }
            gCount = false;
            check(gAllocs.load() == 0, std::string("changing ") + c.name + ": 0 allocations over 8 changes", std::to_string(gAllocs.load()) + " allocations");
            for (int i = 0; i < 6; ++i) r.run(0.4, t0);                  // let the state settle before the next case
        }
    }
}

// ------------------------------------------------------------------------------------------------ 2. non-finite / over-range input
void badInput()
{
    std::printf("\n== 2. Non-finite and over-range host samples\n");
    EngineParams p = base();
    Rig r(p);
    long t0 = 0;
    for (int i = 0; i < 40; ++i) r.run(0.1, t0);
    const auto before = r.e.diag();
    const float nan = std::nanf(""), inf = INFINITY;
    for (size_t i = 0; i < r.in.size(); ++i) r.in[i] = (i % 97 == 3) ? nan : (i % 97 == 50 ? inf : (i % 97 == 70 ? -inf : 0.05f));
    const auto t1 = std::chrono::steady_clock::now();
    r.e.process(r.in.data(), r.ot.data(), r.oa.data(), (int)r.in.size());
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
    bool finite = true, nonzero = false;
    for (size_t i = 0; i < r.ot.size(); ++i) { finite = finite && std::isfinite(r.ot[i]) && std::isfinite(r.oa[i]); nonzero = nonzero || std::fabs(r.ot[i]) > 1e-6f; }
    const auto after = r.e.diag();
    check(finite && after.panics == before.panics, "NaN / +Inf / -Inf samples inside a block: output finite and no emergency re-initialisation", "panics " + std::to_string(after.panics - before.panics) + ", non-finite inputs counted " + std::to_string(after.nonFiniteInputs));
    check(nonzero, "the rest of the block is still processed (the block is not muted)");
    check(ms < 8.0, "a block containing non-finite samples costs no more than a normal block (no audio-thread stall)", f(ms, 2) + " ms for 512 samples");
    for (size_t i = 0; i < r.in.size(); ++i) r.in[i] = 1.0e6f * ((i & 1) ? 1.f : -1.f);
    r.e.process(r.in.data(), r.ot.data(), r.oa.data(), (int)r.in.size());
    finite = true; for (size_t i = 0; i < r.ot.size(); ++i) finite = finite && std::isfinite(r.ot[i]);
    const auto after2 = r.e.diag();
    check(finite && after2.panics == after.panics && after2.clampedInputs > after.clampedInputs, "a 1e6 full-scale input is limited to the supported jack voltage (" + f(kMaxJackVolts, 0) + " V) and counted", "clamped " + std::to_string(after2.clampedInputs - after.clampedInputs) + " samples");
    // recovery: the abusive block (a +-30 V Nyquist-rate square, far outside any guitar signal) is allowed to cost held-state steps, which are
    // counted; what must hold is bounded recovery -- no new unrecovered steps after the first two blocks, and normal output level again
    EngineParams q = base(); Rig ref(q); long tr = 0; float refPeak = 0.f;
    for (int i = 0; i < 70; ++i) { ref.run(0.1, tr); if (i >= 66) for (float v : ref.ot) refPeak = std::max(refPeak, std::fabs(v)); }
    long t = t0; uint64_t failuresAfterSettle = 0; float peakLate = 0.f;
    const auto dAbuse = r.e.diag();
    for (int i = 0; i < 60; ++i) {
        r.run(0.1, t);
        if (i == 1) failuresAfterSettle = r.e.diag().failures;
        if (i >= 56) for (float v : r.ot) peakLate = std::max(peakLate, std::fabs(v));
    }
    const auto after3 = r.e.diag();
    info("the abusive +-30 V Nyquist-rate block cost " + std::to_string(dAbuse.failures - before.failures) + " held-state (unrecovered) steps of " + std::to_string(2 * 512) + ", all counted");
    check(after3.failures == failuresAfterSettle && after3.panics == after2.panics, "bounded recovery: no new unrecovered solver steps after the first two blocks, no emergency re-initialisation", "unrecovered steps in blocks 3-60: " + std::to_string(after3.failures - failuresAfterSettle));
    check(peakLate > 0.5f * refPeak && peakLate < 2.0f * refPeak, "normal output level returns after the abuse", "peak " + f(peakLate, 3) + " FS vs " + f(refPeak, 3) + " FS for an undisturbed engine");
}

// ------------------------------------------------------------------------------------------------ 3. Newton acceptance
void newtonAcceptance()
{
    std::printf("\n== 3. Newton acceptance: converged means balanced (nodal KCL residual, not just a small step)\n");
    for (int silicon = 0; silicon < 2; ++silicon) {
        AmpCircuit a;
        a.cfg.rectifier = silicon ? RectifierType::Silicon : RectifierType::Tube5Y3;
        a.loadNetlist(embeddedNetlist());
        a.prepare(96000.0, 0.5);
        AmpControls c; c.instVol = 0.8; c.tone = 0.7; c.guitarJack = 1; c.load = LoadMode::Dummy8;
        a.setControls(c); a.applyControls(true);
        a.initialiseQuiescent(0.15);
        Network& n = a.network();
        n.trackResidual = true; n.stats = {};
        for (int i = 0; i < 48000; ++i) { if (i % 64 == 0) a.applyControls(false); a.tick(0.15 * std::sin(2 * M_PI * 440.0 * i / 96000.0)); }
        check(n.stats.maxResidual <= 2.0 * n.resTolA && n.stats.failures == 0,
              std::string(silicon ? "silicon rectifier" : "5Y3 rectifier") + ": max nodal KCL residual at every accepted step <= 2x the acceptance threshold, no unrecovered steps",
              "max residual " + f(n.stats.maxResidual * 1e6, 2) + " uA (threshold " + f(n.resTolA * 1e6, 0) + " uA), failures " + std::to_string(n.stats.failures));
    }
}

// ------------------------------------------------------------------------------------------------ 4. dummy load
void dummyLoad()
{
    std::printf("\n== 4. Load modes: coil current, heating and terminal current\n");
    struct Mode { LoadMode m; const char* name; double R; bool coil; };
    const Mode modes[] = {{LoadMode::Speaker, "internal speaker", 0.0, true}, {LoadMode::Dummy8, "dummy 8 ohm", 8.0, false},
                          {LoadMode::Dummy4, "dummy 4 ohm", 4.0, false}, {LoadMode::Dummy16, "dummy 16 ohm", 16.0, false}};
    for (const Mode& m : modes) {
        AmpCircuit a;
        a.loadNetlist(embeddedNetlist());
        a.prepare(96000.0, 0.5);
        AmpControls c; c.instVol = 0.7; c.tone = 0.7; c.guitarJack = 1; c.load = m.m;
        a.setControls(c); a.applyControls(true);
        a.initialiseQuiescent(0.15);
        double coilSq = 0.0, worstRel = 0.0, vpk = 0.0; long n = 0;
        for (int i = 0; i < 48000; ++i) {
            if (i % 64 == 0) a.applyControls(false);
            a.tick(0.08 * std::sin(2 * M_PI * 1000.0 * i / 96000.0));
            if (i > 24000) {
                const double v = a.vTerminal(), ic = a.speakerCurrent(), it = a.terminalCurrent();
                coilSq += ic * ic; vpk = std::max(vpk, std::fabs(v)); ++n;
                if (!m.coil && std::fabs(v) > 1.0) worstRel = std::max(worstRel, std::fabs(it - v / m.R) / (std::fabs(v / m.R)));
            }
        }
        const double coilRms = std::sqrt(coilSq / (double)n);
        if (m.coil) check(coilRms > 0.1, std::string(m.name) + ": the voice coil carries current", "coil current " + f(coilRms, 3) + " A rms at " + f(vpk, 1) + " V peak");
        else check(coilRms < 1e-6 && worstRel < 1e-6, std::string(m.name) + ": voice coil disconnected (no coil current, no heating) and terminal current = V / R", "coil " + f(coilRms * 1e9, 3) + " nA rms, worst |I - V/R| " + f(worstRel, 9));
    }
    EngineParams p = base(); p.load = 1 + 1;                                  // dummy 8 ohm through the engine thermal state
    p.inputCalVolts = 1.0f;
    Rig r(p);
    long t0 = 0;
    for (int i = 0; i < 900; ++i) r.run(0.1, t0);                              // about 9.6 s of playing
    check(r.e.diag().coilTempRise < 1e-3f, "engine, dummy load: the voice coil does not heat (was +26 K phantom heating after 30 s)", f(r.e.diag().coilTempRise, 3) + " K");
    EngineParams q = base(); q.load = 0;
    Rig r2(q);
    t0 = 0;
    for (int i = 0; i < 900; ++i) r2.run(0.1, t0);
    check(r2.e.diag().coilTempRise > 0.05f, "engine, internal speaker: the voice coil does heat", f(r2.e.diag().coilTempRise, 2) + " K");
}

// ------------------------------------------------------------------------------------------------ 5. linear OT
void outputTransformer()
{
    std::printf("\n== 5. Output-transformer core: linear by default, saturation is an explicit option\n");
    AmpCircuit a;
    a.loadNetlist(embeddedNetlist());
    a.prepare(96000.0, 0.5);
    check(!a.network().coreSaturation() && !a.cfg.otSaturation, "default build: linear core (no saturation law assigned before evidence exists, dossier Sec. 12/29)");
    auto run = [&](bool sat, double amp, double f0) {
        EngineParams p = base(); p.load = 2; p.otSaturation = sat; p.speakerHeating = false;
        Rig r(p, 48000.0, 512);
        long t0 = 0; double sq = 0; long cnt = 0;
        for (int i = 0; i < 120; ++i) {
            r.run(amp, t0, f0);
            if (i >= 60) for (float v : r.ot) { sq += (double)v * v; ++cnt; }
        }
        return std::sqrt(sq / (double)cnt);
    };
    const double lo0 = run(false, 0.002, 82.0), lo1 = run(true, 0.002, 82.0);
    check(std::fabs(20 * std::log10(lo1 / lo0)) < 0.02, "small-signal response is identical with and without the saturating law (the linear core is its exact small-signal limit)", f(20 * std::log10(lo1 / lo0), 4) + " dB at 2 mV, 82 Hz");
    const double hi0 = run(false, 0.30, 41.0), hi1 = run(true, 0.30, 41.0);
    info("41 Hz at heavy drive: output " + f(20 * std::log10(hi1 / hi0), 2) + " dB with the ASSUMED saturating law relative to the linear core (phiSat is an unmeasured guess)");
    // switching at run time must not create a flux discontinuity
    EngineParams p = base(); p.load = 2; Rig r(p);
    long t0 = 0;
    for (int i = 0; i < 80; ++i) r.run(0.3, t0, 82.0);
    EngineParams q = r.e.params(); q.otSaturation = true; r.e.setParams(q);
    double jump = 0, prev = r.ot.back();
    for (int i = 0; i < 4; ++i) { r.run(0.3, t0, 82.0); for (float v : r.ot) { jump = std::max(jump, (double)std::fabs(v - prev)); prev = v; } }
    check(jump < 0.15, "enabling the saturating law mid-signal causes no step in the output", "largest sample-to-sample change " + f(jump, 3) + " FS");
}

// ------------------------------------------------------------------------------------------------ 6. timing
void timing()
{
    std::printf("\n== 6. Per-block processing time (512-sample blocks, 48 kHz, heavy drive with control automation)\n");
    for (int os : {0, 1, 2, 3}) {
        EngineParams p = base(); p.oversampleLog2 = os;
        Rig r(p);
        long t0 = 0;
        for (int i = 0; i < 40; ++i) r.run(0.5, t0);
        std::vector<double> t;
        const int blocks = os >= 2 ? 400 : 1200;
        for (int i = 0; i < blocks; ++i) {
            EngineParams q = r.e.params(); q.instVol = 0.4f + 0.3f * (float)std::sin(0.05 * i); q.tone = 0.5f + 0.4f * (float)std::sin(0.031 * i); r.e.setParams(q);
            const auto a = std::chrono::steady_clock::now();
            r.run(0.5, t0);
            t.push_back(std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count());
        }
        std::sort(t.begin(), t.end());
        const double budget = 512.0 / 48000.0;
        double mean = 0; for (double x : t) mean += x; mean /= (double)t.size();
        auto pct = [&](double q) { return t[std::min(t.size() - 1, (size_t)(q * (double)t.size()))]; };
        std::printf("  INFO  %dx: mean %.1f %%, p99 %.1f %%, p99.9 %.1f %%, worst %.1f %% of the %.2f ms real-time budget (%d blocks)\n", 1 << os, 100 * mean / budget, 100 * pct(0.99) / budget, 100 * pct(0.999) / budget, 100 * t.back() / budget, 1000 * budget, blocks);
        if (os == 1) check(mean < budget, "default 2x: mean block time is within real time on this machine", f(100 * mean / budget, 1) + " % of budget");
    }
    info("timings come from whatever machine runs this test (shared CI runners are noisy); worst-case figures are reported, not asserted");
}

} // namespace

int main()
{
    std::printf("validate_realtime: CD57Engine real-time safety and solver robustness\n");
    allocations();
    badInput();
    newtonAcceptance();
    dummyLoad();
    outputTransformer();
    timing();
    std::printf("\nSUMMARY: %d passed, %d failed\n", nPass, nFail);
    return nFail ? 1 : 0;
}
