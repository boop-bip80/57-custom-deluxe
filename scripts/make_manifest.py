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

P = {}
def add(name, value, unit, status, source=None, interval=None, note=None, verified=None):
    d = {"value": value, "unit": unit, "status": status}
    if source: d["source"] = source
    if interval: d["interval"] = interval
    if note: d["note"] = note
    if verified is not None: d["verified_on_modern_specimen"] = verified
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
add("ot_turns_ratio_full_primary", 31.6228, "ratio", "historical_nominal", "S10", note="FTDO-59 8k:8 class; identity with modern OEM part unproven", verified=False)

# ---- ASSUMED (explicit assumption + sensitivity interval, dossier Sec. 28)
add("pt_hv_half_winding_rms", 360.0, "V", "assumed", "calibration", [330.0, 380.0],
    note="supply transformer parameters UNKNOWN; value calibrated so the model reproduces the documented 12.5 W at <5% THD (see validation)")
add("pt_series_resistance_per_half", 90.0, "ohm", "assumed", None, [50.0, 150.0], note="PT secondary + reflected primary resistance")
add("ot_primary_inductance_full", 25.0, "H", "assumed", None, [10.0, 60.0])
add("ot_leakage_inductance_full_ref", 0.040, "H", "assumed", None, [0.01, 0.08])
add("ot_dcr_primary_half", 125.0, "ohm", "assumed", None, [80.0, 200.0])
add("ot_dcr_secondary", 0.35, "ohm", "assumed", None, [0.2, 0.6])
add("ot_core_saturation_flux_per_secondary_turn", 0.060, "V*s", "assumed", None, [0.04, 0.09], note="no public saturation data (dossier Sec. 12)")
add("tube_interelectrode_capacitances", "12AX7/12AY7 1.5-1.7/1.5-1.7/0.46-0.5 pF, 6V6 9.5/0.7/7.5 pF", "pF", "assumed", "S08,S09,S19", note="datasheet-class values")
add("grid_conduction_law", "I=k*(s*softplus((vgk-vc)/s))^1.5", "-", "assumed", None, note="onset and slope need measurements (dossier Sec. 16)")
add("volume_pot_taper", None, "fraction_vs_angle", "unknown", "S01", note="manufacturer advertises revised tapers; numerical law not located")
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

# ---- fitted constitutive models (candidate_fit)
for t in ("12AY7", "12AX7"):
    f = fits["triode_koren"][t]
    add("tube_" + t + "_koren", {k: f[k] for k in ("mu", "ex", "kg1", "kp", "kvb")}, "Koren", "candidate_fit", "S08,S09,S18,S21,S24",
        note="fitted to digitised JJ transfer sheet + RCA anchor; rms error %.3f mA over %d points; no specimen curve-traced" % (f["rms_mA"], f["n_points"]))
v = fits["pentode_6V6"]
add("tube_6V6_pentode", {k: v[k] for k in ("mu", "ex", "kg1", "kp", "fs0", "vk", "q", "va")}, "Koren+screen partition", "candidate_fit", "S19,S22",
    note="fitted to RCA 6V6GT operating point; JJ sheet appears triode-connected and agrees within a few % at 150-200 V")
add("rectifier_5Y3_forward", {"k1_mA_per_V": fits["rectifier_5Y3"]["k1_mA_per_V"], "k2_mA_per_V2": fits["rectifier_5Y3"]["k2_mA_per_V2"]}, "mA", "candidate_fit", "S20,S23")

# ---- calibration (dossier Sec. 24)
CAL = {
    "volts_per_full_scale_input_sample": 1.0,
    "output_samples_per_volt": 0.05,
    "source_impedance_ohm": 100.0,
    "fixed_delay_samples": "derived: oversampler group delay, reported to the host",
}
add("calibration_volts_per_full_scale_input_sample", 1.0, "V", "assumed", None, [0.1, 5.0], note="user-adjustable 'Input Cal'; NOT a Fender specification")
add("calibration_output_samples_per_volt", 0.05, "1/V", "proposed", None, note="0 dBFS = 20 V peak at the speaker terminals (rated 12.5 W sine = -3 dBFS)")
add("source_impedance", 100.0, "ohm", "assumed", None, [0.0, 10000.0], note="line/DI default; pickup+cable model optional")

manifest = {
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
