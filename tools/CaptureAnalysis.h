// CaptureAnalysis.h -- score ONE composite capture (stimulus + speaker-terminal voltage/current) with the Sec. 26 metric library.
//
// The same code scores a model render and (unchanged) a hardware recording that follows the layout in data/capture/capture_metadata.schema.json,
// so a model-versus-hardware comparison cannot differ from a model-versus-model one by metric definition.
#pragma once
#include "ToolCommon.h"

namespace cd57tool {

struct CaptureMetrics {
    // noise floor (first segment, no input)
    double floorRmsV = 0, floorPkV = 0;
    // stepped 1 kHz tone (last 0.2 s)
    double toneGainDb = 0, toneRmsV = 0, tonePeakV = 0, thdPct = 0, powerW = 0, currentRmsA = 0, currentPeakA = 0;
    M::ToneSpectrum tone;
    // two-tone (700 Hz + 1 kHz)
    M::ImdResult imd;
    // swept response, jack volts -> terminal volts, 1/12-octave points; plus harmonic distortion from the same sweep (Farina)
    M::Response resp;
    double bwLoHz = 0, bwHiHz = 0;                       // -3 dB edges relative to the 1 kHz point (0 if not reached inside 20 Hz..20 kHz)
    std::vector<double> thdSweepF, thdSweepPct;          // THD(f) from the sweep, at the listed fundamentals
    // burst -> quiet probe
    M::Envelope probeEnv; M::Recovery rec; double burstPeakV = 0, burstPeakA = 0, probeGainDb = 0, dipDb = 0;
    // whole capture
    double peakV = 0;
};

inline Vec slice(const Vec& v, size_t a, size_t b) { a = std::min(a, v.size()); b = std::min(b, v.size()); return Vec(v.begin() + (long)a, v.begin() + (long)std::max(a, b)); }

// termV in volts, termI in amps (may be empty), both aligned with the stimulus (latency removed).  fixedDelay: declared fixed delay in
// samples between stimulus and response that is REMOVED from phase / group delay (0 for the model after latency compensation).
inline CaptureMetrics analyseComposite(const Composite& c, const Vec& termV, const Vec& termI, double sr, int fixedDelay = 0)
{
    CaptureMetrics m;
    const Segment* sil = c.find("silence");
    if (sil) { const Vec z = slice(termV, sil->begin + (size_t)(0.05 * sr), sil->end); m.floorRmsV = M::rms(z); m.floorPkV = M::peakAbs(z); }
    m.peakV = M::peakAbs(termV);
    const size_t win = (size_t)std::llround(0.2 * sr);

    if (const Segment* t = c.find("tone1k")) {
        const Vec y = slice(termV, t->begin, t->end);
        m.tone = M::analyseTone(y, sr, 1000.0, win, 10);
        m.toneRmsV = m.tone.rms; m.tonePeakV = m.tone.peak; m.thdPct = 100.0 * m.tone.thd;
        m.toneGainDb = M::db20(m.tone.fundamental / std::max(t->level, 1e-300));
        if (!termI.empty()) {
            const Vec i = slice(termI, t->begin, t->end);
            double p = 0; const size_t n0 = y.size() - std::min(win, y.size());
            for (size_t k = n0; k < y.size(); ++k) p += y[k] * i[k];
            m.powerW = p / (double)(y.size() - n0);
            const Vec iw = slice(i, n0, i.size()); m.currentRmsA = M::rms(iw); m.currentPeakA = M::peakAbs(iw);
        } else m.powerW = m.tone.rms * m.tone.rms / 8.0;
    }
    if (const Segment* t = c.find("twotone")) {
        const Vec y = slice(termV, t->begin, t->end);
        m.imd = M::twoToneIMD(y, sr, t->f1, t->f2, win);
    }
    if (const Segment* s = c.find("sweep")) {
        const Segment* g = c.find("gap1");
        const size_t end = g ? g->end : s->end;
        Vec x = slice(c.jackVolts, s->begin, end), y = slice(termV, s->begin, end);
        m.resp = M::transferFunction(x, y, sr, 20.0, 20000.0, fixedDelay);
        // -3 dB edges relative to 1 kHz
        double ref = 0; for (size_t i = 0; i < m.resp.f.size(); ++i) if (std::fabs(m.resp.f[i] - 1000.0) < 0.5 * 1000.0 * (std::pow(2.0, 1.0 / 12.0) - 1.0)) ref = m.resp.magDb[i];
        if (ref == 0) { size_t bi = 0; double bd = 1e9; for (size_t i = 0; i < m.resp.f.size(); ++i) { const double d = std::fabs(std::log(m.resp.f[i] / 1000.0)); if (d < bd) { bd = d; bi = i; } } ref = m.resp.magDb[bi]; }
        size_t i1k = 0; { double bd = 1e9; for (size_t i = 0; i < m.resp.f.size(); ++i) { const double d = std::fabs(std::log(m.resp.f[i] / 1000.0)); if (d < bd) { bd = d; i1k = i; } } }
        for (size_t i = i1k; i-- > 0;) if (m.resp.magDb[i] < ref - 3.0) { m.bwLoHz = m.resp.f[i]; break; }
        for (size_t i = i1k; i < m.resp.f.size(); ++i) if (m.resp.magDb[i] < ref - 3.0) { m.bwHiHz = m.resp.f[i]; break; }
        // harmonic distortion from the sweep: order 2..5 against the linear IR, at fixed fundamentals
        M::Sweep sw; sw.f1 = 20.0; sw.f2 = 20000.0; sw.seconds = (double)(s->end - s->begin) / sr; sw.sr = sr; sw.x = slice(c.jackVolts, s->begin, s->end);
        const M::SweepDeconvolution d = M::deconvolveSweep(sw, y);
        const size_t N = 16384;
        std::vector<std::vector<M::cplx>> S(6);
        for (int k = 1; k <= 5; ++k) S[(size_t)k] = M::toSpectrum(M::harmonicIR(sw, d, k, 0, N), N);
        for (double f0 : {100.0, 200.0, 400.0, 1000.0, 2000.0, 4000.0}) {
            auto mag = [&](int k, double f) { const double kb = f * (double)N / sr; const size_t k0 = (size_t)kb; const double fr = kb - (double)k0; return std::abs(S[(size_t)k][k0]) * (1 - fr) + std::abs(S[(size_t)k][std::min(k0 + 1, N / 2)]) * fr; };
            double hh = 0; for (int k = 2; k <= 5; ++k) if (k * f0 < 0.45 * sr) hh += std::pow(mag(k, k * f0), 2);
            m.thdSweepF.push_back(f0); m.thdSweepPct.push_back(100.0 * std::sqrt(hh) / std::max(mag(1, f0), 1e-300));
        }
    }
    if (const Segment* b = c.find("burst")) { const Vec v = slice(termV, b->begin, b->end); m.burstPeakV = M::peakAbs(v); if (!termI.empty()) m.burstPeakA = M::peakAbs(slice(termI, b->begin, b->end)); }
    if (const Segment* p = c.find("probe")) {
        const Vec y = slice(termV, p->begin, p->end);
        m.probeEnv = M::lockinEnvelope(y, sr, 1000.0, 4, 0.001);
        // landmarks start 6 ms into the probe: the probe's own 2 ms attack ramp plus the 4-cycle lock-in window (4 ms) would otherwise read as an excursion
        m.rec = M::recoveryLandmarks(m.probeEnv, 0.006, 0.1);
        m.probeGainDb = M::db20(m.rec.settled / std::max(p->level, 1e-300));
        m.dipDb = M::db20(std::max(m.rec.minRel, 1e-6));
    }
    return m;
}

// condition naming used by the capture matrix and the comparison tool
inline const char* jackName(int jack) { static const char* n[] = {"none", "I1", "I2", "M1", "M2"}; return n[std::max(0, std::min(jack, 4))]; }
inline const char* levelName(int l) { static const char* n[] = {"clean", "onset", "driven"}; return n[std::max(0, std::min(l, 2))]; }
inline std::string knobTag(double scale0to12) { char b[16]; std::snprintf(b, sizeof b, "%g", scale0to12); return b; }

} // namespace cd57tool
