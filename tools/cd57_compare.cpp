// cd57_compare -- score a TEST capture directory against a REFERENCE capture directory with the dossier Sec. 26 metrics and PROPOSED limits.
//
//   cd57_compare --ref DIR --test DIR [--ids ID1,ID2,...] [--delay N | --delay-from ID] [--report FILE.md] [--json FILE] [--csv FILE]
//                [--min-snr-db 20] [--strict]
//
// Both directories follow the layout written by cd57_capture_matrix --wav (metadata/, processed/, raw/): so REF may be a recording of a physical
// amplifier made with the same programme (Sec. 25) and TEST the model, or both may be model renders (regression, oversampling equivalence,
// perturbation tests).  Nothing here fits a gain to a clip: ONE integer fixed delay is estimated (or given) for the whole comparison and removed
// from every test waveform (Sec. 24/26), and every metric uses the definitions of src/analysis/Metrics.h.
//
// Limits (Sec. 26, PROPOSED project targets, not Fender specifications):
//   clean magnitude response within 0.5 dB over 80 Hz-8 kHz (where SNR is adequate) | phase and group delay reported independently
//   clipping onset within 0.5 dB (definition A: 1 kHz THD = 1 %) | harmonics above -60 dBc within 1 dB | intermodulation within 2 dB
//   burst envelope within 5 % | recovery landmarks within 10 % (|dt| / max(t_ref, 40 ms), compared only for an excursion >= 10 %: the landmark
//   resolution is 4 ms) | held-out waveform NRMSE <= 5 % (disclose the noise floor)
#include <algorithm>
#include <filesystem>
#include <iostream>

#include "CaptureAnalysis.h"
#include "Json.h"

using namespace cd57tool;
namespace fs = std::filesystem;

struct Cap {
    std::string id; double sr = 48000; Composite comp; Vec v, i; bool heldout = false;
    double onsetV = NAN; bool onsetReached = false; bool ok = false; std::string level;
};

static std::string slurp(const std::string& p) { std::ifstream f(p); return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }

static Cap loadCap(const std::string& dir, const std::string& id)
{
    Cap c; c.id = id;
    const std::string mtxt = slurp(dir + "/metadata/" + id + ".json");
    if (mtxt.empty()) return c;
    cd57::Json md; try { md = cd57::Json::parse(mtxt); } catch (...) { return c; }
    c.sr = md.number("sample_rate_hz", 48000);
    c.heldout = md.string("set") == "heldout";
    const cd57::Json* st = md.get("stimulus");
    if (!st) return c;
    c.level = st->string("level");
    if (const cd57::Json* sg = st->get("segments")) for (auto& s : sg->arr) {
        Segment g; g.name = s.string("name"); g.begin = (size_t)s.number("begin", 0); g.end = (size_t)s.number("end", 0); g.f1 = s.number("f1_hz", 0); g.f2 = s.number("f2_hz", 0); g.level = s.number("peak_volts", 0);
        c.comp.seg.push_back(g);
    }
    double sr; Vec j;
    if (!readWav(dir + "/raw/" + id + "_jack_volts.wav", j, sr)) return c;
    if (!readWav(dir + "/raw/" + id + "_terminal_volts.wav", c.v, sr)) return c;
    readWav(dir + "/raw/" + id + "_terminal_amps.wav", c.i, sr);
    c.comp.jackVolts = j;
    const std::string ptxt = slurp(dir + "/processed/" + id + ".json");
    if (!ptxt.empty()) try { const cd57::Json pr = cd57::Json::parse(ptxt); if (const cd57::Json* o = pr.get("onset")) { c.onsetV = o->number("input_peak_V", NAN); if (const cd57::Json* r = o->get("reached")) c.onsetReached = r->b; } } catch (...) {}
    c.ok = true;
    return c;
}

struct Row {
    std::string id, level; bool heldout = false;
    bool snrOk = true;
    double respMax = NAN, respRms = NAN, respAt = NAN, phaseMax = NAN, gdMaxUs = NAN, onsetErr = NAN, harmMax = NAN, fundErr = NAN, imdMax = NAN, envDev = NAN, recErr10 = NAN, tauErr = NAN;
    double nrmse = NAN, nrmseBand = NAN, esr = NAN, nrmsePassage = NAN, floorRefV = NAN;
};

static double relErr(double a, double b) { return (std::isfinite(a) && std::isfinite(b) && std::fabs(a) > 1e-12) ? std::fabs(b - a) / std::fabs(a) : NAN; }

static Row compareOne(const Cap& r, const Cap& t, int delay, double minSnrDb)
{
    Row w; w.id = r.id; w.level = r.level; w.heldout = r.heldout;
    const double sr = r.sr;
    const Vec tv = M::removeDelay(t.v, delay), ti = t.i.empty() ? Vec() : M::removeDelay(t.i, delay);
    const size_t n = std::min(r.v.size(), tv.size());
    // waveform metrics (whole capture), declared: DC kept, delay removed, full band and 80 Hz-8 kHz band
    {
        M::WaveformOptions o; o.count = n > (size_t)delay ? n - (size_t)std::max(delay, 0) : n;
        const auto e = M::waveformError(r.v, tv, o); w.nrmse = e.nrmse; w.esr = e.esrDb;
        M::WaveformOptions ob = o; ob.bandLo = 80; ob.bandHi = 8000; ob.sampleRate = sr; w.nrmseBand = M::waveformError(r.v, tv, ob).nrmse;
        if (r.heldout) w.nrmsePassage = w.nrmse;
    }
    if (r.heldout) return w;
    const CaptureMetrics a = analyseComposite(r.comp, r.v, r.i, sr, 0), b = analyseComposite(t.comp.seg.empty() ? r.comp : t.comp, tv, ti, sr, 0);
    w.floorRefV = a.floorRmsV;
    // SNR gate (Sec. 26: "where SNR is adequate"): the tone must exceed the reference noise floor by minSnrDb; model renders have a ripple floor only
    w.snrOk = a.toneRmsV > std::max(a.floorRmsV, 1e-12) * std::pow(10.0, minSnrDb / 20.0);
    const auto rc = M::compareResponses(a.resp, b.resp, 80.0, 8000.0);
    w.respMax = rc.maxAbsDb; w.respRms = rc.rmsDb; w.respAt = rc.atHz; w.phaseMax = rc.maxPhaseDeg; w.gdMaxUs = rc.maxGroupDelayErrUs;
    const auto hc = M::compareHarmonics(a.tone, b.tone, -60.0);
    w.harmMax = hc.compared ? hc.maxAbsDb : NAN; w.fundErr = hc.fundamentalErrDb;
    w.imdMax = M::compareImd(a.imd, b.imd, -60.0);
    if (!a.probeEnv.a.empty() && !b.probeEnv.a.empty()) {
        w.envDev = M::envelopeDeviation(a.probeEnv, b.probeEnv, 0.0, 0.7, 0.01);
        // Recovery landmarks are timed with a 4-cycle lock-in window (4 ms at 1 kHz) on a 1 ms hop, so a time has a resolution of about 4 ms.  The relative
        // error is therefore |dt| / max(t_ref, 40 ms): the 10 % limit then never asks for better than the 4 ms the measurement can resolve.  The time
        // WITHIN 10 % exists only when the excursion exceeds 10 %, and a fitted time constant is only reliable for an excursion of at least 10 % (a
        // smaller dip is mostly quantisation and ripple), so both are compared only then.
        const double floorS = 0.040;
        if (a.rec.excursion >= 0.10 && std::isfinite(a.rec.tWithin10) && std::isfinite(b.rec.tWithin10)) w.recErr10 = std::fabs(b.rec.tWithin10 - a.rec.tWithin10) / std::max(a.rec.tWithin10, floorS);
        if (a.rec.excursion >= 0.10 && std::isfinite(a.rec.tau) && std::isfinite(b.rec.tau)) w.tauErr = std::fabs(b.rec.tau - a.rec.tau) / std::max(a.rec.tau, floorS);
    }
    return w;
}

struct Worst { double v = 0; std::string id; int n = 0; void add(double x, const std::string& id_) { if (!std::isfinite(x)) return; ++n; if (x > v) { v = x; this->id = id_; } } };

static std::vector<std::string> listIds(const std::string& dir)
{
    std::vector<std::string> ids;
    if (fs::exists(dir + "/metadata")) for (auto& e : fs::directory_iterator(dir + "/metadata")) if (e.path().extension() == ".json") ids.push_back(e.path().stem().string());
    std::sort(ids.begin(), ids.end());
    return ids;
}

int main(int argc, char** argv)
{
    std::string refDir, testDir, idsArg, delayFrom, report, jsonOut, csvOut; int delay = 0; bool delayGiven = false, strict = false; double minSnr = 20.0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i]; auto nx = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--ref") refDir = nx(); else if (a == "--test") testDir = nx(); else if (a == "--ids") idsArg = nx(); else if (a == "--delay") { delay = std::atoi(nx().c_str()); delayGiven = true; }
        else if (a == "--delay-from") delayFrom = nx(); else if (a == "--report") report = nx(); else if (a == "--json") jsonOut = nx(); else if (a == "--csv") csvOut = nx();
        else if (a == "--min-snr-db") minSnr = std::atof(nx().c_str()); else if (a == "--strict") strict = true;
        else { std::cerr << "unknown option " << a << "\n"; return 2; }
    }
    if (refDir.empty() || testDir.empty()) { std::cerr << "usage: cd57_compare --ref DIR --test DIR [--ids a,b] [--delay N | --delay-from ID] [--report f.md] [--json f] [--csv f] [--min-snr-db 20] [--strict]\n"; return 2; }
    std::vector<std::string> ids;
    if (idsArg.empty()) { const auto a = listIds(refDir), b = listIds(testDir); for (auto& x : a) if (std::binary_search(b.begin(), b.end(), x)) ids.push_back(x); }
    else { std::stringstream ss(idsArg); std::string t; while (std::getline(ss, t, ',')) if (!t.empty()) ids.push_back(t); }
    if (ids.empty()) { std::cerr << "no common captures\n"; return 2; }

    std::vector<Cap> R, T;
    for (auto& id : ids) { R.push_back(loadCap(refDir, id)); T.push_back(loadCap(testDir, id)); if (!R.back().ok || !T.back().ok) { std::cerr << "cannot load capture " << id << " (need metadata, processed and raw WAV in both directories)\n"; return 2; } }
    // ONE fixed delay for the whole comparison (Sec. 26)
    int fixedDelay = delay; double delayCorr = NAN;
    if (!delayGiven) {
        size_t k = 0; if (!delayFrom.empty()) for (size_t q = 0; q < ids.size(); ++q) if (ids[q] == delayFrom) k = q;
        const auto d = M::estimateFixedDelay(R[k].v, T[k].v, 4000);
        fixedDelay = d.samples; delayCorr = d.peakCorrelation;
    }
    std::vector<Row> rows;
    for (size_t k = 0; k < ids.size(); ++k) rows.push_back(compareOne(R[k], T[k], fixedDelay, minSnr));

    Worst wResp, wPhase, wGd, wOnset, wHarm, wImd, wEnv, wRec, wTau, wNrmseHeld, wNrmse, wNrmseBand;
    int skippedSnr = 0;
    for (size_t k = 0; k < rows.size(); ++k) {
        const Row& w = rows[k];
        wNrmse.add(w.nrmse, w.id); wNrmseBand.add(w.nrmseBand, w.id);
        if (w.heldout) { wNrmseHeld.add(w.nrmsePassage, w.id); continue; }
        if (!w.snrOk) { ++skippedSnr; continue; }
        wResp.add(w.respMax, w.id); wPhase.add(w.phaseMax, w.id); wGd.add(w.gdMaxUs, w.id); wHarm.add(w.harmMax, w.id); wImd.add(w.imdMax, w.id);
        if (w.level != "clean") { wEnv.add(w.envDev, w.id); wRec.add(w.recErr10, w.id); wTau.add(w.tauErr, w.id); }
    }
    // clipping onset is a per-condition quantity: evaluate once per condition on the 'onset' level capture
    for (size_t k = 0; k < ids.size(); ++k) if (R[k].level == "onset" && std::isfinite(R[k].onsetV) && std::isfinite(T[k].onsetV)) { rows[k].onsetErr = M::db20(T[k].onsetV / R[k].onsetV); wOnset.add(std::fabs(rows[k].onsetErr), ids[k]); }

    struct Crit { const char* name; Worst* w; double limit; const char* unit; double scale; };
    std::vector<Crit> crit = {
        {"clean magnitude response, max |error| 80 Hz-8 kHz", &wResp, 0.5, "dB", 1.0},
        {"clipping onset (1 kHz THD = 1 %), |error|", &wOnset, 0.5, "dB", 1.0},
        {"significant harmonics (> -60 dBc), max |error|", &wHarm, 1.0, "dB", 1.0},
        {"intermodulation products (> -60 dBc), max |error|", &wImd, 2.0, "dB", 1.0},
        {"burst/probe envelope, max relative deviation", &wEnv, 5.0, "%", 100.0},
        {"recovery landmark (time within 10 %), relative error", &wRec, 10.0, "%", 100.0},
        {"recovery time constant, relative error", &wTau, 10.0, "%", 100.0},
        {"held-out waveform NRMSE", &wNrmseHeld, 5.0, "%", 100.0}};
    bool anyFail = false;
    std::ostringstream md;
    md << "# Capture comparison (dossier Sec. 26)\n\nREF `" << refDir << "`  \nTEST `" << testDir << "`  \n" << ids.size() << " captures; fixed delay removed from every TEST waveform: **" << fixedDelay << " samples**"
       << (delayGiven ? " (given)" : " (estimated once, peak correlation " + jnum(delayCorr, 6) + ")") << "; SNR gate " << jnum(minSnr, 3) << " dB (" << skippedSnr << " captures excluded from the tone-based criteria; their noise floor is reported in the CSV).\n\n";
    md << "| criterion (PROPOSED limit) | worst | at | n | verdict |\n|---|---|---|---|---|\n";
    std::cout << "REF " << refDir << "\nTEST " << testDir << "\n" << ids.size() << " captures, fixed delay " << fixedDelay << " samples" << (delayGiven ? " (given)" : " (estimated once)") << "\n";
    std::ostringstream js; js << "{\n  \"captures\": " << ids.size() << ",\n  \"fixed_delay_samples\": " << fixedDelay << ",\n  \"snr_excluded\": " << skippedSnr << ",\n  \"criteria\": [";
    bool first = true;
    for (auto& c : crit) {
        const bool have = c.w->n > 0; const double val = c.w->v * c.scale; const bool pass = !have || val <= c.limit;
        if (have && !pass) anyFail = true;
        char b[256]; std::snprintf(b, sizeof b, "| %s (<= %g %s) | %.4g %s | %s | %d | %s |\n", c.name, c.limit, c.unit, val, c.unit, c.w->id.c_str(), c.w->n, !have ? "NOT EVALUATED" : (pass ? "PASS" : "FAIL"));
        md << b; std::printf("  %-13s %-62s worst %9.4g %-2s (limit %g) at %s [n=%d]\n", !have ? "NOT EVALUATED" : (pass ? "PASS" : "FAIL"), c.name, val, c.unit, c.limit, c.w->id.c_str(), c.w->n);
        js << (first ? "\n    " : ",\n    ") << "{\"name\": " << jstr(c.name) << ", \"worst\": " << jnum(val) << ", \"unit\": " << jstr(c.unit) << ", \"limit\": " << jnum(c.limit) << ", \"n\": " << c.w->n << ", \"at\": " << jstr(c.w->id) << ", \"verdict\": " << jstr(!have ? "not_evaluated" : (pass ? "pass" : "fail")) << "}";
        first = false;
    }
    js << "\n  ],\n  \"reported_not_limited\": {\"phase_max_deg\": " << jnum(wPhase.v) << ", \"group_delay_max_us\": " << jnum(wGd.v) << ", \"whole_capture_nrmse_worst\": " << jnum(wNrmse.v) << ", \"whole_capture_nrmse_band_worst\": " << jnum(wNrmseBand.v) << "}\n}\n";
    md << "\nReported independently (no limit, Sec. 26): worst phase error after the declared delay **" << jnum(wPhase.v, 4) << " deg**, worst group-delay error **" << jnum(wGd.v, 4) << " us**; whole-capture NRMSE worst **"
       << jnum(wNrmse.v * 100, 4) << " %** full band, **" << jnum(wNrmseBand.v * 100, 4) << " %** in 80 Hz-8 kHz (waveform definition: DC kept, no per-clip gain, delay as above).\n";
    md << "\nNot evaluated here (needs a physical amplifier or measurements the dossier says do not exist publicly): boundary calibration against jack volts, speaker complex impedance, two-load comparison, speaker nonlinear isolation, microphone integrity, blind listening (Sec. 27, 26).\n";
    std::printf("  reported: phase worst %.4g deg, group delay worst %.4g us, whole-capture NRMSE worst %.4g %% (80 Hz-8 kHz band %.4g %%)\n", wPhase.v, wGd.v, wNrmse.v * 100, wNrmseBand.v * 100);
    if (!report.empty()) std::ofstream(report) << md.str();
    if (!jsonOut.empty()) std::ofstream(jsonOut) << js.str();
    if (!csvOut.empty()) {
        std::ofstream c(csvOut);
        c << "id,level,snr_ok,floor_ref_V,resp_max_dB,resp_rms_dB,resp_at_Hz,phase_max_deg,group_delay_max_us,onset_err_dB,harm_max_dB,fund_err_dB,imd_max_dB,env_dev,rec10_rel_err,tau_rel_err,nrmse,nrmse_80_8k,esr_dB\n";
        for (auto& w : rows) c << w.id << "," << w.level << "," << (w.snrOk ? 1 : 0) << "," << jnum(w.floorRefV) << "," << jnum(w.respMax) << "," << jnum(w.respRms) << "," << jnum(w.respAt) << "," << jnum(w.phaseMax) << "," << jnum(w.gdMaxUs) << ","
                              << jnum(w.onsetErr) << "," << jnum(w.harmMax) << "," << jnum(w.fundErr) << "," << jnum(w.imdMax) << "," << jnum(w.envDev) << "," << jnum(w.recErr10) << "," << jnum(w.tauErr) << "," << jnum(w.nrmse) << "," << jnum(w.nrmseBand) << "," << jnum(w.esr) << "\n";
    }
    return (strict && anyFail) ? 1 : 0;
}
