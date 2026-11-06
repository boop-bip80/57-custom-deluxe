#!/usr/bin/env python3
"""Generate data/parameters/cd57_nominal.json -- the parameter manifest of dossier Sec. 30.

status vocabulary (dossier Sec. 01 evidence labels + Sec. 28 rule "explicit assumed value with a sensitivity interval"):
  manufacturer_documented  CONFIRMED, published for the modern target (S01/S02)
  historical_nominal       HISTORICAL 5E3 value (S03/S05/S10), not verified on the modern amp
  derived                  DERIVED arithmetic from documented values
  candidate_fit            fitted to digitised public data (scripts/fit_tubes.py)
  proposed                 engineering choice / test requirement, not a Fender specification
  assumed                  public value NOT located -> explicit assumption carrying an interval [lo, hi]
  unknown                  required target data not located; value is null
"""
import json, os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
fits = json.load(open(os.path.join(ROOT, "data/tubes/fitted_models.json")))

# Source revisions, exactly as the dossier's source catalogue (Sec. 32) states them.  A source the project could not fetch is still cited with the
# revision the DOSSIER gives; docs/SOURCE_CATALOGUE.md says which ones were actually available to this implementation.
REVISION = {
    "S01": "Fender product page and photographs, retrieved 5 Nov 2026",
    "S02": "Owner's Manual PN 7708607000 Rev. A (c) 2016",
    "S03": "original Deluxe 5E3 schematic, archive copy (undated)",
    "S04": "service drawing 0073201000 Rev. B, 17 Nov 2006 ('57 Deluxe / Fender 57; NOT the modern Custom model)",
    "S05": "Rob Robinette, How the Fender 5E3 Deluxe Works (annotated schematic and layout, undated web page)",
    "S10": "Mercury Magnetics FTDO-59 product page (historical 5E3 replacement, 8 kOhm : 8 Ohm)",
    "S11": "Eminence speaker FAQ (OEM identification)",
    "S18": "RCA 12AX7 datasheet, archive copy",
    "S19": "6V6GT / 5V6GT datasheet, Valve Museum archive",
    "S21": "JJ Electronic 12AY7 characteristic sheet, retrieved 5 Nov 2026",
    "S22": "JJ Electronic 6V6S characteristic sheet, retrieved 5 Nov 2026",
    "S23": "JJ Electronic 5Y3S characteristic sheet, retrieved 5 Nov 2026",
    "S24": "JJ Electronic ECC83S / 12AX7 characteristic sheet, retrieved 5 Nov 2026",
}
def revision_of(source):
    if not source: return None
    ids = [x.strip() for x in source.split(",") if x.strip() in REVISION]
    return "; ".join(f"{i}: {REVISION[i]}" for i in ids) if ids else None

P = {}
def add(name, value, unit, status, source=None, interval=None, note=None, verified=None, branch=None):
    d = {"value": value, "unit": unit, "status": status}
    if source: d["source"] = source
    if interval: d["interval"] = interval
    if note: d["note"] = note
    if verified is not None: d["verified_on_modern_specimen"] = verified
    # Sec. 04 provenance: branch, source revision, serial range, uncertainty.  Nominal entries are not tied to a serial range (null).
    d["branch"] = branch or ("historical_5e3" if status == "historical_nominal" else "modern_nominal")
    d["source_revision"] = revision_of(source)
    d["serial_range"] = None
    d["uncertainty"] = {"kind": "sensitivity_interval", "lo": interval[0], "hi": interval[1]} if interval else None
    P[name] = d

# ---- CONFIRMED target specification (dossier Sec. 02)
add("rated_output_power", 12.5, "W", "manufacturer_documented", "S01,S02", note="into 8 ohm, <5% THD, 1 kHz; marketed as 12 W. Used as a calibration anchor and acceptance check.")
add("rated_thd", 0.05, "fraction", "manufacturer_documented", "S01,S02")
add("rated_thd_frequency", 1000.0, "Hz", "manufacturer_documented", "S01,S02")
add("mains_power_draw", 85.0, "W", "manufacturer_documented", "S01,S02")
add("speaker_nominal_impedance", 8.0, "ohm", "manufacturer_documented", "S02")
add("input1_impedance", 1.0e6, "ohm", "manufacturer_documented", "S01,S02", note="reproduced by the jack network (checked in validation)")
add("input2_impedance", 136.0e3, "ohm", "manufacturer_documented", "S01,S02", note="reproduced by the jack network (checked in validation)")
add("input2_attenuation", -6.0, "dB", "manufacturer_documented", "S01,S02", note="nominal")
add("tube_complement", "1x12AY7, 1x12AX7, 2x6V6, 1x5Y3", "-", "manufacturer_documented", "S01,S02", note="manual prints 6V6GC, product page 6V6; installed marking must be recorded per specimen")
add("speaker_part", "Eminence Special Design alnico 12in, Fender PN 7708599000", "-", "manufacturer_documented", "S01,S02,S11")
add("fuse_100_120V", "F2A L 250V", "-", "manufacturer_documented", "S02")
add("fuse_220_240V", "F1A L 250V", "-", "manufacturer_documented", "S02")

# ---- DERIVED
add("rated_output_rms_voltage", 10.0, "V", "derived", "S01", note="sqrt(12.5*8)")
add("rated_output_rms_current", 1.25, "A", "derived", "S01")
add("rated_output_peak_voltage_sine", 14.142, "V", "derived", "S01")

# ---- HISTORICAL circuit values are carried in data/netlist/cd57_nominal.net with per-element evidence tags
add("netlist", "data/netlist/cd57_nominal.net", "-", "historical_nominal", "S03,S05",
    note="topology and component values are the historical 5E3 baseline; modern service schematic not authenticated (dossier Sec. 04)", verified=False)
add("ot_turns_ratio_full_primary", 31.6227766, "ratio", "historical_nominal", "S10", note="DRIVES THE MODEL: full-primary : secondary turns ratio, sqrt(8000/8) (dossier Sec. 12 prints 31.6228); FTDO-59 8k:8 class, identity with the modern OEM part unproven", verified=False)

# ---- ASSUMED (explicit assumption + sensitivity interval, dossier Sec. 28)
add("pt_hv_half_winding_rms", 377.4, "V", "assumed", "calibration", [330.0, 400.0],
    note="supply transformer parameters UNKNOWN; value CALIBRATED (cd57_validate --calibrate-pt) so the interpolated 5% THD power into 8 ohm equals the documented 12.5 W for the JJ-fitted tube laws; the anchor weakly constrains this value (see the sensitivity table in the validation report)")
add("pt_series_resistance_per_half", 90.0, "ohm", "assumed", None, [50.0, 150.0], note="PT secondary + reflected primary resistance")
add("ot_primary_inductance_full", 25.5, "H", "assumed", None, [10.0, 60.0], note="DRIVES THE MODEL: small-signal magnetising inductance of the full primary, secondary open, = N_full^2 (P0 + Pair) with the netlist's Pair = 0.5 mH (so the netlist's P0 = 25 mH <-> 25.5 H); value is a guess, not a measurement")
add("ot_leakage_inductance_full_ref", 0.040, "H", "assumed", None, [0.01, 0.08], note="DRIVES THE MODEL: total leakage referred to the full primary = Lla + Llb + n^2 Lls; the netlist's split between windings is kept in proportion")
add("ot_dcr_primary_half", 125.0, "ohm", "assumed", None, [80.0, 200.0], note="DRIVES THE MODEL: DC resistance of one primary half (Ra = Rb)")
add("ot_dcr_secondary", 0.35, "ohm", "assumed", None, [0.2, 0.6], note="DRIVES THE MODEL")
add("ot_core_saturation_flux_per_secondary_turn", 0.060, "V*s", "assumed", None, [0.04, 0.09], note="no public saturation data (dossier Sec. 12)")
add("tube_interelectrode_capacitances", "12AX7/12AY7 1.5-1.7/1.5-1.7/0.46-0.5 pF, 6V6 9.5/0.7/7.5 pF", "pF", "assumed", None, note="datasheet-class values that are NOT contained in the dossier text (external, unverified offline)")
add("triode_grid_conduction_law", "I=k*(s*softplus((vgk-vc)/s))^1.5 (k=0.35 mA/V^1.5, vc=-0.5 V, s=0.15 V)", "-", "assumed", None, note="no grid-current data for the 12AY7/12AX7 in the dossier; onset and slope need measurements (dossier Sec. 16)")
add("volume_pot_taper", None, "fraction_vs_angle", "unknown", "S01", note="manufacturer advertises revised tapers; numerical law not located. The measured maps go in the three per-pot entries below")
POTUNIT = "fraction_vs_angle_11pt"
POTNOTE = "MEASURED MAP (dossier Sec. 08/28): electrical fraction alpha = R(grounded end -> wiper) / R_total at knob rotation 0, 0.1, ..., 1.0 (11 values, monotonic, 0..1), from a resistance-versus-angle measurement; a valid map replaces the assumed taper"
add("volume_pot_taper_instrument", None, POTUNIT, "unknown", "S01", note="Instrument volume. " + POTNOTE)
add("volume_pot_taper_mic", None, POTUNIT, "unknown", "S01", note="Mic volume. " + POTNOTE)
add("tone_pot_taper", None, POTUNIT, "unknown", "S01", note="Tone pot: alpha measured from the 5 nF end to the wiper. " + POTNOTE)
add("volume_pot_taper_provisional", "audio, alpha(0.5)=0.15", "-", "assumed", None, note="selectable in the plugin (audio-15 / audio-10 / linear); not claimed as the modern Fender taper")
add("tone_pot_taper_provisional", "linear", "-", "assumed")
add("pot_total_resistance", 1.0e6, "ohm", "historical_nominal", "S03", [0.8e6, 1.2e6])
add("pot_endpoint_contact_resistance", 20.0, "ohm", "proposed", None, note="documented small contact resistance instead of dividing by zero (dossier Sec. 08)")
add("speaker_Re", 6.4, "ohm", "assumed", None, [5.5, 7.2], note="exact-target value UNKNOWN (dossier manifest example lists null)")
add("speaker_Le", 0.7e-3, "H", "assumed", None, [0.3e-3, 1.2e-3])
add("speaker_fs", 98.0, "Hz", "assumed", None, [75.0, 120.0])
add("speaker_Qms", 5.5, "-", "assumed", None, [3.0, 9.0])
add("speaker_Qes", 1.0, "-", "assumed", None, [0.6, 1.6])
add("speaker_Mms", 0.0195, "kg", "assumed", None, [0.012, 0.03])
add("speaker_Sd", 0.050, "m^2", "assumed", None, [0.045, 0.055])
add("speaker_voice_coil_tempco", 0.00393, "1/K", "proposed", None, note="copper; conductor not verified (dossier Sec. 18)")
add("speaker_thermal_Cth", 8.0, "J/K", "proposed", None, [3.0, 20.0], note="single compartment")
add("speaker_thermal_Rth", 6.0, "K/W", "proposed", None, [3.0, 12.0])
add("speaker_Bl_of_x_and_Kms_of_x", None, "curves", "unknown", None, note="large-signal speaker nonlinearity not modelled (no data)")
add("cabinet_internal_geometry", None, "-", "unknown", "S01", note="no dimensioned drawing located")
add("cabinet_microphone_room_chain", None, "-", "unknown")
add("acoustic_path_difference", 0.42, "m", "assumed", None, [0.25, 0.7], note="PROVISIONAL open-back dipole path difference")
add("acoustic_rear_level", 0.80, "-", "assumed", None, [0.5, 1.0])
add("acoustic_cone_rolloff", 5200.0, "Hz", "assumed", None, [3500.0, 7000.0], note="PROVISIONAL generic 12in driver")
add("acoustic_presence_peak", {"f_Hz": 2900, "gain_dB": 3.0, "Q": 1.3}, "-", "assumed", None, note="PROVISIONAL cone-mode proxy; not PN 7708599000")

# ---- fitted constitutive models (candidate_fit).  These entries DOCUMENT the values compiled into generated/TubeFits.h by
# scripts/fit_tubes.py; the C++ loader does not read them (see docs/TRACEABILITY.md, 'manifest consumption').
for t in ("12AY7", "12AX7"):
    f = fits["triode_koren"][t]
    r = f["fit_operating_region"]
    add("tube_" + t + "_koren", {k: f[k] for k in ("mu", "ex", "kg1", "kp", "kvb")}, "Koren", "candidate_fit", "S21,S24",
        note=("fitted to %d pixel-digitised points of the JJ transfer + plate sheet; operating region (Vp 50-350 V, Vg -6..0 V): rms %.3f mA, "
              "relative %.1f %% (tail-dominated); whole held-out curves and cross-panel prediction in data/tubes/fitted_models.json; "
              "local mu/rp/gm at the RCA point differ from the RCA numbers the dossier quotes (a JJ-vs-RCA data difference, not forced); "
              "no specimen curve-traced; grid conduction ASSUMED") % (f["n_points"], r["rms_mA"], 100 * r["rel_rms"]))
v = fits["pentode_6V6"]
add("tube_6V6_pentode", {k: v[k] for k in ("mu", "ex", "kg1", "kp", "kvb", "fs0", "vk", "q", "va")}, "Koren (Kvb form) + screen partition", "candidate_fit", "S22",
    note=("cathode-current law fitted to the digitised JJ 6V6S sheet (assumed triode-connected, Ia = plate+screen): amp region rms %.2f mA / %.1f %% relative, "
          "positive-grid region %.1f %%; screen partition fs0 and Early voltage va come from an EXTERNAL RCA 6V6GT rating point that is NOT in the dossier; "
          "knee shape vk, q ASSUMED") % (v["fit_amp_region"]["rms_mA"], 100 * v["fit_amp_region"]["rel_rms_above_10mA"], 100 * v["fit_positive_grid"]["rel_rms_above_10mA"]))
g = v["grid"]
add("tube_6V6_grid_conduction", {"a_A_per_V1p5": g["a"], "imax_A": g["imax"], "vc_V": g["vc"], "sg_V": g["sg"]}, "saturating power law", "candidate_fit", "S22",
    note="plateau digitised from the JJ 6V6S dashed Ig curves (Ug=+5 V: %.1f mA, +20 V: %.1f mA at Ua 150-350 V); onset (vc, sg) ASSUMED; Ig dependence on plate voltage below 100 V not modelled" % (g["plateau_mA"]["5"], g["plateau_mA"]["20"]))
y = fits["rectifier_5Y3"]
add("rectifier_5Y3_forward", {"k1_mA_per_V": y["k1_mA_per_V"], "k2_mA_per_V2": y["k2_mA_per_V2"]}, "mA", "candidate_fit", "S23",
    note="per-anode forward law fitted to the digitised JJ 5Y3S right panel (dashed branch is the manufacturer's extrapolation); in-circuit regulation check against the left panel: tools/validate_supply.cpp")

# ---- calibration (dossier Sec. 24)
CAL = {
    "volts_per_full_scale_input_sample": 1.0,
    "output_samples_per_volt": 0.03536,
    "source_impedance_ohm": 100.0,
    "fixed_delay_samples": "derived: oversampler group delay, reported to the host",
}
add("calibration_volts_per_full_scale_input_sample", 1.0, "V", "assumed", None, [0.1, 5.0], note="user-adjustable 'Input Cal'; NOT a Fender specification")
add("calibration_output_samples_per_volt", 0.03536, "1/V", "proposed", None, note="0 dBFS = 28.28 V peak at the speaker terminals (rated 12.5 W sine, 14.14 V peak = -6 dBFS)")
add("source_impedance", 100.0, "ohm", "assumed", None, [0.0, 10000.0], note="line/DI default; pickup+cable model optional")

BRANCHES = {
    "historical_5e3": {"sources": ["S03", "S05", "S10"], "carried_by": "data/netlist/cd57_nominal.net (elements tagged HIST) and the historical_nominal manifest entries",
                       "note": "Original 5E3 topology and nominal values. Not modern manufacturing verification (Sec. 04)."},
    "reissue_2006": {"sources": ["S04"], "carried_by": "NOTHING numeric: the service drawing 0073201000 Rev. B was not available to this implementation",
                     "note": "Earlier reissue generation. Its values must not be assigned to the modern unit automatically (Sec. 04, 14); a delta with branch 'reissue_2006' can carry them once the drawing is read."},
    "modern_nominal": {"sources": ["S01", "S02"], "carried_by": "manufacturer_documented, derived, assumed, proposed and candidate_fit entries",
                       "note": "The modern Custom Deluxe as far as public documents establish it, on top of the historical topology. This is the profile the plugin ships."},
    "specimen": {"sources": [], "carried_by": "a specimen delta merged with `cd57_manifest merge` (see data/parameters/specimen_delta_template.json); empty in the shipped manifest",
                 "note": "Measured values of one serial-numbered amplifier, stored beside the nominal ones, never instead of them."},
}

manifest = {
    "schema_version": 2,
    "branches": BRANCHES,
    "target": "Fender 57 Custom Deluxe",
    "model_family": "815050",
    "pr_number": "PR 728",
    "profile": "provisional_public_source_nominal",
    "profile_note": "Documented approximation of the modern amplifier (dossier Sec. 33). Not a measured one-to-one model: no specimen was measured.",
    "specimen_serial": None,
    "reference_boundary": "input_jack_to_speaker_terminals",
    "dossier": "Fender 57 Custom Deluxe -- Public-source Engineering Dossier, Edition 1, 5 November 2026",
    "sample_rate_hz": None,
    "calibration": CAL,
    "parameters": P,
}
os.makedirs(os.path.join(ROOT, "data/parameters"), exist_ok=True)
with open(os.path.join(ROOT, "data/parameters/cd57_nominal.json"), "w") as f:
    json.dump(manifest, f, indent=1)
from collections import Counter
c = Counter(v["status"] for v in P.values())
print("wrote manifest:", len(P), "parameters;", dict(c))

# ---- specimen delta template: every parameter the model reads, measured = null (a null changes nothing).  Hints say what the dossier asks to measure.
HINT = {
    "pt_hv_half_winding_rms": "AC RMS of ONE half of the high-voltage secondary (centre tap to one anode), unloaded or at stated load; record mains volts and Hz (Sec. 11, 12, 25)",
    "pt_series_resistance_per_half": "DC resistance of one HV half-winding plus the reflected primary resistance (Sec. 12)",
    "ot_turns_ratio_full_primary": "full-primary : secondary turns ratio from a low-level voltage ratio with the other windings open (Sec. 12)",
    "ot_primary_inductance_full": "small-signal inductance of the full primary, secondary open, at a stated frequency and level (Sec. 12)",
    "ot_leakage_inductance_full_ref": "leakage inductance referred to the full primary, secondary shorted (Sec. 12)",
    "ot_dcr_primary_half": "DC resistance of one primary half (Sec. 12)",
    "ot_dcr_secondary": "DC resistance of the secondary (Sec. 12)",
    "ot_core_saturation_flux_per_secondary_turn": "ONLY if a saturation measurement exists (Sec. 12, 29: do not assign a law before evidence); switch the option on to use it",
    "pot_total_resistance": "end-to-end resistance of the volume/tone pots (Sec. 08)",
    "pot_endpoint_contact_resistance": "wiper resistance at the grounded end stop (Sec. 08)",
    "speaker_Re": "voice-coil DC resistance, cold (Sec. 17)",
    "speaker_Le": "voice-coil inductance from the complex impedance fit (Sec. 17)",
    "speaker_fs": "free-air resonance (Sec. 17)", "speaker_Qms": "mechanical Q (Sec. 17)", "speaker_Qes": "electrical Q (Sec. 17)",
    "speaker_Mms": "moving mass (sets acoustic sensitivity only; no electrical effect when fs, Qms, Qes are given)", "speaker_Sd": "effective piston area",
    "speaker_voice_coil_tempco": "only if the conductor is verified (Sec. 18)", "speaker_thermal_Cth": "warming/cooling fit (Sec. 18)", "speaker_thermal_Rth": "warming/cooling fit (Sec. 18)",
    "volume_pot_taper_instrument": "11 alpha values at knob 0, 0.1, ..., 1.0 (Sec. 08)", "volume_pot_taper_mic": "11 alpha values at knob 0, 0.1, ..., 1.0 (Sec. 08)", "tone_pot_taper": "11 alpha values at knob 0, 0.1, ..., 1.0, from the 5 nF end (Sec. 08)",
}
tpl = {
    "schema": "cd57-specimen-delta-v1",
    "delta_of_profile": manifest["profile"],
    "branch": "specimen",
    "how_to_use": ("Fill in 'measured' for the parameters you measured, leave the others null (null changes nothing), fill the 'specimen' block, then: "
                   "cd57_manifest merge data/parameters/cd57_nominal.json YOUR_DELTA.json merged.json  and render with --manifest merged.json "
                   "(cd57_capture_matrix, cd57_render) or build the plugin with -DCD57_MANIFEST=merged.json. Every measured block needs value, unit (the entry's unit), "
                   "instrument and date; record the instrument range and lead compensation (Sec. 04). Nominal values stay in the file beside the measured ones."),
    "specimen": {"serial": None, "revision": None, "market": None, "tube_markings": None, "speaker_label": None, "photo_ids": []},
    "parameters": {k: {"hint": HINT.get(k, ""), "unit": P[k]["unit"], "nominal": P[k]["value"], "measured": None} for k in HINT},
}
for k in HINT: assert k in P, k
with open(os.path.join(ROOT, "data/parameters/specimen_delta_template.json"), "w") as f:
    json.dump(tpl, f, indent=1)
print("wrote specimen_delta_template.json:", len(HINT), "parameters (all measured = null)")
