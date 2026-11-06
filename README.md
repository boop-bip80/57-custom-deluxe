# '57 Custom Deluxe — circuit-level amplifier model (VST3 + Standalone)

A digital recreation of the modern Fender '57 Custom Deluxe (PR 728 / 815050 family), built **at the circuit level** from the public-source
engineering dossier (`Fender '57 Custom Deluxe — Public-source Engineering Dossier`, Edition 1). Every stage is solved as part of one
interacting nonlinear electrical system (modified nodal analysis) — not as a chain of EQ + waveshaper blocks.

> **Honest status.** This is a *documented approximation* of the modern amplifier: the historical 5E3-family topology with modern nominal
> values, transcribed from the dossier's schematic (Fig. 7) and checked against it by an independent re-transcription, with tube laws fitted to
> the dossier's own manufacturer curve sheets. The dossier itself says (pp. 1, 2, 62) that no specimen was measured and that a one-to-one
> claim cannot be made from documents alone, and this project follows that rule. Every parameter carries an evidence label; assumptions carry
> sensitivity intervals; `docs/DOSSIER_COMPLIANCE.md` lists what was audited, what was fixed and what could not be; `docs/TRACEABILITY.md` lists
> every number that does **not** come from the dossier. What a real unit would add (specimen schematic, measured pots, transformers, speaker,
> cabinet, microphone chain) is listed in the dossier's missing-data ledger and cannot be replaced by code.

## What is modelled

| Dossier subsystem | Implementation |
|---|---|
| Jack network, plug states (§06) | Four explicit jack sockets with switching jacks, guitar plug selectable per jack, optional **jumper cable** between any two jacks. The 1 MΩ / 136 kΩ input impedances and the −6 dB of input 2 are **reproduced by the circuit** (not fitted). |
| First stage (§07) | 12AY7 pair with **shared cathode** (820 Ω ∥ 25 µF) so the channels couple through cathode voltage; electrode capacitances between real nodes. |
| Volumes + tone (§08) | Coupled passive network: two volume pots (wiper fed from the plate coupling caps), tone pot with the 500 pF bright path and 5 nF shunt, the **silent channel still loads the driven one**. Pots are two resistances joined at the wiper with a documented endpoint contact resistance. Taper numerics are unknown → assumed and selectable. |
| Gain stage + cathodyne (§09) | 12AX7 stage and bootstrapped cathodyne (1 MΩ returns to the 1.5 kΩ / 56 kΩ junction), coupling-capacitor charge state. |
| Output stage (§10) | Two 6V6 models with plate, **screen and grid current**, shared-cathode bias dynamics, blocking distortion through the coupling-capacitor / grid-current path. |
| Supply (§11) | Explicit 5Y3 rectifier conduction, transformer series resistance, three 16 µF filter nodes (5 kΩ, 22 kΩ): sag, ripple, rail interaction; 50/60 Hz market variants. |
| Output transformer (§12) | Centre-tapped primary, winding resistance, leakage inductance and a flux-state core. **Linear by default**; an assumed saturating law is an explicit option (the dossier says not to assign one before evidence exists). |
| Speaker (§17–18) | Electromechanical equivalent (Re, Le, motional RLC from T/S) inside the circuit so the amp sees a **complex load**; voice-coil heating raises Re (not in the dummy-load modes, where the coil is disconnected). |
| Acoustics (§18–19, 27) | **Provisional** cone-velocity → microphone operator. A measured voltage→mic IR can replace it (route 1), or be factored onto the model's own cone motion (route 2, "hybrid") so cone motion and heating stay in the loop without counting the cone twice. Gain, polarity and arrival time of the IR are preserved. |
| Calibration (§24) | Physical volts at the jack and at the speaker terminals; input calibration is a control; the jack voltage is limited to the 30 V peak the solver is validated for. |
| Numerics (§21, 23) | Modified nodal analysis, θ-method companions, Newton with line search that accepts a step only if the nodal KCL residual is small too, adaptive sub-step fallback with counters, half-band oversampling (1/2/4/8×) with exact whole-sample latency, no heap allocation in the audio path, circuit state kept across blocks and across an oversampling change. |

## How the tube models were obtained

The four JJ characteristic sheets in the dossier (12AY7, ECC83S, 6V6S, 5Y3S) were **digitised at pixel level** (`scripts/digitize_jj_sheets.py`: calibrated
axes with sub-pixel grid fitting, printed-grid removal, ridge tracing of every curve, overlay images for visual verification), then fitted with a global
optimiser (`scripts/fit_tubes.py`) and validated on whole held-out curves and across panels, as the dossier's §16 requires.
Results: 12AY7 5.1 % and 12AX7 4.8 % relative error in the operating region; 6V6 7.1 % in the amplifier's region (19.5 % with a positive grid) — the sheet is
triode-connected, so the screen partition and Early voltage come from an external RCA rating point that is labelled as such. The JJ sheets describe JJ
tubes, not necessarily Fender's stock tubes (the dossier says so too).

## Build

Requirements: CMake ≥ 3.22, a C++17 compiler. JUCE 8.0.15 is fetched automatically (or pass `-DCD57_JUCE_DIR=/path/to/JUCE`). Python 3 with numpy and scipy is
optional (needed for the independent re-implementation test and for re-running the digitiser/fitter).

```bash
# Linux system packages for JUCE
sudo apt-get install -y build-essential cmake ninja-build pkg-config libasound2-dev libx11-dev libxrandr-dev libxinerama-dev \
    libxcursor-dev libxext-dev libfreetype-dev libfontconfig1-dev libgl1-mesa-dev libcurl4-openssl-dev
```

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target CustomDeluxe57_VST3 CustomDeluxe57_Standalone
```

Artefacts: `build/CustomDeluxe57_artefacts/Release/VST3/57 Custom Deluxe.vst3` and `.../Standalone/57 Custom Deluxe`.
Copy the `.vst3` bundle to `~/.vst3` (Linux), `%COMMONPROGRAMFILES%\VST3` (Windows) or `/Library/Audio/Plug-Ins/VST3` (macOS).
GitHub Actions (`.github/workflows/build.yml`) builds all three platforms and runs the test suite; only the **Linux** build was run and tested during development.

## Using it

* **Input:** the plugin takes a mono guitar signal. *Input channel* chooses the mean of L and R (a dual-mono source stays at its calibrated level), left only or right only.
  *Input Calibration* sets the volts at the jack for a 0 dBFS sample (default 1.0 V peak; with *Input trim* ±12 dB the jack never sees more than 20 V): the dossier stresses that this must be calibrated to your interface — it is a control, not a Fender number.
* **Panel:** Instrument Volume, Mic Volume, Tone (printed 0–12 scale — an assumption), power switch, four jacks laid out as photographed (MIC | INST, rows 1 and 2; click to plug the guitar in), jumper cable, pilot jewel.
* **Output:** *Microphone (provisional model)* · *Speaker terminals* (calibrated volts, 0 dBFS = 28.28 V peak, rated 12.5 W sine = −6 dBFS) · *User IR* (voltage→mic capture, route 1) · *User IR on cone motion (hybrid)* (route 2).
* **Service panel** shows live rail voltages (B+1/2/3), 6V6 cathode voltage, voice-coil heating, solver iterations/fallbacks and CPU.
* **Lab controls:** market variant (50/60 Hz and nominal volts from the manual's table), line voltage %, V1/V2 tube swap, silicon rectifier, source model (line/DI or passive pickup + cable), volume taper, output-transformer saturation option, oversampling, specimen-variation seed, hum and noise (seeded; the noise-free circuit stays independently testable).
* **Latency** is reported to the host and is exact and rate-dependent. At 2× (default): 79 / 45 / 13 samples at 44.1 / 48 / 96 kHz; 4×: 87 / 52 / 19; 8×: 90 / 55 / 21; 1×: 0.
* Changing the oversampling factor **keeps the circuit state** (rails, capacitor charge, transformer flux, coil temperature): the new engine adopts it, its up-sampler is primed with recent input, and the old engine is cross-faded out over 512 samples.

**CPU** (one core of a shared cloud container, 512-sample blocks at 48 kHz, heavy drive with control automation, `validate_realtime`): mean / p99.9 of the real-time budget —
1×: 40 % / 112 %, **2× (default): 47 % / 95 %**, 4×: 68 % / 131 %, 8×: 116 % / 273 %. 8× is therefore labelled *offline*. Timings are reported, not gated, because shared runners are noisy.

## Validation

`ctest --test-dir build` runs ten tests (grouped into nine rows below); each reports PASS / FAIL against a criterion fixed in advance, and INFO for numbers that are only recorded.

| Suite | What it establishes | Report |
|---|---|---|
| `dossier_validation` (`cd57_validate`) | everything in dossier §26–27 that does not need a physical amplifier: input impedances, DC point and KCL, control interactions, AC-vs-transient at the operating rates, rated power as a **calibration residual**, harmonics, burst recovery by lock-in, supply ripple, block-size independence | `docs/VALIDATION_REPORT.md`, `docs/NETLIST.md` |
| `netlist_vs_schematic_register` (+ self-test) | the netlist equals an independent re-transcription of Fig. 7 at graph level (14 injected faults all caught) | `docs/SCHEMATIC_TRANSCRIPTION.md` |
| `independent_reimplementation` | a Python implementation sharing no code with `src/dsp` (own parser, stamps, device equations, DC and AC solves) agrees with the C++ solver: DC within 0.24 V, AC within 0.0013 dB / 0.002° | `scripts/independent_check.py` |
| `tube_device_laws` | analytic Jacobians vs finite differences, C¹ continuity, signs, and that the C++ laws reproduce every digitised sheet point | `tools/validate_tubes.cpp` |
| `supply_regulation_vs_jj_5y3s` | the rectifier law in a simulated capacitor-input supply vs the JJ regulation curves: droop within 7 %, constant offset reported | `tools/validate_supply.cpp` |
| `numerics_validation` | aliasing and integration error vs a 16× reference, resampler specification, step-size convergence, assembled Jacobian, KCL along a transient, charge conservation, **whole-amplifier energy balance (0.000 %)** | `docs/NUMERICS_REPORT.md` |
| `realtime_robustness` | zero heap allocation (operator new counted) in steady state and across every control/config change, NaN/Inf and over-range input, Newton acceptance, load modes, linear OT default, state transfer, per-block timing | `tools/validate_realtime.cpp` |
| `acoustic_validation` | the Sec. 27 linear-acoustic identity (route 1 vs route 2) with a **synthetic** H, IR gain/polarity/arrival time, resampling, dipole algebra | `tools/validate_acoustic.cpp` |
| `plugin_processor` | the built plugin's processor driven directly: reported latency, input-channel choice, acoustic modes and IR state, hybrid vs route 1, continuity across an oversampling swap | `tests/ProcessorTest.cpp` |

Also: `cd57_host_test` loads the built VST3 through JUCE's VST3 host (`xvfb-run` on Linux); `cd57_render` is an offline WAV→WAV renderer.

**Not evaluated (needs reference hardware):** magnitude/phase/harmonic/IMD/recovery agreement with a real unit, speaker impedance and nonlinear isolation, microphone chain,
blind listening, the capture programme. The 12.5 W / <5 % THD figure is a **calibration** of the unknown power-transformer voltage, not independent evidence; the validation
report publishes how weakly it constrains that value. The dossier's −80 dBFS alias candidate limit is met for a heavy six-note chord at 2× but not for a heavy single
3.1 kHz tone below 8× (solver truncation dominates); see `docs/NUMERICS_REPORT.md`.

## Provenance and how to refine the model

* `data/netlist/cd57_nominal.net` — readable, versioned netlist; each element has an evidence tag (`CONF`/`HIST`/`ASSUMED`/`DATASHEET`/`PROP`) and source id. `data/netlist/schematic_register.json` is the independent transcription it is checked against.
* `data/parameters/cd57_nominal.json` — the §30 manifest (56 entries; the loader refuses entries without a status, or an assumed value without an interval, and fails on a missing requested key). Only 14 entries drive the model; the rest document values that live elsewhere (listed in the validation report).
* `data/tubes/digitized/` + `scripts/` — the digitised JJ sheets with overlay images, the digitiser, the fitter and `generated/TubeFits.h`. Re-run: `python3 scripts/digitize_jj_sheets.py && python3 scripts/fit_tubes.py && python3 scripts/make_manifest.py`.
* To adopt measurements: edit values in the netlist/manifest (keep the evidence label), re-run `scripts/make_manifest.py`, re-calibrate the power-transformer value with `cd57_validate --calibrate-pt`, run the suites.
  After changing the netlist **topology**, run `cmake --build build --target regen_lu_kernel` (a pattern-signature check falls back to the generic solver if you forget — slower, never wrong).

## Limitations (please read)

* Transformer, speaker, pot-taper, cabinet and supply values are **assumptions** until measured (`assumed`/`unknown` in the manifest). The power-transformer voltage is **calibrated** to the documented 12.5 W / <5 % THD.
* The JJ sheets describe JJ tubes; the 6V6 sheet is triode-connected, so pentode-mode behaviour rests partly on one external RCA rating point (labelled **EXTERNAL** in `docs/TRACEABILITY.md`). Tube fits are 5–7 % in the operating region and worse with a positive grid or near cut-off.
* The modern amplifier's exact service schematic was not available; the historical 5E3 topology is used with the modern nominal values (dossier §04). The one value conflict between the dossier text (0.022 µF) and the drawing (.02) is documented; the dossier's value is used.
* The dossier's cited primary sources (owner's manual, the earlier service drawing, RCA/other datasheets, the Mercury transformer sheet, Koren, ...) could not be fetched from the build environment (outbound access to those hosts is denied). Only the dossier's own text and embedded figures were used. The earlier service drawing's test-point voltages, the RCA 6V6/5Y3 curves and the manual's speaker-jack rules would each tighten the model if they were supplied.
* Acoustic output is a physics-based **provisional** operator, not PN 7708599000 in its cabinet; load a measured voltage→mic IR for a specific sound. The 5.2 kHz cone roll-off may overlap what the voice-coil inductance already does; that cannot be decided without data.
* Large-signal speaker nonlinearity (Bl(x), Kms(x)) is not modelled (no data); the heater warm-up/standby is not simulated at plugin start (the engine starts at a solved quiescent state, as the dossier recommends); the power switch ramps the supply.
* Mono processing; Windows/macOS builds are CI-only and untested here. Latency changes of the hosted VST3 reach a host through the host's own latency-change mechanism.

## Licensing and trademarks

Built on [JUCE 8](https://juce.com) (AGPLv3 / commercial licence): distributing a build of this plugin requires complying with JUCE's licence — **no licence file is included; the repository owner has to choose one accordingly.**
"Fender", "Deluxe" and model names are trademarks of their owners; this is an independent educational model, not affiliated with or endorsed by them. No Fender artwork or logo is used.
No Fender photograph is included. **`data/tubes/source_images/` holds the four JJ Electronic characteristic sheets exactly as embedded in the dossier (the digitiser re-runs from them), and the overlay PNGs in `data/tubes/digitized/overlays/` draw the traced curves on top of them.** They are third-party manufacturer material kept for fitting and verification; the repository owner should confirm that redistributing them is acceptable or delete both directories (the digitised CSVs, the fit and every test work without them at run time; only re-running the digitiser needs the images).
