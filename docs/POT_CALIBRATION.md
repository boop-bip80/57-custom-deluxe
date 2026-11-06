# Physically measuring the modern volume-pot tapers

**Status: measurement-ready, not an authenticated Fender taper.** Fender advertises improved volume-pot tapers for both channels of the modern '57 Custom Deluxe, but public specifications give no rotation-to-resistance curves. The repository's Audio 15% and Audio 10% options are **assumptions**, not original-specification calibrations. A specimen-specific measured curve can supersede the assumption for **each pot independently**.

## Obtain actual data

1. Identify an authentic amplifier specimen and record its serial and revision. Have a qualified amplifier technician work on the hardware. **Unplugging the amplifier is not sufficient protection**: tube amplifiers can retain dangerous high voltage in filter capacitors. Work only after professionally verified discharge and isolation.
2. With the relevant potentiometer electronically isolated from parallel paths in the amplifier, measure its total resistance between its two outer lugs. Record Instrument and Mic separately; optionally Tone too. Merely probing the wiper while the pot remains in circuit gives false resistance readings.
3. Identify the low/grounded end for each volume pot. For Tone, the project's `PotMap` expects the end feeding the 5 nF tone capacitor, which is **not automatically the same as ground**. The direction of measurement is consequential and can reverse the taper.
4. Measure resistance between the wiper and that identified end at **11 reproducible physical knob positions**: 0, 0.1, …, 1.0 (including both mechanical stops), in ohms. Do not estimate readings from knob labels or advertising. Record the meter model, range/accuracy, lead compensation, date and specimen identification.
5. Save a UTF-8 CSV with the following header and 11 rows (numbers below are *synthetic demonstration values*, not Fender data):

```csv
rotation,instrument_ohm,mic_ohm
0,0,0
0.1,9000,25000
0.2,22000,65000
0.3,42000,130000
0.4,76000,220000
0.5,140000,340000
0.6,240000,490000
0.7,390000,640000
0.8,580000,790000
0.9,790000,910000
1.0,1000000,1000000
```

An optional `tone_ohm` column is accepted when you also pass `--tone-total-ohm`.

## Generate an auditable specimen delta

```bash
python3 scripts/fit_pot_tapers.py \
  --csv /path/to/measured-pots.csv \
  --delta /path/to/specimen-pots.json \
  --instrument-total-ohm 1000000 \
  --mic-total-ohm 1000000 \
  --specimen-serial ACTUAL-SERIAL-HERE \
  --instrument 'meter make/model' \
  --instrument-range 'meter range, resolution, excitation' \
  --lead-compensation 'lead/contact compensation method' \
  --date 2026-11-06
```

All values, equipment details, serial and date in this command are **placeholders**. Replace with real observations. The generated `cd57-specimen-delta-v1` JSON contains eleven normalized `alpha` values for each physical volume pot, plus independent measured total resistance and the input CSV SHA-256. The tool refuses missing/extra rows, out-of-range data, nonmonotonic curves, missing provenance, invalid calendar dates and accidental file overwrites. Its ±3% endpoint sanity gate allows small contact resistance but is **not an OEM tolerance**.

Integrate with the project's existing merger:

```bash
build/cd57_manifest merge \
  data/parameters/cd57_nominal.json \
  /path/to/specimen-pots.json \
  /path/to/specimen-merged.json

cmake -S . -B build-specimen -DCD57_MANIFEST=/path/to/specimen-merged.json
cmake --build build-specimen
```

The manifest accepts separate `volume_pot_taper_instrument` and `volume_pot_taper_mic` measured maps. They supersede the common assumed taper choice while retaining its legacy automation/preset behavior for unmeasured builds.

**Model limitation:** The engine currently has one shared `potTotal` resistance. This tool records each measured pot's actual total in provenance but intentionally does **not** silently substitute differing measured totals into that shared engine setting. To achieve specimen-specific absolute resistance parity when the two channels differ, the DSP must first gain independent total-resistance fields; do not claim such parity from this tool alone.

## Quality checks

```bash
python3 scripts/test_fit_pot_tapers.py
```

These synthetic/unit tests establish input, physical orientation and manifest format checks. They cannot demonstrate a sonic match to a Fender amplifier; full verification requires an authentic specimen capture of channel responses versus measured knob positions.

References: [Fender '57 Custom Deluxe product specifications](https://www.fender.com/products/57-custom-deluxe) (revised volume tapers) and [Fender owner manual](https://www.fmicassets.com/Damroot/Original/10001/%2757%20Custom%20Deluxe%20manual%20MULTI%207708607000a.pdf) (channel controls and operating guidance).
