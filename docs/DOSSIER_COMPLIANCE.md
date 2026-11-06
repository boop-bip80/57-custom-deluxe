# Dossier compliance audit (adversarial, evidence-based)

Scope: the implementation at commit `2a356c9` (branch `claude/eager-edison-6lario`) against the "Public-source Engineering
Dossier" (Sec. 02-31). Report only: no source file was changed. Probes live outside the repo (`/tmp/claude-0/audit/probes`);
every number below was measured by building the JUCE-free tools (`-DCD57_BUILD_PLUGIN=OFF`) and driving `CD57Engine` /
`AmpCircuit` / `Network` directly. Baseline reproduced first: `cd57_validate` = 27 passed / 0 failed, report identical to
`docs/VALIDATION_REPORT.md`.

Status of this document: **complete (checkpoint 4)**. Written incrementally; the commit history of `docs/DOSSIER_COMPLIANCE.md` on this branch shows each checkpoint.

Evidence tags: **MEASURED** (I ran it), **READ** (code reading, not executed), **DOSSIER** (quoted requirement).

## 0. Summary

The implementation is a real circuit-level model, not a chain of EQ and waveshapers: an independent re-implementation of the netlist reproduces its DC point to 0.23 V and its
small-signal gains to 0.0025 dB (Sec. 5), the netlist matches Fig. 7 and the Sec. 14 table element for element, and the solver, block independence, 10-minute heavy-drive stability, automation and
rate independence all hold up under attack (Sec. 0.2). The weaknesses are (a) the **validation suite**: of the 27 PASS lines, two are meaningless (alias, burst recovery), one is a
calibration reported as evidence (12.5 W), two measure an artefact (sample-rate THD, "independent" AC solver at a displaced bias point), and several cannot fail; (b) the **power-valve
and rectifier constitutive laws**, which are anchored on numbers from outside the PDF and not fitted to the dossier's own 6V6 sheet; (c) **real-time hygiene**: allocation in the callback, an
unbounded recovery path, an 8x mode that is not real-time; and (d) a handful of **provenance and bookkeeping gaps** (manifest entries that drive nothing, an unrun Jacobian check, a phantom
heating state).

### 0.1 Defects ranked by impact (what to fix first)

| Rank | ID | Sev | One line | Measured evidence |
|---|---|---|---|---|
| 1 | D5 | S1 | 6V6 law fitted to four external numbers, not to the dossier's 6V6 sheet; no error reported where the amp runs (347 V / 298 V / -21 V) | model/sheet current ratio 0.21-1.03 (mean 0.76); class-AB1 target 70 mA, model 59.6 mA; idle plate dissipation 13.5 W per tube |
| 2 | D6 | S1 | 12.5 W / <5 % THD is a one-parameter calibration, +-25 % over its own interval, and mis-measured low | 10.3 / 13.55 / 15.9 W at PT 330 / 360 / 380 V; report prints 12.85 W |
| 3 | D1 | S2 | alias check measures the folded 8th harmonic; true alias floor is far above the dossier's -80 dBFS limit at the default | 2x -39, 4x -58, 8x -72, 16x -86.5 dBFS (ripple-free) |
| 4 | D7 | S2 | convergence is a 10 mV step test; silicon mode accepts a 53 A KCL error; failures inside the plugin's own range; NaN stalls the audio thread | 36 / 89 / 744 failures at 10 V / 80 V / cal 5 V + 24 dB; NaN 56 ms |
| 5 | D9 | S2 | heap allocation inside `process()` whenever sub-stepping engages (README says none) | 10-15 allocations per block under drive |
| 6 | D3 | S2 | burst "35 ms recovery" is broadband rms of the thump; check cannot fail | lock-in: 0.825x at 35 ms, 65 ms to 10 %, 150 ms to 5 %, dip to 0.65x |
| 7 | D8 | S2 | 44.1 kHz THD discrepancy is a non-integer analysis window | 0.12 dB true spread over 14 rate/oversampling configurations |
| 8 | D10 | S2 | dummy-load mode heats a disconnected coil; no terminal-current output | +26 K phantom heating after 30 s; Dossier Sec. 05 asks for V and I |
| 9 | D2 | S2/S3 | decimator transition 0.5-0.55 fs lets 24-26 kHz fold at < 30 dB; latency "exact" but rounded up | -8.7 / -12.2 / -29 dB at 24.5 / 25 / 26.4 kHz; 30.5 vs 31 samples |
| 10 | D11 | S3 | 42 of 55 manifest entries drive nothing; Jacobian check never run; external anchors labelled as dossier-cited; 12AY7 mu/rp +10 % | Jacobian 3.1e-5 (true, but unsupported by tests) |
| 11 | D12, D13, D15, D16, D14 | S3 | 8x not real-time; engine swap loses state; discretisation reported at the best rate; assumed saturation law active; possible HF double count | see each |

What I would fix first: D5 and D6 together (they decide whether any Sec. 26 nonlinear number means anything), then the validation metrics D1/D3/D8 (they currently give false assurance), then D7/D9
(real-time safety), then D10/D11 (bookkeeping).

### 0.2 What held up under attack (positive findings, all measured)

* Independent re-implementation (own parser, stamps, tube laws, DC solver, AC solve): DC <= 0.23 V, AC <= 0.0025 dB and 0.003 deg (Sec. 5). Jacobian vs finite differences 3.1e-5 over 38 heavy-drive states (`p12`).
* Netlist = Fig. 7 = Sec. 14 table, including the jack switching (1.0 M / 136.0 k / -6.03 dB reproduced, not fitted), bootstrapped cathodyne, plate-coupled wipers, bright and tone caps.
* Block-size independence is bit-exact in 32 configurations including speaker heating (D4). Rate independence at 1 kHz: H2-H7 within 0.12 dB over 44.1-192 kHz hosts and 1-8x oversampling (D8), H3 within 0.2 dB down to a 22.05 kHz host (`p27`).
* 10 minutes of randomised heavy plucked drive with speaker heating: 0 non-finite samples, 0 failures, 0 panics, rails and coil temperature bounded (`p14`).
* Automation: no click on an abrupt 0.3->0.9 volume step, no zipper in a slow sweep, power switch decays and recharges smoothly (`p10`).
* LU kernel and generic solver identical at 7 extreme control states with 0 guarded pivots; trapezoid RC error 3.3e-6 V; LC energy 0.9999-1.0000 (`p23`, `p26`).
* Speaker coupling: circuit cone velocity per volt equals the closed form used to calibrate the acoustic block (ratio 1.0000, 0.00 deg); heating changes only Re(T) in the circuit (no second compressor); user-IR and provisional acoustic modes are mutually exclusive.
* The shipped baseline VST3 (built by the coordinator in `/tmp/claude-0/b_full`) passes `cd57_host_test`: 181 checks, "ALL PASSED", latency 31 samples at 44.1/48/96 kHz. I did not rebuild the plugin and did not run valgrind.
* 5Y3 rectifier + 30 ohm source + 4 uF reservoir versus the **held-out** left panel of the JJ 5Y3S sheet: within about 3-4 % (7-18 V low) at Utr 220-350 V (`p25`, reading error +-8 V).

### 0.3 Reproduction index

Probes (all under `/tmp/claude-0/audit/probes/`, built with `build.sh NAME`; none is in the repo): `p1_alias` (D1), `p2_oslat` (D2), `p3_burst` (D3), `p4_block` (D4), `p5_jacks`, `p6_pots`, `p7_dc`, `p8_silicon` (D7),
`p9_stress` (D7, D12), `p10_auto`, `p11_rate` (D8), `p12_jac` (D11), `p13_speaker` (D10, D14), `p14_long`, `p15_alloc` / `p16_alloc2` (D9), `p17_swap` (D13), `p18_tube` (D5, D11), `p19_anchor` (D6),
`p20_acerr` (D15), `p21_hum`, `p22_cpp_ref.cpp` + `p22_indep.py` (Sec. 5), `p23_misc`, `p25_y3`, `p26_sat` (D16), `p27_prep`. The `scripts/fit_tubes.py` numbers in D5 come from a copy of the script run under
`/tmp/claude-0/audit/fitroot` (the repo's `data/` and `generated/` were not touched).

Not verified: the JUCE plugin was read, not rebuilt; Windows/macOS builds; valgrind; anything that needs hardware (Sec. 4).

---

## 1. Ranked defect list

Severity: **S1** changes the electrical/audible result or invalidates a headline claim; **S2** wrong or misleading
validation/diagnostic or real-time hazard; **S3** robustness / hygiene / documentation. Entries are in the order they were found; the impact ranking is in Sec. 0.1.

### D1 (S2, validation invalid) The "strongest alias" figure is not aliasing and not mains ripple: it is the 8th harmonic folded by the half-band transition band

* Where: `tools/cd57_validate.cpp` lines 357-371, reported in `docs/VALIDATION_REPORT.md` Sec. 8 (-14.9 / -27.1 / -27.8 / -27.8 dBFS).
* Reproduce: `/tmp/claude-0/audit/probes/p1_alias.cpp` (3.1 kHz, 60 mV pk, Instrument vol 0.6, speaker load, fs 48 kHz). The strongest
  "non-harmonic" line is at **23 200 Hz** for every oversampling factor. 48 000 - 8 x 3 100 = 23 200 Hz: it is the true 8th
  harmonic (24.8 kHz, above the host Nyquist) leaking through the output decimator and folding. The test only excludes lines
  within 80 Hz of h x 3.1 kHz for h <= 8 *at their unfolded frequencies*, so this line is classified as "alias". It cannot fall
  with oversampling because the host-rate decimation filter is what passes it (see D2).
* Real alias floor, ripple-free supply (caps x1000, same trick as validation Sec. 5), 23.2 kHz line excluded, dBFS re 20 V pk:

  | oversampling | strongest other non-harmonic line |
  |---|---|
  | 1x | -18.3 (20.1 kHz) |
  | 2x (plugin default) | -38.7 |
  | 4x | -58.2 |
  | 8x | -72.2 |
  | 16x | -86.5 |

  With the stock supply the 3 100 +- 120 Hz mains-ripple sidebands (-38.4 dBFS, i.e. about 2 % amplitude modulation of the
  3.1 kHz output by B+ ripple) mask the floor at 4x/8x/16x (strongest lines 3 220 and 2 979 Hz at 4x, 8x and 16x).
* Dossier Sec. 26 candidate limit "additional aliases below -80 dBFS for a documented stimulus": **not met at 1x, 2x, 4x, 8x**
  (-18 / -39 / -58 / -72 dBFS) for this stimulus, met only at 16x (-86.5), which the plugin does not offer. The default is 2x.
* Impact: the one aliasing PASS in the report says nothing about aliasing; the real default-setting alias level is 40 dB above
  the dossier's candidate limit for a heavily driven 3.1 kHz tone (a realistic worst case for the top of a guitar's range).
* Fix: classify lines after folding (compute the folded frequency of every harmonic up to the internal Nyquist, exclude those), run
  the measurement with a ripple-free or ripple-synchronous supply or difference against a higher-rate reference render, and
  compare each factor against a 16x (or 32x) reference render as Sec. 23 asks ("compare realtime candidates at several
  oversampling factors" against "a high-accuracy offline reference at a substantially higher internal rate"). No such
  reference render exists anywhere in the repo.

### D2 (S2/S3) Output decimation filter: wide 0.5-0.55 fs transition band, fractional "exact" latency reported rounded up

* Where: `src/dsp/Oversampler.h` (63-tap, 80 dB half-band at the 2fs->fs stage; 23-tap 60 dB for higher stages).
* Reproduce: `/tmp/claude-0/audit/probes/p2_oslat.cpp` (impulse and tones through `Oversampler::up` then `down`).
  * Attenuation of a component above host Nyquist before folding: 24.5 kHz -8.7 dB, 25.0 kHz -12.2 dB, 26.4 kHz -29 dB, 28.8 kHz
    -82 dB (fs = 48 kHz, identical for 2x/4x/8x). Passband droop: -0.64 dB at 0.45 fs (21.6 kHz), -5 to -6 dB at 0.48 fs.
  * Group delay measured (centroid of the up+down impulse response): **30.500 / 35.750 / 38.375 samples** at 2x / 4x / 8x.
    The engine reports `ceil()`: 31 / 36 / 39. README says "exact reported latency"; it is rounded up by 0.5 / 0.25 / 0.625 sample,
    and a half-sample error is not zero.
* Impact: aliasing of components just above Nyquist is the dominant "alias" line (D1); 5 dB droop above 23 kHz is inaudible; the
  latency error is sub-sample (S3) but contradicts the "exact" wording and Sec. 23 "Report their latency and phase behavior".
* Fix: report fractional latency in the documentation (the host only takes an integer) or use a design with an integer delay
  (odd tap count at each stage chosen so up+down delay is integral); state passband ripple / transition / stopband in the report as Sec. 23 requires.

### D3 (S2, validation invalid) The burst/recovery "35 ms" figure is a broadband-rms artefact; the check cannot fail

* Where: `tools/cd57_validate.cpp` lines 293-301; report Sec. 7 ("returns to within 10 % ... in 35 ms (envelope starts at 5.70x of final)").
* Reproduce: `/tmp/claude-0/audit/probes/p3_burst.cpp` (150 ms burst at 120 mV, then a 3 mV 1 kHz tone, vol 0.5, dummy 8 ohm, 2x).
  The report's "envelope" is the **broadband rms** of one cycle, so it includes the low-frequency thump of the coupling
  capacitors discharging (non-1 kHz residual 5.3 V rms at t = 0, 0.55-0.61 V rms still at 20-45 ms). Measured with a 1 kHz lock-in:

  | t after burst | broadband env (report metric) / final | 1 kHz lock-in / final |
  |---|---|---|
  | 5 ms | 1.63 | 0.90 (window still overlaps the burst tail) |
  | 10 ms | 0.70 | 0.67 |
  | 20 ms | 0.79 | 0.69 |
  | 35 ms | 0.93 (report: "within 10 %") | **0.825** |
  | 65 ms | 0.93 | 0.91 (first window within 10 %) |
  | 100 ms | 0.94 | 0.934 |
  | 150 ms | 0.96 | 0.96 |
  | 300 ms | 0.99 | 0.99 |

  So the real small-signal gain after a loud burst dips to ~0.65-0.7 x settled (about -3.5 dB) and needs about 65 ms to get within
  10 % and about 150 ms to get within 5 % (the 16 uF x 22 k supply section and the 0.1 uF x 220 k grid leaks both contribute).
  The report understates the 10 % recovery time by roughly a factor of two and the check
  (`env.front() != fin && isfinite(ms) && ms < 3000`) is satisfied by any floating-point difference and any recovery below 3 s.
* Dossier: Sec. 09, 10, 26 ("Recovery landmarks within 10 %", "Burst envelope within 5 %", "Track both cathodyne outputs and both output-valve grids").
  The report logs none of the internal trajectories the dossier asks for; my probe prints k34, g3, g4, c3, c4, m, b1-b3, p3, p4, kcy, p2b.
* Fix: measure a narrow-band (lock-in) envelope, report the full dip-and-recovery trajectory and time constants, and make the check
  compare against an explicitly stated physical bound (e.g. recovery time within a derived window of the isolated RC scales in Sec. 14).

### D4 (S3, positive finding that stands) Block-size independence is genuine

* Reproduce: `/tmp/claude-0/audit/probes/p4_block.cpp`: blocks 1, 7, 37, 64, 255, 480, 511 and maxBlock 4096/block 100 versus 512,
  at 1x, 2x, 4x, 8x, speaker load, **speaker heating ON**: max difference **0.000e+00 V** in all 32 cases (48 000 x 6 s heavy-drive
  signal). The test in the report switches heating off (`baseParams().speakerHeating = false`) while its comment claims to cover
  "heating"; the claim is nevertheless true when heating is enabled. Caveat: host automation is applied at block boundaries
  (`CD57Engine::process` calls `applyPending` once per call), so automation *timing* is block-quantised; this is inherent to
  hosts and not a defect, but "same input -> same output regardless of block size" does not extend to automated parameters.

### D5 (S1) The 6V6 power-valve law is not fitted to the dossier's 6V6 data; it is a four-number fit to values from outside the PDF, with unreported error where the amplifier runs

* Where: `scripts/fit_tubes.py` lines 96-160, `generated/TubeFits.h`, `src/dsp/Devices.h::evalPentode`; manifest note on `tube_6V6_pentode`.
* What the fit uses: RCA 6V6GT 250 V / 250 V / -12.5 V = 45 mA, Ig2 4.5 mA, gm 4.1 mS, rp 52 k, and "class-AB1, 250 ohm shared Rk: 2 x 35 = 70 mA".
  **None of these numbers is in the dossier text** (S19 is a bare citation; the dossier quotes numbers only for the 12AY7). They are labelled
  `DATASHEET ... cited by the dossier` in the script header. Per the project rule they must be tagged "external, not in dossier".
  The only 6V6 curves the dossier does contain (JJ 6V6S, Fig. 11) are explicitly **not used** as a constraint ("cross-check ... NOT used").
  Shape parameters `Vk = 55`, `q = 1.5`, `Ex ~ 1.35`, `mu ~ 11` are priors, not data.
* Measured (copy of the script run in `/tmp`, repo untouched): model / JJ-sheet current ratio if the sheet is triode-connected:
  1.03, 0.99 (150-200 V, Vg -2 V), then **0.98, 0.90, 0.89, 0.78, 0.81, 0.71** at 150-200 V with Vg -6, -12, -20 V (200/-6, 150/-6, 200/-12, 150/-12, 200/-20, 150/-20), and **0.85, 0.57, 0.68, 0.21, 0.45** at 100/-2, 50/-2, 100/-6, 50/-6, 100/-12.
  The manifest note says "agrees within a few % at 150-200 V": true for 3 of the 8 points in that range only (worst -29 %). The class-AB1 constraint
  the script itself sets (70 mA) ends at **59.6 mA** (-15 %).
* The amplifier does not run where any of this data lives: idle point is Vpk 347 V, Vsk 298 V, Vgk -21.1 V (probe `p7_dc.cpp`, `p18_tube.cpp`):
  gm 3.19 mS, rp 69 k, Ip 38.8 mA, Is 3.38 mA. Cutoff is a long Koren-form tail (Vgk -40 V: 6.4 mA; -50 V: 2.1 mA; -60 V: 0.68 mA, plate 347 V / screen 298 V),
  which governs class-AB crossover, blocking and rail collapse. Dossier Sec. 16: "Report the fitted domain and show error outside it" - not done.
  Idle plate dissipation 13.5 W per tube (screen 1.0 W) at nominal, 16.7 W at 110 % line, 20.5 W with the optional silicon rectifier.
* Impact: this law sets rated power, THD knee, sag, bias shift and blocking recovery, i.e. every Sec. 26 nonlinear number. Combined with D6 it
  means the "12.5 W" agreement is a property of the calibrated transformer voltage, not of a validated valve.
* Fix: (a) tag the anchors external; (b) fit to the JJ 6V6S sheet (Fig. 11) as the in-dossier dataset and state whether it is triode-connected;
  (c) report residuals on the fitted domain and an explicit extrapolation error at 347/298/-21 V; (d) add the screen-current and g1-current
  surfaces the dossier requires as separate fits.

### D6 (S1 for the claim, S2 numerically) The 12.5 W / <5 % THD "anchor" is a one-parameter calibration with +-25 % sensitivity, reported as PASS beside independent checks, and the coarse search under-reports the miss

* Where: `tools/cd57_validate.cpp` lines 195-213 (search `ev *= 1.1`, takes the maximum P with THD < 5 %, window 11.25-13.75 W);
  `data/parameters/cd57_nominal.json` `pt_hv_half_winding_rms` = 360 V "calibrated" with interval [330, 380].
* Reproduce: `/tmp/claude-0/audit/probes/p19_anchor.cpp`, fine amplitude steps (x1.03), coherent 40-cycle windows, interpolated to exactly THD = 5 %
  (vol 0.55, tone 1.0, 8 ohm dummy, 1 kHz):

  | PT secondary (V rms per half) | P at THD = 5 % | idle B+1 | idle cathode |
  |---|---|---|---|
  | 330 (interval low) | **10.31 W** | 340.2 V | 19.0 V |
  | 342 | 11.59 W | 352.9 V | 19.9 V |
  | 360 (shipped) | **13.55 W** | 372.1 V | 21.1 V |
  | 370 | 14.69 W | 382.8 V | 21.8 V |
  | 380 (interval high) | **15.85 W** | 393.4 V | 22.6 V |

  So (i) over the model's own sensitivity interval the headline number moves from -17 % to +27 % of 12.5 W; (ii) at the shipped value the true
  5 %-THD power is **13.55 W (+8 %)**, not the 12.85 W printed in the report: the x1.1 amplitude grid (0.83 dB) with "largest P below 5 %"
  quantises the answer low by up to a grid step; (iii) THD at exactly 12.5 W is about 3.1 % (the spec "12.5 W at < 5 % THD" is a
  *minimum*; the model exceeds it, but the check is written as a two-sided +-10 % window); (iv) the THD curve (probe output) is smooth and monotone:
  1.2 % at 5.3 W, 2.7 % at 12.0 W, 5.3 % at 13.6 W, 18 % at 14.9 W.
* The report does say "CALIBRATION" in an INFO line, but counts the check in "27 passed" with the independent ones, and no sensitivity study
  appears anywhere although Sec. 13 ("sensitivity analysis around the verified baseline") and Sec. 28 ("assumed value with a sensitivity interval") ask for it.
* Fix: move the anchor out of the PASS count (report it as a calibration residual), publish the sensitivity table above, use the fine-step
  interpolated crossing, and test the spec as `P(THD<5 %) >= 12.5 W`.

### D7 (S2) Solver acceptance is a 10 mV step test, not a residual test; silicon-rectifier mode accepts states with a 53 A KCL error; "unrecovered failures" occur inside the plugin's own parameter range; one NaN mutes a block and stalls the audio thread 56 ms

* Where: `Network::newton` (convergence = |dz| < 1e-2 V + 1e-4 |z|, `absTolV`, `relTol`); `CD57Engine::process` NaN path (`recover()`).
* Reproduce `p8_silicon.cpp`: stock 5Y3: worst converged-step KCL residual 5.7e-7 A, peak diode current 0.31 A, 1.00 Newton it/step, 0 fall-backs.
  `rectifier = silicon`: **worst residual 52.9 A**, peak diode current 53 A (the circuit can only supply ~1 A through the 90 ohm source), up to 14 iterations; the validation
  "KCL residual < 1 uA" check runs only for the stock rectifier, and `failures` stays 0 because a step-size test passed. Dossier Sec. 21/26: "Record solver failures";
  "no ... hidden solver failures".
* Reproduce `p9_stress.cpp` (speaker load, vol 0.9, 1 kHz, 0.4 s): jack amplitude 0.05 / 0.2 / 0.6 V: 0 failures (0 / 1.0 / 2.7 % of steps sub-stepped);
  **10 V: 36 failures; 80 V: 89 failures**; inputCal 5 V + trim +24 dB (79 V at the jack, both are plugin parameter ranges): **744 failures** in 102 400 steps;
  1e6 FS input: **165 ms for one 512-sample block** and 774 cumulative failures on that engine (NaN, Inf and 1e6 blocks) (15x real time; the worst case per sample is
  20 iterations x (1 + 2 + 4 + 8 + 16 + 32 sub-steps) = 1 260 Newton iterations against a typical 1.25). The report's failure check stops at 600 mV at the jack.
  The fall-back for an unrecovered step is "hold the last accepted state for one step" (counted, shown in the UI) - defined and visible, as Sec. 31 requires, but its rate inside the supported range is not stated.
* NaN: one NaN sample in a 512-block -> the whole block is output as zeros, `recover()` re-runs `initialiseQuiescent` on the audio thread
  (**56 ms** for a 10.7 ms block; Inf 50 ms), and the next block starts from the quiescent state (history lost).
* Fix: add a residual criterion (scaled per unknown class, as Sec. 21 says), include the silicon option in the KCL test, state the supported jack-voltage range
  (clamp inputCal x trim to what the solver can follow), sanitise non-finite input before it enters the oversampler, and bound `recover()`.

### D8 (S2, validation invalid) The 44.1 kHz "THD differs" row is an analysis artefact (non-integer window); with coherent windows the model is rate-independent to 0.12 dB

* Where: `analyseTone` in `tools/cd57_validate.cpp` line 83-98: `per = round(sr/f0)`, `n = per * 40`, analysis frequency `fa = sr*round(f0*n/sr)/n`.
  At 44.1 kHz `per = 44` (true 44.1), the 40-"cycle" window is 39.9 cycles and the analysis bin is **1002.27 Hz** for a 1000 Hz signal.
* The report's table (Sec. 8): THD 5.66 % / 6.27 % / 6.26 % at 44.1 / 48 / 96 kHz; the check then looks only at H3 with a 1.5 dB tolerance (the dossier's own tolerance is 1 dB).
* Reproduce `p11_rate.cpp` (40 ms coherent lock-in windows, exact cycles at every rate): 1 kHz, 10 / 30 / 60 mV pk, Instrument vol 0.5, dummy load; host 44.1, 48, 96, 192 kHz x 1x, 2x, 4x, 8x (14 configurations)
  against the 96 kHz x 8x = 768 kHz render: **worst |dHn| over H2-H7 (those above -60 dBc) = 0.12 dB** (all configurations, all three drives). The reported 44.1 kHz difference is the window, not the model.
  A side result: the steady-state 1 kHz harmonic levels are already converged at **1x** (0.08 dB), so oversampling only matters for high fundamentals (D1).
* Fix: use coherent windows (choose N = integer cycles; lock-in) and report all of H2-H9 against a high-rate reference, with the dossier's 1 dB criterion.

### D9 (S2) Heap allocation in the audio callback (README and TRACEABILITY say there is none)

* Reproduce `p15_alloc.cpp`, `p16_alloc2.cpp` (global `operator new` counter around `CD57Engine::process`):
  * every step that falls back to **sub-stepping** allocates (`Network::step` builds a local `Snapshot` of eight `std::vector`s): **10.4 allocations per 512-sample block
    on average (worst 27) at 0.3 V at the jack (vol 0.9), 14.8 (worst 36) at 0.6 V**, 0 up to 0.1 V. Any signal loud enough to need sub-stepping hits it.
  * first `process()` after `prepare`: 4 allocations (`Oversampler::up/down` use `assign` on scratch vectors that `prepare` never sizes).
  * automating market / V1 / V2 / rectifier / source / specimen variation: 7-16 allocations per change (`makeConfig` -> `Manifest::apply` string temporaries).
  * `recover()` after non-finite output and (for the engine, not the plugin) an in-place oversampling change: 3 and 2 360 allocations plus the whole
    `initialiseQuiescent` run on the audio thread.
* The plugin keeps the in-place oversampling rebuild off the audio thread (verified by reading `PluginProcessor.cpp`), which is good; the rest remains.
  Sec. 23: "Avoid allocation, file access, locks and unbounded loops in the audio callback. Preallocate state and scratch buffers."
* Fix: make the snapshot a preallocated member, size the oversampler scratch in `prepare`, keep a pre-built `AmpConfig` for the config path.

### D10 (S2) Dummy-load mode heats a disconnected voice coil, and the engine exposes no terminal current

* Where: `AmpCircuit::speakerCurrent()` = `(V(sp) - V(sx)) / voiceCoilR_`; in `Dummy8/4/16` mode `Re` is set to 1e11 ohm, so `V(sx)` floats near 0 and the
  function returns `V(sp)/Re`, a current that does not flow. `CD57Engine::process` integrates `i^2` of this into the thermal state.
* Reproduce `p13_speaker.cpp`: 110 Hz tone, vol 0.7, heating ON, 30 s. Speaker load: 14.9 V rms, coil +6.3 K. **Dummy 8 ohm load (coil disconnected): 7.9 W, coil +26.0 K
  (Re x 1.10)**, displayed in the UI "voice-coil heating" and carried into the circuit when the user switches back to the speaker.
* The dossier (Sec. 05) wants "the speaker-terminal voltage **and current** as model outputs": the engine outputs voltage only; `speakerCurrent()` is the coil current only
  (wrong in dummy mode, partial with the extension speaker).
* Fix: take the terminal current from the transformer secondary winding current (`xfm.i[2]`) and the coil current from the Re branch only when connected; gate heating on load mode.

### D11 (S3) Provenance and manifest hygiene

* Only **14 of the 55** manifest entries are read by the engine (`Manifest::apply`: pt voltage/Rs, pot total/contact, 10 speaker values). The 5 output-transformer values, the four
  `acoustic_*` values, the tube fits, `source_impedance`, the entire `calibration` block (loader never reads it) and `speaker_Sd` (consumed into `SpeakerTS::Sd`, which nothing uses)
  are documentation only (13 of the 14 read entries affect the model; `speaker_Sd` does not): the same numbers are duplicated in the netlist / `Acoustic.h` / `TubeFits.h`, so editing the manifest silently changes nothing.
  Sec. 30: "A model loader should fail clearly when a mandatory calibrated field is absent" - the `calibration` fields are never checked.
* `Devices.h` line 5 says the derivatives are "checked against finite differences in the tests". `Network::checkJacobian` exists but **no test calls it**. I ran it (`p12_jac.cpp`) on
  38 states of a heavy-drive transient: worst relative error **3.1e-5**, so the claim is true but unsupported by the suite.
* Evidence labels: the 12AX7 anchor (1.2 mA, mu 100, rp 62.5 k, gm 1.6 mS) and every 6V6 number are tagged as cited by the dossier; the dossier only gives the 12AY7 reference. They are external.
* The dossier 12AY7 reference (3 mA, mu 40, rp 22.8 k, gm 1.75 mS at 250 V/-4 V) vs the model (`p18_tube.cpp`): Ip 3.04 mA, gm 1.746 mS, **rp 25.2 k (+10 %), mu 44.0 (+10 %)**;
  at the amplifier's own V1 point (109 V, -2 V): mu 45.5, rp 33 k, gm 1.37 mS. Consistent with the 4.5 % rms fit to the JJ sheet; not flagged in the report.

### D12 (S3) Real-time cost is only reported for "typical playing"; 8x is not real-time on the reference container

* README: "1x 16 %, 2x 23 %, 4x 42 % of a core" (8x omitted although the plugin offers it). Measured here (`p9_stress.cpp`, 512-sample blocks at 48 kHz, 4 cores shared with other jobs, so treat the
  worst-case column as noisy): mean / worst block time as % of the 10.67 ms budget - 1x: 9-25 % / 13-62 %; 2x: 19-42 % / 22-127 %; 4x: 40-67 % / 60-160 %; **8x: 80-105 % / 124-309 %**.
  Sec. 23: "Measure worst-case and high-percentile processing time under demanding drive and automation, not just mean CPU with a clean sine." Not done in the repo.

### D13 (S3) Engine replacement on an oversampling change loses all physical state and is undocumented

* `PluginProcessor` swaps in a freshly built engine (quiescent rails, empty capacitors, cold coil) with no crossfade. Reproduce `p17_swap.cpp` (330 Hz, vol 0.6, 2 s of playing then swap 2x -> 4x):
  at 50 mV pk the first 50 ms after the swap differs from the continuing engine by **0.51 FS** (signal peak 0.48 FS) and a **0.15-0.19 FS** offset persists for > 1 s (rail sag and cathode history).
  Sec. 23 asks the product to "document whether a preset change resets or transfers stored charge"; neither README nor the UI says so.
* Related: stereo input is **averaged** (`0.5 (L + R)`), README says "summed". A mono source on one channel with the other silent loses 6 dB, i.e. changes the drive level of a level-calibrated nonlinear model.
  The user IR is loaded with `Normalise::yes`, which discards the absolute gain calibration Sec. 19 says to preserve. Bypass is a hard dry pass-through with no declared latency compensation.

### D14 (S3, not provable either way) Possible double counting of the speaker's high-frequency roll-off

* In-loop: voice-coil `Le` = 0.7 mH (corner `Re/(2 pi Le)` = 1.46 kHz) shapes cone velocity per volt. Acoustic operator: a further 5.2 kHz second-order low-pass ("cone roll-off"),
  a +3 dB presence peak and a position-dependent low-pass, all provisional. Measured voltage-to-mic response (`p13_speaker.cpp`, dB re 1 kHz): 50 Hz -20.0, 100 Hz -4.8, 200 Hz +2.4, 500 Hz +4.4,
  2 kHz +1.6, 3 kHz -1.2, **5 kHz -16.6, 8 kHz -25.1**. Whether the 5.2 kHz filter repeats part of what `Le` and the mic-position filter already do cannot be decided without data;
  Sec. 18/27 name exactly this hazard. The coupling itself is correct: circuit cone velocity per terminal volt equals the closed-form model used to calibrate the acoustic block to
  **1.0000 +- 1e-4 and 0.00 deg at 30 Hz-3 kHz** (`p13_speaker.cpp`), and the thermal compressor changes only `Re(T)` in the circuit (no second compressor in the acoustic path): no double count on those two points.

### D15 (S3) Discretisation error is reported at the most favourable rate only

* The validation AC-vs-transient table runs at host 96 kHz x 2 = 192 kHz internal (0.01 dB). `p20_acerr.cpp` repeats it at the settings users run:
  48 kHz x 2 (default): +0.07 dB at 8 kHz, -0.20 dB at 16 kHz; **48 kHz x 1: +0.33 dB at 8 kHz, -1.31 dB at 16 kHz; 44.1 kHz x 1: +0.40 dB at 8 kHz, -1.94 dB at 16 kHz**.
  All still inside the 0.5 dB / 80 Hz-8 kHz target, but the report should state the rate dependence.

* The same table is evaluated at **B+1 = 404 V, not the nominal 372 V**: after the x1000 capacitor trick `initialiseQuiescent(0.05)` leaves the 16 mF reservoirs far from their steady
  state (probe `p22_cpp_ref.cpp`: b1 404.3 V, b2 346.2 V, b3 251.5 V, 6V6 plate 398.9 V, cathode 23.2 V versus 372.1 / 319.5 / 231.7 / 368 / 21.1 V stock). The reported gains
  (e.g. 54.69 dB at 1 kHz) are therefore at a bias point 8 % hotter than the amplifier the rest of the report describes. The stamps-vs-transient comparison is still valid; the numbers are not "the" small-signal gain.

### D16 (S3) An assumed transformer saturation law is active, which Sec. 29 says should wait for data

* Sec. 29 "What should wait for data": "... assign a transformer saturation law ...". The netlist assigns `phisat = 0.060 V*s` (tanh core, interval 0.04-0.09, ASSUMED). It does engage:
  at full drive the peak tanh argument is **0.72-0.73** at 41 and 82 Hz (`p23_misc.cpp`). Effect of the assumed value (`p26_sat.cpp`, vol 0.8, dummy 8 ohm, 41.2 Hz, 40 mV pk): phisat 0.04 / **0.06** / 0.09 /
  none -> **4.91 / 5.61 / 5.96 / 6.23 W** and THD **31.3 / 24.7 / 22.5 / 21.1 %**. At 82.4 Hz (low E) the spread is < 1 % in power and < 0.3 pp THD. So the assumption is
  immaterial for guitar range and material only below ~60 Hz. It is labelled and has an interval; it is nevertheless exactly the kind of assigned law the dossier says not to assign yet.

---

## 2. Adversarial review of the PASS results in `docs/VALIDATION_REPORT.md`

Question asked of every check: could it fail, were stimulus/threshold chosen so it passes, does it measure what its title says. "Genuine" = it can fail and measures its title.

| # | Check (report wording) | Verdict | Evidence |
|---|---|---|---|
| 1 | manifest loads and validates | genuine, narrow | the loader enforces status / interval / null rules, but only 14 of 55 entries are read and 13 affect the model; `calibration` block never read (D11) |
| 2 | max nodal KCL residual < 1 uA | genuine for the stock 5Y3 only | 5.7e-7 A measured (`p8`); the silicon-rectifier option reaches **52.9 A** while "converged" (D7); the residual is evaluated by the same assembly the solver uses |
| 3-6 | input Z 1 M / 136 k, -6 dB, Mic same | genuine, not fitted | my jack matrix (`p5_jacks`): Inst1 1000.1 k, Inst2 136.0 k and -6.03 dB, Mic1/Mic2 identical; netlist topology confirmed against Fig. 7 crops (jack block, tone/volume wiring, cathodyne, supply nodes) |
| 7 | max power with THD < 5 % within 10 % of 12.5 W | **calibration, not evidence** | one free parameter (PT voltage) was tuned to it; +-25 % over its own interval; coarse x1.1 grid reports 12.85 W where the interpolated crossing is 13.55 W (D6) |
| 8 | transient vs "independent" AC solver, worst 0.01 dB | genuine for stamps/integration, **not independent** of netlist and device code | shares netlist parse, element lists, `evalTriode/Pentode/Diode`, `corePhi/corePhiPrime`; run on a modified supply (caps x1000) at B+1 = 404 V and at 192 kHz internal rate (D15). Real independence supplied by my Python re-implementation: DC within 0.23 V, AC within **0.0025 dB / 0.003 deg** (Section 5 below) |
| 9 | silent Mic channel loads the Instrument channel | genuine | -17.7 dB at Mic vol 1.0 (report) and my sweep: 23.45 -> 5.99 dB; below Mic vol 0.8 the effect is < 1.1 dB |
| 10 | tone clockwise raises HF/LF at two volumes | genuine but only for the Instrument channel | on the Mic channel with Instrument vol 0 the HF balance **falls** from tone 0.5 to 1.0 (500 pF bright cap becomes a shunt): Mic vol 0.5 gain at 5 kHz 13.3 dB (tone 0.5) vs 4.8 dB (tone 1.0). Physically implied by the dossier's connection map, but not stated by the report |
| 11 | pot endpoints solve | genuine, weak | finite at endpoints; the 20 ohm contact resistance leaves a residual of -38 dB (1 kHz) at Instrument vol 0 and a 47.7 dB step between knob 0.00 and 0.05 (`p6_pots`) |
| 12 | burst/recovery memory present and bounded | **cannot fail** | D3 |
| 13 | independent of host block size | genuine | D4 (bit-exact in 32 configurations, heating on) |
| 14 | automation keeps samples finite | weak | `p10_auto`: no click and no zipper in reality (abrupt 0.3->0.9 reaches the target over ~90 ms, slow sweep sidebands identical to constant-volume run); the check itself only tests `isfinite` |
| 15-16 | LU kernel installed / same as generic | trivial identity | 0.000 difference in 7 extreme control states with 0 guarded pivots (`p23_misc`): conditioning at the extremes is actually fine, but the identity test cannot detect an ill-conditioned matrix |
| 17 | no unrecovered failures up to 600 mV | genuine but range too small | 36 failures at 10 V, 89 at 80 V, 744 at inputCal 5 V + 24 dB (D7) |
| 18 | H3 agrees 44.1/48/96 within 1.5 dB | stimulus/metric artefact | D8: the THD column is a window artefact; true spread 0.12 dB |
| 19 | 2x lowers the strongest alias > 6 dB | **meaningless** | D1 |
| 20-21 | ripple at 2 x mains (60 / 50 Hz) | genuine, coarse | resolution 5.9 Hz vs +-6 Hz tolerance: only rejects the wrong harmonic. Idle hum is a pure 2 x mains line, 6.35 mV rms (60 Hz) / 8.12 mV (50 Hz) at the terminals, 64 / 62 dB below 10 V (`p21_hum`) |
| 22 | efficiency 20-65 % | non-discriminating | passes for almost any amplifier |
| 23-25 | K_in worked example, 10 V rated | genuine, arithmetic | reproduces the dossier numbers exactly |

---

## 3. Requirement matrix (dossier Sec. 02-31)

Status: **MET** (evidence checked by me) / **PARTIAL** / **MISSING** (software could do it, not done) / **HW** (needs hardware or data the dossier says does not exist) / **N/A** (the project chose a different, permitted route).
Evidence: `pNN` = my probe in `/tmp/claude-0/audit/probes`, `file:line` = repo source read, `Dn` = defect above.

| ID | Sec | Requirement (condensed) | Status | Evidence |
|---|---|---|---|---|
| R01 | 01 | Define the target at a measurable boundary | MET | manifest `reference_boundary` = input_jack_to_speaker_terminals; `CD57Engine` outputs terminal volts |
| R02 | 01 | Use the five evidence labels | MET | all 94 netlist elements carry `ev=`; manifest `status` vocabulary mirrors them. None of the ASSUMED netlist elements carries an interval (intervals only in the manifest) |
| R03 | 02 | Record serial, market voltage, date, tube markings, speaker label before claiming a unit match | HW | `specimen_serial: null`; market table (9 variants) implemented in `CD57Engine.h:23` |
| R04 | 02 | Preserve full tube marking in the specimen record | HW | manifest note only |
| R05 | 02 | Model the speaker jacks as part of the load connection | PARTIAL | five load modes (speaker, speaker+8 ohm ext, 8/4/16 ohm dummy); jack switching topology of the manual not modelled (manual not in dossier text) |
| R06 | 02 | 10.0 V rms / 14.14 V pk anchor, "not a complete output specification" | MET | validation Sec. 10; D6 for the caveat |
| R07 | 04 | Label HISTORICAL uses; four separate branches (5E3, earlier reissue, modern nominal, specimen); a change states which branch it modifies | PARTIAL | one netlist, one branch ("historical 5E3 + modern nominal"); labels yes, branch separation/delta no |
| R08 | 04 | Per parameter: value, units, source, source revision, evidence label, uncertainty, serial range; measured stored beside nominal | PARTIAL | manifest has value/unit/status/source, interval for `assumed` only; no source revision, no serial range, no measured-vs-nominal fields |
| R09 | 05 | Expose speaker-terminal voltage **and current** before adding microphone sound | PARTIAL | voltage yes; no current output; `speakerCurrent()` is coil current and wrong in dummy mode (D10) |
| R10 | 05 | Acoustic stage has its own calibrated boundary | PARTIAL | separate stage/output mode; level normalised at 1 kHz, no absolute Pa/V (`Acoustic.h::calibrate1k`) |
| R11 | 05 | Document that internal parameter combinations can fit the same output | MET | README "Limitations", manifest `profile_note` |
| R12 | 06 | Calibrate the jack voltage represented by a sample; 0 dB trim is not a Fender number | MET | `inputCalVolts`, K_in worked example verified; actual rig calibration HW |
| R13 | 06 | Every plug state is its own connection graph | MET | `p5_jacks`: 4 single-jack states + 6 patch cables x 4 guitar jacks, impedances and gains differ as physics predicts |
| R14 | 06 | Input 2 is not a constant -6 dB with the same impedance | MET | circuit-derived: 136.0 k, -6.03 dB, input capacitance via real nodes (`p5`) |
| R15 | 06 | Four jack sockets explicit; two occupied jacks of one channel; patch cable connects nodes | PARTIAL | four sockets and tip-to-tip patch implemented (Inst1 + I2-M1: Zin 532 k, both grids driven -0.55 / -1.12 dB); **only one source can be plugged**, so two-source mixing and one-source-per-channel interaction cannot be exercised |
| R16 | 06 | Document the supported jack subset and validate it first | PARTIAL | documented in README; the report validates only the 4 single-jack states |
| R17 | 06 | Test table: Inst1, Inst2, Mic1/2, both jacks one channel, one input per channel, patch, no source | PARTIAL | all rows measurable with one source in `p5`/`p21` (no-source idle: finite, 6.35 mV hum); the two two-source rows are not representable |
| R18 | 06 | Electrode capacitances between actual nodes, no fixed Miller capacitor | MET | netlist `Cgk/Cgp/Cpk` per tube; 6V6 capacitances backward-Euler by design |
| R19 | 07 | Two triodes with own Ip(Vpk,Vgk); shared cathode sums both currents; no crossfade | MET | netlist V1A/V1B on node k1; shared-cathode crosstalk -75 dB (`p5`) |
| R20 | 07 | Consistent sign convention; one-stage charge-conservation test before the full amp | PARTIAL | KCL < 1 uA overall; analytic Jacobian matches finite differences to 3e-5 (`p12`); **no one-stage charge test and `checkJacobian` is never called by the suite** (D11) |
| R21 | 07 | Solve DC at actual supply before audio; check plate/cathode voltages | MET | `initialiseQuiescent`; table in `p7`: V1 plate 111 V / k 1.98 V, V2A 151 V, cathodyne plate 189 V / cathode 44.2 V, 6V6 plate 368 V / k34 21.1 V. Idle 6V6 plate dissipation 13.5 W (external rating 12 W) |
| R22 | 07 | Local gain approximation checked; fit the whole surface, not one number | PARTIAL | surface fitted to JJ transfer sheets; RCA anchor mu/rp +10 % (D11) |
| R23 | 07 | Keep the bypass capacitor's physical charge state | MET | `Ck1` 25 uF state; block-independent (D4) |
| R24 | 07 | Specimen triode mismatch preserved; random variation only as separate creative feature | MET / HW | variation default 0 (`variationAmount`), seeded; measured mismatch HW |
| R25 | 08 | Volumes and tone as one coupled passive circuit | MET | `p6_pots`; Fig. 7 wiring verified |
| R26 | 08 | Electrical pot fraction stored separately from knob angle | MET | `PotMap::alpha` |
| R27 | 08 | Default = measured taper when available, temporary curve identified as assumption | MET (assumption labelled) | `volume_pot_taper` unknown + `..._provisional` assumed; UI label "Volume Taper (assumed)"; measured map HW |
| R28 | 08 | Connection map (0.1 uF to wiper; one end to mix, other to ground; tone wiper to mix; 500 pF to Bright wiper; 0.005 uF to ground) | MET | netlist lines; verified against Fig. 7 crops |
| R29 | 08 | Unused channel keeps plate impedance and coupling cap | MET | -17.7 dB at the other volume's maximum |
| R30 | 08 | Pot = two resistances at the wiper; endpoints merged or small contact resistance; no tone end directly grounded | MET | 20 ohm contact (labelled `proposed`); tone ends via caps. Note the 47.7 dB first step (Sec. 2 row 11) |
| R31 | 08 | Do not present an assumed exponent as the modern taper | MET | assumption in manifest and UI |
| R32 | 08 | Dense sampling near low volume | PARTIAL | report samples 0.2/0.5/0.9; I sampled 0, 0.005, 0.01, 0.02 ... (`p6`): model gain -38 / -9.5 / -3.6 / +2.2 dB |
| R33 | 08 | Solve the full network or verify a state-space form against an independent solver | MET | AC solver + my independent re-implementation (Section 5) |
| R34 | 08 | Sweep tone at several volumes, inactive volume with active fixed, low/medium/high drive | PARTIAL | AC sweeps yes (`p6`); drive-level dependence of control interaction not swept anywhere |
| R35 | 09 | Gain stage and cathodyne separate electrical functions; no ideal inverter | MET | netlist V2A, V2B with real loads |
| R36 | 09 | Both following power-valve grids and coupling caps in the same solve | MET | netlist `Cc_3/Cc_4`, `Rgl_*`, `Rgs_*` |
| R37 | 09 | Cathodyne grid return to the 1.5 k / 56 k junction (bootstrap) | MET | `Rgl_2b g2b j`; DC: grid 43.0 V, cathode 44.2 V |
| R38 | 09 | Test bursts followed by silence/low level; track both cathodyne outputs and both power grids | PARTIAL | burst test exists but its metric is invalid (D3); trajectories not reported (my `p3_burst` prints them) |
| R39 | 10 | Two power-valve models exposing Ip, Is, Ig; supply/cathode currents sum | MET (structure) | `evalPentode`; quality D5 |
| R40 | 10 | Keep shared-cathode state across blocks | MET | D4 |
| R41 | 10 | Blocking distortion via explicit capacitor equation and grid-current path | MET | `p3`: after a 120 mV burst g3 +24.6 V / g4 -35.7 V coupling transients, 1 kHz gain dip to 0.65x, recovery 65 ms to 10 % |
| R42 | 10 | Correct centre-tap convention; record winding reference (full / half / secondary) | MET | netlist comment "Ns = 1, secondary-referred"; manifest names `_full`, `_half`; plate-to-plate 8 kohm by construction (read) |
| R43 | 10 | No fixed "even harmonics" control | MET | none exists |
| R44 | 10 | Describe actual conduction behaviour, not a class label | MISSING | no per-valve current waveform / conduction-angle report |
| R45 | 11 | Rectifier conduction + transformer R + reservoir + downstream loads; no single sag amount | MET | explicit 5Y3 diodes, 3 x 16 uF nodes; idle ripple B+1 26.8 V pp, B+2 0.4, B+3 ~0 (`p7`) |
| R46 | 11 | No bleeder inserted for convenience | MET | only GMIN 1e-12 S |
| R47 | 11 | Nonlinear rectifier law with transformer resistance | MET (candidate) | fit to digitised right panel; **held-out left panel**: model 7-12 V (about 3 %) below the sheet at 10 mA and 8-18 V at 120 mA for Utr 220-350 V (`p25_y3`, sheet read by eye +-8 V) |
| R48 | 11 | Preserve mains phase; test 50 and 60 Hz | MET | `phase_`; 100 / 120 Hz ripple found (coarse, Sec. 2 rows 20-21) |
| R49 | 11 | Startup: quiescent init or explicit controlled trajectory; no numerical-pop-as-standby | MET | `p10`: power-off decays in 0.6 s (heaters 2 s), power-on rises over > 3 s; no standby switch modelled |
| R50 | 11 | Averaged supply validated against explicit | N/A | no averaged model |
| R51 | 12 | Ideal CT transformer + R + leakage + magnetising branch; parasitic C only if measured | MET | `Xfm`: Ra=Rb=125, Rs=0.35, Ll 8+8 mH, 24 uH, 25 H; no capacitance |
| R52 | 12/29 | Saturation law should wait for evidence; no elaborate hysteresis | PARTIAL | tanh law assigned (labelled assumed, interval) and active at LF (D16) |
| R53 | 13 | Constitutive detail (ESR, leakage, voltage dependence) only when measured | MET | capacitance only; ESR/leakage absent (permitted: "when measurements support it") |
| R54 | 13 | Separate capacitor coefficients per role | MET | none shared |
| R55 | 13 | Deterministic hum separate from stochastic noise; seed; noise-free circuit testable | MET | noiseScale 0 default; hum is circuit ripple (pure 2 x mains line, `p21`) |
| R56 | 13 | Sensitivity analysis one parameter at a time before Monte Carlo; no unrelated distributions for one component | PARTIAL | variation feature draws independent per element (creative, optional); **no one-at-a-time sensitivity study**; I ran PT voltage (D6) |
| R57 | 14 | Netlist equals the historical baseline table | MET | every value checked: 4 x 68 k, 2 x 1 M, 2 x 100 k, 820/25 uF, 2 x 0.1 uF, 1 M pots (1e6 via `potTotal`), 500 pF/0.005 uF, 100 k/1.5 k/25 uF, 0.022 uF, 56 k/56 k, 1.5 k/1 M, 0.1 uF/220 k x 2, 250 ohm/25 uF, 3 x 16 uF, 5 k, 22 k |
| R58 | 14 | Time-scale table only as sanity check, no seven one-pole envelopes | MET | none in the DSP |
| R59 | 15 | Fitting data per element (Ip surface, cutoff, grid current, electrode capacitances; 6V6 plate+screen+grid; 5Y3 forward+supply) | PARTIAL | triode surfaces fitted to JJ sheets (rms 0.07 / 0.08 mA, 4.5 / 9.1 % relative); grid law assumed; capacitances datasheet-class; **6V6 not fitted to dossier data (D5)** |
| R60 | 16 | Weighted objective; reserve validation segments; report fitted domain and error outside it | MISSING | `fit_tubes.py` fits every digitised point; no held-out set; no extrapolation error reported (D5) |
| R61 | 16 | Fit screen and grid current separately with current conservation | PARTIAL | conservation exact; screen partition shape and grid law are assumptions |
| R62 | 16 | No positive-grid clamp to hide solver failures | MET | smooth law, no clamp |
| R63 | 16 | Continuity of current and derivatives, low-voltage and boundary limits | PARTIAL | Jacobian 3e-5 (`p12`); not tested in-suite |
| R64 | 16 | Overflow-safe softplus, scaling, double precision, no hard state clipping | MET | `Devices.h`; Newton step caps are not state clips |
| R65 | 17 | Linear coupled electromechanical model | MET | cone velocity per volt: circuit vs closed form 1.0000 and 0.00 deg, 30 Hz-3 kHz (`p13`); radiation load F_acoustic omitted |
| R66 | 17 | Do not substitute another driver's parameters; name generic substitutions | MET | generic values labelled `assumed` with intervals |
| R67 | 17 | Complex voltage/current fit; large-signal Bl(x), Kms(x), L(x) | HW | `unknown` entries |
| R68 | 18 | Short-term nonlinear vs slow heating separated; single thermal compartment | PARTIAL | slow thermal only; phantom heating in dummy mode (D10) |
| R69 | 18 | Do not assign copper tempco unverified | MET (labelled) | `proposed`, "conductor not verified" |
| R70 | 18 | Option A or B, never both; thermal compressor must not repeat Re(T) | MET | exclusive output modes; heating changes only Re(T) in the circuit; D14 for the HF-roll-off hazard |
| R71 | 18/27 | Hybrid factorisation H G*/(\|G\|^2 + lambda) with regularisation | HW | needs measured H |
| R72 | 19 | Internal volume not from the external box; mic frame; IR conventions; preserve absolute gain | PARTIAL | path difference assumed; mic position normalised; IR loaded with `Normalise::yes` (D13) |
| R73 | 20 | Layered architecture | MET | `CD57Engine`/`AmpCircuit`/`Network`/`Acoustic`/plugin |
| R74 | 20 | High-accuracy offline solve for identification and a bounded realtime solver | PARTIAL | one solver; the "reference" is the same code at higher oversampling |
| R75 | 21 | MNA with topology in a readable versioned netlist | MET | `data/netlist/cd57_nominal.net` (`.rev 0.1.0`) |
| R76 | 21 | Compare integration methods and step sizes to a high-accuracy reference | PARTIAL | step-size dependence measured by me (D15, `p11`); BE vs trapezoid never compared; reference is itself trapezoidal |
| R77 | 21 | Newton with predictor, damping/line search, fallback, recorded statistics, no undetected unconverged return | PARTIAL | all present; acceptance is a step test (D7) |
| R78 | 21 | KCL residuals, DC equilibrium, capacitor charge, power flow | PARTIAL | KCL and DC yes (stock only); RC accuracy 3.3e-6 V and LC energy 0.9999-1.0000 pass in my `p23/p26`; no in-suite energy/charge test |
| R79 | 21 | Same output for any block size | MET | D4 |
| R80 | 21 | Full vs partitioned solves at control extremes and heavy grid current | MISSING | no partitioned solve exists; extremes covered by kernel-vs-generic only (identical, 0 guarded pivots, 7 states) |
| R81 | 21 | Cached factorisations only after validity under parameter changes | MET | refactorised every Newton iteration; no pivoting but conditioning verified at extremes (`p23`) |
| R82 | 22 | No partition that silently delays a coupling by one sample | MET | single simultaneous solve; sources, speaker and acoustic input at the same step (read) |
| R83 | 22 | Record provenance/licence of external code, tube models, IRs | PARTIAL | README licensing section; **no LICENSE file** (README says to choose one); external anchor numbers not tagged (D5/D11) |
| R84 | 23 | Support 44.1/48/96 kHz | MET | D8 (+192 kHz): spread 0.12 dB |
| R85 | 23 | High-accuracy reference at a higher rate; compare several oversampling factors | MISSING | no reference render in the repo; I produced one (`p1`, `p11`): D1 |
| R86 | 23 | Physical time constants in seconds; coefficients regenerated per rate | MET | `Network::build(h)`; 44.1/48/96/192 agree |
| R87 | 23 | Resampler ripple, transition width, stopband; latency and phase reported | PARTIAL | transition 0.45-0.55 fs, 80 dB / 60 dB stages in code comment only; latency rounded up (D2) |
| R88 | 23 | Automation: smooth the physical parameter, preserve endpoints, test slow/abrupt at silence and signal; smoothing time in the product spec | PARTIAL | smoothing 20 ms in the pot-fraction domain, endpoint snap 1e-5 (read); no clicks or zipper (`p10`); the 20 ms figure is in code, not the spec |
| R89 | 23 | No allocation/locks/unbounded loops in the callback; preallocate | MISSING | D9, D7 |
| R90 | 23 | Handle denormals, invalid samples, nonfinite solver output | PARTIAL | plugin `ScopedNoDenormals`, panic counter; no denormal slowdown in 30 s silence after a burst (`p23`); NaN path costs 56 ms and mutes a block (D7) |
| R91 | 23 | Measure worst-case and high-percentile time under drive and automation | MISSING | only "typical playing" CPU; D12 |
| R92 | 23 | Preset serialisation, sample-rate change, bypass, reset, latency; state of a preset change documented | PARTIAL | APVTS XML, rebuild at `prepareToPlay`, hard bypass, latency reported (rounded); transfer/reset semantics undocumented, OS change resets (D13) |
| R93 | 24 | Physical volts/amps inside; digital conversions at explicit boundaries | MET | K_in/K_out; worked example reproduced |
| R94 | 24 | Distinct peak/RMS/p-p conventions | MET | naming in code and report |
| R95 | 24/25 | Source impedance, converter response, reference channel, raw samples | HW | |
| R96 | 25 | Capture programme (repeatability, DC/passive, 4 input modes, 27 control combos, 324 captures, transients, loads, acoustic, held-out) | HW (data) / MISSING (software harness) | no tool renders the model over the capture matrix (27 x 4 x 3 conditions) as a regression/reference dataset, although this is possible without hardware (Sec. 29 "can start immediately") |
| R97 | 26 | Metric definitions (NRMSE, ESR, fixed onset definition, group delay after fixed-delay removal) | MISSING | no NRMSE/ESR code, no clipping-onset definition, no phase/group-delay report |
| R98 | 26 | Clean response, onset, harmonics, IMD, burst envelope, recovery, held-out NRMSE vs hardware | HW | listed as NOT EVALUATED in the report (correct) |
| R99 | 26 | Aliases below -80 dBFS for a documented stimulus | **FAIL** | -39 / -58 / -72 dBFS at 2x / 4x / 8x (3.1 kHz, 60 mV pk), D1 |
| R100 | 26 | Runtime integrity: no non-finite samples, unbounded states, dropouts, hidden solver failures | PARTIAL | 10-minute heavy-drive run clean (`p14`: 0 failures, 0 panics, rails and coil bounded); failures inside the plugin's own ranges, dropouts at 8x/worst case (D7, D12) |
| R101 | 26 | No per-clip gain refitting | MET | none |
| R102 | 26 | Diagnostic views: spectra, harmonics vs drive, IMD, residuals, envelopes, rail/cathode trajectories, impedance magnitude/phase | PARTIAL | harmonics vs drive and an IMD line only; no plots, no trajectories, no impedance curves |
| R103 | 26 | Blind level-matched listening | HW | |
| R104 | 27 | Eight acceptance experiments | PARTIAL | 8 (numerical) partly; 1-7 HW; none pre-registered |
| R105 | 27 | Avoid duplication traps (amp-to-mic after modelled amp, second impedance network, repeated thermal effect, repeated IR content) | MET | exclusive acoustic modes; single in-loop speaker; D14 hazard only |
| R106 | 28 | Unknowns as null/unknown or assumed with sensitivity interval; generic substitutions named | PARTIAL | intervals present (24 assumed, 4 unknown); no sensitivity analysis (D6) |
| R107 | 29 | Milestones A-H need named evidence | PARTIAL | C partly (independent re-implementation exists only in this audit); G partial (D9, D12, D13); A, D-F, H HW |
| R108 | 29 | Do not freeze BOM, claim an OEM speaker model, assign a saturation law, certify 1:1 acoustics | PARTIAL | BOM labelled nominal, no 1:1 claim anywhere; saturation law assigned (D16) |
| R109 | 30 | Machine-readable manifest with provenance; fail clearly on missing mandatory fields or require an explicit provisional profile | PARTIAL | explicit `profile` enforced; `calibration` block unchecked; a rejected manifest falls back to compiled defaults and is reported rather than failing; 14/55 entries consumed (D11) |
| R110 | 30 | Minimum capture metadata, raw and processed stored separately | MISSING | no schema (cannot be filled without HW) |
| R111 | 30 | Engineering package: verified schematic, netlist, component register, source catalogue, calibration files, plots, build instructions, validation reports | PARTIAL | netlist, `NETLIST.md`, build docs, report yes; no source catalogue, calibration files or plots |
| R112 | 31 | Charge/flux-form residual, consistent algebraic constraints | MET | trapezoid companions equivalent for linear storage; winding flux state explicit |
| R113 | 31 | Linear stamps with terminal orientation; device currents sum to zero; analytic Jacobian incl. moving cathode/screen | MET | `p12`: 3.1e-5 over 38 states; KCL 5.7e-7 A |
| R114 | 31 | Finite-difference Jacobian check in a development harness | PARTIAL | function exists, never run by any test (D11) |
| R115 | 31 | Skeleton: validate provenance, connection graph per jack state, allocate everything, DC solve, record initial state, per-step residual/iterations, defined fallback, terminal V and I | PARTIAL | all but: allocation (D9), recorded initial state, per-step residual, terminal current (D10), fallback "how often permitted" |
| R116 | 31 | Energy checks on isolated elements; passive load must not generate energy | PARTIAL | RC/LC pass in my probes; none in the suite |
| R117 | 33 | Handoff questions answered honestly; result described as documented approximation | MET | README/report wording consistent with the dossier |

---

## 4. Requirements that are impossible without hardware measurements (not omissions)

These need a physical unit, a measurement rig or data the dossier states does not exist publicly; they must not be read as defects:

1. Serial/revision/market/tube/speaker identity of a specimen (R03, R04); the modern as-built schematic and revision (Sec. 04, 28).
2. Measured pot resistance-versus-angle maps and total resistances (Sec. 08, 28).
3. Installed tube curves, specimen mismatch, grid-current onset and screen partition of the actual 6V6 pair (Sec. 07, 15, 16, 28).
4. Supply- and output-transformer parameters: ratio, DCR, magnetising and leakage inductance, loss, saturation (Sec. 12, 28).
5. OEM speaker complex impedance, Thiele-Small and large-signal Bl(x), Kms(x), L(x), thermal parameters (Sec. 17, 18, 28).
6. Cabinet internal geometry, microphone grid, room chain, IRs, directivity (Sec. 19, 28), and therefore the linear acoustic identity and its regularised factorisation (Sec. 27 experiment 4).
7. Calibration of the reamp/interface chain: K_in, K_out, source impedance, fixed delay (Sec. 24).
8. Every Sec. 26 comparison against the amplifier: clean response within 0.5 dB, onset within 0.5 dB, harmonics within 1 dB, IMD within 2 dB, burst 5 %, recovery 10 %, held-out NRMSE <= 5 %, blind listening.
9. Sec. 27 experiments 1-3 and 5-7 and the whole capture programme (Sec. 25), the repeatability floor, and the held-out generalisation test.
10. Milestones A, D, E, F, H of Sec. 29; the Sec. 33 "verified netlist / controls calibrated / accuracy independently tested" answers.
11. Component tolerance statistics (Sec. 13): only a sensitivity study around an unverified nominal is possible without a specimen.

What *is* possible without hardware and is therefore a genuine omission when missing: the items marked MISSING or PARTIAL in Section 3 whose evidence column cites a defect (D1-D16), notably the reference-rate comparison, the metric implementations (NRMSE/ESR/onset/group delay), the capture-matrix reference render, per-valve conduction reporting, terminal-current output, and the in-suite Jacobian/energy/charge tests.

---

## 5. Independent re-implementation used for Milestone C (software versus software)

`/tmp/claude-0/audit/probes/p22_indep.py` (numpy/scipy; not in the repo): own netlist parser (value suffixes, evidence tags), own MNA stamps, own Koren/pentode laws taken from `scripts/fit_tubes.py` and the fitted constants, own DC solver (`scipy.optimize.root`) and own complex AC solve; B+1 held at the C++ value, rectifier omitted.
Configuration: Instrument 1, vol 0.5, mic 0, tone 0.7, 8 ohm dummy load, supply caps x1000 (the validation's own condition).

* DC: worst node difference 0.23 V on rails (b2/b3, the C++ state is not fully settled with 16 mF reservoirs), 0.1 V on preamp plates, <= 0.02 V on cathodes; residual 8e-12 A.
* AC (100 Hz, 1 kHz, 5 kHz; nodes b_g, p1b, m, p2a, p2b, kcy, g3, p3, sp): worst **0.0025 dB and 0.003 deg**.
* Conclusion: parser, stamps, units (Meg/k/u/n/p), transformer winding convention and the Koren implementation in `Devices.h` agree with an independent implementation of the same netlist and fit constants. This does **not** validate the netlist against the real amplifier (only against Fig. 7, which I checked by eye on enlarged crops) nor the fit constants against hardware.

---

## 6. Resolution status (follow-up work after this audit)

Everything above describes the implementation at commit `2a356c9`. The defects were then worked through; this table records what happened to each, with the test that now guards it. "Resolved" means the cause was fixed and a test that can fail now covers it; where something is only reduced, or cannot be decided from the public record, it says so.

| ID | Status | What was done | Guarded by |
|---|---|---|---|
| D1 alias figure | **Resolved (measurement); the -80 dBFS limit is only partly met** | The legacy figure was two things: the true 8th harmonic folding through the decimator transition band, and genuine mains-ripple modulation (-39 dBFS, independent of the oversampling factor, -99 dBFS when the supply ripple is removed). Aliasing and total numerical error are now measured against a 16x reference of the same model. Against that reference a six-note chord at heavy drive reaches -86 dBFS at 2x and -113 dBFS at 8x, but a heavy 3.1 kHz sine only reaches -54.5 dBFS at 2x and -77.6 dBFS at 8x, limited by solver truncation rather than aliasing: the dossier's -80 dBFS candidate limit is met for the chord at 2x and not for that single tone at any factor below 8x. | `validate_numerics` sections A, C, D (CTest `numerics_validation`); `docs/NUMERICS_REPORT.md` |
| D2 decimator and latency | **Resolved** | New resampler design: passband ripple <= 0.0002 dB, alias and image rejection >= 103 dB, linear phase, latency padded to a whole number of host samples and reported exactly (2x: 79 / 45 / 13 samples at 44.1 / 48 / 96 kHz). The latency is larger than before (31 samples at 48 kHz 2x); that is the price of the rejection. | `validate_numerics` section B; `cd57_processor_test` (reported latency = engine latency at each rate) |
| D3 burst recovery | **Resolved** | Re-measured with a 1 kHz lock-in instead of broadband RMS: after a 150 ms burst at 120 mV the quiet tone's gain dips to 0.19x of its settled level and is within 10 % after 52 ms and 5 % after 111 ms. The check asserts that memory exists and recovery is bounded, and can fail. | `cd57_validate` section 7 |
| D4 block independence | Positive finding, kept | - | `cd57_validate` section 8 |
| D5 6V6 law | **Resolved; accuracy limited by the model family** | Cathode-current law refitted to the pixel-digitised JJ 6V6S sheet (Koren form with a Kvb generalisation; global optimiser; whole-curve hold-out and cross-panel checks). Grid conduction taken from the dashed Ig curves (saturating law). The Early factor is normalised to the screen so that Ip + Is = Ik when the screen is strapped to the plate, as on the sheet. Error against the sheet: 7.1 % relative in the amplifier's operating region, 19.5 % where the grid is positive. The RCA 6V6GT numbers are used only as a labelled consistency check and for the screen partition and Early voltage (not in the dossier). | `scripts/fit_tubes.py`, `data/tubes/fitted_models.json`, `validate_tubes` (C++ law vs every digitised point) |
| D6 12.5 W anchor | **Resolved** | Measured at the interpolated 5 % THD crossing and reported as a calibration residual with a sensitivity table over the manifest's own PT interval (now 330-400 V); the PT secondary is recalibrated to 377.4 V. A labelled regression guard remains so a model change forces a recalibration; it is not presented as evidence. | `cd57_validate` section 4, `--calibrate-pt` |
| D7 Newton acceptance, bad input | **Resolved** | A solve is accepted only if the nodal KCL residual is also below 20 uA (silicon-rectifier mode used to accept a 52.8 A error, now 17 uA). NaN/Inf host samples are zeroed and counted; the jack voltage is limited to the 30 V peak the solver is validated for, and counted. Recovery from a +-30 V Nyquist-rate abuse block is bounded to the following block. | `validate_realtime` sections 2, 3 |
| D8 44.1 kHz THD | **Resolved** | Coherent analysis windows; H2, H3 spread across 44.1 / 48 / 96 kHz is now <= 0.02 dB. | `cd57_validate` section 8 |
| D9 allocation in `process()` | **Resolved for the engine** | Sub-step buffers preallocated, manifest base configuration cached at prepare time, engine state transfer allocation-free: zero allocations measured (operator new replaced) in steady state under heavy drive and across every control and configuration change. Allocations inside JUCE itself are not measured. | `validate_realtime` section 1 |
| D10 dummy-load heating | **Resolved** | Coil current is zero when the coil is disconnected; terminal current is exposed (`AmpCircuit::terminalCurrent`, I = V/R in the dummy modes). | `validate_realtime` section 4 |
| D11 bookkeeping | **Partly resolved** | The loader now records which manifest entries the model reads (14 of 56) and fails on a missing requested key; the other 42 are explicit documentation entries (listed in the validation report). The assembled Jacobian is checked against finite differences. The 12AY7 local mu / rp are reported next to the RCA numbers (they follow the JJ sheet, +9-10 %). **No LICENSE file was added: choosing a licence for a JUCE-based plugin is the owner's decision.** | `cd57_validate` section 1; `validate_numerics` section E |
| D12 real-time headroom | **Reported, not changed** | Heavy drive with automation on this container: 2x mean 45-47 % of the real-time budget, p99.9 about 95 %; 4x mean 68 %, p99.9 131 %; 8x mean 116 %. The 8x choice is labelled "offline: not real-time". Timing is reported, not gated (CI runners are too noisy). | `validate_realtime` section 6 |
| D13 engine swap, stereo, IR normalisation | **Resolved** | Oversampling change keeps the circuit state (the new engine adopts it, its up-sampler is primed with recent input, and the retiring engine is cross-faded for 512 samples): post-swap peak 1.017x of steady, DC 1e-6 FS (was a 0.51 FS pop and a 0.15 FS offset for over a second). The stereo input is an explicit choice (mean / left / right). IRs keep gain, polarity and arrival time. A failed IR load now clears the IR instead of keeping the previous one. | `cd57_processor_test`; `validate_realtime` section 7; `validate_acoustic` |
| D14 possible HF double count | **Not decidable** | The 5.2 kHz cone roll-off in the provisional acoustic stage may repeat part of what the voice-coil inductance and the mic-position filter already do. Without a measured response this cannot be decided; every parameter of that stage stays labelled PROVISIONAL, and a measured IR replaces the stage entirely. | - |
| D15 AC vs transient | **Resolved** | Compared at the nominal mean B+1 and at the rates the plugin actually runs: 1x +0.32 dB at 8 kHz, 2x +0.07 dB, 4x +0.01 dB. The model itself is checked by an independent implementation that shares no code (own parser, stamps, device equations, DC and AC solves): DC within 0.24 V, AC within 0.0013 dB and 0.002 deg. | `cd57_validate` section 5; `scripts/independent_check.py` (CTest `independent_reimplementation`) |
| D16 transformer saturation | **Resolved** | The output-transformer core is linear by default (same small-signal response); the saturating law is an explicit, labelled option whose threshold is an unmeasured guess. Switching it mid-signal causes no step. | `validate_realtime` section 5 |

Also added from this audit's own methods: the whole-amplifier energy balance closes to 0.000 % (sources = dissipation + stored-energy change), the one-stage KCL / charge-conservation test, and the in-circuit 5Y3 regulation check against the JJ sheet (`validate_supply`: droop within 7 % of the sheet; a near-constant -14 V offset that depends on the sheet's unstated Utr / Rs / frequency interpretation and is absorbed by the calibrated PT).

The items in Section 4 remain impossible without hardware. That is the dossier's own conclusion (pp. 1, 2, 62): this is a documented approximation of the modern '57 Custom Deluxe, not a verified one-to-one model.

---

## 7. Second follow-up: requirements the audit listed as MISSING or PARTIAL that software could close

Section 6 records the defects D1-D16. This section records the items of the requirement matrix (Section 3) that the audit marked **MISSING** or **PARTIAL** although they need no hardware, and what happened to each. "Closed" means a mechanism exists
and a test that can fail covers it; nothing here changes the conclusion of Section 4.

| ID | Requirement (condensed) | Was | Now | Evidence |
|---|---|---|---|---|
| R07 | four separate model branches; a change says which branch it modifies | PARTIAL | **Closed (structure).** Every manifest entry names its branch; the specimen branch is a delta merged beside the nominal values. The `reissue_2006` branch is declared and **empty**: the service drawing was not available, and the dossier forbids importing it automatically | `Manifest.h`, `validate_manifest` |
| R08 | value, units, source, **source revision**, evidence label, uncertainty, **serial range**; measured stored beside nominal | PARTIAL | **Closed.** Schema 2 requires branch, source_revision and serial_range keys on every entry (refused otherwise); `measured` blocks need value, unit, instrument and date | `validate_manifest` sections 1, 3, 4 |
| R09 | speaker-terminal voltage **and current** as model outputs | PARTIAL | **Closed.** Terminal current is a host-rate engine output (own resampler, off by default, allocation-free) and enters the capture metadata and the comparison | `CD57Engine::terminalCurrentBlock`, `cd57_capture_matrix` |
| R15, R16, R17 | two occupied jacks, one input per channel, patch; validate the supported subset first | PARTIAL | **Closed.** A second plug (driven or terminated) in any other jack; the Sec. 06 table rows are representable and validated against closed-form predictions of the drawing's network; the plugin exposes it | `validate_jacks` (17 checks), `ProcessorTest`, netlist second source chain |
| R44 | describe the actual conduction behaviour, not a class label | MISSING | **Closed.** Per-valve conduction angle, overlap, plate/screen/grid current, dissipation and bias shift over a drive sweep; consistency check against the circuit | `cd57_conduction`, `docs/CONDUCTION_REPORT.md` |
| R56, R106 | one-parameter-at-a-time sensitivity before Monte Carlo; assumed values carry an interval | PARTIAL | **Closed.** 41 parameters moved across their stated range, 15 features, effects in dB, ranked, per-feature dominant parameters, with direction / isolation / degenerate-range self-checks | `cd57_sensitivity`, `docs/SENSITIVITY_REPORT.md` |
| R27 (R31) | a measured pot map replaces the assumed taper when available | MET (assumption labelled) | **Strengthened.** A measured 11-point map can be supplied per pot (Instrument, Mic, tone) through a specimen delta; invalid maps are reported manifest errors | `Manifest::potMap`, `validate_manifest` section 5 |
| R96 | capture programme: repeatability, DC/passive, four input modes, 27 control combinations, 324 captures, transients, loads, acoustic, held-out | HW / MISSING (software) | **Closed on the software side.** 324 + 72 endpoint + 33 jack-state + 36 held-out captures rendered with stimulus, metadata and raw/processed separation; the same layout takes a hardware dataset. **No hardware data exists** | `cd57_capture_matrix`, `data/reference/capture_matrix_summary.csv` |
| R97 | metric definitions (NRMSE, ESR, fixed onset definition, group delay after fixed-delay removal) | MISSING | **Closed.** `Metrics.h`; 34 checks against analytic answers; documented DC handling, alignment, band-limiting, near-silence rule | `validate_metrics` |
| R102 | diagnostic views: spectra, harmonics vs drive, IMD, residuals, envelopes, trajectories, impedance, acoustic position | PARTIAL | **Closed.** 11 figures regenerated by one script | `docs/DIAGNOSTICS.md`, `docs/figures/` |
| R110 | minimum capture metadata, raw and processed stored separately | MISSING | **Closed.** JSON schema + a dependency-free validator in the test suite; every render writes metadata and processed results separately | `data/capture/capture_metadata.schema.json`, `scripts/check_capture_matrix.py` |
| R111 | engineering package: verified schematic, netlist, register, **source catalogue, calibration files, plots**, build instructions, reports | PARTIAL | **Closed.** Source catalogue (what was and was not readable), model calibration + hardware calibration template, plots | `docs/SOURCE_CATALOGUE.md`, `data/calibration/`, `docs/figures/` |
| R107 (milestones) | each milestone needs its named evidence | PARTIAL | B, C (independent re-implementation, now in the suite), G done for the nominal model; **D, E, F, H and A need hardware.** `docs/HARDWARE_ROUTE.md` states the step and the tool for each | `docs/HARDWARE_ROUTE.md` |

### Items deliberately NOT changed, and why

| ID | Item | Decision |
|---|---|---|
| R99 | additional aliases below -80 dBFS | **Still only partly met, now better understood.** Section F of the numerics report tightens the Newton tolerances 10^4 x and the heavy-drive residual moves by 0.06 dB: it is time-step truncation of the integrator, not convergence. A candidate limit ("set the final limits after quantifying...", Sec. 26) is met for the chord at 2x, the 1 kHz tone at 4x and the 3.1 kHz tone only at 16x; no solver change was made on a hunch that the data show cannot pay off |
| R80 | full versus partitioned solves | **Not applicable.** The partition the dossier warns about does not exist here (one simultaneous solve); solver-structure risk is covered by the generated LU kernel vs the generic solver (bit-identical), the assembled-Jacobian check and the independent Python re-implementation |
| R05 | the manual's speaker-jack and combined-impedance rules | **Cannot be done:** the manual was not readable (SOURCE_CATALOGUE.md). Five load modes remain |
| R74, R76 | a high-accuracy offline reference; compare integration methods | Already provided by numerics sections C and D (16x / 32x same-model references; theta per element class). They are references of the *same* model, as the audit noted; an independent solver would not change that |
| R03, R04, R24 (measured), R67, R71, R95, R98, R103 | specimen identity, tube markings, large-signal speaker, measured H, converter calibration, comparison with the amplifier, listening | Need hardware or data the dossier says do not exist publicly (Section 4) |

### One finding of this follow-up worth recording

The first sensitivity run reported a 60 dB effect for a 0.8-1.2 MOhm pot range. The cause was in the loader, not the model: an `interval` was only read for entries whose status was `assumed`, so the historical-nominal pot total was loaded as the range 0..0 ohm.
The loader now reads an interval wherever one is given, and the sensitivity tool refuses to run with a missing range. A one-parameter study that had silently used a zero range would have published a wrong measurement priority.
