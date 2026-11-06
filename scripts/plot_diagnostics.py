#!/usr/bin/env python3
"""plot_diagnostics.py -- render the dossier Sec. 26 "Diagnostic views" as PNG figures in docs/figures/.

  python3 scripts/plot_diagnostics.py --build BUILD_DIR          # runs cd57_diagnostics there, then plots
  python3 scripts/plot_diagnostics.py --data DIR                  # plots CSVs already produced by cd57_diagnostics

Besides the diagnostics CSVs it reads the committed reference data (data/reference/*.csv). Every figure carries its provenance in the title or
caption: these are views of the MODEL (a documented approximation), not measurements of a Fender amplifier.
"""
import argparse, csv, math, os, subprocess, sys, tempfile
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
REF = os.path.join(ROOT, "data", "reference")
plt.rcParams.update({"figure.dpi": 100, "savefig.dpi": 110, "font.size": 8.5, "axes.grid": True, "grid.alpha": 0.3, "axes.titlesize": 9, "legend.fontsize": 7.5,
                     "figure.facecolor": "white", "axes.facecolor": "white"})
NOTE = "documented approximation of the modern '57 Custom Deluxe; not measured hardware"

def read(path):
    with open(path) as f: return list(csv.DictReader(f))

def groupby(rows, key):
    out = {}
    for r in rows: out.setdefault(r[key], []).append(r)
    return out

def col(rows, k): return np.array([float(r[k]) for r in rows])

def save(fig, outdir, name):
    fig.tight_layout(); p = os.path.join(outdir, name); fig.savefig(p); plt.close(fig); print("  wrote", p)

def logfreq(ax, lo=20, hi=20000):
    ax.set_xscale("log"); ax.set_xlim(lo, hi); ax.set_xlabel("frequency (Hz)")
    ax.xaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: f"{v:g}" if v < 1000 else f"{v/1000:g}k"))

def fig_response(d, out):
    rows = read(os.path.join(d, "response.csv")); g = groupby(rows, "group")
    fig, ax = plt.subplots(2, 4, figsize=(15, 6.2))
    titles = {"tone": "tone control (Instrument 1, volume 7.2/12)", "volume": "Instrument volume (tone 7.8/12)", "jack": "jack states (the four inputs)", "interaction": "the silent channel still loads (guitar in Inst. 1)"}
    for i, key in enumerate(["tone", "volume", "jack", "interaction"]):
        for name, rr in groupby(g[key], "case").items():
            f = col(rr, "f_hz"); ax[0, i].plot(f, col(rr, "mag_db"), label=name); ax[1, i].plot(f, col(rr, "phase_deg"), label=name)
        ax[0, i].set_title(titles[key]); ax[0, i].set_ylabel("jack EMF -> speaker terminals (dB)"); ax[1, i].set_ylabel("phase (deg)"); ax[0, i].legend(); logfreq(ax[0, i]); logfreq(ax[1, i])
        ax[0, i].set_ylim(max(ax[0, i].get_ylim()[0], 0), None)
    fig.suptitle("Small-signal response at the idle operating point (AC solve of the full circuit, 8 ohm load): " + NOTE, fontsize=9)
    save(fig, out, "fig01_response_controls.png")

def fig_impedance(d, out):
    rows = read(os.path.join(d, "impedance.csv")); g = groupby(rows, "kind")
    fig, ax = plt.subplots(2, 2, figsize=(11, 6.2))
    for name, rr in groupby(g["input"], "case").items():
        f = col(rr, "f_hz"); ax[0, 0].plot(f, col(rr, "mag_ohm") / 1e3, label="jack " + name); ax[1, 0].plot(f, col(rr, "phase_deg"), label="jack " + name)
    ax[0, 0].set_yscale("log"); ax[0, 0].yaxis.set_major_formatter(matplotlib.ticker.ScalarFormatter()); ax[0, 0].yaxis.set_minor_formatter(matplotlib.ticker.NullFormatter()); ax[0, 0].set_yticks([100, 200, 500, 1000]); ax[0, 0].set_ylabel("|Z| (kohm)"); ax[0, 0].set_title("input impedance at each jack (reproduced by the circuit, not fitted: 1 Mohm / 136 kohm of Sec. 02/06)")
    ax[0, 0].axhline(1000, color="k", lw=0.5, ls=":"); ax[0, 0].axhline(136, color="k", lw=0.5, ls=":"); ax[1, 0].set_ylabel("phase (deg)")
    for a in (ax[0, 0], ax[1, 0]): a.legend(); logfreq(a)
    for kind, label in (("speaker", "ASSUMED speaker model (generic 12-inch alnico; PN 7708599000 unknown)"), ("output", "amplifier output impedance (speaker removed)")):
        for name, rr in groupby(g[kind], "case").items():
            f = col(rr, "f_hz"); ax[0, 1].plot(f, col(rr, "mag_ohm"), label=label); ax[1, 1].plot(f, col(rr, "phase_deg"), label=label)
    ax[0, 1].set_yscale("log"); ax[0, 1].yaxis.set_major_formatter(matplotlib.ticker.ScalarFormatter()); ax[0, 1].yaxis.set_minor_formatter(matplotlib.ticker.NullFormatter()); ax[0, 1].set_yticks([3, 5, 10, 20, 50, 100]); ax[0, 1].set_ylabel("|Z| (ohm)"); ax[0, 1].set_title("speaker-terminal impedances (Sec. 17: the load is complex)"); ax[1, 1].set_ylabel("phase (deg)")
    for a in (ax[0, 1], ax[1, 1]): a.legend(); logfreq(a)
    save(fig, out, "fig02_impedance.png")

def fig_harmonics(d, out):
    rr = read(os.path.join(d, "harmonics_vs_drive.csv")); P = col(rr, "power_W")
    fig, ax = plt.subplots(1, 2, figsize=(11, 4.2))
    for k, lab in (("h2_dbc", "H2"), ("h3_dbc", "H3"), ("h4_dbc", "H4"), ("h5_dbc", "H5")): ax[0].plot(P, col(rr, k), label=lab)
    ax[0].set_xscale("log"); ax[0].set_xlabel("output power into 8 ohm (W)"); ax[0].set_ylabel("harmonic level (dBc)"); ax[0].set_ylim(-100, 0); ax[0].axhline(-60, color="k", ls=":", lw=0.6)
    ax[0].set_title("harmonic levels versus drive, 1 kHz (Sec. 26 compares those above -60 dBc)"); ax[0].legend()
    ax[1].plot(P, col(rr, "thd_pct")); ax[1].set_xscale("log"); ax[1].set_yscale("log"); ax[1].set_xlabel("output power into 8 ohm (W)"); ax[1].set_ylabel("THD (%)")
    ax[1].axhline(5, color="r", ls="--", lw=0.7); ax[1].axvline(12.5, color="r", ls="--", lw=0.7); ax[1].text(12.9, 0.12, "documented rating\n12.5 W at 5 % THD\n(a calibration anchor,\nnot independent evidence)", color="r", fontsize=7)
    ax[1].set_title("THD versus power")
    save(fig, out, "fig03_harmonics_vs_drive.png")

def fig_spectra(d, out):
    rows = read(os.path.join(d, "spectra.csv")); g = groupby(rows, "case")
    fig, ax = plt.subplots(1, 2, figsize=(13, 4.4))
    for name, rr in g.items():
        if name.startswith("two-tone"): continue
        ax[0].plot(col(rr, "f_hz"), col(rr, "dbv"), lw=0.8, label=name)
    ax[0].set_ylim(-130, 25); ax[0].set_xlim(0, 20000); ax[0].set_xlabel("frequency (Hz)"); ax[0].set_ylabel("speaker-terminal level (dBV peak)"); ax[0].set_title("spectra of a 1 kHz tone at three levels (8 ohm load; the 120 Hz ripple sidebands are real supply modulation)"); ax[0].legend()
    for name, rr in g.items():
        if name.startswith("two-tone"):
            f = col(rr, "f_hz"); y = col(rr, "dbv"); ax[1].plot(f, y, lw=0.8)
            for k, (a, b) in {"f1": (1, 0), "f2": (0, 1), "2f1-f2": (2, -1), "2f2-f1": (-1, 2), "3f1-2f2": (3, -2), "3f2-2f1": (-2, 3)}.items():
                fx = a * 700 + b * 1000
                if 0 < fx < 6000:
                    i = int(np.argmin(abs(f - fx))); ax[1].annotate(k, (f[i], y[i]), textcoords="offset points", xytext=(0, 6), ha="center", fontsize=6.5)
            ax[1].set_title(name)
    ax[1].set_xlim(0, 6000); ax[1].set_ylim(-130, 20); ax[1].set_xlabel("frequency (Hz)"); ax[1].set_ylabel("dBV peak")
    save(fig, out, "fig04_spectra_imd.png")

def fig_waveforms(d, out):
    g = groupby(read(os.path.join(d, "waveforms.csv")), "case")
    fig, ax = plt.subplots(1, 3, figsize=(13, 3.6))
    for a, (name, rr) in zip(ax, g.items()):
        a.plot(col(rr, "t_ms"), col(rr, "terminal_V"), label="speaker terminals (V)"); a2 = a.twinx(); a2.plot(col(rr, "t_ms"), col(rr, "jack_V") * 1e3, color="C1", lw=0.8, label="jack (mV)"); a2.grid(False)
        a.set_title(name); a.set_xlabel("time (ms)"); a.set_ylabel("terminal volts"); a2.set_ylabel("jack mV", color="C1")
    save(fig, out, "fig05_waveforms.png")

def fig_residual(d, out):
    rr = read(os.path.join(d, "residual.csv")); t = col(rr, "t_ms")
    fig, ax = plt.subplots(1, 2, figsize=(11, 3.8))
    ax[0].plot(t, col(rr, "ref_8x_V"), "k", lw=0.8); ax[0].set_title("3.1 kHz, 60 mV at the jack: the 8x render (reference)"); ax[0].set_xlabel("time (ms)"); ax[0].set_ylabel("terminal V")
    for k, lab in (("res_1x_V", "1x"), ("res_2x_V", "2x (default)"), ("res_4x_V", "4x")): ax[1].plot(t, col(rr, k) * 1e3, label=lab)
    ax[1].set_title("time-domain residual against the 8x render of the same model"); ax[1].set_xlabel("time (ms)"); ax[1].set_ylabel("residual (mV)"); ax[1].legend()
    save(fig, out, "fig06_oversampling_residual.png")

def fig_burst(d, out):
    rr = read(os.path.join(d, "burst.csv")); t = col(rr, "t_ms")
    fig, ax = plt.subplots(2, 3, figsize=(14, 6.2))
    ax[0, 0].plot(t, col(rr, "b1_V"), label="B+1"); ax[0, 0].plot(t, col(rr, "b2_V"), label="B+2 (screens)"); ax[0, 0].plot(t, col(rr, "b3_V"), label="B+3 (preamp)"); ax[0, 0].set_title("rails"); ax[0, 0].legend()
    ax[0, 1].plot(t, col(rr, "vk_V")); ax[0, 1].set_title("6V6 shared cathode (bias memory, Sec. 10)")
    ax[0, 2].plot(t, col(rr, "g3_V"), label="V3 grid"); ax[0, 2].plot(t, col(rr, "g4_V"), label="V4 grid"); ax[0, 2].set_title("power-valve grids (coupling-capacitor charge: blocking, Sec. 10)"); ax[0, 2].legend()
    ax[1, 0].plot(t, col(rr, "p2b_V"), label="cathodyne plate"); ax[1, 0].plot(t, col(rr, "kcy_V"), label="cathodyne cathode"); ax[1, 0].set_title("cathodyne outputs (Sec. 09)"); ax[1, 0].legend()
    ax[1, 1].plot(t, col(rr, "p1b_V")); ax[1, 1].set_title("first-stage plate (V1B)")
    env = col(rr, "out_env_V"); post = t >= 156; settled = np.mean(env[t >= t.max() - 70])
    ax[1, 2].plot(t[post], 20 * np.log10(env[post] / settled)); ax[1, 2].set_ylabel("probe level re settled (dB; 4-cycle lock-in)"); ax[1, 2].set_title("quiet 1 kHz probe after the 150 ms, 40 mV burst: gain dip and recovery")
    ax[1, 2].axhline(0, color="k", lw=0.5)
    for a in ax.flat: a.set_xlabel("time (ms), burst starts at 0"); a.axvspan(0, 150, color="orange", alpha=0.12) if a is not ax[1, 2] else None
    fig.suptitle("State trajectories around a burst (Sec. 26 'rail/cathode trajectories'): " + NOTE, fontsize=9)
    save(fig, out, "fig07_burst_trajectories.png")

def fig_conduction(out):
    rr = read(os.path.join(REF, "conduction.csv")); wf = groupby(read(os.path.join(REF, "conduction_waveforms.csv")), "level")
    fig, ax = plt.subplots(1, 3, figsize=(14, 4))
    for name, c in (("low", "C0"), ("rated", "C1"), ("heavy", "C3")):
        r = wf[name]; t = col(r, "t_ms"); ax[0].plot(t, col(r, "ip_V3_mA"), color=c, label=f"V3, {name}"); ax[0].plot(t, col(r, "ip_V4_mA"), color=c, ls="--", label=f"V4, {name}")
    ax[0].set_title("plate currents over one 1 kHz cycle (low ~1 W, rated ~12 W, heavy)"); ax[0].set_xlabel("time (ms)"); ax[0].set_ylabel("mA")
    X = col(rr, "jack_peak_V")[1:] * 1e3; P = col(rr, "P_out_W")[1:]
    ax[1].plot(X, col(rr, "ip3_duty")[1:] * 360, label="V3"); ax[1].plot(X, col(rr, "ip4_duty")[1:] * 360, label="V4"); ax[1].plot(X, col(rr, "overlap")[1:] * 360, "k--", label="both conducting")
    ax[1].set_xscale("log"); ax[1].set_xlabel("1 kHz drive, jack peak (mV)"); ax[1].set_ylabel("conduction angle (deg of 360)"); ax[1].set_title("conduction angle (plate current > 1 mA) versus drive"); ax[1].legend(loc="lower left")
    ax2 = ax[2].twinx(); ax2.grid(False)
    ax[2].plot(X, col(rr, "vk_mean_V")[1:], "C0", label="mean cathode bias"); ax[2].set_ylabel("cathode bias (V)", color="C0")
    ax2.plot(X, col(rr, "b1_V")[1:], "C1", label="B+1 mean"); ax2.set_ylabel("B+1 (V)", color="C1")
    ax[2].set_xscale("log"); ax[2].set_xlabel("1 kHz drive, jack peak (mV)"); ax[2].set_title("bias shift (up) and rail sag (down) with drive")
    ax[0].legend(ncol=2, loc="upper center", fontsize=6.5)
    save(fig, out, "fig08_valve_conduction.png")

def fig_acoustic(d, out):
    g = groupby(read(os.path.join(d, "acoustic.csv")), "case")
    fig, ax = plt.subplots(1, 1, figsize=(8.5, 4.2))
    for name, rr in g.items(): ax.plot(col(rr, "f_hz"), col(rr, "mag_db_re_1k"), label=name, lw=1.4 if name.startswith("speaker-terminal") else 1.0, ls="--" if name.startswith("speaker-terminal") else "-")
    ax.set_ylabel("level re 1 kHz (dB)"); ax.set_ylim(-45, 20); logfreq(ax); ax.legend(); ax.set_title("PROVISIONAL acoustic operator at three mic positions vs terminal response\n(a measured voltage-to-microphone IR replaces this stage)")
    save(fig, out, "fig09_acoustic_positions.png")

def fig_sensitivity(out):
    rows = read(os.path.join(REF, "sensitivity.csv"))
    rows = sorted(rows, key=lambda r: -float(r["score_dB"]))[:26][::-1]
    groups = sorted(set(r["group"] for r in rows)); cmap = {g: f"C{i}" for i, g in enumerate(groups)}
    fig, ax = plt.subplots(1, 1, figsize=(11, 7.4))
    ax.barh([r["parameter"][:58] for r in rows], [float(r["score_dB"]) for r in rows], color=[cmap[r["group"]] for r in rows])
    ax.set_xscale("symlog", linthresh=0.5); ax.axvline(0.5, color="k", ls=":", lw=0.7); ax.set_xlabel("largest effect on any of 15 features when the parameter moves across its stated range (dB)")
    ax.legend(handles=[matplotlib.patches.Patch(color=cmap[g], label=g) for g in groups], loc="lower right", fontsize=7)
    ax.set_title("One-parameter-at-a-time sensitivity (Sec. 13): which unknowns matter most (top 26 of 41)")
    save(fig, out, "fig10_sensitivity.png")

def fig_capture_matrix(out):
    rows = [r for r in read(os.path.join(REF, "capture_matrix_summary.csv")) if r["set"] == "core" and r["level"] == "clean"]
    jacks = ["I1", "I2", "M1", "M2"]; fig, ax = plt.subplots(2, 1, figsize=(13, 6.4))
    for a, key, title, cm in ((ax[0], "small_signal_gain_dB", "small-signal gain, jack to 8 ohm terminals (dB)", "viridis"), (ax[1], "onset_V", "clipping onset: input peak for 1 kHz THD = 1 % (mV, log)", "magma_r")):
        M = np.zeros((4, 27)); lab = []
        for j, jk in enumerate(jacks):
            rr = sorted([r for r in rows if r["jack"] == jk], key=lambda r: (float(r["inst12"]), float(r["mic12"]), float(r["tone12"])))
            M[j, :] = [float(r[key]) * (1e3 if key == "onset_V" else 1) for r in rr]; lab = [f"{r['inst12']}/{r['mic12']}/{r['tone12']}" for r in rr]
        im = a.imshow(np.log10(M) if key == "onset_V" else M, aspect="auto", cmap=cm); a.set_yticks(range(4)); a.set_yticklabels(jacks); a.set_xticks(range(27)); a.set_xticklabels(lab, rotation=90, fontsize=6.5)
        a.set_title(title + "   (columns: instrument/mic/tone knob, printed 0-12 scale)"); a.grid(False)
        V = np.log10(M) if key == "onset_V" else M; nv = (V - V.min()) / (V.max() - V.min() + 1e-12)
        for j in range(4):
            for i in range(27):
                bright = nv[j, i] > 0.62 if key == "small_signal_gain_dB" else nv[j, i] < 0.45        # viridis is bright at high values, magma_r is bright at low values
                a.text(i, j, f"{M[j, i]:.0f}", ha="center", va="center", fontsize=5.5, color="k" if bright else "w")
        plt.colorbar(im, ax=a, pad=0.01)
    save(fig, out, "fig11_capture_matrix_overview.png")

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--build", default=""); ap.add_argument("--data", default=""); ap.add_argument("--out", default=os.path.join(ROOT, "docs", "figures"))
    a = ap.parse_args(); os.makedirs(a.out, exist_ok=True)
    d = a.data
    if not d:
        exe = os.path.join(a.build, "cd57_diagnostics")
        if not os.path.exists(exe): sys.exit(f"{exe} not found: build the tools first or pass --data")
        d = tempfile.mkdtemp(prefix="cd57_diag_"); subprocess.check_call([exe, "--out", d])
    for fn in (fig_response, fig_impedance, fig_harmonics, fig_spectra, fig_waveforms, fig_residual, fig_burst, fig_acoustic): fn(d, a.out)
    fig_conduction(a.out); fig_sensitivity(a.out); fig_capture_matrix(a.out)

if __name__ == "__main__":
    main()
