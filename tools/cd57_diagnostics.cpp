// cd57_diagnostics -- the data behind the dossier Sec. 26 "Diagnostic views":
//   "Publish spectra, harmonic levels versus drive, intermodulation spectra, time-domain residuals, burst envelopes, rail/cathode trajectories
//    where available, impedance magnitude/phase and acoustic position comparisons.  One aggregate error score can hide a narrow frequency defect
//    or a serious recovery mismatch."
//
//   cd57_diagnostics --out DIR
// writes CSV files that scripts/plot_diagnostics.py turns into the figures in docs/figures/:
//   response.csv            AC jack-EMF -> speaker-terminal response (magnitude, phase) for tone, volume, jack and channel-interaction states
//   impedance.csv           input impedance at each jack (the 1 MOhm / 136 kOhm of Sec. 02/06 reproduced by the circuit), amplifier output impedance,
//                           and the (ASSUMED) speaker model's impedance (Sec. 17)
//   harmonics_vs_drive.csv  fundamental, power, THD, H2..H5 versus input level at 1 kHz
//   spectra.csv             1 kHz tone at three levels and a two-tone (700 Hz + 1 kHz) spectrum
//   waveforms.csv           terminal-voltage cycles at three levels
//   residual.csv            time-domain residual of 1x / 2x / 4x oversampling against an 8x render of the same model
//   burst.csv               1 ms trajectories of the rails, shared cathode, power-valve grids, cathodyne nodes and the output envelope around a burst
//   acoustic.csv            provisional jack-to-microphone response at three microphone positions against the terminal response
#include <filesystem>
#include <iostream>

#include "CaptureAnalysis.h"

using namespace cd57tool;
using cplx = std::complex<double>;
using cd57::Network;

static EngineParams baseP()
{
    EngineParams p; p.oversampleLog2 = 1; p.instVol = 0.55f; p.micVol = 0.f; p.tone = 0.65f; p.jack = 1; p.load = 2; p.speakerHeating = false; p.inputCalVolts = 1.f; p.outputRefVolts = 28.28f;
    return p;
}
static std::vector<double> freqGrid(double f1 = 20.0, double f2 = 20000.0, int perOct = 12)
{
    std::vector<double> f; const int n = (int)std::floor(std::log2(f2 / f1) * perOct) + 1;
    for (int i = 0; i < n; ++i) f.push_back(f1 * std::pow(2.0, (double)i / perOct));
    return f;
}
static cplx acGain(Rig& r, double f, const char* node)
{
    Network& n = r.e.amp().network();
    const auto v = n.acSolve(f, n.nodeId("pu"));
    return v[(size_t)n.nodeId(node)] / n.getR(n.resId("Rsrc"));
}

int main(int argc, char** argv)
{
    std::string out = "diag_out";
    for (int i = 1; i + 1 < argc; i += 2) { if (std::string(argv[i]) == "--out") out = argv[i + 1]; else { std::cerr << "usage: cd57_diagnostics --out DIR\n"; return 2; } }
    std::filesystem::create_directories(out);
    const double sr = 48000.0;
    const auto F = freqGrid();

    // ------------------------------------------------------------------ response.csv
    {
        std::ofstream o(out + "/response.csv"); o << "group,case,f_hz,mag_db,phase_deg\n";
        auto sweep = [&](const std::string& group, const std::string& name, const EngineParams& p) {
            Rig r(p, sr);
            for (double f : F) { const cplx g = acGain(r, f, "sp"); o << group << "," << name << "," << f << "," << M::db20(std::abs(g)) << "," << std::arg(g) * 180.0 / M::kPi << "\n"; }
        };
        for (double t : {0.0, 0.25, 0.5, 0.75, 1.0}) { EngineParams p = baseP(); p.instVol = 0.6f; p.tone = (float)t; sweep("tone", "tone " + jnum(t * 12, 3) + "/12", p); }
        for (double v : {0.2, 0.4, 0.6, 0.8, 1.0}) { EngineParams p = baseP(); p.instVol = (float)v; p.tone = 0.65f; sweep("volume", "instrument volume " + jnum(v * 12, 3) + "/12", p); }
        for (int j = 1; j <= 4; ++j) { EngineParams p = baseP(); p.jack = j; p.instVol = j <= 2 ? 0.6f : 0.f; p.micVol = j <= 2 ? 0.f : 0.6f; sweep("jack", std::string("jack ") + jackName(j), p); }
        for (double m : {0.0, 0.3, 0.6, 1.0}) { EngineParams p = baseP(); p.instVol = 0.6f; p.micVol = (float)m; sweep("interaction", "mic volume " + jnum(m * 12, 3) + "/12 (guitar in Instrument 1)", p); }
    }
    // ------------------------------------------------------------------ impedance.csv
    {
        std::ofstream o(out + "/impedance.csv"); o << "kind,case,f_hz,mag_ohm,phase_deg\n";
        for (int j = 1; j <= 4; ++j) {
            EngineParams p = baseP(); p.jack = j; p.instVol = 0.6f; p.micVol = 0.6f; Rig r(p, sr);
            Network& n = r.e.amp().network();
            const int tp = n.nodeId("tp"), pu = n.nodeId("pu");
            const double L = 1e-6, Ccab = 20e-12;      // line-mode source chain of the netlist (Lpu, Ccab)
            for (double f : F) {
                const auto v = n.acSolve(f, pu); const double w = 2 * M::kPi * f;
                const cplx Vtp = v[(size_t)tp], Il = (v[(size_t)pu] - Vtp) / cplx(0, w * L);
                const cplx Iamp = Il - cplx(0, w * Ccab) * Vtp;                          // current into the amplifier's jack network only
                const cplx Z = Vtp / Iamp;
                o << "input," << jackName(j) << "," << f << "," << std::abs(Z) << "," << std::arg(Z) * 180.0 / M::kPi << "\n";
            }
        }
        {   // amplifier output impedance at the speaker terminals (speaker disconnected): 1 A injected at the terminals
            EngineParams p = baseP(); p.load = 2; Rig r(p, sr); Network& n = r.e.amp().network();
            n.setR(n.resId("Rdum"), 1e11); n.setR(n.resId("Re"), 1e11); n.refreshLinear();
            for (double f : F) { const auto v = n.acSolve(f, n.nodeId("sp")); const cplx Z = v[(size_t)n.nodeId("sp")]; o << "output,amplifier output impedance (load removed)," << f << "," << std::abs(Z) << "," << std::arg(Z) * 180.0 / M::kPi << "\n"; }
        }
        {   // the ASSUMED speaker model (generic 12-inch alnico driver, NOT PN 7708599000): Ze = Re + jw Le + Bl^2 / (Rms + jw Mms + 1/(jw Cms))
            cd57::SpeakerTS s; cd57::Manifest man; man.load(cd57::embeddedManifest()); cd57::AmpConfig c; man.apply(c); s = c.speaker;
            for (double f : F) {
                const double w = 2 * M::kPi * f, Bl = s.Bl();
                const cplx Zm = cplx(s.Rms(), 0) + cplx(0, w * s.Mms) + 1.0 / cplx(0, w * s.Cms());
                const cplx Z = cplx(s.Re, w * s.Le) + Bl * Bl / Zm;
                o << "speaker,assumed speaker model," << f << "," << std::abs(Z) << "," << std::arg(Z) * 180.0 / M::kPi << "\n";
            }
        }
    }
    // ------------------------------------------------------------------ harmonics vs drive, waveforms
    Rig tmpl(baseP(), sr), w(baseP(), sr);
    {
        std::ofstream o(out + "/harmonics_vs_drive.csv"); o << "in_peak_V,out_rms_V,power_W,thd_pct,h2_dbc,h3_dbc,h4_dbc,h5_dbc,fund_gain_db\n";
        for (double L = 0.001; L < 0.6; L *= std::pow(10.0, 1.5 / 20.0)) {
            w.restoreFrom(tmpl);
            const Vec y = w.run(sineBurst(sr, 1000.0, L, 0.16, 0.02));
            const auto t = M::analyseTone(y, sr, 1000.0, (size_t)std::llround(0.1 * sr), 10);
            o << L << "," << t.rms << "," << t.rms * t.rms / 8.0 << "," << 100 * t.thd << "," << t.dbc[1] << "," << t.dbc[2] << "," << t.dbc[3] << "," << t.dbc[4] << "," << M::db20(t.fundamental / L) << "\n";
        }
    }
    const double levels[3] = {0.005, 0.0105, 0.04};
    const char* lname[3] = {"clean (5 mV)", "onset (10.5 mV)", "driven (40 mV)"};
    {
        std::ofstream sp(out + "/spectra.csv"); sp << "case,f_hz,dbv\n";
        std::ofstream wf(out + "/waveforms.csv"); wf << "case,t_ms,jack_V,terminal_V\n";
        for (int k = 0; k < 3; ++k) {
            w.restoreFrom(tmpl);
            const Vec x = sineBurst(sr, 1000.0, levels[k], 0.3, 0.02); Vec y = w.run(x);
            const size_t n = (size_t)std::llround(0.2 * sr), s0 = y.size() - n;
            // n = 9600 is not a power of two: use a direct DFT on the harmonic grid of 5 Hz bins up to 20 kHz (coherent: 0.2 s window)
            for (int b = 1; b <= 4000; ++b) {
                double c = 0, s = 0; const double wv = 2 * M::kPi * (5.0 * b) / sr;
                for (size_t i = 0; i < n; ++i) { c += y[s0 + i] * std::cos(wv * (double)i); s += y[s0 + i] * std::sin(wv * (double)i); }
                const double amp = std::sqrt(c * c + s * s) * 2.0 / (double)n;
                sp << lname[k] << "," << 5.0 * b << "," << M::db20(amp) << "\n";
            }
            for (size_t i = s0; i < s0 + (size_t)(0.002 * sr); ++i) wf << lname[k] << "," << (double)(i - s0) / sr * 1e3 << "," << x[i] << "," << y[i] << "\n";
        }
        // two-tone: 700 Hz + 1 kHz at the onset level (peak of the sum)
        w.restoreFrom(tmpl);
        Vec x((size_t)(0.3 * sr)); for (size_t i = 0; i < x.size(); ++i) { const double t = (double)i / sr; x[i] = 0.5 * 0.02 * (std::sin(2 * M::kPi * 700.0 * t) + std::sin(2 * M::kPi * 1000.0 * t)) * std::min(1.0, t / 0.02); }
        const Vec y = w.run(x); const size_t n = (size_t)std::llround(0.2 * sr), s0 = y.size() - n;
        for (int b = 1; b <= 4000; ++b) {
            double c = 0, s = 0; const double wv = 2 * M::kPi * (5.0 * b) / sr;
            for (size_t i = 0; i < n; ++i) { c += y[s0 + i] * std::cos(wv * (double)i); s += y[s0 + i] * std::sin(wv * (double)i); }
            sp << "two-tone 700 Hz + 1 kHz (20 mV peak sum)," << 5.0 * b << "," << M::db20(std::sqrt(c * c + s * s) * 2.0 / (double)n) << "\n";
        }
    }
    // ------------------------------------------------------------------ oversampling residual against an 8x render of the same model
    {
        std::ofstream o(out + "/residual.csv"); o << "t_ms,ref_8x_V,res_1x_V,res_2x_V,res_4x_V\n";
        const Vec x = sineBurst(sr, 3100.0, 0.06, 0.2, 0.01);
        std::vector<Vec> y;
        for (int os : {3, 0, 1, 2}) { EngineParams p = baseP(); p.oversampleLog2 = os; Rig r(p, sr); y.push_back(r.run(x)); }
        const size_t s0 = (size_t)(0.15 * sr);
        for (size_t i = s0; i < s0 + (size_t)(0.003 * sr); ++i) o << (double)(i - s0) / sr * 1e3 << "," << y[0][i] << "," << y[1][i] - y[0][i] << "," << y[2][i] - y[0][i] << "," << y[3][i] - y[0][i] << "\n";
    }
    // ------------------------------------------------------------------ burst trajectories (1 ms resolution)
    {
        EngineParams p = baseP(); Rig r(p, sr, 48);
        Vec x((size_t)(0.15 * sr), 0.0); for (size_t i = 0; i < x.size(); ++i) { const double t = (double)i / sr; x[i] = 0.04 * std::sin(2 * M::kPi * 1000.0 * t) * std::min(1.0, t / 0.005); }
        const Vec pr = sineBurst(sr, 1000.0, 0.0004, 0.6, 0.002); x.insert(x.begin(), (size_t)(0.1 * sr), 0.0); x.insert(x.end(), pr.begin(), pr.end());
        cd57::AmpCircuit& amp = r.e.amp();
        struct S { double t, b1, b2, b3, vk, g3, g4, p2b, kcy, p1b; }; std::vector<S> traj;
        const Vec y = r.run(x, nullptr, [&](size_t idx) { traj.push_back({(double)idx / sr, amp.nodeV("b1"), amp.nodeV("b2"), amp.nodeV("b3"), amp.nodeV("k34"), amp.nodeV("g3"), amp.nodeV("g4"), amp.nodeV("p2b"), amp.nodeV("kcy"), amp.nodeV("p1b")}); });
        const auto env = M::lockinEnvelope(y, sr, 1000.0, 4, 0.001);
        std::ofstream o(out + "/burst.csv"); o << "t_ms,b1_V,b2_V,b3_V,vk_V,g3_V,g4_V,p2b_V,kcy_V,p1b_V,out_env_V\n";
        for (size_t i = 0; i < traj.size(); ++i) {
            const double t = traj[i].t; size_t e = (size_t)std::llround(t / 0.001); e = std::min(e, env.a.size() ? env.a.size() - 1 : 0);
            o << (t - 0.1) * 1e3 << "," << traj[i].b1 << "," << traj[i].b2 << "," << traj[i].b3 << "," << traj[i].vk << "," << traj[i].g3 << "," << traj[i].g4 << "," << traj[i].p2b << "," << traj[i].kcy << "," << traj[i].p1b << "," << (env.a.empty() ? 0.0 : env.a[e]) << "\n";
        }
    }
    // ------------------------------------------------------------------ provisional acoustic path: three microphone positions vs the terminal response
    {
        std::ofstream o(out + "/acoustic.csv"); o << "case,f_hz,mag_db_re_1k\n";
        const M::Sweep sw = M::makeLogSweep(20.0, 20000.0, 2.0, sr, 0.005);
        struct Pos { const char* name; float rad, dist; };
        std::vector<std::pair<std::string, Vec>> curves;
        std::vector<Pos> pos = {{"mic on the dust cap at 2.5 cm (radial 0.0 / distance 0.0)", 0.0f, 0.0f}, {"default (radial 0.15 / distance 0.10)", 0.15f, 0.10f}, {"off-axis near the cone edge at 30 cm (radial 0.8 / distance 0.5)", 0.8f, 0.5f}};
        for (auto& ps : pos) {
            EngineParams p = baseP(); p.load = 0; p.micRadial = ps.rad; p.micDistance = ps.dist; p.speakerHeating = false; Rig r(p, sr);
            Vec xin = sw.x; xin.resize(sw.x.size() + (size_t)(0.3 * sr), 0.0);
            Vec term(xin.size() + (size_t)r.latency), mic(term.size());
            std::vector<float> in(512), ot(512), oa(512);
            for (size_t i = 0; i < term.size(); i += 512) {
                const int m = (int)std::min<size_t>(512, term.size() - i);
                for (int k = 0; k < m; ++k) in[(size_t)k] = i + (size_t)k < xin.size() ? (float)(xin[i + (size_t)k] / r.kIn()) : 0.f;
                r.e.process(in.data(), ot.data(), oa.data(), m);
                for (int k = 0; k < m; ++k) { term[i + (size_t)k] = ot[(size_t)k]; mic[i + (size_t)k] = oa[(size_t)k]; }
            }
            term.erase(term.begin(), term.begin() + r.latency); mic.erase(mic.begin(), mic.begin() + r.latency);
            const auto Hm = M::transferFunction(xin, mic, sr, 20.0, 20000.0), Ht = M::transferFunction(xin, term, sr, 20.0, 20000.0);
            auto ref = [&](const M::Response& h) { size_t bi = 0; double bd = 1e9; for (size_t i = 0; i < h.f.size(); ++i) { const double d = std::fabs(std::log(h.f[i] / 1000.0)); if (d < bd) { bd = d; bi = i; } } return h.magDb[bi]; };
            for (size_t i = 0; i < Hm.f.size(); ++i) o << std::string(ps.name) << "," << Hm.f[i] << "," << Hm.magDb[i] - ref(Hm) << "\n";
            if (&ps == &pos[1]) for (size_t i = 0; i < Ht.f.size(); ++i) o << "speaker-terminal voltage (no acoustic stage)," << Ht.f[i] << "," << Ht.magDb[i] - ref(Ht) << "\n";
        }
    }
    std::cout << "diagnostic data written to " << out << "\n";
    return 0;
}
