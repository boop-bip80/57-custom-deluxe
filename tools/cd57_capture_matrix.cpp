// cd57_capture_matrix -- render the dossier Sec. 25 capture programme through the MODEL, so that the same programme run on a physical amplifier
// produces a directly comparable dataset (tools/cd57_compare.cpp) and so that model changes can be regression-tested against a committed reference.
//
//   cd57_capture_matrix --out DIR [--set core|endpoints|jackstates|heldout|all] [--quick] [--sr 48000] [--os 1] [--load 2] [--threads N]
//                       [--wav] [--only SUBSTR] [--timestamp ISO8601] [--summary FILE.csv]
//                       [--line PERCENT] [--taper 0|1|2] [--market 0..8] [--v1 0|1] [--v2 0|1] [--rect 0|1] [--manifest FILE]   (engine perturbations)
//
// "jackstates" adds the Sec. 06 rows beyond the four single inputs (both jacks of one channel, one input per channel, channel-jump patch, no source).
// Matrix (Sec. 25): 4 single-input modes x 3 values per knob (low/mid/high = 2/6/10 on the printed 0-12 scale; 27 combinations)
//                   = 108 conditions x 3 levels (clean, onset, driven) = 324 captures ("core"); "endpoints" adds every knob at 0 and 12 with
//                   the others at mid (24 conditions x 3 levels); "heldout" adds 12 conditions at INTERMEDIATE knob values driven by a
//                   synthetic guitar passage (held-out generalisation set).
// Levels:  clean = 5 mV peak at the jack; onset = input peak where the 1 kHz THD at the speaker terminals reaches 1 % (Metrics.h definition A,
//          searched per condition; if 1 % is not reached below 20 V the cap is used and 'onset_reached' is false); driven = onset + 12 dB (cap 20 V).
// Each capture starts from the engine's solved quiescent state (documented warm-up: none) so every capture is independent of order.
// Outputs: metadata/<id>.json (Sec. 30 minimum capture metadata, raw facts), processed/<id>.json (metrics), raw/<id>_*.wav (with --wav;
//          float32, PHYSICAL units: volts / amps), summary.csv, matrix.json.  Raw and processed are stored separately (Sec. 30).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

#include "CaptureAnalysis.h"

using namespace cd57tool;
namespace fs = std::filesystem;

// engine options shared by every condition (the same programme can be rendered with a perturbed engine to test that the metrics can fail)
static EngineParams gBase;
static std::string gManifest;

struct Cond {
    std::string set; int jack = 1; double inst = 6, mic = 2, tone = 6;
    int jack2 = 0; bool split = false; int patch = 0;      // second plug (split = driven by the same signal, otherwise a terminated cable); jumper cable code (AmpControls::patch)
    std::string label() const    // jack state, e.g. I1, I1+I2s (second plug driven by a split of the stimulus), I1+I2t (terminated), M1~p4 (jumper I1-M2)
    {
        std::string s = jackName(jack);
        if (jack2) s += std::string("+") + jackName(jack2) + (split ? "s" : "t");
        if (patch) s += "~p" + std::to_string(patch);
        return s;
    }
    std::string id() const { return std::string("CM-") + label() + "-i" + knobTag(inst) + "m" + knobTag(mic) + "t" + knobTag(tone) + (set == "heldout" ? "-H" : ""); }
};
static const char* patchName(int p) { static const char* n[] = {"none", "I1-I2", "M1-M2", "I1-M1", "I1-M2", "I2-M1", "I2-M2"}; return n[std::max(0, std::min(p, 6))]; }
struct Capture {
    Cond c; int level = 0; double levelV = 0, onsetV = 0, comp1dBV = 0, ssGainDb = 0; bool onsetReached = false;
    CaptureMetrics m; double b1End = 0, b2End = 0, b3End = 0, kEnd = 0, b1Min = 0, coilK = 0; double itPerStep = 0; uint64_t fallbacks = 0, failures = 0;
    double rmsV = 0, peakV = 0, crest = 0;                // held-out passages
    uint64_t hash = 0; std::string id, setName; double elapsed = 0;
    Vec stim, termV, termI;
    std::vector<Segment> seg;
};

static std::vector<Cond> buildConditions(const std::string& set, bool quick)
{
    std::vector<Cond> v;
    const double kv[3] = {2, 6, 10};
    if (quick) {
        v.push_back({"core", 1, 6, 2, 6}); v.push_back({"core", 2, 10, 2, 2}); v.push_back({"core", 3, 2, 6, 10}); v.push_back({"core", 4, 6, 10, 6});
        return v;
    }
    if (set == "core" || set == "all")
        for (int j = 1; j <= 4; ++j) for (double a : kv) for (double b : kv) for (double t : kv) v.push_back({"core", j, a, b, t});
    if (set == "endpoints" || set == "all")
        for (int j = 1; j <= 4; ++j) for (double e : {0.0, 12.0}) { v.push_back({"endpoints", j, e, 6, 6}); v.push_back({"endpoints", j, 6, e, 6}); v.push_back({"endpoints", j, 6, 6, e}); }
    if (set == "jackstates" || set == "all") {
        // dossier Sec. 06 test table beyond the four single inputs, at mid controls (both channels live): both jacks of one channel, one input per channel,
        // the documented channel-jump patch, no source.  'split' = the same signal on both plugs (a Y-cable), 't' = a terminated plug (guitar at rest)
        struct J { int j, j2; bool split; int patch; };
        const J js[] = {{1, 2, true, 0}, {1, 2, false, 0}, {2, 1, false, 0}, {3, 4, true, 0}, {3, 4, false, 0}, {1, 3, true, 0}, {1, 3, false, 0}, {3, 1, false, 0}, {3, 0, false, 4}, {1, 0, false, 5}, {0, 0, false, 0}};
        for (auto& x : js) { Cond c{"jackstates", x.j, 6, 6, 6}; c.jack2 = x.j2; c.split = x.split; c.patch = x.patch; v.push_back(c); }
    }
    if (set == "heldout" || set == "all") {
        uint32_t s = 20261105u; auto pick = [&]() { static const double vals[6] = {3, 4, 5, 7, 8, 9}; s = s * 1664525u + 1013904223u; return vals[(s >> 16) % 6]; };
        for (int k = 0; k < 12; ++k) v.push_back({"heldout", 1 + (k % 4), pick(), pick(), pick()});
    }
    return v;
}

static EngineParams makeParams(const Cond& c, int os, int load)
{
    EngineParams p = gBase; p.oversampleLog2 = os; p.instVol = (float)(c.inst / 12.0); p.micVol = (float)(c.mic / 12.0); p.tone = (float)(c.tone / 12.0);
    p.jack = c.jack; p.jack2 = c.jack2; p.patch = c.patch; p.load = load; p.inputCalVolts = 1.0f; p.outputRefVolts = 28.28f;
    p.speakerHeating = (load == 0 || load == 1);
    return p;
}

// THD / fundamental of the terminal voltage for a 1 kHz tone of peak `level`, from the quiescent state
static M::LevelPoint toneAt(Rig& w, const Rig& tmpl, double level, bool split)
{
    w.restoreFrom(tmpl);
    const Vec x = sineBurst(w.sr, 1000.0, level, 0.14, 0.02);
    const Vec y = w.run(x, nullptr, nullptr, split ? &x : nullptr);
    const auto t = M::analyseTone(y, w.sr, 1000.0, (size_t)std::llround(0.1 * w.sr), 10);
    return {level, t.fundamental, t.thd};
}

struct Onset { double thd1 = NAN, comp = NAN, ssGainDb = 0; };
static Onset searchOnset(Rig& w, const Rig& tmpl, double cap, bool split)
{
    std::vector<M::LevelPoint> pts;
    double lo = 1e-3, L = 1e-3; bool bracket = false; double hiL = 0;
    // 6 dB steps up to 1 % THD (or the cap), then bisection in log level
    while (true) {
        const auto p = toneAt(w, tmpl, L, split); pts.push_back(p);
        if (p.thd >= 0.01) { hiL = L; bracket = true; break; }
        lo = L; if (L >= cap) break;
        L = std::min(cap, L * 2.0);
    }
    if (bracket && hiL > lo) {
        double a = lo, b = hiL;
        for (int i = 0; i < 4; ++i) { const double m = std::sqrt(a * b); const auto p = toneAt(w, tmpl, m, split); pts.push_back(p); (p.thd >= 0.01 ? b : a) = m; }
    }
    std::sort(pts.begin(), pts.end(), [](const M::LevelPoint& a, const M::LevelPoint& b) { return a.inPeak < b.inPeak; });
    const M::Onset o = M::findOnset(pts, 0.01, 1.0);
    Onset r; r.thd1 = o.thd1Input; r.comp = o.comp1dBInput; r.ssGainDb = M::db20(o.smallSignalGain);
    return r;
}

static void runCondition(const Cond& c, int os, double sr, int load, std::vector<Capture>& out, const std::string& outDir, bool wav)
{
    const EngineParams p = makeParams(c, os, load);
    Rig tmpl(p, sr, 512, gManifest), w(p, sr, 512, gManifest);
    const double cap = 20.0;
    const bool splitDrive = c.jack2 && c.split;
    const Onset on = searchOnset(w, tmpl, cap, splitDrive);
    const bool reached = std::isfinite(on.thd1);
    const double onsetV = reached ? on.thd1 : cap;
    const double levels[3] = {0.005, onsetV, std::min(cap, onsetV * std::pow(10.0, 12.0 / 20.0))};
    for (int li = 0; li < 3; ++li) {
        Capture cp; cp.c = c; cp.level = li; cp.levelV = levels[li]; cp.onsetV = onsetV; cp.onsetReached = reached; cp.comp1dBV = on.comp; cp.ssGainDb = on.ssGainDb;
        cp.id = c.id() + "-" + levelName(li); cp.setName = c.set;
        const auto t0 = std::chrono::steady_clock::now();
        w.restoreFrom(tmpl);
        const auto st0 = w.e.diag();
        Composite comp; Vec passage;
        if (c.set == "heldout") {
            passage = guitarPassage(sr, cp.levelV, 1);
            comp.jackVolts = passage; comp.seg.push_back({"passage", 0, passage.size(), 0, 0, cp.levelV}); comp.hash = fnv1a(passage);
        } else comp = makeComposite(sr, cp.levelV);
        double b1min = 1e9; const Segment* burst = comp.find("burst"); const Segment* probe = comp.find("probe");
        auto onBlock = [&](size_t idx) {
            if (burst && probe && idx >= burst->begin && idx <= probe->begin + (size_t)(0.1 * sr)) b1min = std::min(b1min, w.e.amp().nodeV("b1"));
        };
        Vec cur; const Vec vt = w.run(comp.jackVolts, &cur, onBlock, splitDrive ? &comp.jackVolts : nullptr);
        const auto st1 = w.e.diag();
        cp.hash = comp.hash; cp.seg = comp.seg;
        if (c.set == "heldout") { cp.rmsV = M::rms(vt); cp.peakV = M::peakAbs(vt); cp.crest = cp.peakV / std::max(cp.rmsV, 1e-12); cp.m.peakV = cp.peakV; }
        else cp.m = analyseComposite(comp, vt, cur, sr, 0);
        cp.b1End = st1.b1; cp.b2End = st1.b2; cp.b3End = st1.b3; cp.kEnd = st1.cathode; cp.b1Min = b1min > 1e8 ? st1.b1 : b1min; cp.coilK = st1.coilTempRise;
        const uint64_t steps = st1.steps - st0.steps; cp.itPerStep = steps ? double(st1.iterations - st0.iterations) / double(steps) : 0.0;
        cp.fallbacks = st1.substepped - st0.substepped; cp.failures = st1.failures - st0.failures;
        cp.elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (wav) {
            fs::create_directories(outDir + "/raw");
            writeWavF32(outDir + "/raw/" + cp.id + "_jack_volts.wav", comp.jackVolts, sr);
            writeWavF32(outDir + "/raw/" + cp.id + "_terminal_volts.wav", vt, sr);
            writeWavF32(outDir + "/raw/" + cp.id + "_terminal_amps.wav", cur, sr);
        }
        cp.stim.clear();
        out.push_back(std::move(cp));
    }
}

static std::string nowIso() { std::time_t t = std::time(nullptr); char b[40]; std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t)); return b; }

static void writeMetadata(const Capture& c, const std::string& dir, double sr, int os, int load, const std::string& stamp, bool wav, const std::string& netlistRev, const std::string& profile)
{
    fs::create_directories(dir + "/metadata"); fs::create_directories(dir + "/processed");
    const EngineParams p = makeParams(c.c, os, load);
    const cd57::MarketVariant& mv = cd57::marketVariants()[p.market];
    cd57::PotMap pm; pm.taper = cd57::PotTaper::Audio15;
    static const char* loadName[] = {"speaker_model", "speaker_model_plus_8ohm_extension", "dummy_8_ohm", "dummy_4_ohm", "dummy_16_ohm"};
    std::ostringstream o;
    o << "{\n  \"schema\": \"cd57-capture-v1\",\n  \"id\": " << jstr(c.id) << ",\n  \"set\": " << jstr(c.setName) << ",\n  \"synthetic\": true,\n";
    o << "  \"specimen\": {\"serial\": null, \"model\": \"Fender 57 Custom Deluxe (815050 family, PR 728)\", \"source\": " << jstr("MODEL cd57_nominal netlist rev " + netlistRev + ", manifest profile " + profile) << ", \"tube_markings\": null, \"speaker_label\": null, \"revision\": null},\n";
    o << "  \"datetime_utc\": " << jstr(stamp) << ",\n";
    o << "  \"mains\": {\"volts\": " << jnum(mv.volts) << ", \"hz\": " << jnum(mv.hz) << ", \"line_percent\": 100},\n";
    o << "  \"warmup\": {\"state\": \"solved_quiescent_initial_state\", \"seconds\": 0},\n";
    o << "  \"source_impedance_ohm\": 100, \"source_model\": \"line_DI_ideal_EMF_plus_series_R\",\n";
    o << "  \"jack_state\": {\"guitar_jack\": " << jstr(jackName(c.c.jack)) << ", \"second_plug\": " << jstr(c.c.jack2 ? jackName(c.c.jack2) : "none") << ", \"second_plug_source\": "
      << jstr(c.c.jack2 ? (c.c.split ? "same signal as the first plug (split)" : "terminated cable (guitar at rest)") : "none") << ", \"patch_cable\": " << jstr(patchName(c.c.patch)) << ", \"speaker_load\": " << jstr(loadName[std::min(load, 4)]) << "},\n";
    o << "  \"knobs\": {\"instrument_volume_printed\": " << jnum(c.c.inst) << ", \"mic_volume_printed\": " << jnum(c.c.mic) << ", \"tone_printed\": " << jnum(c.c.tone)
      << ", \"knob_angle_deg\": null, \"pot_electrical_fraction\": {\"instrument\": " << jnum(pm.alpha(c.c.inst / 12.0)) << ", \"mic\": " << jnum(pm.alpha(c.c.mic / 12.0)) << ", \"tone\": " << jnum(c.c.tone / 12.0)
      << "}, \"pot_map_status\": \"assumed (audio 15 % taper for volumes, linear tone): numerical taper of the modern amplifier is UNKNOWN\"},\n";
    o << "  \"stimulus\": {\"id\": " << jstr(c.setName == "heldout" ? "synthetic-guitar-passage-v1" : "composite-v1") << ", \"hash_fnv1a_float32\": " << jstr(hex64(c.hash)) << ", \"sample_rate_hz\": " << jnum(sr) << ", \"peak_volts_at_jack\": " << jnum(c.levelV)
      << ", \"level\": " << jstr(levelName(c.level)) << ", \"segments\": [";
    for (size_t i = 0; i < c.seg.size(); ++i) o << (i ? ", " : "") << "{\"name\": " << jstr(c.seg[i].name) << ", \"begin\": " << c.seg[i].begin << ", \"end\": " << c.seg[i].end << ", \"f1_hz\": " << jnum(c.seg[i].f1) << ", \"f2_hz\": " << jnum(c.seg[i].f2) << ", \"peak_volts\": " << jnum(c.seg[i].level) << "}";
    o << "]},\n";
    o << "  \"calibration\": {\"volts_per_full_scale_input_sample\": " << jnum(p.inputCalVolts) << ", \"output_reference_volts_per_full_scale\": " << jnum(p.outputRefVolts) << ", \"output_attenuation_db\": null, \"fixed_delay_samples_removed\": 0, \"note\": \"model latency compensated inside the rig\"},\n";
    o << "  \"adc_dac\": {\"model\": null, \"gain\": null, \"note\": \"synthetic render: no converter\"},\n";
    o << "  \"sample_rate_hz\": " << jnum(sr) << ",\n  \"microphone_pose\": null,\n";
    o << "  \"thermal_state\": {\"speaker\": \"cold\", \"coil_temperature_rise_K_end\": " << jnum(c.coilK) << ", \"heating_modelled\": " << jbool(p.speakerHeating) << "},\n";
    o << "  \"ambient\": null,\n  \"operator_notes\": \"Rendered by cd57_capture_matrix. Level 'onset' = 1 kHz THD of 1 % at the speaker terminals" << (c.onsetReached ? "" : "; NOT reached below the 20 V cap, the cap is used") << ".\",\n";
    o << "  \"model\": {\"oversampling\": " << (1 << os) << ", \"netlist_rev\": " << jstr(netlistRev) << ", \"profile\": " << jstr(profile) << "},\n";
    o << "  \"files\": {\"raw\": " << (wav ? "{\"jack_volts\": " + jstr("raw/" + c.id + "_jack_volts.wav") + ", \"terminal_volts\": " + jstr("raw/" + c.id + "_terminal_volts.wav") + ", \"terminal_amps\": " + jstr("raw/" + c.id + "_terminal_amps.wav") + "}" : std::string("null"))
      << ", \"processed\": " << jstr("processed/" + c.id + ".json") << "}\n}\n";
    std::ofstream(dir + "/metadata/" + c.id + ".json") << o.str();

    std::ostringstream q;
    const CaptureMetrics& m = c.m;
    q << "{\n  \"id\": " << jstr(c.id) << ",\n  \"onset\": {\"definition\": \"1 kHz THD = 1 % at the speaker terminals, input peak volts\", \"input_peak_V\": " << jnum(c.onsetV) << ", \"reached\": " << jbool(c.onsetReached)
      << ", \"comp_1dB_input_peak_V\": " << jnum(c.comp1dBV) << ", \"small_signal_gain_db\": " << jnum(c.ssGainDb) << "},\n";
    if (c.setName == "heldout") q << "  \"passage\": {\"rms_V\": " << jnum(c.rmsV) << ", \"peak_V\": " << jnum(c.peakV) << ", \"crest\": " << jnum(c.crest) << "},\n";
    else {
        q << "  \"noise_floor\": {\"rms_V\": " << jnum(m.floorRmsV) << ", \"peak_V\": " << jnum(m.floorPkV) << "},\n";
        q << "  \"tone_1k\": {\"gain_db\": " << jnum(m.toneGainDb) << ", \"rms_V\": " << jnum(m.toneRmsV) << ", \"peak_V\": " << jnum(m.tonePeakV) << ", \"power_W\": " << jnum(m.powerW) << ", \"current_rms_A\": " << jnum(m.currentRmsA) << ", \"thd_pct\": " << jnum(m.thdPct) << ", \"harmonics_dbc\": [";
        for (size_t k = 1; k < m.tone.dbc.size(); ++k) q << (k > 1 ? ", " : "") << jnum(m.tone.dbc[k]);
        q << "]},\n  \"imd_700_1000\": {\"total_dbc\": " << jnum(m.imd.totalDb) << ", \"products\": [";
        for (size_t k = 0; k < m.imd.products.size(); ++k) q << (k ? ", " : "") << "{\"a\": " << m.imd.products[k].a << ", \"b\": " << m.imd.products[k].b << ", \"hz\": " << jnum(m.imd.products[k].hz) << ", \"dbc\": " << jnum(m.imd.products[k].dbc) << "}";
        q << "]},\n  \"sweep\": {\"bandwidth_lo_hz\": " << jnum(m.bwLoHz) << ", \"bandwidth_hi_hz\": " << jnum(m.bwHiHz) << ", \"response\": [";
        for (size_t k = 0; k < m.resp.f.size(); ++k) q << (k ? ", " : "") << "[" << jnum(m.resp.f[k], 6) << ", " << jnum(m.resp.magDb[k], 6) << ", " << jnum(m.resp.phaseRad[k], 6) << "]";
        q << "], \"thd_vs_f\": [";
        for (size_t k = 0; k < m.thdSweepF.size(); ++k) q << (k ? ", " : "") << "[" << jnum(m.thdSweepF[k]) << ", " << jnum(m.thdSweepPct[k]) << "]";
        q << "]},\n  \"burst_probe\": {\"burst_peak_V\": " << jnum(m.burstPeakV) << ", \"probe_gain_db\": " << jnum(m.probeGainDb) << ", \"dip_db\": " << jnum(m.dipDb) << ", \"within_10pct_ms\": " << jnum(m.rec.tWithin10 * 1e3)
          << ", \"within_5pct_ms\": " << jnum(m.rec.tWithin5 * 1e3) << ", \"tau_ms\": " << jnum(m.rec.tau * 1e3) << ", \"settled_V\": " << jnum(m.rec.settled) << "},\n";
    }
    q << "  \"rails_end\": {\"b1_V\": " << jnum(c.b1End) << ", \"b2_V\": " << jnum(c.b2End) << ", \"b3_V\": " << jnum(c.b3End) << ", \"k34_V\": " << jnum(c.kEnd) << ", \"b1_min_V\": " << jnum(c.b1Min) << "},\n";
    q << "  \"solver\": {\"iterations_per_step\": " << jnum(c.itPerStep) << ", \"substep_fallbacks\": " << c.fallbacks << ", \"unrecovered_failures\": " << c.failures << "}\n}\n";
    std::ofstream(dir + "/processed/" + c.id + ".json") << q.str();
}

static std::string csvHeader()
{
    return "id,set,jack,inst12,mic12,tone12,level,level_V,onset_V,onset_reached,comp1dB_V,small_signal_gain_dB,floor_mV,tone_gain_dB,tone_rms_V,tone_peak_V,power_W,thd_pct,h2_dbc,h3_dbc,h4_dbc,h5_dbc,"
           "imd_total_dbc,imd_2f1mf2_dbc,resp_100_dB,resp_400_dB,resp_1k_dB,resp_4k_dB,resp_8k_dB,bw_lo_Hz,bw_hi_Hz,thd_sweep_400_pct,thd_sweep_1k_pct,burst_peak_V,probe_gain_dB,dip_dB,t10_ms,t5_ms,tau_ms,"
           "b1_end_V,b1_min_V,k34_end_V,i_rms_A,passage_rms_V,passage_peak_V,iter_per_step,fallbacks,failures";
}
static std::string csvRow(const Capture& c)
{
    const CaptureMetrics& m = c.m;
    auto at = [&](double f) { if (m.resp.f.empty()) return std::string(""); size_t bi = 0; double bd = 1e9; for (size_t i = 0; i < m.resp.f.size(); ++i) { const double d = std::fabs(std::log(m.resp.f[i] / f)); if (d < bd) { bd = d; bi = i; } } return jnum(m.resp.magDb[bi], 7); };
    auto h = [&](size_t k) { return m.tone.dbc.size() > k ? jnum(m.tone.dbc[k], 7) : std::string(""); };
    double im = NAN; for (auto& p : m.imd.products) if (p.a == 2 && p.b == -1) im = p.dbc;
    const bool ho = c.setName == "heldout";
    auto thd = [&](size_t i) { return m.thdSweepPct.size() > i ? jnum(m.thdSweepPct[i], 7) : std::string(""); };
    std::ostringstream o;
    o << c.id << "," << c.setName << "," << c.c.label() << "," << c.c.inst << "," << c.c.mic << "," << c.c.tone << "," << levelName(c.level) << "," << jnum(c.levelV, 8) << "," << jnum(c.onsetV, 8) << "," << (c.onsetReached ? 1 : 0) << "," << jnum(c.comp1dBV, 8) << "," << jnum(c.ssGainDb, 7) << ","
      << (ho ? "" : jnum(m.floorRmsV * 1e3, 7)) << "," << (ho ? "" : jnum(m.toneGainDb, 7)) << "," << (ho ? "" : jnum(m.toneRmsV, 8)) << "," << (ho ? "" : jnum(m.tonePeakV, 8)) << "," << (ho ? "" : jnum(m.powerW, 8)) << "," << (ho ? "" : jnum(m.thdPct, 7)) << ","
      << (ho ? "" : h(1)) << "," << (ho ? "" : h(2)) << "," << (ho ? "" : h(3)) << "," << (ho ? "" : h(4)) << "," << (ho ? "" : jnum(m.imd.totalDb, 7)) << "," << (ho ? "" : jnum(im, 7)) << ","
      << (ho ? "" : at(100)) << "," << (ho ? "" : at(400)) << "," << (ho ? "" : at(1000)) << "," << (ho ? "" : at(4000)) << "," << (ho ? "" : at(8000)) << "," << (ho ? "" : jnum(m.bwLoHz, 6)) << "," << (ho ? "" : jnum(m.bwHiHz, 6)) << ","
      << (ho ? "" : thd(2)) << "," << (ho ? "" : thd(3)) << "," << (ho ? "" : jnum(m.burstPeakV, 8)) << "," << (ho ? "" : jnum(m.probeGainDb, 7)) << "," << (ho ? "" : jnum(m.dipDb, 7)) << "," << (ho ? "" : jnum(m.rec.tWithin10 * 1e3, 6)) << ","
      << (ho ? "" : jnum(m.rec.tWithin5 * 1e3, 6)) << "," << (ho ? "" : jnum(m.rec.tau * 1e3, 6)) << "," << jnum(c.b1End, 7) << "," << jnum(c.b1Min, 7) << "," << jnum(c.kEnd, 7) << "," << (ho ? "" : jnum(m.currentRmsA, 7)) << ","
      << (ho ? jnum(c.rmsV, 8) : "") << "," << (ho ? jnum(c.peakV, 8) : "") << "," << jnum(c.itPerStep, 5) << "," << c.fallbacks << "," << c.failures;
    return o.str();
}

int main(int argc, char** argv)
{
    std::string outDir = "capture_out", set = "all", only, stamp, summary;
    bool quick = false, wav = false; double sr = 48000.0; int os = 1, load = 2, threads = (int)std::max(1u, std::thread::hardware_concurrency());
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i]; auto nx = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--out") outDir = nx(); else if (a == "--set") set = nx(); else if (a == "--quick") quick = true; else if (a == "--wav") wav = true;
        else if (a == "--sr") sr = std::atof(nx().c_str()); else if (a == "--os") os = std::atoi(nx().c_str()); else if (a == "--load") load = std::atoi(nx().c_str());
        else if (a == "--line") gBase.linePercent = (float)std::atof(nx().c_str()); else if (a == "--taper") gBase.taper = std::atoi(nx().c_str());
        else if (a == "--market") gBase.market = std::atoi(nx().c_str()); else if (a == "--v1") gBase.v1 = std::atoi(nx().c_str()); else if (a == "--v2") gBase.v2 = std::atoi(nx().c_str());
        else if (a == "--rect") gBase.rectifier = std::atoi(nx().c_str());
        else if (a == "--manifest") { std::ifstream mf(nx()); gManifest.assign((std::istreambuf_iterator<char>(mf)), std::istreambuf_iterator<char>()); if (gManifest.empty()) { std::cerr << "cannot read manifest\n"; return 2; } }
        else if (a == "--threads") threads = std::max(1, std::atoi(nx().c_str())); else if (a == "--only") only = nx(); else if (a == "--timestamp") stamp = nx(); else if (a == "--summary") summary = nx();
        else { std::cerr << "unknown option " << a << "\n"; return 2; }
    }
    if (stamp.empty()) stamp = nowIso();
    std::vector<Cond> conds = buildConditions(set, quick);
    if (!only.empty()) conds.erase(std::remove_if(conds.begin(), conds.end(), [&](const Cond& c) { return c.id().find(only) == std::string::npos; }), conds.end());
    fs::create_directories(outDir);
    cd57::Network nn; nn.parse(cd57::embeddedNetlist()); const std::string netRev = nn.revision;
    cd57::Manifest man; man.load(cd57::embeddedManifest());
    std::cerr << conds.size() << " conditions x 3 levels = " << conds.size() * 3 << " captures, " << threads << " threads, host " << sr << " Hz, oversampling " << (1 << os) << "x, load " << load << "\n";

    std::vector<std::vector<Capture>> res(conds.size());
    std::atomic<size_t> next{0}, done{0};
    auto worker = [&]() { for (;;) { const size_t i = next++; if (i >= conds.size()) break; runCondition(conds[i], os, sr, load, res[i], outDir, wav); const size_t d = ++done; if (d % 8 == 0 || d == conds.size()) std::cerr << "  " << d << "/" << conds.size() << " conditions\n"; } };
    std::vector<std::thread> th; for (int t = 0; t < threads; ++t) th.emplace_back(worker);
    for (auto& t : th) t.join();

    std::vector<Capture> all; for (auto& v : res) for (auto& c : v) all.push_back(std::move(c));
    std::sort(all.begin(), all.end(), [](const Capture& a, const Capture& b) { return a.id < b.id; });
    std::ofstream csv(summary.empty() ? outDir + "/summary.csv" : summary);
    csv << csvHeader() << "\n";
    uint64_t fails = 0, fall = 0; size_t notReached = 0;
    for (auto& c : all) { writeMetadata(c, outDir, sr, os, load, stamp, wav, netRev, man.profile); csv << csvRow(c) << "\n"; fails += c.failures; fall += c.fallbacks; if (c.level == 1 && !c.onsetReached) ++notReached; }
    std::ostringstream mj;
    mj << "{\n  \"schema\": \"cd57-capture-matrix-v1\",\n  \"captures\": " << all.size() << ",\n  \"conditions\": " << conds.size() << ",\n  \"set\": " << jstr(quick ? "quick" : set) << ",\n  \"sample_rate_hz\": " << jnum(sr) << ",\n  \"oversampling\": " << (1 << os)
       << ",\n  \"load\": " << load << ",\n  \"onset_definition\": \"1 kHz THD = 1 % at the speaker terminals\",\n  \"onset_cap_V\": 20,\n  \"ids\": [";
    for (size_t i = 0; i < all.size(); ++i) mj << (i ? ", " : "") << jstr(all[i].id);
    mj << "]\n}\n";
    std::ofstream(outDir + "/matrix.json") << mj.str();
    std::cerr << all.size() << " captures written to " << outDir << "; onset not reached below the cap in " << notReached << " conditions; substep fallbacks " << fall << ", unrecovered solver failures " << fails << "\n";
    return fails ? 3 : 0;
}
