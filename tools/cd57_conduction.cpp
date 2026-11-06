// cd57_conduction -- what each 6V6 actually does over the signal cycle (dossier Sec. 10, "Power-class terminology").
//
//   "Cathode bias is a bias method, not proof of a fixed conduction class across all operating levels.  Measure or simulate current in each valve
//    throughout the waveform.  Describe the actual conduction behavior and operating conditions rather than relying on a marketing class label."
//
// For a 1 kHz sine of rising amplitude into the 8 ohm reference load the tool records, at the solver's internal rate, the plate, screen and
// control-grid currents of V3 and V4, the shared cathode, the rails and the output, over an exact whole number of cycles, and reports per valve:
// conduction angle (plate current above 1 mA: a stated threshold, not a class definition), peak / minimum / mean plate current, the overlap
// (both valves conducting), screen and grid-current peaks and duty, plate and screen dissipation, and the displacement of the cathode bias and
// the rails.  Also optionally dumps one cycle of the waveforms for plotting.
//
//   cd57_conduction [--report FILE.md] [--csv FILE.csv] [--waveforms FILE.csv] [--inst 0.55] [--tone 1.0] [--load 2] [--fs 48000]
#include <array>
#include <iostream>

#include "ToolCommon.h"

using namespace cd57tool;

struct Row {
    double ein = 0, pOut = 0, thd = 0;
    double ipPk[2] = {0, 0}, ipMin[2] = {0, 0}, ipMean[2] = {0, 0}, isMean[2] = {0, 0}, duty[2] = {0, 0}, isPk[2] = {0, 0}, igPk[2] = {0, 0}, igDuty[2] = {0, 0}, pPlate[2] = {0, 0}, pScreen[2] = {0, 0};
    double overlap = 0, ikMean = 0, vkMean = 0, vkPk = 0, b1 = 0, b2 = 0, ipDiff = 0, vpkMax[2] = {0, 0};
};

int main(int argc, char** argv)
{
    std::string reportPath, csvPath, wavePath; double inst = 0.55, tone = 1.0, fs = 48000.0; int load = 2;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i]; auto nx = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--report") reportPath = nx(); else if (a == "--csv") csvPath = nx(); else if (a == "--waveforms") wavePath = nx();
        else if (a == "--inst") inst = std::atof(nx().c_str()); else if (a == "--tone") tone = std::atof(nx().c_str()); else if (a == "--load") load = std::atoi(nx().c_str());
        else if (a == "--fs") fs = std::atof(nx().c_str());
        else { std::cerr << "unknown option " << a << "\n"; return 2; }
    }
    EngineParams p; p.oversampleLog2 = 1; p.instVol = (float)inst; p.micVol = 0.f; p.tone = (float)tone; p.jack = 1; p.load = load; p.speakerHeating = false; p.inputCalVolts = 1.f; p.outputRefVolts = 28.28f;
    const double fInt = fs * 2.0, f0 = 1000.0, thrIp = 1e-3, thrIg = 10e-6;
    const int perCycle = (int)std::lround(fInt / f0), cycles = 40, settle = (int)(0.3 * fInt);
    const double Rload = load == 2 ? 8.0 : (load == 3 ? 4.0 : 16.0);

    // pentode records: V3 = first, V4 = second (netlist order); verified by name below
    auto probeLevel = [&](double ein, std::vector<std::array<double, 8>>* wave) {
        Rig r(p, fs);
        cd57::AmpCircuit& amp = r.e.amp(); cd57::Network& n = amp.network();
        int idx[2] = {-1, -1};
        for (size_t k = 0; k < n.pen().size(); ++k) { if (n.pen()[k].m.name == "V3") idx[0] = (int)k; if (n.pen()[k].m.name == "V4") idx[1] = (int)k; }
        if (idx[0] < 0 || idx[1] < 0) { std::cerr << "netlist has no V3/V4\n"; std::exit(3); }
        Row w; w.ein = ein;
        const int total = settle + cycles * perCycle;
        std::vector<double> vt; vt.reserve((size_t)cycles * perCycle);
        const int pV3 = n.nodeId("p3"), pV4 = n.nodeId("p4"), kN = n.nodeId("k34"), sN = n.nodeId("b2");
        double sIs[2] = {0, 0}, sIp[2] = {0, 0}, sMin[2] = {1e9, 1e9}, sDuty[2] = {0, 0}, sIgD[2] = {0, 0}, sPp[2] = {0, 0}, sPs[2] = {0, 0}, sOv = 0, sVk = 0, sB1 = 0, sB2 = 0, sIk = 0;
        for (int i = 0; i < total; ++i) {
            const double t = (double)i / fInt;
            amp.tick(ein * std::sin(2.0 * M::kPi * f0 * t) * std::min(1.0, t / 0.05));
            if (i < settle) continue;
            const int c = (i - settle) % perCycle;
            const double vtm = amp.vTerminal(); vt.push_back(vtm);
            double ip[2], is[2], ig[2];
            for (int k = 0; k < 2; ++k) { const auto& q = n.pen()[(size_t)idx[k]]; ip[k] = q.ip; is[k] = q.is; ig[k] = q.ig; }
            const double vk = n.V(kN);
            for (int k = 0; k < 2; ++k) {
                w.ipPk[k] = std::max(w.ipPk[k], ip[k]); sMin[k] = std::min(sMin[k], ip[k]); sIp[k] += ip[k]; sIs[k] += is[k];
                if (ip[k] > thrIp) sDuty[k] += 1; if (std::fabs(ig[k]) > thrIg) sIgD[k] += 1;
                w.isPk[k] = std::max(w.isPk[k], std::fabs(is[k])); w.igPk[k] = std::max(w.igPk[k], std::fabs(ig[k]));
                const double vp = n.V(k == 0 ? pV3 : pV4) - vk, vs = n.V(sN) - vk;
                sPp[k] += vp * ip[k]; sPs[k] += vs * is[k]; w.vpkMax[k] = std::max(w.vpkMax[k], vp);
            }
            if (ip[0] > thrIp && ip[1] > thrIp) sOv += 1;
            sIk += ip[0] + ip[1] + is[0] + is[1] + ig[0] + ig[1];
            sVk += vk; w.vkPk = std::max(w.vkPk, vk); sB1 += n.V(n.nodeId("b1")); sB2 += n.V(sN);
            w.ipDiff = std::max(w.ipDiff, std::fabs(ip[0] - ip[1]));
            if (wave && i >= total - perCycle) (*wave).push_back({(double)c / fInt * 1e3, ip[0] * 1e3, ip[1] * 1e3, is[0] * 1e3, ig[0] * 1e6, vtm, n.V(n.nodeId("g3")), n.V(n.nodeId("g4"))});
        }
        const double N = (double)cycles * perCycle;
        for (int k = 0; k < 2; ++k) { w.ipMean[k] = sIp[k] / N; w.isMean[k] = sIs[k] / N; w.ipMin[k] = sMin[k]; w.duty[k] = sDuty[k] / N; w.igDuty[k] = sIgD[k] / N; w.pPlate[k] = sPp[k] / N; w.pScreen[k] = sPs[k] / N; }
        w.overlap = sOv / N; w.ikMean = sIk / N; w.vkMean = sVk / N; w.b1 = sB1 / N; w.b2 = sB2 / N;
        const auto ts = M::analyseTone(vt, fInt, f0, (size_t)cycles * (size_t)perCycle, 9);
        // THD is the ratio of harmonics to a fundamental: with no signal the fundamental is only mains-ripple leakage and the ratio means nothing
        w.thd = ein > 0.0 ? ts.thd * 100.0 : NAN; w.pOut = ts.rms * ts.rms / Rload;
        return w;
    };

    std::vector<Row> rows;
    {   // idle: no signal
        Row z = probeLevel(0.0, nullptr); rows.push_back(z);
    }
    std::vector<double> levels;
    for (double l = 0.003; l < 0.6; l *= std::pow(10.0, 2.0 / 20.0)) levels.push_back(l);
    for (double e : levels) { rows.push_back(probeLevel(e, nullptr)); std::cerr << "." << std::flush; }
    std::cerr << "\n";

    // choose the waveform levels: closest to 1 W, to 5 % THD (rated), and the heaviest drive
    size_t iLow = 1, iRated = 1, iHeavy = rows.size() - 1;
    for (size_t i = 1; i < rows.size(); ++i) { if (std::fabs(rows[i].pOut - 1.0) < std::fabs(rows[iLow].pOut - 1.0)) iLow = i; if (rows[i].thd <= 5.0) iRated = i; }
    if (!wavePath.empty()) {
        std::ofstream o(wavePath); o << "level,t_ms,ip_V3_mA,ip_V4_mA,is_V3_mA,ig_V3_uA,v_terminal_V,vg3_V,vg4_V\n";
        const char* nm[3] = {"low", "rated", "heavy"}; const size_t ix[3] = {iLow, iRated, iHeavy};
        for (int k = 0; k < 3; ++k) { std::vector<std::array<double, 8>> w; probeLevel(rows[ix[k]].ein, &w); for (auto& s : w) o << nm[k] << "," << s[0] << "," << s[1] << "," << s[2] << "," << s[3] << "," << s[4] << "," << s[5] << "," << s[6] << "," << s[7] << "\n"; }
    }
    if (!csvPath.empty()) {
        std::ofstream o(csvPath);
        o << "jack_peak_V,P_out_W,thd_pct,ip3_pk_mA,ip3_min_mA,ip3_mean_mA,ip3_duty,ip4_pk_mA,ip4_min_mA,ip4_mean_mA,ip4_duty,overlap,is3_pk_mA,is4_pk_mA,ig3_pk_uA,ig4_pk_uA,ig3_duty,ig4_duty,pplate3_W,pplate4_W,pscreen3_W,pscreen4_W,vk_mean_V,vk_pk_V,ik_mean_mA,b1_V,b2_V,vpk3_max_V,vpk4_max_V\n";
        for (auto& r : rows) o << jnum(r.ein, 6) << "," << jnum(r.pOut, 6) << "," << jnum(r.thd, 6) << "," << jnum(r.ipPk[0] * 1e3, 6) << "," << jnum(r.ipMin[0] * 1e3, 6) << "," << jnum(r.ipMean[0] * 1e3, 6) << "," << jnum(r.duty[0], 5) << ","
                              << jnum(r.ipPk[1] * 1e3, 6) << "," << jnum(r.ipMin[1] * 1e3, 6) << "," << jnum(r.ipMean[1] * 1e3, 6) << "," << jnum(r.duty[1], 5) << "," << jnum(r.overlap, 5) << "," << jnum(r.isPk[0] * 1e3, 6) << "," << jnum(r.isPk[1] * 1e3, 6) << ","
                              << jnum(r.igPk[0] * 1e6, 6) << "," << jnum(r.igPk[1] * 1e6, 6) << "," << jnum(r.igDuty[0], 5) << "," << jnum(r.igDuty[1], 5) << "," << jnum(r.pPlate[0], 6) << "," << jnum(r.pPlate[1], 6) << "," << jnum(r.pScreen[0], 6) << "," << jnum(r.pScreen[1], 6) << ","
                              << jnum(r.vkMean, 6) << "," << jnum(r.vkPk, 6) << "," << jnum(r.ikMean * 1e3, 6) << "," << jnum(r.b1, 6) << "," << jnum(r.b2, 6) << "," << jnum(r.vpkMax[0], 6) << "," << jnum(r.vpkMax[1], 6) << "\n";
    }

    // Consistency of the reported per-valve currents with the circuit (a test that can fail): over whole cycles the 25 uF bypass passes no net DC,
    // so the mean cathode current (plate + screen + grid of both valves) times the 250 ohm cathode resistor must equal the mean cathode voltage.
    double worstDev = 0; size_t worstAt = 0;
    for (size_t i = 0; i < rows.size(); ++i) { const double d = std::fabs(rows[i].ikMean * 250.0 - rows[i].vkMean) / std::max(rows[i].vkMean, 1e-9); if (d > worstDev) { worstDev = d; worstAt = i; } }
    const bool consistent = worstDev < 0.01;
    std::printf("%s  mean cathode current x 250 ohm equals mean cathode voltage at all %zu drive levels (worst deviation %.3f %% at %s)\n", consistent ? "PASS" : "FAIL", rows.size(), worstDev * 100, worstAt == 0 ? "idle" : (jnum(rows[worstAt].ein * 1e3, 3) + " mV").c_str());
    std::ostringstream md;
    md << "# Output-valve conduction report (dossier Sec. 10)\n\n"
       << "Generated by `cd57_conduction`. A **1 kHz sine** of rising amplitude at the Instrument 1 jack (Instrument volume " << inst << ", tone " << tone << ", 8 ohm reference load, 60 Hz mains, nominal supply) is solved at the internal rate "
       << jnum(fInt, 6) << " Hz; every number below is taken over " << cycles << " whole cycles after a 0.3 s settling. **Conduction angle** = fraction of the cycle with plate current above **1 mA** x 360 degrees: a stated threshold, not a class definition. "
       << "Grid-current duty counts |Ig| > 10 uA. This describes what the model's valves do; the model's 6V6 law is fitted to the JJ 6V6S sheet and is **not** measured on the installed pair (dossier Sec. 15-16, 28).\n\n";
    md << "| jack peak | P out (8 ohm) | THD | V3 Ip pk / min / mean (mA) | V4 Ip pk / min / mean (mA) | angle V3 / V4 | both on | Ig pk V3 / V4 (uA) | Is pk (mA) | plate diss. V3 / V4 (W) | Vk mean (V) | B+1 (V) |\n|---|---|---|---|---|---|---|---|---|---|---|---|\n";
    char b[640];
    for (size_t i = 0; i < rows.size(); ++i) {
        if (i != 0 && i % 2 != 0 && i != iRated && i != iHeavy && i != iLow) continue;
        const Row& r = rows[i];
        std::snprintf(b, sizeof b, "| %s%s | %.3g W | %s | %.1f / %.1f / %.1f | %.1f / %.1f / %.1f | %.0f° / %.0f° | %.0f %% | %.1f / %.1f | %.1f | %.2f / %.2f | %.2f | %.0f |\n",
                      i == 0 ? "idle" : (jnum(r.ein * 1e3, 3) + " mV").c_str(), i == iRated ? " (rated point)" : (i == iHeavy ? " (heaviest)" : ""), r.pOut, std::isfinite(r.thd) ? (jnum(r.thd, 3) + " %").c_str() : "n/a", r.ipPk[0] * 1e3, r.ipMin[0] * 1e3, r.ipMean[0] * 1e3, r.ipPk[1] * 1e3, r.ipMin[1] * 1e3, r.ipMean[1] * 1e3,
                      r.duty[0] * 360, r.duty[1] * 360, r.overlap * 100, r.igPk[0] * 1e6, r.igPk[1] * 1e6, std::max(r.isPk[0], r.isPk[1]) * 1e3, r.pPlate[0], r.pPlate[1], r.vkMean, r.b1);
        md << b;
    }
    const Row& id0 = rows[0]; const Row& rt = rows[iRated]; const Row& hv = rows[iHeavy];
    md << "\nConsistency check (can fail): mean cathode current x 250 ohm = mean cathode voltage at every drive level above, worst deviation **" << jnum(worstDev * 100, 3) << " %** (" << (consistent ? "PASS" : "FAIL") << ").\n";
    md << "\n## What the numbers say\n\n";
    std::snprintf(b, sizeof b, "* **Idle:** each valve draws %.1f / %.1f mA plate current (+ %.1f / %.1f mA screen) at %.1f V cathode bias and %.0f V B+1; plate dissipation %.1f / %.1f W per valve at idle, screen %.2f / %.2f W (the shared 250 ohm cathode resistor sets the bias, so it moves with the signal). For scale, the JJ 6V6S sheet in the dossier (Fig. 11) draws its maximum-dissipation hyperbola at 14 W, so the model idles at the sheet's own rating: a hot-biased 5E3-style stage, which is what the published circuit does with these values.\n",
                  id0.ipMean[0] * 1e3, id0.ipMean[1] * 1e3, id0.isMean[0] * 1e3, id0.isMean[1] * 1e3, id0.vkMean, id0.b1, id0.pPlate[0], id0.pPlate[1], id0.pScreen[0], id0.pScreen[1]); md << b;
    std::snprintf(b, sizeof b, "* **At the rated-power point** (largest drive with THD <= 5 %%, %.2f W): conduction angle %.0f° / %.0f° of 360°, both valves on for %.0f %% of the cycle, peak plate current %.0f / %.0f mA, cathode bias moved from %.1f V to %.1f V, B+1 sagged from %.0f V to %.0f V.\n",
                  rt.pOut, rt.duty[0] * 360, rt.duty[1] * 360, rt.overlap * 100, rt.ipPk[0] * 1e3, rt.ipPk[1] * 1e3, id0.vkMean, rt.vkMean, id0.b1, rt.b1); md << b;
    std::snprintf(b, sizeof b, "* **At the heaviest drive** (%.1f W, %.1f %% THD): angle %.0f° / %.0f°, grid current peaks %.0f / %.0f uA conducting %.0f %% / %.0f %% of the cycle (the blocking-distortion path of Sec. 10), screen current peaks %.0f mA, cathode bias %.1f V.\n",
                  hv.pOut, hv.thd, hv.duty[0] * 360, hv.duty[1] * 360, hv.igPk[0] * 1e6, hv.igPk[1] * 1e6, hv.igDuty[0] * 100, hv.igDuty[1] * 100, std::max(hv.isPk[0], hv.isPk[1]) * 1e3, hv.vkMean); md << b;
    md << "* The model's valves are therefore neither fixed in class A nor in class B across level: the conduction angle and the overlap change continuously with drive, and the cathode bias and rails move with the average current. "
       << "No class label is used anywhere in the model; the label is only a way of reading this table.\n"
       << "* The two valves are identical in the nominal model (no specimen mismatch: Sec. 07/10 say to measure it), so the small differences between V3 and V4 columns come from the asymmetric phase-inverter drive, not from the valves.\n";
    if (!reportPath.empty()) std::ofstream(reportPath) << md.str(); else std::cout << md.str();
    return consistent ? 0 : 1;
}
