# Dossier compliance audit (adversarial, evidence-based)

Scope: the implementation at commit `2a356c9` (branch `claude/eager-edison-6lario`) against the "Public-source Engineering
Dossier" (Sec. 02-31). Report only: no source file was changed. Probes live outside the repo (`/tmp/claude-0/audit/probes`);
every number below was measured by building the JUCE-free tools (`-DCD57_BUILD_PLUGIN=OFF`) and driving `CD57Engine` /
`AmpCircuit` / `Network` directly. Baseline reproduced first: `cd57_validate` = 27 passed / 0 failed, report identical to
`docs/VALIDATION_REPORT.md`.

Status of this document: **checkpoint 2 of 4** (defects D1-D15 written; PASS review partly updated; requirement matrix and hardware-gated list follow) (written incrementally; sections marked `TODO-AUDIT` are not finished yet).

Evidence tags: **MEASURED** (I ran it), **READ** (code reading, not executed), **DOSSIER** (quoted requirement).

---

## 1. Ranked defect list (living)

Severity: **S1** changes the electrical/audible result or invalidates a headline claim; **S2** wrong or misleading
validation/diagnostic; **S3** robustness / hygiene / documentation.

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
  0.99-1.03 at 150-200 V, Vg -2 V; **0.90, 0.89, 0.78, 0.81, 0.71** at 150-200 V with Vg -6...-20 V; **0.57, 0.21, 0.45** at 50-100 V.
  The manifest note says "agrees within a few % at 150-200 V": true for 3 of the 8 points in that range only. The class-AB1 constraint
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
  quantises the answer low by up to a grid step; (iii) THD at exactly 12.5 W is 2.9 % (the spec "12.5 W at < 5 % THD" is a
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
  1e6 FS input: 774 failures and **165 ms for one 512-sample block** (15x real time; worst-case Newton work per sample is about 63x the typical,
  maxIter 20 x (1 + 2 + 4 + 8 + 16 + 32) sub-steps). The report's failure check stops at 600 mV at the jack.
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
  * every call that falls back to **sub-stepping** allocates (`Network::step` builds a local `Snapshot` of eight `std::vector`s): **10.4 allocations per 512-sample block
    on average (worst 27) at 0.3 V at the jack, 14.8 (worst 36) at 0.6 V**, 0 below ~0.1 V. Any driven guitar signal will hit it.
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

* Only **15 of the 55** manifest entries are read by the engine (`Manifest::apply`: pt voltage/Rs, pot total/contact, 10 speaker values). The 5 output-transformer values, the four
  `acoustic_*` values, the tube fits, `source_impedance`, the entire `calibration` block (loader never reads it) and `speaker_Sd` (consumed into `SpeakerTS::Sd`, which nothing uses)
  are documentation only: the same numbers are duplicated in the netlist / `Acoustic.h` / `TubeFits.h`, so editing the manifest silently changes nothing.
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

---

## 2. Adversarial review of the PASS results in `docs/VALIDATION_REPORT.md`

| # | Check | Can it fail? | Verdict |
|---|---|---|---|
| 1 | manifest loads | yes (loader rejects missing status / interval) | genuine but weak: only 15 of 55 manifest entries drive the model (see D-manifest, TODO-AUDIT) |
| 2 | KCL residual < 1 uA | yes | genuine; residual measured at convergence of the same Newton the solver uses (not independent of the stamps) |
| 3-6 | input Z 1 M / 136 k, -6 dB | yes | genuine and not fitted: the netlist follows Fig. 7 (verified against the figure crop) |
| 7 | 12.5 W / <5 % THD | yes, but it is the calibration anchor | `ptHalfVrms = 360 V` was chosen to make it pass (manifest says so); it is reported as PASS in the same list as independent checks. Search is over 3 volumes x input levels in x1.1 steps and takes the **maximum** power with THD < 5 %, window +-10 % (11.25-13.75 W) |
| 8 | transient vs "independent" AC solver | partly | shares the netlist, the element lists, `evalTriode/evalPentode/evalDiode` and `corePhi/corePhiPrime` with the transient solver; independent only in stamps/LU/time integration. Runs on a modified circuit (supply caps x1000) |
| 9-11 | silent channel loads active channel; tone monotone; endpoints finite | yes | genuine (AC solver only; no nonlinear check) |
| 12 | burst/recovery memory present | effectively no | see D3 |
| 13 | block size | yes | genuine (D4) |
| 14 | automation finite | weak | only asserts no NaN, not absence of clicks |
| 15-16 | LU kernel installed / identical | trivial | generic and unrolled paths execute the same flop sequence, so bit-equality is expected; says nothing about numerical conditioning |
| 17 | no unrecovered solver failures | yes | genuine but counts only fallbacks that were *detected* |
| 18 | H3 agrees across 44.1/48/96 within 1.5 dB | chosen metric | THD differs 5.66 % vs 6.27 % (about 10 % relative) and the dossier's own tolerance is 1 dB for significant harmonics |
| 19 | alias drops > 6 dB with 2x | meaningless | see D1 |
| 20-21 | ripple at 2 x mains | yes (1x vs 2x) | resolution is 48 000/8192 = 5.9 Hz and the tolerance is +-6 Hz, so it only rejects "wrong harmonic"; genuine for that |
| 22 | efficiency 20-65 % | very wide band | passes for almost any physically sensible amp |
| 23-25 | K_in worked example, rated V | yes (arithmetic) | genuine but trivially so |

(Rows 1, 7, 8, 14-18, 22 get their experiments in later checkpoints.)

---

## 3. Requirement matrix

`TODO-AUDIT` (next checkpoint).

## 4. Requirements that are impossible without hardware data

`TODO-AUDIT` (next checkpoint).
