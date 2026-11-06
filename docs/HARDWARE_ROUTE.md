# The route from this documented approximation to a measured match (dossier Sec. 01, 04, 24-30, 33)

The dossier is explicit (pp. 1, 2, 62): public documents do **not** establish a one-to-one digital model. What it does ask for is "a circuit-informed engineering foundation and an
explicit route to a measured match". This file is that route, step by step, with the tool that performs each step. Everything up to step 1 exists and is tested; steps 1-7 need one
physical amplifier and a measurement chain, which is exactly the part no software can replace.

| Dossier question before a 1:1 claim (Sec. 33) | Today | What closes it |
|---|---|---|
| Which physical unit? | not applicable: no specimen | step 1 |
| Which boundary? | **defined**: input-jack voltage -> speaker-terminal voltage *and current*; acoustic stage separate and provisional | step 6 for the acoustic boundary |
| Are controls calibrated? | **no**: pot laws are assumed (selectable); a measured-map route exists | step 2 |
| Is the netlist verified? | verified against the *historical* Fig. 7 (independent re-transcription, graph-level, 14 injected faults all caught); **not** against the modern factory circuit | step 1 + 3 |
| Are missing parameters explicit? | **yes**: every manifest entry has status, branch, source revision, serial range, interval; nothing unlabelled | - |
| Does the model preserve memory? | **yes** (rails, capacitor charge, bias, flux, coil heat, across blocks and oversampling changes); bias/supply/coupling recovery tests pass *as model behaviour* | step 5 compares it with the amplifier |
| Is the acoustic split coherent? | **yes** algebraically (route 1 vs route 2 identity, synthetic H) | step 6 with a measured H |
| Is accuracy independently tested? | **no**: nothing has been compared with hardware | steps 4-5 |
| Is realtime behaviour sound? | **yes** (zero allocations measured, bounded solver, state across blocks) | - |
| Can another engineer reproduce it? | **yes**: sources catalogued (`SOURCE_CATALOGUE.md`), data, code, reports, one command per step | - |

## Step 1 - identify the specimen and freeze the target (Sec. 04, 29 milestone A)

Record serial, revision, market voltage, **complete tube markings**, speaker label, wiring photographs, ambient and warm-up (`data/calibration/calibration_template.json`, the `specimen` block of the delta
below). Preserve ambiguous markings as photographs. Do not infer tube characteristics from the family name (Sec. 02). Obtain the correct factory service revision if one exists and keep any difference from the
historical netlist as a **versioned delta** (Sec. 04), not as an edit of the nominal netlist.

## Step 2 - measure the passive and DC unknowns and enter them as a specimen delta (Sec. 08, 11, 12, 17, 28)

`data/parameters/specimen_delta_template.json` lists every parameter the model reads with a hint for how to measure it (pot maps at 11 knob angles, pot total resistance, transformer ratio / DC
resistances / magnetising and leakage inductance, power-transformer secondary at a stated mains voltage, speaker Re / Le / fs / Q's). Fill in `measured` blocks (value, unit equal to the entry's, instrument,
instrument range, lead compensation, date); leave the rest `null` - **null changes nothing**.

```bash
cd57_manifest merge data/parameters/cd57_nominal.json my_specimen_delta.json merged.json    # refuses unknown keys, unit mismatches, missing instrument/date, another base profile
cd57_manifest check merged.json                                                                # shows each measured value BESIDE its nominal one and says whether it is inside the assumed interval
```

The merge keeps the nominal value and its status in place, adds the measured block next to it, and labels the entry branch `specimen` (Sec. 04: "store a measured value alongside its nominal marking").
A measured pot map replaces the assumed taper for that pot only (Sec. 08: "the default should be a measured mapping when available"). Render with `--manifest merged.json`
(`cd57_render`, `cd57_capture_matrix`), or embed it in a build with `-DCD57_MANIFEST=merged.json`.

Note: tube characteristics, the output-transformer saturation law and the speaker's large-signal behaviour have no manifest entry because they are **curves, not numbers**; they need the captures of step 3 and
code, not a delta (see `SOURCE_CATALOGUE.md`, Sec. 15-16 and 29 "what should wait for data").

## Step 3 - calibrate the boundary (Sec. 24, 27 experiment 1)

Measure the reamp/interface chain (gain, frequency response, source impedance) and the capture chain (converter, attenuation, delay) and record them in the calibration file. One fixed calibration - never a gain refitted per clip
(Sec. 26). Safety: internal nodes are dangerous even after power-off; speaker output needs a rated load and rated attenuation and never goes directly into an interface input (Sec. 24).

## Step 4 - run the capture programme (Sec. 25)

`cd57_capture_matrix --out model_dir --set all --wav` renders the model through the programme the hardware has to go through: 4 single-input modes x 27 knob combinations (low/mid/high = 2/6/10 on the printed scale)
x 3 levels (clean = 5 mV peak at the jack, **onset** = the input peak at which the 1 kHz THD at the speaker terminals reaches 1 %, searched per condition, **driven** = onset + 12 dB) = **324 captures**, plus
72 endpoint captures (every knob at 0 and 12), 33 **jack-state** captures (both jacks of one channel, one input per channel with a split or a terminated plug, the channel-jump jumper, no source: Sec. 06 table) and
36 **held-out** captures at intermediate knob values with a synthetic guitar passage (Sec. 22, 25). The `--wav` output contains, per capture, the **stimulus** (`raw/<id>_jack_volts.wav`, in volts at the jack: play it
through the calibrated reamp chain, unchanged) and the model's terminal voltage and current. Record the amplifier the same way into the same layout:

```
<dir>/metadata/<id>.json    Sec. 30 minimum capture metadata, validated by data/capture/capture_metadata.schema.json (set "synthetic": false; fill the nulls: serial, tube markings, speaker label, knob angles, ADC/DAC, attenuation, microphone pose, ambient)
<dir>/raw/<id>_jack_volts.wav, <id>_terminal_volts.wav, <id>_terminal_amps.wav     (float32, physical units: volts / amps)
<dir>/processed/<id>.json   optional; "onset": {"input_peak_V": ..., "reached": ...} enables the clipping-onset criterion
```

Repeat clean and driven conditions at least three times first (Sec. 25 "Repeatability") to know the amplifier's own floor before setting final limits.

## Step 5 - compare, with the Sec. 26 metrics (Sec. 26, 27 experiments 1-3, 6)

```bash
cd57_compare --ref hardware_dir --test model_dir --report comparison.md --json comparison.json --csv comparison.csv
```

One integer fixed delay is estimated (or given with `--delay`) for the whole comparison and removed from every model waveform; nothing is fitted per clip. The verdict table applies the dossier's PROPOSED limits: clean
response within 0.5 dB over 80 Hz-8 kHz (where the SNR gate of 20 dB is met), clipping onset within 0.5 dB, harmonics above -60 dBc within 1 dB, intermodulation within 2 dB, burst envelope within 5 %, recovery landmarks
within 10 %, held-out NRMSE <= 5 %, with phase and group delay reported independently. Each metric has been verified against an analytic answer (`validate_metrics`, 34 checks), and the pipeline has been shown to
**fail** a perturbed model and **pass** an identical one (`capture_compare_selfcheck`), so a failing hardware comparison is informative and a passing one is not an artefact. The model-side numbers of this repository
are the regression reference: `data/reference/capture_matrix_summary.csv`.

When a criterion fails, `docs/SENSITIVITY_REPORT.md` says which unknown parameters can move that feature by more than the tolerance (e.g. the power-transformer secondary and the mains voltage dominate onset, power and
harmonic balance; the output-transformer primary inductance and the speaker fs dominate the bass), i.e. what to measure next - before any Monte Carlo (Sec. 13).

## Step 6 - the acoustic boundary (Sec. 17-19, 27 experiments 4, 5, 7)

Measure the speaker's complex impedance (terminal voltage and current sweep), then voltage-to-microphone impulse responses at documented positions (radial, azimuth, distance, microphone model and orientation: Sec. 19). Load
a measured IR in the plugin (route 1, applied to the terminal voltage) **or** use the hybrid route (the IR factored onto the model's own cone motion with a regularised residual so the cone is not counted twice). Do not
apply a full IR to a model that already contains the cone (Sec. 18, 27 "duplication traps").

## Step 7 - release evidence (Sec. 29 milestone H)

Held-out passages and intermediate control values, blind level-matched listening with predeclared criteria, parameter provenance, reproducible builds. "Sounds like a tweed amp" is not evidence (Sec. 29).

## What cannot be done without hardware

The same list the compliance audit keeps (`docs/DOSSIER_COMPLIANCE.md` Sec. 4): the specimen's identity and the modern as-built schematic; measured pot maps and total resistances; installed-tube curves, mismatch, grid-current onset and screen
partition; supply- and output-transformer parameters; the OEM speaker's complex impedance, large-signal Bl(x), Kms(x), L(x) and thermal behaviour; the cabinet's internal geometry and compliance; the microphone/room chain; the calibration of the
reamp/interface chain; every comparison against the amplifier; component tolerance statistics. The software side - metrics, harness, reference dataset, delta mechanism, schema, comparison, sensitivity ranking - is complete and tested.
