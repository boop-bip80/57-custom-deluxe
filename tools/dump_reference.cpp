// dump_reference -- prints the C++ solver's DC node voltages and AC node gains for the validation baseline, for scripts/independent_check.py.
//
// Baseline: Instrument 1, instrument volume 0.5, mic volume 0, tone 0.7, 8 ohm dummy load, speaker heating off, and the three supply
// capacitors scaled x1000 so the rails carry no ripple (the AC solver linearises at a single operating point).
#include <cmath>
#include <complex>
#include <cstdio>

#include "CD57Engine.h"
#include "EmbeddedData.h"

using namespace cd57;

int main()
{
    EngineParams p;
    p.oversampleLog2 = 1; p.instVol = 0.5f; p.micVol = 0.0f; p.tone = 0.7f; p.jack = 1; p.load = 2;
    p.speakerHeating = false; p.inputCalVolts = 1.0f; p.outputRefVolts = 20.0f;
    CD57Engine e;
    e.prepare(48000.0, 512, embeddedNetlist(), p, embeddedManifest());
    AmpCircuit& a = e.amp();
    Network& n = a.network();
    for (const char* nm : {"C1", "C2", "C3"}) n.setC(n.capId(nm), n.cap()[n.capId(nm)].Cnom * 1000.0);
    n.refreshLinear();
    a.initialiseQuiescent(0.05);
    for (int i = 0; i < 4800; ++i) a.tick(0.0);
    std::printf("DC\n");
    for (const char* nm : {"b1", "b2", "b3", "p1b", "p1n", "k1", "p2a", "kb2", "p2b", "kcy", "j", "p3", "p4", "k34", "g2b", "sp"})
        std::printf("%s %.6f\n", nm, a.nodeV(nm));
    std::printf("AC\n");
    const int pu = n.nodeId("pu");
    const double Rsrc = n.getR(n.resId("Rsrc"));
    for (double f : {100.0, 1000.0, 5000.0}) {
        const auto v = n.acSolve(f, pu);
        for (const char* nm : {"b_g", "p1b", "m", "p2a", "p2b", "kcy", "g3", "p3", "sp"}) {
            const std::complex<double> g = v[n.nodeId(nm)] / Rsrc;
            std::printf("%g %s %.6f %.4f\n", f, nm, 20 * std::log10(std::abs(g)), std::arg(g) * 180.0 / M_PI);
        }
    }
    return 0;
}
