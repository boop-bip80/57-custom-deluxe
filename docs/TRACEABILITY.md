# Dossier traceability

Status: **Implemented** · **Partial** (provisional/assumed values) · **Not claimed** (needs data the public record lacks).
This is a documented approximation of the modern '57 Custom Deluxe (dossier Sec. 33). Nothing here is a measured one-to-one match.

| § | Topic | Status | Where |
|---|---|---|---|
| 02 | Verified target spec (12.5 W/<5 % THD, 1 M/136 k inputs, tubes, 8 Ω, market variants) | Implemented. The 1 M / 136 k / −6 dB figures are reproduced by the jack network; 12.5 W is a **calibration** (see below), not evidence | `cd57_validate` §3–4, `CD57Engine.h` `marketVariants()`, manifest |
| 03 | Photographic atlas | Reference only; the panel layout follows the photographs (2×2 jack block, three knobs, jewel) | `plugin/PluginEditor.cpp` |
| 04 | Source lineage / four branches | Implemented: evidence labels on every element and manifest entry; every manifest entry also carries `branch` (historical_5e3 / reissue_2006 / modern_nominal / specimen), `source_revision`, `serial_range` and `uncertainty`; measurements enter as a **versioned specimen delta stored beside the nominal value**. The `reissue_2006` branch is declared and **empty** (service drawing 0073201000 was not available) | netlist tags, manifest, `Manifest::mergeDelta`, `validate_manifest`, `docs/SOURCE_CATALOGUE.md` |
| 05 | Architecture, model boundary | Implemented: jack EMF → speaker terminals (voltage and, at the internal rate, current), plus cone velocity | `AmpCircuit.h`, `CD57Engine.h` |
| 06 | Inputs, four jacks, patch | Implemented, **including a second plug** (a second guitar or a terminated cable) so both jacks of one channel and one input per channel can be exercised; closed-form checks (−6.02 dB, 119.7 kΩ, superposition) | netlist (jack block, second source chain), `AmpCircuit::applyControls`, `validate_jacks` |
| 07 | 12AY7 pair, shared cathode state | Implemented; one-stage KCL/charge test | netlist V1, `validate_numerics` E |
| 08 | Interacting volumes/tone; pot as two resistances; endpoints | Implemented; **taper numerics unknown → assumed, selectable** | netlist, `PotMap` |
| 09 | Gain stage + bootstrapped cathodyne | Implemented | netlist V2A/V2B |
| 10 | Cathode-biased push-pull; plate/screen/grid; blocking distortion | Implemented; burst recovery measured by lock-in (gain dips to 0.19×, back within 5 % after 111 ms); per-valve conduction angle, overlap, grid and screen current, dissipation and bias shift reported over a drive sweep (no class label used) | `Devices.h` `evalPentode`, `cd57_validate` §7, `cd57_conduction`, `docs/CONDUCTION_REPORT.md` |
| 11 | Rectifier, supply, sag, 50/60 Hz | Implemented (explicit rectifier). 5Y3 law checked in-circuit against the JJ regulation curves: droop within 7 %, constant −14 V offset that depends on the sheet's unstated conditions; PT values assumed/calibrated | `validate_supply`, `cd57_validate` §9 |
| 12 | Transformer: leakage, magnetising, flux state | Implemented with a **linear core by default**; the saturating law is an explicit option with an assumed threshold (Sec. 12/29: no saturation law before evidence) | `Network::Xfm`, `validate_realtime` §5 |
| 13 | Components, tolerances, specimen variation, hum vs noise | Optional creative feature (seeded); **one-parameter-at-a-time sensitivity study** (41 parameters, 15 features, ranked, with the §26 tolerance count) done before any Monte Carlo | `AmpConfig::variationAmount`, hum/noise, `cd57_sensitivity`, `docs/SENSITIVITY_REPORT.md` |
| 14 | Historical component baseline | Implemented verbatim; independently re-transcribed from Fig. 7 and diffed at graph level (the one value conflict, V2A→V2B coupling cap, is documented: dossier 0.022 µF used, drawing prints .02) | netlist, `docs/SCHEMATIC_TRANSCRIPTION.md`, `scripts/check_netlist_against_register.py` |
| 15–16 | Tube atlas, constitutive fits, grid/screen current, overflow-safe maths | Implemented. Fits are to the **pixel-digitised** JJ sheets (calibration residual < 0.7 px), global optimiser, whole-curve hold-out and cross-panel checks. Errors: 12AY7 5.1 %, 12AX7 4.8 % relative in the operating region; 6V6 7.1 % (amp region), 19.5 % with a positive grid. The 6V6 sheet is triode-connected; screen partition comes from an external anchor (ledger below) | `scripts/digitize_jj_sheets.py`, `scripts/fit_tubes.py`, `data/tubes/`, `validate_tubes` |
| 17–18 | Speaker electrical/mechanical, thermal | Implemented (linear + thermal); T/S **assumed**; Bl(x) **not claimed**; no heating in the dummy-load modes | `AmpCircuit.h`, `CD57Engine.h`, `validate_realtime` §4 |
| 19 | Cabinet, mic, room | **Partial/provisional**; room not modelled; measured IR supported with gain, polarity and arrival time preserved | `Acoustic.h`, `AcousticFactor.h`, `validate_acoustic` |
| 20 | Layered architecture | Implemented (calibration → circuit → transformer/load port → acoustic → shell) | `CD57Engine.h` |
| 21 | MNA, θ-methods, Newton, checks | Implemented. Newton acceptance requires a small nodal KCL residual as well as a small step; assembled Jacobian vs finite differences, KCL along transients, energy balance closes to 0.000 %, independent implementation agrees (DC 0.24 V, AC 0.0013 dB) | `Network.h`, `validate_numerics` E, `scripts/independent_check.py` |
| 22 | WDF / neural alternatives | Not used (nodal reference chosen; no training data exists) | — |
| 23 | Rate, aliasing, latency, realtime | Aliasing and integration error measured against a 16× reference; resampler ripple ≤ 0.0002 dB, rejection ≥ 103 dB, exact whole-sample latency; zero heap allocation in the engine; oversampling change keeps circuit state. The −80 dBFS candidate limit is met for a heavy chord at 2×, **not** for a heavy single 3.1 kHz tone below 8× (solver truncation) | `validate_numerics`, `validate_realtime`, `Oversampler.h` |
| 24 | Calibration/physical units | Implemented (worked example verified); input limited to the validated 30 V peak at the jack | validation §10 |
| 25 | Capture matrix | Programme **implemented and rendered through the model**: 4 inputs × 27 knob combinations × clean / onset / driven = 324 captures, + endpoints, jack states, held-out passages; stimulus, §30 metadata and raw/processed separation as the dossier asks. **No hardware captures exist** | `cd57_capture_matrix`, `data/capture/`, `data/reference/capture_matrix_summary.csv` |
| 26–27 | Metrics / eight experiments | The §26 metrics (NRMSE / ESR, fixed-delay removal, response / phase / group delay, onset, harmonics, IMD, burst envelope, recovery) are **implemented and verified against analytic answers**; `cd57_compare` applies the §26 limits to any two capture sets and is shown to pass identical renders and fail a perturbed model. Comparisons with the amplifier are **NOT EVALUATED** (no hardware). Experiment 4 (linear acoustic identity) is verified algebraically with a synthetic H only | `src/analysis/Metrics.h`, `validate_metrics`, `cd57_compare`, `docs/VALIDATION_REPORT.md`, `validate_acoustic` |
| 28 | Missing-data ledger | Mirrored as `unknown`/`assumed` manifest entries with intervals | manifest |
| 29 | Milestones | B (independent transcription), C (independent simulation), G (numerics/real-time) done for the nominal model; A, D, E, F, H need hardware | this table |
| 30 | Parameter manifest | Implemented + validating loader (schema 2); 20 of 59 entries drive the model (including the output transformer and measured pot maps), the other 39 are explicit documentation entries (listed in the validation report); specimen delta merge; capture-metadata schema and calibration files | `data/parameters`, `data/capture`, `data/calibration`, `Manifest.h`, `cd57_manifest` |
| 31 | Solver skeleton | Implemented (fallback is defined, counted and reported) | `Network::step` |
| 33 | Handoff checklist | Honest answer: **nominal documented approximation**, not a 1:1 claim | README |

## Numbers that are NOT from the dossier (ledger)

The dossier cites sources it does not reproduce (S02 manual, S04 service drawing, S08/S19/S20 RCA sheets, ...). They could not be fetched from the build environment (egress policy), so only the dossier's own text and embedded figures were used. Everything else is one of:

| Value | Used for | Label |
|---|---|---|
| RCA 6V6GT rating point: 250 V / 250 V / −12.5 V, Ip 45 mA, Ig2 4.5 mA, gm 4.1 mS, rp 52 kΩ | screen partition fs0 = 4.5/49.5 and Early voltage va = rp·Ip = 2340 V; consistency check of the JJ-fitted law (model gives 48.1 mA / 4.8 mA, i.e. +7 %) | **EXTERNAL** (not in the dossier) |
| 6V6 knee shape (vk = 55 V, q = 1.5) | screen partition vs plate voltage | ASSUMED |
| Grid-conduction onset (6V6: vc −0.4 V, sg 0.2 V; triodes: k 0.35 mA/V^1.5, vc −0.5 V, sg 0.15 V) | grid current | ASSUMED (the 6V6 plateau is digitised from the dashed Ig curves; triodes have no data) |
| Interelectrode capacitances (12AX7/12AY7 1.5–1.7 / 0.46–0.5 pF, 6V6 9.5 / 0.7 / 7.5 pF) | parasitic capacitors in the netlist | datasheet-class, not in the dossier text, unverified |
| Power-transformer secondary 377.4 V RMS per half | rail voltage | **CALIBRATED** so the interpolated 5 % THD power into 8 Ω equals the rated 12.5 W; the anchor constrains it only weakly (sensitivity table in the validation report) |
| PT series resistance 90 Ω, output-transformer DCR / leakage / magnetising inductance, speaker T/S and thermal values, pot tapers, acoustic path/roll-off | supply, load, speaker, acoustics | ASSUMED with intervals (manifest) |
| Output transformer ratio 8 kΩ : 8 Ω | transformer | dossier text (S10), an aftermarket part; identity with the OEM part unproven |
| V2A→V2B coupling capacitor 0.022 µF | cathodyne coupling | dossier §14 value; the drawing prints .02 (one-token alternative documented) |

## Manifest consumption

`cd57_validate` §1 prints which manifest entries the model reads and fails if a requested key is missing, so a typo can no longer fall back silently to a hard-coded default. The model now reads 20 entries: the power-transformer
secondary voltage and series resistance; the output transformer's turns ratio, magnetising inductance, leakage inductance, primary and secondary DC resistance and (optional) saturation flux; the pot total resistance and endpoint contact resistance;
the speaker's Re, Le, fs, Qms, Qes, Mms, Sd, voice-coil tempco and the two thermal values; and — when a specimen delta provides them — the three measured pot maps. A measured value is used in preference to the nominal one, which stays in the entry.
The remaining entries document values that live in the netlist, in `generated/TubeFits.h` or in plugin controls, or are unknown/unmodelled. At the shipped values the manifest-driven transformer reproduces the netlist's own numbers exactly
(`cd57_validate` §1), so the two sources cannot disagree silently.
