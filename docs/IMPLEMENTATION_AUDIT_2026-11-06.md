# Modern '57 Custom Deluxe: hardware-fidelity issue closure (6 November 2026)

**Scope:** Four high/medium-priority discrepancies between this **documented approximation** and Fender's modern handwired '57 Custom Deluxe. This is an engineering status report, *not* a numerical accuracy certificate or a claim that a Fender specimen was measured.

## Primary sources and scope of inference

1. Fender, [modern '57 Custom Deluxe owner's manual](https://www.fmicassets.com/Damroot/Original/10001/%2757%20Custom%20Deluxe%20manual%20MULTI%207708607000a.pdf), control panel and speaker jack instructions. Establishes **the presence and operation** of separate Standby and Power switches, the warm-up advice, and permitted speaker arrangements. It **does not** establish the modern standby switch's position in the internal high-voltage circuit.
2. Fender, [modern product description and technical specifications](https://www.fender.com/products/57-custom-deluxe). Establishes the single 12-inch, 8 Ω Eminence Special Design alnico speaker and revised Instrument/Mic volume-pot tapers. **Does not publish their electrical/physical curves.**
3. Eminence, [OEM speaker information](https://eminence.com/a/faq). Custom OEM drivers may have proprietary specifications; a catalog model's curve must not be assigned to the installed Fender OEM driver without authentication.
4. KLIPPEL, [measurement service](https://klippel.de/support/measurement-service.html) and [electrical impedance measurement fundamentals](https://www.klippel.de/know-how/measurements/transducer-parameters/electrical-impedance.html). Clarifies the distinction between complex small-signal impedance and nonlinear speaker characterization.

The historical 5E3 and older '57 Deluxe schematics are valuable reference topologies **but are not authenticated modern PR 728 wiring evidence**. A more specific supply switch claim requires modern service drawings or inspection of a particular serial-numbered amplifier.

## Change/evidence matrix

| Priority / issue | Implemented in the model and plugin | Validation target | Still unknown |
|---|---|---|---|
| **High — modern Standby missing** | New independent `standbyOn` bool (default **true** for old saved presets), a second faceplate switch, and a **provisional HV-excitation gate**. POWER controls heater/rectifier emission. STANDBY lets capacitor/rail states evolve electrically instead of muting the final buffer. State copied during oversampling-rate transfer. | C++ realtime/supply-state checks of HV interruption, warm emission, discharge, return from standby and state transfer; JUCE processor state round-trip and legacy-state check. | Exact physical switch contact position, transition/discharge times, effects on the modern Fender chassis, true 60-second heater warm-up curve. |
| **High — OEM speaker uncharacterized** | Preserve explicitly **assumed** generic electro-mechanical defaults. Add calibrated complex free-air impedance fitter to extract five identifiable small-signal parameters, export a schema-compatible specimen delta and report held-out/repeat residuals. | Synthetic known-parameter reconstruction, nonphysical/uncalibrated input refusal, provenance, held-out points and independent repeat when supplied. | Actual OEM PN 7708599000 impedance curve, Mms/Sd, true large-signal Bl(x), Kms(x), inductance/compression, cabinet/room/mic IR. |
| **Medium — unsupported speaker-load choices presented as ordinary** | Keep the original five-choice **plugin parameter** unchanged by design, including its normalized automation indices `0–4`; label index 1 internal+extension **NOT recommended by Fender** and dummy modes bench-only. Add a **separate External speaker wiring** plugin parameter (None, single external 8 Ω, dual external 16 Ω); internally, append core mode indices `5–6`, with internal OEM coil disconnected. Two 16 Ω nominal resistive branches in parallel. | **Host regression pending:** legacy normalized parameter values, branch resistance, current, solver stability, heat only when internal coil connected, switch transitions, newly added external selector's state. | Electrical and acoustic response of any actual external driver/cabinet; nominal resistor proxies are explicitly provisional. |
| **Medium — modern volume tapers guessed** | Keep assumed Audio15/Audio10/Linear fallback explicit. New importer converts 11 physical resistance samples from separately measured Instrument and Mic pots (optional Tone) into independent specimen-delta curves. Plugin displays actual manifest-derived measured/assumed status. | Endpoint, monotonic, finite, range and metadata validation; independent pot-map precedence over assumed selector; specimen-delta merge. | Real modern Fender taper curves, lot variation, physical endpoint/contact behavior until a specimen is measured. The engine still uses a shared total track resistance; differing per-pot measured totals are provenance only until that circuit field is separated. |

## Reproducible verification gates

```bash
cmake -S . -B build -G Ninja -DCD57_BUILD_PLUGIN=OFF
cmake --build build -j2
ctest --test-dir build --output-on-failure

# Synthetic model and data-validation tests, NOT tests against a real Fender amp:
python3 scripts/test_fit_speaker_impedance.py       # requires numpy/scipy
python3 scripts/test_fit_pot_tapers.py                # standard-library only

# After obtaining actual, provenance-linked measurements, merge a specimen delta:
build/cd57_manifest merge data/parameters/cd57_nominal.json measured_specimen_delta.json measured_merged.json
build/cd57_manifest check measured_merged.json

# Embed the measured specimen in a separate, reproducible plugin build:
cmake -S . -B build-specimen -DCD57_MANIFEST=/absolute/path/to/measured_merged.json
cmake --build build-specimen --target CustomDeluxe57_VST3 CustomDeluxe57_Standalone
```

`ctest` verifies implementation invariants and synthetic fixture agreement. It does **not** establish absolute physical accuracy. The JUCE plugin processor/host tests and cross-platform VST3/Standalone builds are CI gates, separate from the core-only command above.

## Hardware-data acceptance before stronger claims

1. **Identify the actual speaker and chassis.** Photograph Fender/Eminence label **PN 7708599000**, record serial/revision, market, wiring and installed tubes. Confirm the target is the **modern** '57 Custom Deluxe, not an earlier Deluxe reissue.
2. **Measure pot tracks individually.** Each pot isolated from parallel components; calibrated rotation, end-to-wiper resistance and total resistance at eleven positions, repeated with stated instrument/uncertainty/lead compensation. Keep Instrument and Mic curves independent.
3. **Calibrate electrical speaker impedance.** Measure complex (Z(f)=V(f)/I(f)) at low drive, free-air mounting for free-air model parameters; record temperature, test level and fixture phase compensation. Hold out samples and run a truly separate repeat sweep. Retain frequency-resolved errors. *In-cabinet* sweeps are also valuable, but do not mislabel them as free-air T/S measurements.
4. **Record independent cabinet/microphone captures.** Full voltage-to-mic transfer (mic model, pose, distance, azimuth, room and gain); isolate nonlinear response and thermal compression before claiming anything about dynamic speaker parity.
5. **Verify modern standby electronics.** Obtain a genuine modern schematic or qualified circuit-tracing measurements. Compare actual switching transition, B+ decay, emission/heater behavior and recovery against the model; revise the HV-gate topology as necessary.
6. **Use preregistered comparisons.** Reamp interface calibration, fixed delay, held-out control positions/guitar passages, magnitude/phase, onset, THD/IMD, transient/sag/recovery and blind level-matched listening. The existing capture/compare harness is described in [`HARDWARE_ROUTE.md`](HARDWARE_ROUTE.md).

**Release-status boundary:** The implementation can be described as *modern-control-complete at a documented behavioral level, with measurement-ready calibration tooling*. It **cannot** be described as 97%, 99%, or 1:1 accurate to a Fender '57 Custom Deluxe until authenticated hardware recordings and parameter measurements meet independent acceptance criteria.
