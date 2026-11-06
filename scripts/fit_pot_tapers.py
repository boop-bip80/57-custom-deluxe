#!/usr/bin/env python3
"""Convert physically measured, independent '57 Custom Deluxe pot sweeps into specimen deltas.

The CSV records resistance between the wiper and the *grounded/low* track end
for each channel (tone: between wiper and the 5 nF capacitor end), with the pot
isolated from surrounding amplifier circuitry. Rows must be at physical knob
positions 0, 0.1, ... 1.0. Never infer measurements from an advertised taper.
"""

from __future__ import annotations

import argparse
import csv
from datetime import date
import hashlib
import json
import math
from pathlib import Path
import re
import sys


class CalibrationError(ValueError):
    """Input data cannot substantiate a specimen-specific pot taper."""


CHANNELS = {
    "instrument_ohm": "volume_pot_taper_instrument",
    "mic_ohm": "volume_pot_taper_mic",
    "tone_ohm": "tone_pot_taper",
}


def read_resistances(path: Path, totals: dict[str, float]) -> tuple[dict[str, list[float]], str]:
    if not path.is_file():
        raise CalibrationError(f"CSV not found: {path}")
    raw = path.read_bytes()
    try:
        content = raw.decode("utf-8-sig")
    except UnicodeDecodeError as exc:
        raise CalibrationError("CSV must be UTF-8") from exc
    reader = csv.DictReader(content.splitlines())
    fields = reader.fieldnames
    required = {"rotation", "instrument_ohm", "mic_ohm"}
    if not fields or len(fields) != len(set(fields)) or not required.issubset(fields):
        raise CalibrationError("CSV requires unique columns rotation,instrument_ohm,mic_ohm (tone_ohm optional)")
    actual = set(fields) - {"rotation"}
    if actual - set(CHANNELS):
        raise CalibrationError(f"unknown CSV columns: {sorted(actual - set(CHANNELS))}")
    if actual != set(totals):
        raise CalibrationError("each CSV channel requires exactly one measured --*-total-ohm value")
    rows = list(reader)
    if len(rows) != 11:
        raise CalibrationError("exactly 11 resistance readings at knob positions 0, 0.1, ..., 1.0 are required")
    alphas = {key: [] for key in totals}
    for i, row in enumerate(rows):
        if None in row or any(v is None for v in row.values()):
            raise CalibrationError(f"CSV row {i+2} has missing or surplus fields")
        try:
            pos = float(row["rotation"])
        except (ValueError, TypeError) as exc:
            raise CalibrationError(f"bad rotation on CSV row {i+2}") from exc
        if not math.isfinite(pos) or abs(pos - i/10.0) > 1e-4:
            raise CalibrationError("rows must be ordered at physical knob rotations 0, 0.1, ..., 1.0")
        for key, total in totals.items():
            try:
                resistance = float(row[key])
            except (ValueError, TypeError) as exc:
                raise CalibrationError(f"invalid measured resistance for {key} on CSV row {i+2}") from exc
            if not math.isfinite(resistance) or resistance < 0 or resistance > 1.03 * total:
                raise CalibrationError(f"{key} has invalid, negative, or above-total resistance on row {i+2}")
            a = resistance / total
            # Avoid writing alpha > 1 to a manifest; a reading above 1 is an
            # inconsistent measured total, not a valid taper point.
            if a > 1.0:
                raise CalibrationError(f"{key} exceeds its independently measured total resistance")
            alphas[key].append(a)
    for key, values in alphas.items():
        if values[0] > 0.03 or values[-1] < 0.97:
            raise CalibrationError(f"{key} cannot reach both end stops within 3% of track resistance")
        if any(values[i] + 1e-12 < values[i-1] for i in range(1, len(values))):
            raise CalibrationError(f"{key} has non-monotonic resistance measurements")
    return alphas, hashlib.sha256(raw).hexdigest()


def make_delta(template_path: Path, readings: dict[str, list[float]], totals: dict[str, float],
               metadata: dict[str, str]) -> dict:
    try:
        result = json.loads(template_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise CalibrationError(f"cannot load specimen template: {exc}") from exc
    if result.get("schema") != "cd57-specimen-delta-v1":
        raise CalibrationError("template schema must be cd57-specimen-delta-v1")
    if not isinstance(result.get("specimen"), dict) or not isinstance(result.get("parameters"), dict):
        raise CalibrationError("template is missing specimen/parameters objects")
    # Never silently transfer another experiment's existing 'measured' data
    # into the purportedly new specimen provenance.
    if any(not isinstance(p, dict) or p.get("measured") is not None for p in result["parameters"].values()):
        raise CalibrationError("template must have only null measured entries")
    result["specimen"]["serial"] = metadata["specimen_serial"]
    for column, values in readings.items():
        parameter = CHANNELS[column]
        slot = result["parameters"].get(parameter)
        if not isinstance(slot, dict) or slot.get("unit") != "fraction_vs_angle_11pt":
            raise CalibrationError(f"template lacks measured taper parameter {parameter} with correct unit")
        slot["measured"] = {
            "value": values,
            "unit": "fraction_vs_angle_11pt",
            "instrument": metadata["instrument"],
            "instrument_range": metadata["instrument_range"],
            "lead_compensation": metadata["lead_compensation"],
            "date": metadata["date"],
            "source_sha256": metadata["source_sha256"],
            "measurement_method": "isolated_wiper_to_low_end_vs_knob_rotation",
            "measured_total_resistance_ohm": totals[column],
            "calibration_note": "11 measured fractions for one physical pot; no Fender nominal taper inferred",
        }
    return result


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--csv", required=True, type=Path, help="11-row measured physical sweep CSV")
    p.add_argument("--delta", required=True, type=Path, help="output specimen-delta JSON")
    p.add_argument("--instrument-total-ohm", required=True, type=float, help="total track resistance measured across Instrument pot outer lugs")
    p.add_argument("--mic-total-ohm", required=True, type=float, help="total track resistance measured across Mic pot outer lugs")
    p.add_argument("--tone-total-ohm", type=float, help="total track resistance of optional Tone pot")
    p.add_argument("--specimen-serial", required=True, help="literal serial or specimen ID observed on hardware")
    p.add_argument("--instrument", required=True, help="meter make/model and measurement equipment")
    p.add_argument("--instrument-range", required=True, help="meter range and uncertainty")
    p.add_argument("--lead-compensation", required=True, help="how meter leads/contact resistance were compensated")
    p.add_argument("--date", required=True, help="measurement date, YYYY-MM-DD")
    p.add_argument("--overwrite", action="store_true", help="explicitly allow overwriting an existing output JSON")
    p.add_argument("--template", type=Path, default=Path(__file__).resolve().parents[1] / "data/parameters/specimen_delta_template.json")
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        for flag in ("specimen_serial", "instrument", "instrument_range", "lead_compensation"):
            if not str(getattr(args, flag)).strip():
                raise CalibrationError(f"--{flag.replace('_','-')} must be nonblank")
        if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", args.date):
            raise CalibrationError("--date must be valid ISO YYYY-MM-DD")
        try:
            date.fromisoformat(args.date)
        except ValueError as exc:
            raise CalibrationError("--date must be valid ISO YYYY-MM-DD") from exc
        totals = {"instrument_ohm": args.instrument_total_ohm, "mic_ohm": args.mic_total_ohm}
        if args.tone_total_ohm is not None:
            totals["tone_ohm"] = args.tone_total_ohm
        if any(not math.isfinite(v) or v <= 0 or v > 1e8 for v in totals.values()):
            raise CalibrationError("each measured total resistance must be finite and in (0, 100 Mohm]")
        resolved_input = {args.csv.resolve(), args.template.resolve()}
        if args.delta.resolve() in resolved_input:
            raise CalibrationError("output must not replace the CSV or specimen template")
        if args.delta.exists() and not args.overwrite:
            raise CalibrationError("output file already exists; use a new path or explicit --overwrite")
        alphas, digest = read_resistances(args.csv, totals)
        metadata = {"specimen_serial": args.specimen_serial.strip(), "instrument": args.instrument.strip(),
                    "instrument_range": args.instrument_range.strip(), "lead_compensation": args.lead_compensation.strip(),
                    "date": args.date, "source_sha256": digest}
        result = make_delta(args.template, alphas, totals, metadata)
        args.delta.parent.mkdir(parents=True, exist_ok=True)
        args.delta.write_text(json.dumps(result, indent=2, allow_nan=False) + "\n", encoding="utf-8")
        print(f"PASS: recorded {len(alphas)} physically measured pot taper(s) for specimen {metadata['specimen_serial']}")
        for name, curve in alphas.items():
            print(f"  {name}: alpha(0.5)={curve[5]:.6f}, measured total={totals[name]:g} ohm")
        print("  Modern Fender taper numbers are NOT inferred without these measurements.")
        return 0
    except (CalibrationError, OSError) as exc:
        print(f"REFUSED: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
