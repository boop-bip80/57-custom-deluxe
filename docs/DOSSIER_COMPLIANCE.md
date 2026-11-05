# Dossier compliance audit (adversarial, evidence-based)

Scope: the implementation at commit `2a356c9` (branch `claude/eager-edison-6lario`) against the "Public-source Engineering
Dossier" (Sec. 02-31). Report only: no source file was changed. Probes live outside the repo (`/tmp/claude-0/audit/probes`);
every number below was measured by building the JUCE-free tools (`-DCD57_BUILD_PLUGIN=OFF`) and driving `CD57Engine` /
`AmpCircuit` / `Network` directly. Baseline reproduced first: `cd57_validate` = 27 passed / 0 failed, report identical to
`docs/VALIDATION_REPORT.md`.

Status of this document: **checkpoint 1 of N** (written incrementally; sections marked `TODO-AUDIT` are not finished yet).

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
