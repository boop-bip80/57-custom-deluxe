// validate_jacks -- the four-jack, two-plug model against closed-form predictions of the historical grid network (dossier Sec. 06).
//
//   "Model four jack sockets explicitly.  Two occupied jacks on the same channel alter the switching and the interaction between sources.  A patch cable
//    between channels also connects electrical nodes."  The Sec. 06 test table: Instrument 1 only, Instrument 2 only, Mic 1 / Mic 2 only, both jacks of
//    one channel (mixing and jack-switch topology), one input per channel (shared cathode and passive mix interaction), documented patch arrangement,
//    no source (noise and numerical equilibrium).
//
// The predictions below are fixed BEFORE running, from the drawing's network alone (two 68 kOhm grid resistors, a 1 MOhm leak at jack 1, switch contacts
// that ground / tie the unused tip):
//   * a source in jack 2 only, or in jack 1 with a TERMINATED plug in jack 2, drives the grid through 68 kOhm against another 68 kOhm to ground: -6.02 dB
//     relative to the single-plug Instrument 1 case (which sees the two 68 kOhm in parallel and no divider);
//   * the input impedance at jack 1 with the other jack occupied is 1 MOhm || (68 k + 68 k) = 119.7 kOhm;
//   * a plug in the OTHER channel changes the first channel's gain by < 0.01 dB (its 100 Ohm termination replaces a 0.02 Ohm switch contact);
//   * with two driven plugs the small-signal output is the superposition of the two single-source outputs;
//   * a plug in the same jack as the first is ignored; a patch-cable end on an occupied jack is ignored.
#include <cmath>
#include <complex>
#include <cstdio>
#include <iostream>
#include <string>

#include "ToolCommon.h"

using namespace cd57tool;
using cplx = std::complex<double>;
using cd57::Network;

static int nPass = 0, nFail = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "")
{
    (ok ? nPass : nFail)++;
    std::printf("  %s  %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.empty() ? "" : "   [", detail.empty() ? "" : (detail + "]").c_str());
}
static std::string f(double v, int p = 3) { char b[64]; std::snprintf(b, sizeof b, "%.*f", p, v); return b; }

static EngineParams P(int jack, int jack2 = 0, int patch = 0)
{
    EngineParams p; p.oversampleLog2 = 1; p.instVol = 0.6f; p.micVol = 0.6f; p.tone = 0.65f; p.jack = jack; p.jack2 = jack2; p.patch = patch; p.load = 2; p.speakerHeating = false;
    p.inputCalVolts = 1.f; p.outputRefVolts = 28.28f;
    return p;
}
// small-signal gain (dB) from the EMF of source 1 (or 2) to the speaker terminals, from the AC solve at the idle point
static double gainDb(const EngineParams& p, int source = 1, double fHz = 1000.0)
{
    Rig r(p, 48000.0);
    Network& n = r.e.amp().network();
    const auto v = n.acSolve(fHz, n.nodeId(source == 1 ? "pu" : "pu2"));
    return 20.0 * std::log10(std::abs(v[(size_t)n.nodeId("sp")] / n.getR(n.resId(source == 1 ? "Rsrc" : "Rsrc2"))) + 1e-30);
}
// input impedance of the amplifier at the tip of the jack where source 1 is plugged (the cable capacitance belongs to the source side)
static double zinOhm(const EngineParams& p, double fHz)
{
    Rig r(p, 48000.0);
    Network& n = r.e.amp().network();
    const auto v = n.acSolve(fHz, n.nodeId("pu")); const double w = 2 * M::kPi * fHz;
    const cplx Vtp = v[(size_t)n.nodeId("tp")], Il = (v[(size_t)n.nodeId("pu")] - Vtp) / cplx(0, w * 1e-6);
    return std::abs(Vtp / (Il - cplx(0, w * 20e-12) * Vtp));
}
// level (volts peak) of the spectral line at f in y (last n samples, coherent)
static double lineAt(const Vec& y, double sr, double f, size_t n) { return M::analyseTone(y, sr, f, n, 1).fundamental; }
static Vec runTwo(const EngineParams& p, const Vec& x1, const Vec* x2)
{
    CD57Engine e; e.prepare(48000.0, 512, cd57::embeddedNetlist(), p, cd57::embeddedManifest());
    const int lat = e.latencySamples(); const size_t n = x1.size(), total = n + (size_t)lat;
    std::vector<float> a(512), b(512), ot(512), oa(512); Vec out(total);
    for (size_t i = 0; i < total; i += 512) {
        const int m = (int)std::min<size_t>(512, total - i);
        for (int k = 0; k < m; ++k) { const size_t j = i + (size_t)k; a[(size_t)k] = j < n ? (float)x1[j] : 0.f; b[(size_t)k] = (x2 && j < n) ? (float)(*x2)[j] : 0.f; }
        e.process(a.data(), x2 ? b.data() : nullptr, ot.data(), oa.data(), m);
        for (int k = 0; k < m; ++k) out[i + (size_t)k] = (double)ot[(size_t)k] * 28.28;
    }
    out.erase(out.begin(), out.begin() + lat);
    return out;
}

int main()
{
    const double sr = 48000.0;

    std::printf("\n== 1. single plugs: the four inputs (Sec. 06 rows 1-3)\n");
    const double gI1 = gainDb(P(1)), gI2 = gainDb(P(2)), gM1 = gainDb(P(3)), gM2 = gainDb(P(4));
    check(std::fabs((gI1 - gI2) - 6.0206) < 0.15 && std::fabs((gM1 - gM2) - 6.0206) < 0.15, "Instrument 2 and Mic 2 are 6.02 dB below Instrument 1 and Mic 1 (the drawing's 68 k / 68 k divider)", f(gI1 - gI2, 3) + " / " + f(gM1 - gM2, 3) + " dB");
    const double zI1 = zinOhm(P(1), 100.0), zI2 = zinOhm(P(2), 100.0);
    check(std::fabs(zI1 / 1e6 - 1.0) < 0.06 && std::fabs(zI2 / 136e3 - 1.0) < 0.01, "input impedance 1 MOhm (Instrument 1) and 136 kOhm (Instrument 2), reproduced by the circuit", f(zI1 / 1e3, 1) + " k / " + f(zI2 / 1e3, 2) + " k");

    std::printf("\n== 2. both jacks of one channel (the mixing and switching topology)\n");
    const double gI1t = gainDb(P(1, 2));
    check(std::fabs((gI1 - gI1t) - 6.0206) < 0.15, "Instrument 1 with a terminated plug in Instrument 2 is 6.02 dB down: the second 68 kOhm now returns to ground", "single " + f(gI1, 3) + " dB, with plug " + f(gI1t, 3) + " dB, drop " + f(gI1 - gI1t, 3));
    const double zboth = zinOhm(P(1, 2), 100.0), zexp = 1.0 / (1.0 / 1e6 + 1.0 / 136e3);
    check(std::fabs(zboth / zexp - 1.0) < 0.02, "its input impedance becomes 1 MOhm || 136 kOhm = 119.7 kOhm", f(zboth / 1e3, 2) + " k vs " + f(zexp / 1e3, 2) + " k");
    const double g2in1 = gainDb(P(1, 2), 2);
    check(std::fabs(g2in1 - gI1t) < 0.15, "by symmetry a source in the second plug sees the same -6 dB path to the grid", "second plug " + f(g2in1, 3) + " dB vs first plug " + f(gI1t, 3) + " dB");
    // the other combination of the channel: guitar in 2, terminated plug in 1
    const double gI2t = gainDb(P(2, 1));
    check(std::fabs(gI2t - gI2) < 0.2, "a terminated plug in Instrument 1 changes nothing for a source in Instrument 2 (jack 1's switch grounded its tip anyway)", f(gI2t, 3) + " vs " + f(gI2, 3) + " dB");

    std::printf("\n== 3. one input per channel (Sec. 06: shared cathode and passive mix interaction)\n");
    const double gI1m = gainDb(P(1, 3));
    check(std::fabs(gI1m - gI1) < 0.01, "a terminated plug in the OTHER channel (Mic 1) leaves Instrument 1's gain unchanged (< 0.01 dB)", "change " + f(gI1m - gI1, 5) + " dB");
    const double gM1i = gainDb(P(3, 1));
    check(std::fabs(gM1i - gM1) < 0.01, "and the other way round (Mic 1 with a plug in Instrument 1)", "change " + f(gM1i - gM1, 5) + " dB");

    std::printf("\n== 4. rules\n");
    check(std::fabs(gainDb(P(1, 1)) - gI1) < 1e-9, "a second plug in the SAME jack is ignored (one jack holds one plug)", f(gainDb(P(1, 1)) - gI1, 12) + " dB");
    check(std::fabs(gainDb(P(1, 0, 3)) - gI1) < 1e-9, "a patch-cable end on the occupied jack is ignored (the guitar plug wins)", f(gainDb(P(1, 0, 3)) - gI1, 12) + " dB");
    check(std::fabs(gainDb(P(1, 3, 3)) - gainDb(P(1, 3))) < 1e-9, "a patch-cable end on the second plug's jack is ignored as well");
    check(std::fabs(gainDb(P(3, 0, 4)) - gM1) > 0.5, "a jumper from Instrument 1 to Mic 2 with the guitar in Mic 1 DOES change the loading (channel jumping is a real interaction)", f(gainDb(P(3, 0, 4)) - gM1, 3) + " dB");

    std::printf("\n== 5. two DRIVEN plugs: superposition and the second input path through the engine\n");
    {
        const size_t n = (size_t)(0.5 * sr), win = (size_t)(0.2 * sr);
        const double f1 = 1000.0, f2 = 1300.0, a = 0.0008, b = 0.0008;       // 0.8 mV each: small-signal (THD far below 0.1 %)
        Vec x1(n), x2(n); for (size_t i = 0; i < n; ++i) { const double t = (double)i / sr; x1[i] = a * std::sin(2 * M::kPi * f1 * t) * std::min(1.0, t / 0.02); x2[i] = b * std::sin(2 * M::kPi * f2 * t) * std::min(1.0, t / 0.02); }
        // channel B (Instrument 1 + Instrument 2): both plugs driven
        const Vec yBoth = runTwo(P(1, 2), x1, &x2), y1only = runTwo(P(1, 2), x1, nullptr), y2only = runTwo(P(2, 1), x2, nullptr);
        // note: in the single-source runs the other plug is terminated, so the network around each source is the same as with both driven
        const double L1 = lineAt(yBoth, sr, f1, win), L1s = lineAt(y1only, sr, f1, win), L2 = lineAt(yBoth, sr, f2, win);
        // y2only has the guitar in jack 2 and the terminated plug in jack 1: the same two-plug network, driven only from jack 2
        const Vec y2alt = runTwo(P(1, 2), Vec(n, 0.0), &x2); const double L2s = lineAt(y2alt, sr, f2, win);
        (void)y2only;
        check(std::fabs(M::db20(L1 / L1s)) < 0.05 && std::fabs(M::db20(L2 / L2s)) < 0.05, "with both jacks of a channel driven, each line equals its single-source line within 0.05 dB (the mix is linear at small signal)",
              "1 kHz " + f(M::db20(L1 / L1s), 4) + " dB, 1.3 kHz " + f(M::db20(L2 / L2s), 4) + " dB");
        check(L1s > 1e-3 && L2s > 1e-3, "and both lines are really present at the terminals (not a zero comparison)", f(L1s * 1e3, 1) + " mV and " + f(L2s * 1e3, 1) + " mV peak");
        // one input per channel: Instrument 1 and Mic 1, each driven
        const Vec yC = runTwo(P(1, 3), x1, &x2), yC1 = runTwo(P(1, 3), x1, nullptr), yC2 = runTwo(P(1, 3), Vec(n, 0.0), &x2);
        const double c1 = lineAt(yC, sr, f1, win), c1s = lineAt(yC1, sr, f1, win), c2 = lineAt(yC, sr, f2, win), c2s = lineAt(yC2, sr, f2, win);
        check(std::fabs(M::db20(c1 / c1s)) < 0.05 && std::fabs(M::db20(c2 / c2s)) < 0.05 && c2s > 1e-3, "one input per channel: the Instrument 1 and Mic 1 signals add linearly at small signal", f(M::db20(c1 / c1s), 4) + " / " + f(M::db20(c2 / c2s), 4) + " dB");
        // a terminated plug is bit-identical to a driven plug with an all-zero signal
        const Vec z(n, 0.0); const Vec yz = runTwo(P(1, 2), x1, &z);
        double worst = 0; for (size_t i = 0; i < n; ++i) worst = std::max(worst, std::fabs(yz[i] - y1only[i]));
        check(worst == 0.0, "a second input that is all zeros is bit-identical to a terminated plug", "max |difference| " + f(worst, 12) + " V");
    }

    std::printf("\n== 6. no source\n");
    {
        EngineParams p = P(0); Rig r(p, sr);
        const Vec y = r.run(Vec((size_t)(0.3 * sr), 0.0));
        check(std::isfinite(M::rms(y)) && M::rms(y) < 0.02, "no plug at all: finite, only the mains ripple at the terminals", "rms " + f(M::rms(y) * 1e3, 3) + " mV");
    }
    std::printf("\n%d passed, %d failed\n", nPass, nFail);
    return nFail ? 1 : 0;
}
