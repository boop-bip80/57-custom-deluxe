// cd57_sensitivity -- one-parameter-at-a-time sensitivity of the model's headline features (dossier Sec. 13, 28).
//
//   "Perform sensitivity analysis around the verified baseline before a Monte Carlo study.  Vary one parameter or a physically correlated group,
//    observe the output feature it changes, and prioritize measurements accordingly.  Do not assign unrelated random distributions to parameters
//    constrained by the same component or measurement."                                                                                  (Sec. 13)
//   "Use null/unknown in the parameter manifest, or an explicit assumed value with a sensitivity interval."                               (Sec. 28)
//
// Every parameter below is moved to the LOW and HIGH end of its stated range while everything else stays at the nominal model, and the features
// listed in the report are measured again.  Ranges come from, in this order: the manifest's own sensitivity interval (assumed entries),
// the netlist's per-element tolerance, a documented alternative (e.g. 0.02 vs 0.022 uF), or a PROPOSED width stated in the table.  Every effect is
// expressed in dB so that the parameters rank on a single scale; the score is the largest |effect| over all features.  The result is a measurement
// priority list: the parameters whose unknown value can move a feature by more than the Sec. 26 tolerance (0.5 dB clean response, 1 dB harmonics,
// 0.5 dB onset) are the ones a specimen measurement must pin down first.
//
//   cd57_sensitivity [--report FILE.md] [--csv FILE.csv] [--threads N] [--quick]
#include <algorithm>
#include <atomic>
#include <iostream>
#include <thread>

#include "ToolCommon.h"

using namespace cd57tool;
using cd57::AmpCircuit;
using cd57::Network;

// ------------------------------------------------------------------------------------------------ perturbations
struct Scale { std::string name; double f; };
struct Variant {
    EngineParams p;
    std::string manifest;                       // modified manifest text
    std::vector<Scale> res, cap, ind, tube;     // multiplicative perturbations applied after construction (R/C/L via .var, tubes via .scale)
    bool capAllElectrode = false; double electrodeFactor = 1.0;
    bool tubeAll = false;
};

static std::string setManifestValue(const std::string& text, const std::string& key, double v)
{
    const std::string k = "\"" + key + "\"";
    size_t p = text.find(k);
    if (p == std::string::npos) { std::cerr << "manifest key not found: " << key << "\n"; std::exit(3); }
    const size_t q = text.find("\"value\":", p);
    size_t a = q + 8; while (text[a] == ' ') ++a;
    size_t b = a; while (b < text.size() && text[b] != ',' && text[b] != '\n' && text[b] != '}') ++b;
    char buf[64]; std::snprintf(buf, sizeof buf, "%.9g", v);
    return text.substr(0, a) + buf + text.substr(b);
}

struct Param {
    std::string name, group, source, unit;
    double nominal, lo, hi;
    std::function<void(Variant&, double)> apply;
    bool oneSided = false;                      // categorical alternative: only the 'hi' state differs from nominal
};

static std::vector<Param> buildParams(const cd57::Manifest& man)
{
    std::vector<Param> P;
    auto man2 = [&](const char* key, const char* group, const char* unit) {
        const cd57::ManifestEntry* e = man.find(key);
        const std::string k = key;
        if (!e || !e->hasValue || !e->hasInterval) { std::cerr << "manifest entry '" << key << "' has no numeric value and sensitivity interval: refusing to run with a made-up range\n"; std::exit(3); }
        P.push_back({k, group, "manifest interval", unit, e->value, e->lo, e->hi, [k](Variant& v, double x) { v.manifest = setManifestValue(v.manifest, k, x); }, false});
    };
    auto elem = [&](const char* label, const char* group, const char* source, std::vector<std::string> res, std::vector<std::string> cap, double fLo, double fHi) {
        P.push_back({label, group, source, "x nominal", 1.0, fLo, fHi, [res, cap](Variant& v, double f) { for (auto& n : res) v.res.push_back({n, f}); for (auto& n : cap) v.cap.push_back({n, f}); }, false});
    };
    // --- input
    P.push_back({"source_impedance (line/DI mode)", "Input level / source impedance", "manifest interval 0-10 kOhm (1 Ohm floor)", "ohm", 100.0, 1.0, 10000.0, [](Variant& v, double x) { v.p.sourceR = (float)x; }, false});
    P.push_back({"source = passive pickup + cable (6 kohm, 2.5 H, 500 pF)", "Input level / source impedance", "categorical alternative (plugin source model)", "-", 0, 0, 1, [](Variant& v, double) { v.p.sourceMode = 1; }, true});
    // --- controls
    man2("pot_total_resistance", "Control taper / total resistance", "ohm");
    P.push_back({"volume_taper_audio10", "Control taper / total resistance", "alternative law: alpha(0.5) = 0.10 instead of 0.15", "-", 0, 0, 1, [](Variant& v, double) { v.p.taper = 1; }, true});
    P.push_back({"volume_taper_linear", "Control taper / total resistance", "alternative law: linear", "-", 0, 0, 1, [](Variant& v, double) { v.p.taper = 2; }, true});
    {   // pot_endpoint_contact_resistance has no manifest interval ('proposed'): PROPOSED x0.25 / x4 around the nominal
        const cd57::ManifestEntry* e = man.find("pot_endpoint_contact_resistance");
        P.push_back({"pot_endpoint_contact_resistance", "Control taper / total resistance", "PROPOSED x0.25 / x4", "ohm", e->value, e->value * 0.25, e->value * 4.0,
                     [](Variant& v, double x) { v.manifest = setManifestValue(v.manifest, "pot_endpoint_contact_resistance", x); }, false});
    }
    // --- tubes (specimen operating point): current scale of each device's characteristic
    auto tubeScale = [&](const char* label, std::vector<std::string> names, double lo, double hi, const char* src) {
        P.push_back({label, "Tube operating point / grid current", src, "x nominal current", 1.0, lo, hi, [names](Variant& v, double f) { for (auto& n : names) v.tube.push_back({n, f}); }, false});
    };
    tubeScale("V1 (12AY7 pair) current scale", {"V1A", "V1B"}, 0.92, 1.08, "PROPOSED +-8 % (the specimen-variation width)");
    tubeScale("V2A (12AX7 gain stage) current scale", {"V2A"}, 0.92, 1.08, "PROPOSED +-8 %");
    tubeScale("V2B (cathodyne) current scale", {"V2B"}, 0.92, 1.08, "PROPOSED +-8 %");
    tubeScale("V3+V4 (6V6 pair, together) current scale", {"V3", "V4"}, 0.94, 1.06, "PROPOSED +-6 %");
    P.push_back({"V3 vs V4 mismatch (V3 +x, V4 -x)", "Tube operating point / grid current", "PROPOSED +-6 % (opposite signs)", "x nominal current", 1.0, 0.94, 1.06,
                 [](Variant& v, double f) { v.tube.push_back({"V3", f}); v.tube.push_back({"V4", 2.0 - f}); }, false});
    P.push_back({"V1 type = 12AX7 (tube swap)", "Tube operating point / grid current", "categorical alternative", "-", 0, 0, 1, [](Variant& v, double) { v.p.v1 = 1; }, true});
    P.push_back({"electrode capacitances (all) scale", "Tube operating point / grid current", "PROPOSED x0.5 / x2 (datasheet class, not in the dossier)", "x nominal", 1.0, 0.5, 2.0,
                 [](Variant& v, double f) { v.capAllElectrode = true; v.electrodeFactor = f; }, false});
    // --- supply
    man2("pt_hv_half_winding_rms", "Supply resistance / capacitance", "V");
    man2("pt_series_resistance_per_half", "Supply resistance / capacitance", "ohm");
    elem("filter capacitors C1-C3 (together)", "Supply resistance / capacitance", "netlist tol 20 %", {}, {"C1", "C2", "C3"}, 0.8, 1.2);
    elem("dropping resistors 5k, 22k (together)", "Supply resistance / capacitance", "netlist tol 5 %", {"Rdrop1", "Rdrop2"}, {}, 0.95, 1.05);
    P.push_back({"rectifier = silicon diodes", "Supply resistance / capacitance", "categorical alternative (lab option)", "-", 0, 0, 1, [](Variant& v, double) { v.p.rectifier = 1; }, true});
    P.push_back({"line voltage", "Supply resistance / capacitance", "PROPOSED +-5 % mains", "% of nominal", 100.0, 95.0, 105.0, [](Variant& v, double x) { v.p.linePercent = (float)x; }, false});
    // --- output transformer
    {   // the turns ratio has no manifest interval ('historical_nominal'): PROPOSED +-3 %
        const cd57::ManifestEntry* e = man.find("ot_turns_ratio_full_primary");
        P.push_back({"ot_turns_ratio_full_primary", "Transformer low-frequency behavior", "PROPOSED +-3 % ratio (S10 identity with the OEM part unproven)", "ratio", e->value, e->value * 0.97, e->value * 1.03,
                     [](Variant& v, double x) { v.manifest = setManifestValue(v.manifest, "ot_turns_ratio_full_primary", x); }, false});
    }
    man2("ot_primary_inductance_full", "Transformer low-frequency behavior", "H");
    man2("ot_leakage_inductance_full_ref", "Transformer low-frequency behavior", "H");
    man2("ot_dcr_primary_half", "Transformer low-frequency behavior", "ohm");
    man2("ot_dcr_secondary", "Transformer low-frequency behavior", "ohm");
    // --- speaker
    man2("speaker_Re", "Speaker complex impedance", "ohm");
    man2("speaker_Le", "Speaker complex impedance", "H");
    man2("speaker_fs", "Speaker complex impedance", "Hz");
    man2("speaker_Qms", "Speaker complex impedance", "-");
    man2("speaker_Qes", "Speaker complex impedance", "-");
    man2("speaker_Mms", "Speaker complex impedance", "kg");
    // --- passives (circuit values; the dossier's table is HISTORICAL)
    elem("preamp coupling caps 0.1 uF x2 (together)", "Passive values (historical baseline)", "netlist tol 10 %", {}, {"Cc_1b", "Cc_1n"}, 0.9, 1.1);
    elem("gain-stage coupling cap 0.022 uF (0.02 uF = drawing value)", "Passive values (historical baseline)", "documented alternative .02 vs 0.022", {}, {"Cc_2"}, 0.02 / 0.022, 1.1);
    elem("output coupling caps 0.1 uF x2 (together)", "Passive values (historical baseline)", "netlist tol 10 %", {}, {"Cc_3", "Cc_4"}, 0.9, 1.1);
    elem("bright 500 pF", "Passive values (historical baseline)", "netlist tol 10 %", {}, {"Cbright"}, 0.9, 1.1);
    elem("tone shunt 5 nF", "Passive values (historical baseline)", "netlist tol 10 %", {}, {"Ctone"}, 0.9, 1.1);
    elem("cathode bypass caps 25 uF (all three)", "Passive values (historical baseline)", "netlist tol 20 %", {}, {"Ck1", "Ck2a", "Ck34"}, 0.8, 1.2);
    elem("plate loads 100k/100k/100k/56k (together)", "Passive values (historical baseline)", "netlist tol 5 %", {"Rpl_1b", "Rpl_1n", "Rpl_2a", "Rpl_2b"}, {}, 0.95, 1.05);
    elem("cathode resistors 820/1.5k/1.5k/56k/250 (together)", "Passive values (historical baseline)", "netlist tol 5 %", {"Rk1", "Rk2a", "Rkb", "Rtail", "Rk34"}, {}, 0.95, 1.05);
    elem("output grid leaks 220k x2 (together)", "Passive values (historical baseline)", "netlist tol 5 %", {"Rgl_3", "Rgl_4"}, {}, 0.95, 1.05);
    elem("input grid stoppers 68k x4 (together)", "Passive values (historical baseline)", "netlist tol 5 %", {"Rb_a", "Rb_b", "Rn_a", "Rn_b"}, {}, 0.95, 1.05);
    elem("output grid stoppers 1.5k x2 (together)", "Passive values (historical baseline)", "netlist tol 5 %", {"Rgs_3", "Rgs_4"}, {}, 0.95, 1.05);
    return P;
}

// ------------------------------------------------------------------------------------------------ measurement
struct Features {
    double gain1k = 0, gainLF = 0, gainHF = 0;                 // dB (AC, 8 ohm): 1 kHz, 100 Hz re 1 kHz, 5 kHz re 1 kHz
    double onsetV = 0, p5 = 0, thdFix = 0, h2Fix = 0, h3Fix = 0;
    double b1 = 0, vk = 0, hum = 0, dip = 0;
    double gain1kSpk = 0, gainLFSpk = 0, thdFixSpk = 0, powerFixSpk = 0;
    bool ok = true;
};
static const char* kFeatureName[] = {"gain 1 kHz (8 ohm)", "bass shape 100 Hz re 1 kHz", "treble shape 5 kHz re 1 kHz", "clipping onset (1 % THD input)", "power at 5 % THD", "THD at fixed drive",
                                     "H2 at fixed drive", "H3 at fixed drive", "idle B+1", "hum at the terminals", "post-burst probe dip", "gain 1 kHz (speaker)", "bass shape (speaker)", "THD at fixed drive (speaker)", "output level at fixed drive (speaker)"};
constexpr int kNF = 15;
// Sec. 26 tolerances used to judge an effect: clean response / onset / level 0.5 dB, harmonics and distortion 1 dB; hum and recovery dip are judged at 1 dB
static const double kTol[kNF] = {0.5, 0.5, 0.5, 0.5, 0.5, 1.0, 1.0, 1.0, 0.5, 1.0, 1.0, 0.5, 0.5, 1.0, 0.5};

static std::unique_ptr<Rig> makeRig(const Variant& v, double sr, int load)
{
    EngineParams p = v.p; p.load = load; p.speakerHeating = false;
    auto r = std::make_unique<Rig>(p, sr, 512, v.manifest);
    Network& n = r->e.amp().network();
    for (auto& s : v.res) { bool f = false; for (auto& e : n.res()) if (e.m.name == s.name) { e.var *= s.f; f = true; } if (!f) { std::cerr << "no resistor " << s.name << "\n"; std::exit(3); } }
    for (auto& s : v.cap) { bool f = false; for (auto& e : n.cap()) if (e.m.name == s.name) { e.var *= s.f; f = true; } if (!f) { std::cerr << "no capacitor " << s.name << "\n"; std::exit(3); } }
    for (auto& s : v.tube) { bool f = false; for (auto& e : n.tri()) if (e.m.name == s.name) { e.scale *= s.f; f = true; } for (auto& e : n.pen()) if (e.m.name == s.name) { e.scale *= s.f; f = true; } if (!f) { std::cerr << "no tube " << s.name << "\n"; std::exit(3); } }
    if (v.capAllElectrode) for (auto& e : n.cap()) if (e.m.name.rfind("Cgk_", 0) == 0 || e.m.name.rfind("Cgp_", 0) == 0 || e.m.name.rfind("Cpk_", 0) == 0) e.var *= v.electrodeFactor;
    n.refreshLinear();
    r->e.reset();                                   // re-solve the quiescent state of the perturbed circuit
    return r;
}

static double acGain(Rig& r, double f)
{
    Network& n = r.e.amp().network();
    const int pu = n.nodeId("pu");
    const auto v = n.acSolve(f, pu);
    return 20.0 * std::log10(std::abs(v[(size_t)n.nodeId("sp")] / n.getR(n.resId("Rsrc"))) + 1e-30);
}

struct Pt { double in, fund, thd, rmsV; };
static Pt toneAt(Rig& w, const Rig& tmpl, double level)
{
    w.restoreFrom(tmpl);
    const Vec y = w.run(sineBurst(w.sr, 1000.0, level, 0.14, 0.02));
    const auto t = M::analyseTone(y, w.sr, 1000.0, (size_t)std::llround(0.1 * w.sr), 10);
    return {level, t.fundamental, t.thd, t.rms};
}

static Features measure(const Variant& v, double sr)
{
    Features F;
    auto A = makeRig(v, sr, 2), Aw = makeRig(v, sr, 2);
    const double g1k = acGain(*A, 1000.0);
    F.gain1k = g1k; F.gainLF = acGain(*A, 100.0) - g1k; F.gainHF = acGain(*A, 5000.0) - g1k;
    const auto d0 = A->e.diag(); F.b1 = d0.b1; F.vk = d0.cathode;
    // hum floor
    { Vec z((size_t)(0.25 * sr), 0.0); const Vec y = A->run(z); F.hum = M::rms(Vec(y.begin() + (long)(0.05 * sr), y.end())); }
    A->restoreFrom(*Aw);                            // (A was used for the hum run; Aw is still the pristine quiescent template)
    // level search: 3 dB steps, then bisection for 1 % and 5 % THD
    std::vector<Pt> pts; double lo1 = 1e-3, hi1 = 0, lo5 = 0, hi5 = 0, L = 1e-3;
    while (L <= 20.0) {
        const Pt p = toneAt(*A, *Aw, L); pts.push_back(p);
        if (!hi1 && p.thd >= 0.01) hi1 = L; else if (!hi1) lo1 = L;
        if (p.thd >= 0.05) { hi5 = L; break; }
        lo5 = L; L *= std::pow(10.0, 3.0 / 20.0);
    }
    if (!hi1 || !hi5) { F.ok = false; return F; }
    auto bisect = [&](double a, double b, double target) { for (int i = 0; i < 5; ++i) { const double m = std::sqrt(a * b); const Pt p = toneAt(*A, *Aw, m); pts.push_back(p); (p.thd >= target ? b : a) = m; } };
    bisect(lo1, hi1, 0.01); bisect(lo5, hi5, 0.05);
    std::sort(pts.begin(), pts.end(), [](const Pt& a, const Pt& b) { return a.in < b.in; });
    auto crossing = [&](double target, double* powerOut) {
        for (size_t i = 1; i < pts.size(); ++i) if (pts[i - 1].thd < target && pts[i].thd >= target) {
            const double a = (std::log(target) - std::log(pts[i - 1].thd)) / (std::log(pts[i].thd) - std::log(pts[i - 1].thd));
            if (powerOut) { const double p0 = pts[i - 1].rmsV * pts[i - 1].rmsV / 8.0, p1 = pts[i].rmsV * pts[i].rmsV / 8.0; *powerOut = std::exp(std::log(p0) + a * (std::log(p1) - std::log(p0))); }
            return std::exp(std::log(pts[i - 1].in) + a * (std::log(pts[i].in) - std::log(pts[i - 1].in)));
        }
        return std::numeric_limits<double>::quiet_NaN();
    };
    double p1 = 0; F.onsetV = crossing(0.01, &p1); F.p5 = 0; crossing(0.05, &F.p5);
    // fixed drive (12 mV peak at the jack)
    {
        A->restoreFrom(*Aw); const Vec y = A->run(sineBurst(sr, 1000.0, 0.012, 0.14, 0.02));
        const auto t = M::analyseTone(y, sr, 1000.0, (size_t)std::llround(0.1 * sr), 10);
        F.thdFix = t.thd; F.h2Fix = t.dbc[1]; F.h3Fix = t.dbc[2];
    }
    // post-burst probe dip: 40 mV burst (150 ms) then a quiet 1 kHz probe (-40 dB)
    {
        A->restoreFrom(*Aw);
        Vec x = sineBurst(sr, 1000.0, 0.04, 0.15, 0.005); const Vec pr = sineBurst(sr, 1000.0, 0.0004, 0.5, 0.002); x.insert(x.end(), pr.begin(), pr.end());
        const Vec y = A->run(x); const Vec yp(y.begin() + (long)(0.15 * sr), y.end());
        const auto env = M::lockinEnvelope(yp, sr, 1000.0, 4, 0.001); const auto r = M::recoveryLandmarks(env, 0.006, 0.1);
        F.dip = M::db20(std::max(r.minRel, 1e-6));
    }
    // with the speaker model as the load
    {
        auto S = makeRig(v, sr, 0), Sw = makeRig(v, sr, 0);
        const double s1k = acGain(*S, 1000.0); F.gain1kSpk = s1k; F.gainLFSpk = acGain(*S, 100.0) - s1k;
        Sw->restoreFrom(*S);
        const Vec y = Sw->run(sineBurst(sr, 1000.0, 0.012, 0.14, 0.02));
        const auto t = M::analyseTone(y, sr, 1000.0, (size_t)std::llround(0.1 * sr), 10);
        F.thdFixSpk = t.thd; F.powerFixSpk = t.rms * t.rms / 8.0;
    }
    return F;
}

static double effectDb(int i, const Features& a, const Features& n)       // a vs nominal n, in dB (signed)
{
    auto r20 = [](double x, double y) { return 20.0 * std::log10(std::max(x, 1e-30) / std::max(y, 1e-30)); };
    switch (i) {
        case 0: return a.gain1k - n.gain1k; case 1: return a.gainLF - n.gainLF; case 2: return a.gainHF - n.gainHF;
        case 3: return r20(a.onsetV, n.onsetV); case 4: return 10.0 * std::log10(std::max(a.p5, 1e-30) / std::max(n.p5, 1e-30));
        case 5: return r20(a.thdFix, n.thdFix); case 6: return a.h2Fix - n.h2Fix; case 7: return a.h3Fix - n.h3Fix;
        case 8: return r20(a.b1, n.b1); case 9: return r20(a.hum, n.hum); case 10: return a.dip - n.dip;
        case 11: return a.gain1kSpk - n.gain1kSpk; case 12: return a.gainLFSpk - n.gainLFSpk; case 13: return r20(a.thdFixSpk, n.thdFixSpk);
        default: return 10.0 * std::log10(std::max(a.powerFixSpk, 1e-30) / std::max(n.powerFixSpk, 1e-30));
    }
}

int main(int argc, char** argv)
{
    std::string reportPath, csvPath; int threads = (int)std::max(1u, std::thread::hardware_concurrency()); bool quick = false; const double sr = 48000.0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i]; auto nx = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--report") reportPath = nx(); else if (a == "--csv") csvPath = nx(); else if (a == "--threads") threads = std::max(1, std::atoi(nx().c_str())); else if (a == "--quick") quick = true;
        else { std::cerr << "unknown option " << a << "\n"; return 2; }
    }
    cd57::Manifest man; if (!man.load(cd57::embeddedManifest()).empty()) { std::cerr << "manifest rejected\n"; return 3; }
    std::vector<Param> P = buildParams(man);
    if (quick) { std::vector<Param> q; for (auto& p : P) if (p.name == "pt_hv_half_winding_rms" || p.name == "ot_dcr_primary_half" || p.name == "speaker_Re" || p.name == "pot_total_resistance" || p.name == "speaker_Mms") q.push_back(p); P = q; }

    Variant nominal; nominal.p = EngineParams(); nominal.p.oversampleLog2 = 1; nominal.p.instVol = 0.55f; nominal.p.micVol = 0.f; nominal.p.tone = 0.65f; nominal.p.jack = 1; nominal.p.inputCalVolts = 1.f; nominal.p.outputRefVolts = 28.28f;
    nominal.manifest = cd57::embeddedManifest();
    struct Job { size_t pi; int side; };                    // side 0 = lo, 1 = hi
    std::vector<Job> jobs; for (size_t i = 0; i < P.size(); ++i) { if (!P[i].oneSided) jobs.push_back({i, 0}); jobs.push_back({i, 1}); }
    std::vector<Features> res(jobs.size()); Features nom, nom2;
    std::atomic<size_t> next{0}; std::atomic<int> done{0};
    std::vector<std::thread> th;
    for (int t = 0; t < threads; ++t) th.emplace_back([&]() {
        for (;;) {
            const size_t j = next++; if (j > jobs.size()) break;
            if (j == jobs.size()) { nom = measure(nominal, sr); continue; }
            Variant v = nominal; const Param& p = P[jobs[j].pi]; p.apply(v, jobs[j].side == 0 ? p.lo : p.hi);
            res[j] = measure(v, sr);
            const int d = ++done; if (d % 10 == 0) std::cerr << "  " << d << "/" << jobs.size() << "\n";
        }
    });
    for (auto& t : th) t.join();
    nom2 = measure(nominal, sr);                            // determinism check: nominal measured twice must be identical
    bool deterministic = true;
    for (int i = 0; i < kNF; ++i) if (std::fabs(effectDb(i, nom2, nom)) > 1e-9) deterministic = false;

    struct Row { std::string name, group, source, unit; double nominal, lo, hi; double eff[kNF][2]; double score = 0; int scoreFeature = 0; bool failed = false; };
    std::vector<Row> rows;
    for (size_t pi = 0; pi < P.size(); ++pi) {
        Row r; r.name = P[pi].name; r.group = P[pi].group; r.source = P[pi].source; r.unit = P[pi].unit; r.nominal = P[pi].nominal; r.lo = P[pi].lo; r.hi = P[pi].hi;
        for (int i = 0; i < kNF; ++i) r.eff[i][0] = r.eff[i][1] = 0.0;
        for (size_t j = 0; j < jobs.size(); ++j) if (jobs[j].pi == pi) {
            if (!res[j].ok) r.failed = true;
            for (int i = 0; i < kNF; ++i) r.eff[i][jobs[j].side] = effectDb(i, res[j], nom);
        }
        for (int i = 0; i < kNF; ++i) for (int s = 0; s < 2; ++s) { const double e = std::fabs(r.eff[i][s]); if (std::isfinite(e) && e > r.score) { r.score = e; r.scoreFeature = i; } }
        rows.push_back(r);
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.score > b.score; });

    // report
    std::ostringstream md;
    md << "# One-parameter-at-a-time sensitivity (dossier Sec. 13, 28)\n\n"
       << "Generated by `cd57_sensitivity`. Around the nominal model (Instrument 1, Instrument volume 6.6/12, tone 7.8/12, 60 Hz mains, 48 kHz host, 2x oversampling) every parameter below is moved to the "
       << "low and the high end of its stated range **alone**, and " << kNF << " features are measured again. Effects are signed and expressed in **dB** (levels as 20 log10 of the ratio, power as 10 log10, "
       << "responses as the dB change) so that all parameters rank on one scale; the score is the largest |effect| over all features. The nominal model measured twice gives identical features: **"
       << (deterministic ? "yes" : "NO") << "**.\n\n"
       << "Ranges: the manifest's own sensitivity interval for assumed entries; the netlist's per-element tolerance; a documented alternative (0.02 uF drawing value vs 0.022 uF); otherwise a PROPOSED width stated in the table. "
       << "A range is **not** a measurement and not a probability distribution: it says how far the value would have to be wrong to matter.\n\n"
       << "Features: gain and shape from the linearised (AC) circuit at the idle point; clipping onset = input peak at 1 kHz THD of 1 %; power at 5 % THD into 8 ohm; THD, H2, H3 for a fixed 12 mV peak input; idle B+1; hum; the gain dip of a quiet probe after a 40 mV burst; "
       << "and the same gain/shape/THD/power with the speaker model as the load.\n\n";
    md << "Dossier Sec. 26 tolerances for orientation: clean response 0.5 dB, onset 0.5 dB, harmonics 1 dB.\n\n";
    md << "## Ranking (largest effect first)\n\n| # | parameter | group (Sec. 13 priority class) | range lo .. hi | source of the range | worst effect | on feature | effect lo / hi (dB) | features outside the Sec. 26 tolerance (of 15) |\n|---|---|---|---|---|---|---|---|---|\n";
    int rank = 1;
    for (auto& r : rows) {
        int cnt = 0; for (int i = 0; i < kNF; ++i) if (std::fabs(r.eff[i][0]) > kTol[i] || std::fabs(r.eff[i][1]) > kTol[i]) ++cnt;
        char b[900];
        const double e0 = r.eff[r.scoreFeature][0], e1 = r.eff[r.scoreFeature][1];
        std::string range = (r.lo == 0 && r.hi == 1 && r.nominal == 0) ? "alternative state" : (jnum(r.lo, 4) + " .. " + jnum(r.hi, 4) + " " + r.unit);
        std::snprintf(b, sizeof b, "| %d | %s | %s | %s | %s | **%.2f dB** | %s | %+.2f / %+.2f | %d |\n", rank++, r.name.c_str(), r.group.c_str(), range.c_str(), r.source.c_str(), r.score, kFeatureName[r.scoreFeature], e0, e1, cnt);
        md << b;
    }
    md << "\n## Full effect table (dB, low end / high end)\n\n| parameter |";
    for (int i = 0; i < kNF; ++i) md << " " << kFeatureName[i] << " |";
    md << "\n|---|"; for (int i = 0; i < kNF; ++i) md << "---|"; md << "\n";
    for (auto& r : rows) { md << "| " << r.name << " |"; for (int i = 0; i < kNF; ++i) { char b[64]; if (std::fabs(r.eff[i][0]) < 0.005 && std::fabs(r.eff[i][1]) < 0.005) std::snprintf(b, sizeof b, " 0 |"); else std::snprintf(b, sizeof b, " %+.2f / %+.2f |", r.eff[i][0], r.eff[i][1]); md << b; } md << "\n"; }
    md << "\n## Dominant parameters per feature (top three by |effect|)\n\n| feature | Sec. 26 tolerance | 1st | 2nd | 3rd |\n|---|---|---|---|---|\n";
    for (int i = 0; i < kNF; ++i) {
        std::vector<std::pair<double, std::string>> v;
        for (auto& r : rows) v.push_back({std::max(std::fabs(r.eff[i][0]), std::fabs(r.eff[i][1])), r.name});
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
        md << "| " << kFeatureName[i] << " | " << jnum(kTol[i], 2) << " dB |";
        for (int k = 0; k < 3; ++k) { char b[200]; std::snprintf(b, sizeof b, " %s (%.2f dB) |", v[(size_t)k].second.c_str(), v[(size_t)k].first); md << b; }
        md << "\n";
    }
    md << "\nReading the tables: H2 and H3 at a fixed low drive sit 40-60 dB below the fundamental, so a small change of bias moves them by many dB; they rank high for almost every electrical parameter and say 'the harmonic balance needs the real operating point', not 'this component is dominant'. "
       << "The column 'features outside the Sec. 26 tolerance' is therefore the better guide to what a measurement must pin down. `speaker_Mms` shows no electrical effect on purpose: with fs, Qms and Qes held fixed the electrical equivalent circuit "
       << "(R_es = Re Qms / Qes, C_mes = Qes / (w0 Re), L_ces = Re / (w0 Qes)) does not depend on Mms; Mms only sets the acoustic sensitivity (cone velocity per volt), which lies outside the electrical boundary.\n";
    // group summary
    md << "\n## What this says about the measurement priorities (Sec. 13 table)\n\n| Sec. 13 high-priority class | largest effect of any parameter in the class | which parameter |\n|---|---|---|\n";
    std::map<std::string, std::pair<double, std::string>> g;
    for (auto& r : rows) { auto& e = g[r.group]; if (r.score > e.first) e = {r.score, r.name}; }
    std::vector<std::pair<std::string, std::pair<double, std::string>>> gv(g.begin(), g.end());
    std::sort(gv.begin(), gv.end(), [](auto& a, auto& b) { return a.second.first > b.second.first; });
    for (auto& x : gv) { char b[400]; std::snprintf(b, sizeof b, "| %s | %.2f dB | %s |\n", x.first.c_str(), x.second.first, x.second.second.c_str()); md << b; }
    md << "\nNot in this study: **cabinet and microphone transfer** (Sec. 13 lists it as dominating the recorded acoustic spectrum). It lies outside the electrical boundary (jack to speaker terminals) and its parameters (`acoustic_*`) are provisional "
       << "constants of the acoustic operator, not part of the circuit; a measured voltage-to-microphone IR replaces them. Component values of the modern amplifier, installed tubes, the real pot laws and the OEM speaker/transformer are unknown (Sec. 28); "
       << "this table says which of those unknowns the electrical result is most sensitive to.\n";
    if (!reportPath.empty()) std::ofstream(reportPath) << md.str(); else std::cout << md.str();
    if (!csvPath.empty()) {
        std::ofstream c(csvPath); c << "parameter,group,source,lo,hi,nominal,score_dB,score_feature";
        for (int i = 0; i < kNF; ++i) c << "," << kFeatureName[i] << " lo dB," << kFeatureName[i] << " hi dB";
        c << "\n";
        for (auto& r : rows) { c << "\"" << r.name << "\",\"" << r.group << "\",\"" << r.source << "\"," << jnum(r.lo) << "," << jnum(r.hi) << "," << jnum(r.nominal) << "," << jnum(r.score, 6) << ",\"" << kFeatureName[r.scoreFeature] << "\""; for (int i = 0; i < kNF; ++i) c << "," << jnum(r.eff[i][0], 6) << "," << jnum(r.eff[i][1], 6); c << "\n"; }
    }
    int failed = 0; for (auto& r : rows) if (r.failed) ++failed;
    // self-checks (these can fail): direction, isolation and a guard against a degenerate range
    int bad = 0;
    auto find = [&](const std::string& n) -> const Row* { for (auto& r : rows) if (r.name == n) return &r; return nullptr; };
    auto chk = [&](bool ok, const std::string& what) { std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what.c_str()); if (!ok) ++bad; };
    if (const Row* r = find("pt_hv_half_winding_rms")) chk(r->eff[4][0] < 0 && r->eff[4][1] > 0 && r->eff[8][0] < 0 && r->eff[8][1] > 0, "a higher power-transformer voltage raises the 5 %-THD power and idle B+1 (both signs, right direction)");
    if (const Row* r = find("speaker_Re")) { double mx = 0; for (int i = 0; i < 11; ++i) mx = std::max(mx, std::max(std::fabs(r->eff[i][0]), std::fabs(r->eff[i][1]))); chk(mx < 1e-9 && std::fabs(r->eff[11][0]) > 0.05, "the voice-coil resistance changes nothing into the 8 ohm resistive load (exactly 0) and does change the speaker-loaded gain"); }
    if (const Row* r = find("pot_total_resistance")) chk(std::fabs(r->eff[0][0]) < 3.0 && std::fabs(r->eff[0][1]) < 3.0 && std::fabs(r->eff[0][0]) > 1e-3, "a +-20 % pot total moves the gain by a few dB at most and not by zero (guards a degenerate range)");
    if (const Row* r = find("speaker_Mms")) { double mx = 0; for (int i = 0; i < kNF; ++i) mx = std::max(mx, std::max(std::fabs(r->eff[i][0]), std::fabs(r->eff[i][1]))); chk(mx < 1e-6, "Mms has no electrical effect when fs, Qms and Qes are held (the equivalent circuit does not contain it)"); }
    chk(deterministic, "the nominal model measured twice gives identical features");
    chk(failed == 0, "every level search found its 1 % and 5 % THD crossings");
    std::printf("%zu parameters, %zu perturbed runs; nominal reproducible: %s; runs where a level search failed: %d\n", P.size(), jobs.size(), deterministic ? "yes" : "NO", failed);
    for (size_t i = 0; i < std::min<size_t>(rows.size(), 8); ++i) std::printf("  %2zu. %-52s %6.2f dB  (%s)\n", i + 1, rows[i].name.c_str(), rows[i].score, kFeatureName[rows[i].scoreFeature]);
    return bad == 0 ? 0 : 1;
}
