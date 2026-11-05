# Dossier traceability

Status: **Implemented** · **Partial** (provisional/assumed values) · **Not claimed** (needs data the public record lacks).

| § | Topic | Status | Where |
|---|---|---|---|
| 02 | Verified target spec (12.5 W/<5 % THD, 1 M/136 k inputs, tubes, 8 Ω, market variants) | Implemented; spec values reproduced/used as checks | `cd57_validate` §3–4, `CD57Engine.h` `marketVariants()`, manifest |
| 03 | Photographic atlas | Reference only (UI uses original artwork) | — |
| 04 | Source lineage / four branches | Implemented as evidence labels; historical + modern-nominal branch | netlist tags, manifest `status` |
| 05 | Architecture, model boundary | Implemented: jack EMF → speaker terminals, plus cone velocity | `AmpCircuit.h`, `CD57Engine.h` |
| 06 | Inputs, four jacks, patch | Implemented | netlist (jack block), `AmpCircuit::applyControls` |
| 07 | 12AY7 pair, shared cathode state | Implemented | netlist V1, `Network.h` |
| 08 | Interacting volumes/tone; pot as two resistances; endpoints | Implemented; **taper numerics unknown → assumed, selectable** | netlist, `PotMap` |
| 09 | Gain stage + bootstrapped cathodyne | Implemented | netlist V2A/V2B |
| 10 | Cathode-biased push-pull; plate/screen/grid; blocking distortion | Implemented | `Devices.h` `evalPentode`, netlist |
| 11 | Rectifier, supply, sag, 50/60 Hz | Implemented (explicit rectifier); PT values assumed | netlist supply, validation §9 |
| 12 | Transformer: leakage, magnetising, flux state | Implemented; parasitics/saturation **assumed** | `Network::Xfm` |
| 13 | Components, tolerances, specimen variation, hum vs noise | Implemented as optional creative feature (seeded) | `AmpConfig::variationAmount`, hum/noise |
| 14 | Historical component baseline | Implemented verbatim | netlist |
| 15–16 | Tube atlas, constitutive fits, grid/screen current, overflow-safe maths | Implemented; **candidate fits** from digitised JJ/RCA data | `scripts/fit_tubes.py`, `Devices.h` |
| 17–18 | Speaker electrical/mechanical, thermal | Implemented (linear + thermal); T/S **assumed**; Bl(x) **not claimed** | `AmpCircuit.h`, `CD57Engine.h` |
| 19 | Cabinet, mic, room | **Partial/provisional**; room not modelled; user IR supported | `Acoustic.h` |
| 20 | Layered architecture | Implemented (calibration → circuit → transformer/load port → acoustic → shell) | `CD57Engine.h` |
| 21 | MNA, θ-methods, Newton, checks | Implemented incl. independent AC cross-check, KCL residual, failure counters | `Network.h`, validation §2,5,8 |
| 22 | WDF / neural alternatives | Not used (nodal reference chosen; no training data exists) | — |
| 23 | Rate, aliasing, latency, realtime | Implemented: 44.1/48/96 kHz, 1–8× oversampling, latency reporting, no audio-thread allocation | `Oversampler.h`, plugin |
| 24 | Calibration/physical units | Implemented (worked example verified) | validation §10 |
| 25 | Capture matrix | Not applicable without hardware | — |
| 26–27 | Metrics / eight experiments | Evaluated where no hardware is needed; remainder **listed as NOT EVALUATED** | `docs/VALIDATION_REPORT.md` |
| 28 | Missing-data ledger | Mirrored as `unknown`/`assumed` manifest entries with intervals | manifest |
| 29 | Milestones | A–E, G done for the nominal model; F partial; H (held-out hardware evidence) not possible | this table |
| 30 | Parameter manifest | Implemented + validating loader | `data/parameters`, `Manifest.h` |
| 31 | Solver skeleton | Implemented (fallback is defined, counted and reported) | `Network::step` |
| 33 | Handoff checklist | Honest answer: **nominal documented approximation**, not a 1:1 claim | README |
