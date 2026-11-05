# '57 Custom Deluxe — circuit-level amplifier model (VST3 + Standalone)

A digital recreation of the modern Fender '57 Custom Deluxe (PR 728 / 815050 family) built **at the circuit level** from the
public-source engineering dossier (`Fender '57 Custom Deluxe — Public-source Engineering Dossier`, Edition 1). Every
stage is solved as part of one interacting nonlinear electrical system — not as a chain of EQ + waveshaper blocks.

> **Honest status.** This is a *documented approximation* of the modern amplifier: the historical 5E3-family topology with
> modern nominal values. The dossier is explicit that no specimen was measured and that a one-to-one claim cannot be made from
> documents alone. This project follows that rule: every parameter carries its evidence label, assumptions carry sensitivity
> intervals, and `docs/VALIDATION_REPORT.md` lists what was verified and what was **not** (needs reference hardware).

## What is modelled

| Dossier subsystem | Implementation |
|---|---|
| Jack network, plug states (§06) | Four explicit jack sockets with switching jacks, guitar plug selectable per jack, optional **jumper cable** between any two jacks. Input impedances 1 MΩ / 136 kΩ and the −6 dB of input 2 are **reproduced by the circuit** (not fitted). |
| First stage (§07) | 12AY7 pair with **shared cathode** (820 Ω ∥ 25 µF) so the channels couple through cathode voltage; electrode capacitances between real nodes. |
| Volumes + tone (§08) | Coupled passive network: two volume pots (wiper fed from the plate coupling caps), tone pot with 500 pF bright path and 5 nF shunt, the **silent channel still loads the driven one**. Pots are two resistances joined at the wiper with a documented contact resistance at the endpoints. |
| Gain stage + cathodyne (§09) | 12AX7 stage and bootstrapped cathodyne (1 MΩ returns to the 1.5 kΩ/56 kΩ junction), coupling-cap charge state. |
| Output stage (§10) | Two 6V6 models with plate, **screen and grid current**, shared-cathode bias dynamics, blocking distortion through the coupling-cap/grid-current path. |
| Supply (§11) | Explicit 5Y3 rectifier conduction, transformer series resistance, three 16 µF filter nodes (5 kΩ, 22 kΩ): sag, ripple, rail interaction. 50/60 Hz market variants. |
| Output transformer (§12) | Centre-tapped primary, winding resistance, leakage inductance and a **core-flux state with saturation**. |
| Speaker (§17–18) | Electromechanical equivalent (Re, Le, motional RLC from T/S) inside the circuit so the amp sees a **complex load**; voice-coil heating raises Re. |
| Acoustics (§18–19) | **Provisional** cone-velocity → microphone operator (open-back dipole, directivity, mic position). Optional **user IR** (voltage→mic) *replaces* it — never both (the dossier's double-counting rule). |
| Calibration (§24) | Physical volts at the jack and at the speaker terminals; input calibration is a control. |
| Numerics (§21, 23) | Modified nodal analysis, θ-method companions, Newton with line search, adaptive sub-step fallback with counters, half-band oversampling (1/2/4/8×) with exact reported latency, state persists across blocks. |

## Build

Requirements: CMake ≥ 3.22, a C++17 compiler. JUCE 8.0.15 is fetched automatically (or pass `-DCD57_JUCE_DIR=/path/to/JUCE`).

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
GitHub Actions (`.github/workflows/build.yml`) builds all three platforms; only the **Linux** build was run and tested during development.

Other targets: `cd57_validate` (acceptance suite), `cd57_render` (offline WAV→WAV renderer), `cd57_host_test` (loads the built VST3 through JUCE's VST3 host), `regen_lu_kernel` (see below).

## Using it

* **Input:** the plugin takes a mono guitar signal (stereo is summed). *Input Calibration* sets the volts at the jack for a 0 dBFS sample (default 1.0 V peak); the dossier stresses that this must be calibrated to your interface — it is a control, not a Fender number.
* **Panel:** Instrument Volume, Mic Volume, Tone (printed 0–12 scale — an assumption), power switch, four jacks (click to plug the guitar in), jumper cable, pilot jewel.
* **Output:** *Microphone (provisional model)* · *Speaker terminals* (calibrated volts, 0 dBFS = 28.28 V peak, rated 12.5 W sine = −6 dBFS) · *User IR* (load a voltage→mic capture).
* **Service panel** shows live rail voltages (B+1/2/3), 6V6 cathode voltage, voice-coil heating, solver iterations/fallbacks and CPU.
* **Lab controls:** market variant (50/60 Hz and nominal volts from the manual's table), line voltage %, V1/V2 tube swap, silicon rectifier, source model (line/DI or passive pickup + cable), volume taper, oversampling, specimen-variation seed, hum and noise (seeded; the noise-free circuit stays independently testable).
* Latency is reported to the host (0 / 31 / 36 / 39 samples for 1× / 2× / 4× / 8×).

CPU on the development container (one core, Release): **1× 16 %, 2× 23 %, 4× 42 %** of a core at 48 kHz for typical playing; the
solver averages ~1.0–1.8 Newton iterations per sample. Heavy sustained overdrive costs more (adaptive sub-stepping engages for ~1 % of steps).

## Validation

`cd57_validate` implements every acceptance check of dossier §26–27 that **does not need a physical amplifier** and writes
`docs/VALIDATION_REPORT.md` (committed) and `docs/NETLIST.md` (element register with evidence labels). Current result: **27 passed, 0 failed**.

Highlights: input 1/2 impedance 999.9 kΩ / 136.0 kΩ and −6.03 dB (reproduced, not fitted) · rated power 12.8 W at <5 % THD (a *calibration anchor*, labelled as such) ·
transient solver vs an independent dense complex AC solver agree to **0.01 dB** (60 Hz–8 kHz) · output bit-identical across host block sizes ·
unrolled LU kernel identical to the generic solver · zero unrecovered solver failures · 50/60 Hz ripple at 2×mains.
`cd57_host_test` loads the built VST3 through JUCE's host at 44.1/48/96 kHz with awkward block sizes, automation, state round-trip and a live engine rebuild — **clean under valgrind**.

**Not evaluated (needs reference hardware):** magnitude/phase/harmonic/IMD/recovery agreement with a real unit, speaker impedance and nonlinear isolation, microphone chain, blind listening. See the report.

## Provenance and how to refine the model

* `data/netlist/cd57_nominal.net` — readable, versioned netlist; each element has an evidence tag (`CONF`/`HIST`/`ASSUMED`/`DATASHEET`) and source id.
* `data/parameters/cd57_nominal.json` — the §30 manifest (55 parameters: 12 manufacturer-documented, 3 historical-nominal, 3 derived, 4 candidate fits, 5 proposed, **24 assumed with intervals, 4 unknown**). The loader refuses entries without a status, or an assumed value without an interval.
* `scripts/fit_tubes.py` — fits Koren-form tube models to the dossier's digitised JJ curves + RCA anchors (`data/tubes/*.csv`, residuals in `fitted_models.json`).
* To adopt measurements: edit values in the netlist/manifest (keep the evidence label), re-run `scripts/make_manifest.py`, run `cd57_validate`.
  After changing the netlist **topology**, run `cmake --build build --target regen_lu_kernel` (a pattern-signature check falls back to the generic solver if you forget — slower, never wrong).

## Limitations (please read)

* Transformer, speaker, pot-taper, tube-curve, cabinet and supply values are **assumptions** until measured (`assumed`/`unknown` in the manifest). The power-transformer voltage was **calibrated to the documented 12.5 W / <5 % THD**, so that figure is not independent evidence.
* The modern amplifier's exact service schematic was not available; the historical 5E3 topology is used with the modern nominal values (dossier §04).
* Acoustic output is a physics-based **provisional** operator, not PN 7708599000 in its cabinet. Load a measured voltage→mic IR for a specific sound.
* Large-signal speaker nonlinearity (Bl(x), Kms(x)) is not modelled (no data); heater warm-up/standby is not simulated at plugin start (the engine starts at a solved quiescent state, as the dossier recommends); the power switch ramps the supply.
* Mono processing; Windows/macOS builds are CI-only and untested here.

## Licensing and trademarks

Built on [JUCE 8](https://juce.com) (AGPLv3 / commercial licence): distributing a build of this plugin requires complying with JUCE's licence — **no licence file is included; choose one for this repository accordingly.**
"Fender", "Deluxe" and model names are trademarks of their owners; this is an independent educational model, not affiliated with or endorsed by them. No Fender artwork or logo is used.
The dossier's third-party photos/graphs are not redistributed here; only values digitised from them for fitting.
