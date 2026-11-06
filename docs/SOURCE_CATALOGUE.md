# Source catalogue (dossier Sec. 32 and 30: "source catalogue" of the engineering package)

The dossier cites 32 sources (S01-S32) and 17 figure assets. This table says, for each, **what the dossier uses it for**, **whether this implementation could read it**, and
**where it enters the repository** — so that nothing in the model is attributed to a document nobody read.

**Availability.** The dossier PDF itself (text, tables and the 17 embedded figures) is the only input this implementation had. Every cited web source was tested from the build
container and refused by the network policy (the proxy rejected the connection to `fmicassets.com`, `adrsoundsense.co.uk`, `r-type.org`, `drtube.com`, `mercurymagnetics.com`,
`normankoren.com`, `prowessamplifiers.com`, `eminence.com`, `fender.com`, ...). Nothing below is therefore "checked against the primary document"; values attributed to a source are
those the **dossier text states**. Where the dossier only *names* a source, the model does not use a number from it.

Evidence labels are the dossier's (Sec. 01): CONFIRMED, HISTORICAL, DERIVED, PROPOSED, UNKNOWN.

| ID | Source (as the dossier catalogues it) | Dossier uses it for | Read by this implementation? | Where it enters the repository |
|---|---|---|---|---|
| S01 | Fender '57 Custom Deluxe official product specifications and photographs (retrieved 5 Nov 2026) | CONFIRMED target specification (Sec. 02), component-change advertising, photographs (Figs 1-6) | Dossier text and Figs 1-6 only | manifest `manufacturer_documented` entries; market table `CD57Engine.h::marketVariants()`; panel/jack layout of the plugin UI (Fig. 3). Photographs are **not** redistributed |
| S02 | Fender Owner's Manual PN 7708607000 Rev. A (c) 2016 (+ S17, a mirror of it) | model-specific numbers: 12.5 W / <5 % THD, 1 MOhm / 136 kOhm, tubes, speaker, mains, fuse, part numbers, market variants (Sec. 02) | **Manual not read**; dossier text only | manifest entries; the manual's speaker-jack and combined-impedance rules are **not modelled** (five load modes instead) |
| S03 | Original Deluxe 5E3 schematic, archive copy | HISTORICAL topology and component baseline (Sec. 05, 14) | Via the dossier's Fig. 7 (Robinette's annotated copy) | `data/netlist/cd57_nominal.net` (elements tagged HIST) and the independent re-transcription `data/netlist/schematic_register.json` |
| S04 | '57 Deluxe / Fender 57 service drawing 0073201000 Rev. B, 17 Nov 2006 | evidence of an earlier reissue generation; **must not** be assigned to the modern unit (Sec. 04, 14) | **No** | nothing numeric. Manifest branch `reissue_2006` is declared and empty; the drawing's test-point voltages would be a cross-check once read |
| S05 | Rob Robinette, *How the Fender 5E3 Deluxe Works* (annotated schematic and layout) | HISTORICAL annotated drawing and layout (Figs 7, 8) | Figs 7 and 8 only | the netlist (every element checked against Fig. 7); the one dossier-vs-drawing value conflict (0.022 uF vs `.02`) is documented in the netlist |
| S06 | Merlin Blencowe, *The Cathodyne Phase Inverter* | cathodyne operation and overload (Sec. 09) | **No** (the dossier states the relation `|A_each| ~ mu R / (r_p + (mu+2) R)`) | modelled as a real circuit (V2B with its loads and bootstrap), not as an ideal inverter |
| S07 | Norman Koren, *Improved Vacuum Tube Models for SPICE Simulations* | tube-model family (Sec. 16) | **No** (method known; parameters are **fitted here**) | `src/dsp/Devices.h` (Koren form with a Kvb generalisation), `scripts/fit_tubes.py` |
| S08 | RCA 12AY7 manufacturer data, 1 April 1953 | one reference point: 250 V / -4 V, ~3 mA, mu 40, r_p 22.8 kOhm, g_m 1.75 mS (Sec. 07) | Dossier text (the point) | reported **beside** the model's local values in the validation report (the JJ-sheet fit is +9-10 % in mu and r_p there); not used as a constraint |
| S09 | 12AY7 manufacturer datasheet, Valve Museum archive | "additional characteristics" (Sec. 07) | **No** | - |
| S10 | Mercury Magnetics FTDO-59 output transformer | historical 5E3 replacement: 8 kOhm : 8 Ohm, n = sqrt(1000) = 31.6228 (Sec. 12); identity with the OEM part **unproven** | Dossier text | manifest `ot_turns_ratio_full_primary` (now drives the model); the winding resistances, leakage and magnetising inductance are **ASSUMED** with intervals |
| S11 | Eminence, Speaker FAQ (OEM identification) | custom OEM specifications are proprietary; identify the driver by label (Sec. 17) | **No** | the speaker model is a generic 12-inch alnico driver, every parameter `assumed` with an interval; never labelled PN 7708599000 |
| S12 | Eminence, *Understanding Loudspeaker Power Ratings* | OEM vs catalogue rating practice (Sec. 17) | **No** | - |
| S13 | Klippel, *Transducer Nonlinearities (Curve Shape)* | Bl(x), Kms(x), L(x) mechanisms (Sec. 17); Figs 13, 14 are **generic** | Dossier text and figures | large-signal speaker nonlinearity is **not modelled** (no data); listed `unknown` in the manifest |
| S14 | Klippel, *Compression of Fundamental Components* | thermal and nonlinear compression (Sec. 18); Figs 15, 16 are **generic** | Dossier text and figures | single-compartment thermal state (`C_th`, `R_th`, PROPOSED), no second compressor; heating changes only R_e(T) in the circuit |
| S15 | Siegfried Linkwitz, *Models for a Dipole Loudspeaker Design* | open-baffle radiation model and its limits (Sec. 19, Fig. 17) | Dossier text and Fig. 17 | provisional acoustic operator (`src/dsp/Acoustic.h`): ideal dipole with a path difference, **PROVISIONAL** |
| S16 | Fender, Open-back and Closed-back Speaker Enclosures | enclosure behaviour overview (Sec. 19) | **No** | - |
| S17 | Fender Owner's Manual, searchable mirror | mirror of S02, "not independent corroboration" | **No** | - |
| S18 | RCA 12AX7 datasheet, archive copy | average 12AX7 characteristics (Sec. 15) | **No** | the 12AX7 law is fitted to the JJ ECC83S sheet (S24) |
| S19 | 6V6GT / 5V6GT datasheet, Valve Museum archive | primary historical 6V6 data (Sec. 15) | **No** | the RCA rating-point numbers (250 V / 250 V / -12.5 V, 45 mA, Ig2 4.5 mA, g_m 4.1 mS, r_p 52 kOhm) that fix the screen partition and the Early voltage are **EXTERNAL**: they are not in the dossier text and are labelled as such in `docs/TRACEABILITY.md` |
| S20 | 5Y3GT datasheet, Valve Museum archive | primary historical rectifier curves (Sec. 15) | **No** | the rectifier law is fitted to the JJ 5Y3S sheet (S23) |
| S21 | JJ Electronic, 12AY7 characteristic sheets | modern 12AY7 example (Fig. 9) | **Fig. 9** | pixel-digitised (`data/tubes/digitized/12AY7_JJ_S21.csv`) and fitted |
| S22 | JJ Electronic, 6V6S characteristic sheets | modern 6V6 example (Fig. 11) | **Fig. 11** | pixel-digitised, fitted (cathode-current law, dashed Ig curves); the sheet is triode-connected |
| S23 | JJ Electronic, 5Y3S characteristic sheets | rectifier reference (Fig. 12) | **Fig. 12** | pixel-digitised; forward law fitted to the right panel, the left (regulation) panel used as a held-out check (`tools/validate_supply.cpp`) |
| S24 | JJ Electronic, ECC83S / 12AX7 characteristic sheets | modern 12AX7 example (Fig. 10) | **Fig. 10** | pixel-digitised and fitted |
| S25 | Karjalainen and Pakarinen, Wave Digital Simulation of a Vacuum-Tube Amplifier (ICASSP 2006) | WDF method (Sec. 22, 31) | **No** | not used: the nodal reference was chosen (Sec. 20, 21) |
| S26 | Pakarinen, Tikander and Karjalainen, Wave Digital Modeling of the Output Chain of a Vacuum-Tube Amplifier (DAFx 2009) | coupled output stage, transformer and speaker (Sec. 22) | **No** | the coupled output chain is solved simultaneously in the nodal model |
| S27 | Darabundit, Roosenburg and Smith, Neural Net Tube Models for Wave Digital Filters (DAFx20in22) | learned multiport device in a WDF (Sec. 22) | **No** | not used (no training data exists for this target) |
| S28 | Wright, Damskagg, Juvela and Valimaki, Real-Time Guitar Amplifier Emulation with Deep Learning (2020) | learned models (Sec. 22) | **No** | not used |
| S29 | Carson et al., Sample Rate Independent Recurrent Neural Networks for Audio Effects Processing (2024) | rate handling of recurrent models (Sec. 23) | **No** | not applicable: the circuit model keeps physical time constants in seconds and regenerates its coefficients per rate |
| S30 | Juvela et al., End-to-End Amp Modeling: From Data to Controllable Guitar Amplifier Models (ICASSP 2023) | controllable learned models (Sec. 22) | **No** | not used |
| S31 | Angelo Farina, Simultaneous Measurement of Impulse Response and Distortion with a Swept-Sine Technique (AES 108, 2000) | exponential-sweep method for a linear response and harmonic orders (Sec. 25) | **No** (method implemented from the published description) | `src/analysis/Metrics.h` (`makeLogSweep`, `deconvolveSweep`, `harmonicIR`), verified against an analytic cubic nonlinearity in `tools/validate_metrics.cpp`; used by `cd57_capture_matrix` and `cd57_compare` |
| S32 | Yeh, Automated Physical Modeling of Nonlinear Audio Circuits for Real-Time Audio Effects, Part II (IEEE TASLP 20(4), 2012) | schematic-derived nonlinear modelling reference (Sec. 21, 31) | **No** | the modified-nodal-analysis formulation of `src/dsp/Network.h` |

## Figures embedded in the dossier (Sec. 32, "Figure source URLs")

| Dossier figure | Asset | Provenance | Use here |
|---|---|---|---|
| 1-6 | cover, rear, panel, caps, speaker, tubes | Fender product photographs (S01) | identification only; **not** redistributed. The plugin UI follows the panel and jack geometry of Fig. 3 |
| 7, 8 | schematic, layout | Robinette's annotated 5E3 drawings (S05) | the netlist's source drawing (Fig. 7); Fig. 8 for physical routing |
| 9-12 | 12ay7_curves, 12ax7_curves, 6v6_curves, 5y3_curves | JJ Electronic sheets (S21-S24) | the four tube datasets; redistributed in `data/tubes/source_images/` for the digitiser (see the licensing note in the README) |
| 13-16 | force_factor, stiffness, compression, thermal | Klippel generic illustrations (S13, S14) | interpretation only; **not** this driver |
| 17 | dipole | Linkwitz ideal-dipole polar plot (S15) | interpretation only |

## What would change the model if a source became readable

The sources that would most tighten the model, in the order the sensitivity study (`docs/SENSITIVITY_REPORT.md`) ranks the unknowns they bear on: **S02** (speaker-jack and combined-impedance rules; the manual's
voltage and fuse values), **S04** (the earlier service drawing's DC test-point voltages as a cross-check on the supply and bias, *labelled HISTORICAL and never assigned to the modern unit automatically*),
**S19 / S20** (RCA 6V6GT and 5Y3GT curves: the screen partition and the rectifier law currently rest on one external rating point and on the JJ sheets), **S10** (transformer winding data). None of them
establishes the modern specimen; the dossier's own conclusion (Sec. 01, 33) stands: this is a documented approximation until a specimen is measured.
