# Speaker impedance measurement and calibration (specimen only)

**Status: measurement-ready tooling, not an OEM characterisation.** The modern Fender ’57 Custom Deluxe uses an Eminence Special Design 12-inch 8-ohm alnico speaker identified in Fender literature as **PN 7708599000**. We do **not** possess an authenticated complex-impedance sweep, T/S parameter set, cabinet response, or microphone capture of that exact driver. The existing `SpeakerTS` numbers are provisional assumptions. Do not label a synthetic test curve, a generic alnico driver, or an in-cabinet sweep as measurements of that Eminence part.

The script `scripts/fit_speaker_impedance.py` provides a **reproducible path to fit a measured specimen's small-signal free-air electrical impedance** and generate five manifest-compatible `measured` entries. It changes nothing about the nominal profile by itself. The measured specimen must be identified by its physical label and serial/fixture records. **No measurement data are supplied by this repository.**

## Scope and equations

The present simulator defines its linear electrical speaker equivalent using `SpeakerTS` (`src/dsp/AmpCircuit.h`). For a low-level, free-air complex terminal impedance sweep with angular frequency `ω = 2πf`, the equivalent is:

```
Z(f) = Re + j ω Le + (Re Qms / Qes) / [1 + j Qms (f/fs − fs/f)]
```

The five independently fitted parameters are `speaker_Re` (Ω), `speaker_Le` (H), `speaker_fs` (Hz), `speaker_Qms` (unitless), and `speaker_Qes` (unitless). The fitted values describe **the model's equivalent** at the measured signal level and temperature; fitted `Re` is not independently a four-wire *DC* resistance reading. A separate low-level four-wire DC resistance reading is desirable as a cross-check. Do not call the fitted inductance a complete nonlinear/eddy-current inductance model.

**A single electrical free-air sweep cannot independently determine moving mass `Mms` or piston area `Sd`.** Those entries remain `null`; likewise, it cannot establish force factor `Bl(x)`, suspension `Kms(x)`, displacement/thermal nonlinearity, speaker frequency response, cone breakup, enclosure response, or a voltage-to-microphone IR. An impedance-fit pass is *not* evidence of audibly accurate Fender speaker/cabinet tone. Physical added-mass/known-volume tests, acoustic measurements, or dedicated nonlinear transducer tests are needed for those separately.

## Collect an authentic complex sweep

1. Identify the individual **speaker specimen**: read and photograph the physical manufacturer/model, part code, date code, serial/identifying marks and any relevant revision. Do not assume that a speaker in an amplifier is the documented stock part merely because the amplifier is labelled ’57 Custom Deluxe. Preserve the unedited photographs separately.
2. **Disconnect the speaker from the amplifier and measure it as a passive component**, suspended in free air clear of nearby reflective surfaces and obstructions, without an enclosure affecting its resonance. Do not work inside a tube amplifier unless you are trained and equipped to handle lethal stored capacitor voltages; this workflow never requires opening or powering the amplifier chassis.
3. Use a purpose-built small-signal impedance measurement instrument or a correctly verified two-channel voltage/current impedance jig. Measure true **complex terminal impedance** `Z(f) = V_terminal(f)/I_terminal(f)`, including sign-preserving phase, using a consistently defined polarity. A magnitude-only impedance plot, a DC multimeter value, or a cabinet/mic response is not an acceptable substitute.
4. State the excitation waveform/voltage or current, measured frequency range, instrument/ADC or jig model, measurement ranges, physical lead arrangement, ambient/voice-coil temperature, and calibration/phase/lead compensation. Perform and document relevant open/short/known-load and phase checks against known impedances over the full sweep. Check instrument amplitude and phase limits. Keep the driver cool and use sufficiently low signal to remain in the linear regime; repeat at a second modest signal level to check reproducibility.
5. Provide at least 32 distinct frequencies spanning a factor of at least 12, including the motional-resonance peak and well above it. The fitter additionally requires data **below 0.8× and above 4× the detected real-impedance peak**, to help constrain DC-like and inductive terms. A dense logarithmic sweep, with additional samples around resonance, is preferable. Preserve raw instrument files and export one CSV with calibrated complex real and imaginary impedance (not `|Z|` and phase).

Exact CSV column names (extra ignored metadata may be archived separately):

```csv
frequency_hz,real_ohm,imag_ohm
20.0,6.5,-1.2
25.0,6.7,-1.8
```

The two CSV rows above are **illustrative, incomplete fictional values**, not an actual sweep or Fender driver values. The `imag_ohm` sign is physically meaningful; don't drop it or convert it to absolute magnitude.

## Fit to a specimen delta

Python 3 plus NumPy and SciPy are needed. From the repository root:

```bash
python3 -m pip install numpy scipy
python3 scripts/fit_speaker_impedance.py \
  --csv /path/to/actual-specimen-free-air-complex.csv \
  --delta /path/to/specimen-speaker-delta.json \
  --report /path/to/specimen-speaker-fit-report.json \
  --mounting free_air \
  --specimen-serial "ACTUAL-SPECIMEN-ID" \
  --speaker-label "EXACT MARKINGS OBSERVED ON SPEAKER" \
  --instrument "INSTRUMENT MODEL / ADC AND CURRENT SENSING CHAIN" \
  --instrument-range "VOLTAGE OR CURRENT RANGE, LEVEL, FREQUENCY SETTINGS" \
  --calibration-method "OPEN/SHORT/REFERENCE, LEAD AND PHASE COMPENSATION DETAILS" \
  --date 2026-11-06 \
  --temperature-c 22.0
```

**Replace every placeholder above with an actual, traceable observation.** By default, outputs are not overwritten; provide fresh paths for a different specimen or repeat, or pass `--overwrite` explicitly for an intentional rerun. Neither the CSV nor specimen template can be used as an output destination. If you use `--template`, pass a correctly structured `cd57-specimen-delta-v1` JSON template with *all* `measured` entries initially `null`; this fitter does not silently propagate prior unrelated measurements.

For a substantially stronger reproducibility check, record a **second actual sweep** of the same suspended speaker, preferably after re-seating the measurement leads and independently checking the fixture against known resistors. Provide that distinct file via:

```bash
# Append these arguments to the command above:
--validation-csv /path/to/second-actual-free-air-sweep.csv \
--max-repeat-rms-percent 5
```

The second sweep is **not re-fitted**: its complex impedance is compared against the first sweep's *fixed* estimated parameters, with a default rejection gate of 5% complex relative RMS and a frequency-span check. The repeat-file SHA-256, comparison metrics and fixed-parameter flag are saved with the report; the delta also records the repeat hash/error. A repeat using the same instrument/fixture may share the **same systematic calibration error** as the first. This test improves reproducibility evidence, but it is not an independent reference validation or proof of OEM accuracy. For genuine independent verification, change or independently calibrate the measurement chain, verify actual speaker identity, and examine frequency-resolved residuals.

When both a delta and report are requested, the tool stages both JSON files before publishing either. Existing output bytes are retained for rollback if a publication step fails. Each individual rename is atomic, but **two different files cannot be committed atomically as one filesystem transaction**, and a power loss or concurrent external modification can still interrupt the pair. Archive raw captures and verify output hashes after each measured-specimen run.

The generated delta preserves every nominal entry from `data/parameters/specimen_delta_template.json`, adds measured blocks only for the five fit parameters, and leaves the unknown speaker mass, piston area, acoustic, thermal, and all other entries unmeasured. Each measured block records its value/unit, instrument, instrument range, lead/phase compensation, date, mounting, temperature, full input-file SHA-256, held-out residual and approximate local 95% fit interval. The report also records input range, training/holdout errors, parameter bounds/identifiability check and limitations.

After independent review of the data/provenance, merge it with the nominal manifest:

```bash
# Build first using the repository CMake instructions, then use the resulting cd57_manifest tool:
cd57_manifest merge data/parameters/cd57_nominal.json \
  /path/to/specimen-speaker-delta.json /path/to/merged-specimen-manifest.json
```

Pass the merged manifest to offline renders with `--manifest`, or build the plugin with `-DCD57_MANIFEST=/path/to/merged-specimen-manifest.json`. This is a **specimen-specific calibration**, never a silent replacement of nominal assumed values for all users.

## Automated acceptance and interpretation

- The fitter rejects missing phase/imaginary component, duplicate/nonfinite frequencies, non-positive real impedance, too few points, too narrow a sweep, an inadequately bracketed resonance peak, missing provenance, cabinet mounting, and parameters on imposed physical bounds.
- It fits five positive parameters with a robust complex residual, withholding every seventh frequency sample as a deterministic **held-out validation set**. The default reject threshold is 10% held-out complex relative RMS error (`--max-holdout-rms-percent` can tighten it). Because these samples come from the **same sweep**, interleaved holdout points do *not* guard against systematic fixture bias or correlated errors. A fit that passes this broad gate may still be **too poor for scientific comparison**: ideally acquire cleaner data, examine residuals by frequency and compare independent repeat measurements before adopting a profile. Do not interpret 10% as a precision target.
- A local Jacobian condition-number check rejects badly identifiable fits. Local 95% intervals in the report are *approximate statistical intervals only*; they do not include instrument accuracy, fixture residuals, systematic distortion, environmental changes, parameter correlation beyond linearisation, or inter-unit variation. The fit should be repeated at multiple low levels and temperatures when possible.
- A synthetic-data unit test can establish that the algorithm reconstructs its own assumed equivalent. **It cannot validate the actual Eminence driver model.** To run synthetic and refusal tests: `python3 scripts/test_fit_speaker_impedance.py -v`.

## Remaining work before an OEM-fidelity claim

1. Obtain a **verified actual PN 7708599000** sample and repeatable free-air complex sweeps under logged conditions, ideally for multiple samples. Distinguish verified OEM data from a generic replacement.
2. Obtain an independent `Re` cold four-wire check; evaluate eddy-current and level-dependent residuals and improve the inductance/nonlinear model only if the data require it.
3. Determine `Mms`, `Sd`, displacement response and electro-mechanical behavior with independently documented measurements; do not estimate these from one unperturbed impedance curve.
4. Measure the **complete cabinet + driver + microphone** amplitude/phase response or suitable IR with documented position and room treatment; keep this acoustic calibration separate from terminal electrical impedance.
5. Compare measured vs predicted impedance, power-amp load interaction, dynamic level-dependent tests, and blind, level-matched recorded A/B listening tests. Preserve raw captures, scripts, deltas, and pass/fail evidence for every specimen. Without those data, the audible match to the real Fender speaker remains unknown.
