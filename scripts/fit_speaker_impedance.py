#!/usr/bin/env python3
"""Fit the nominal CD57 *linear* speaker impedance equivalent to a calibrated free-air sweep.

This deliberately does not identify an OEM driver from the curve. A complex
impedance sweep identifies Re, Le, fs, Qms and Qes in the model's equivalent;
it does NOT independently identify Mms, Sd, Bl(x), Kms(x), or cabinet acoustics.
The resulting specimen delta is compatible with ``cd57_manifest merge``.

Input CSV columns: frequency_hz,real_ohm,imag_ohm. Complex impedance must be
terminal-voltage / terminal-current, with phase and fixture compensation already
calibrated, measured at a stated low signal level with the driver in free air.
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

import numpy as np
from scipy.optimize import least_squares


PARAMETERS = ("speaker_Re", "speaker_Le", "speaker_fs", "speaker_Qms", "speaker_Qes")
PARAMETER_UNITS = {"speaker_Re": "ohm", "speaker_Le": "H", "speaker_fs": "Hz", "speaker_Qms": "-", "speaker_Qes": "-"}
LOWER = np.array([0.1, 1.0e-6, 10.0, 0.3, 0.05], dtype=float)
UPPER = np.array([100.0, 0.05, 1000.0, 100.0, 30.0], dtype=float)


class CaptureError(ValueError):
    """The input or its physical provenance cannot support the requested fit."""


def impedance(frequency_hz: np.ndarray, parameters: np.ndarray) -> np.ndarray:
    """Exactly the linear small-signal electrical equivalent in SpeakerTS.

    Z = Re + j*w*Le + Bl^2/(Rms + j*w*Mms + 1/(j*w*Cms)).
    Eliminating the unidentifiable Mms gives the formula below, in terms of
    Re, Le, fs, Qms and Qes. No cabinet acoustic parameters are implied.
    """
    re, le, fs, qms, qes = parameters
    ratio = frequency_hz / fs
    return re + 1j * (2.0 * math.pi * frequency_hz * le) + (re * qms / qes) / (1.0 + 1j * qms * (ratio - 1.0 / ratio))


def load_capture(path: Path) -> tuple[np.ndarray, np.ndarray, str]:
    if not path.is_file():
        raise CaptureError(f"input CSV does not exist: {path}")
    raw = path.read_bytes()
    try:
        content = raw.decode("utf-8-sig")
    except UnicodeDecodeError as exc:
        raise CaptureError("input CSV must be UTF-8") from exc
    reader = csv.DictReader(content.splitlines())
    if reader.fieldnames is None or len(reader.fieldnames) != len(set(reader.fieldnames)):
        raise CaptureError("CSV column names must be present and unique")
    rows = list(reader)
    if not rows:
        raise CaptureError("empty impedance CSV")
    required = {"frequency_hz", "real_ohm", "imag_ohm"}
    if not required.issubset(rows[0]):
        raise CaptureError("CSV requires columns frequency_hz,real_ohm,imag_ohm (complex Z = V/I)")
    points: list[tuple[float, complex]] = []
    for n, row in enumerate(rows, start=2):
        if None in row or any(value is None for value in row.values()):
            raise CaptureError(f"CSV row {n} has too many or too few fields")
        try:
            f = float(row["frequency_hz"])
            re = float(row["real_ohm"])
            im = float(row["imag_ohm"])
        except (ValueError, TypeError) as exc:
            raise CaptureError(f"invalid numeric data on CSV row {n}") from exc
        if not all(math.isfinite(x) for x in (f, re, im)) or not (f > 0.0 and re > 0.0):
            raise CaptureError(f"row {n}: frequency and real impedance must be positive and every value finite")
        if f < 0.1 or f > 1.0e6 or max(abs(re), abs(im)) > 1.0e9:
            raise CaptureError(f"row {n}: implausible frequency/impedance outside measurement sanity limits")
        points.append((f, complex(re, im)))
    if len(points) < 32:
        raise CaptureError("at least 32 complex-impedance points are required")
    points.sort(key=lambda p: p[0])
    freq = np.array([p[0] for p in points], dtype=float)
    z = np.array([p[1] for p in points], dtype=complex)
    if np.any(np.diff(freq) <= 0.0):
        raise CaptureError("frequencies must be distinct (one complex-impedance measurement per frequency)")
    if freq[-1] / freq[0] < 12.0:
        raise CaptureError("insufficient sweep span: highest frequency must be at least 12 times lowest")
    return freq, z, hashlib.sha256(raw).hexdigest()


def _objective(log_parameters: np.ndarray, freq: np.ndarray, z: np.ndarray) -> np.ndarray:
    predicted = impedance(freq, np.exp(log_parameters))
    # Relative complex residual avoids allowing the large resonance peak to
    # overwhelm low-frequency Re and high-frequency inductive observations.
    scale = np.maximum(np.abs(z), 1.0)
    delta = (predicted - z) / scale
    return np.concatenate((delta.real, delta.imag))


def _metrics(freq: np.ndarray, z: np.ndarray, fitted: np.ndarray) -> dict[str, float]:
    err = np.abs(impedance(freq, fitted) - z) / np.maximum(np.abs(z), 1.0)
    return {
        "points": int(len(freq)),
        "complex_relative_rms_percent": float(100.0 * np.sqrt(np.mean(err * err))),
        "complex_relative_p95_percent": float(100.0 * np.percentile(err, 95)),
        "complex_relative_max_percent": float(100.0 * np.max(err)),
    }


def fit_capture(freq: np.ndarray, z: np.ndarray, max_holdout_rms_percent: float = 10.0) -> dict:
    if len(freq) < 32 or len(freq) != len(z):
        raise CaptureError("at least 32 valid frequency/impedance pairs are required")
    # Withhold every seventh point after sorting: deterministic, spans the
    # whole spectrum and cannot be optimized against by the solver.
    held = np.arange(len(freq)) % 7 == 0
    train = ~held
    fpeak = float(freq[np.argmax(z.real)])
    if not (freq[0] < 0.8 * fpeak and freq[-1] > 4.0 * fpeak and 15.0 < fpeak < 900.0):
        raise CaptureError("no adequately resolved free-air resonance: sweep must extend below and at least 4x above its real-impedance peak")
    re0 = max(0.1, float(np.min(z.real)))
    # A robust initial inductance estimate at the highest points; a small
    # positive floor keeps the optimizer away from the log singularity.
    le0 = max(1.0e-6, float(np.median(z.imag[-max(4, len(z)//10):] / (2.0 * math.pi * freq[-max(4, len(z)//10):]))))
    qms0 = 5.0
    qes0 = re0 * qms0 / max(re0, float(np.max(z.real) - re0))
    start = np.clip(np.array([re0, le0, fpeak, qms0, qes0]), LOWER * 1.01, UPPER / 1.01)
    result = least_squares(_objective, np.log(start), args=(freq[train], z[train]),
                           bounds=(np.log(LOWER), np.log(UPPER)),
                           loss="soft_l1", f_scale=0.03, max_nfev=20000,
                           xtol=1e-12, ftol=1e-12, gtol=1e-12)
    if not result.success:
        raise CaptureError(f"nonlinear speaker fit did not converge: {result.message}")
    vals = np.exp(result.x)
    if np.any(vals < LOWER * 1.001) or np.any(vals > UPPER * 0.999):
        raise CaptureError("fit reached a parameter bound: the simple speaker model or sweep is insufficient")
    train_metrics = _metrics(freq[train], z[train], vals)
    held_metrics = _metrics(freq[held], z[held], vals)
    if held_metrics["complex_relative_rms_percent"] > max_holdout_rms_percent:
        raise CaptureError(f"held-out fit residual {held_metrics['complex_relative_rms_percent']:.2f}% exceeds {max_holdout_rms_percent:.2f}%: refuse an unsupported measured profile")

    # Local statistical uncertainty from the fit Jacobian, NOT a statement
    # about between-specimen variation, fixture calibration or model bias.
    jac = result.jac
    singular = np.linalg.svd(jac, compute_uv=False)
    condition = float(singular[0] / singular[-1]) if singular[-1] > 0 else float("inf")
    if not np.isfinite(condition) or condition > 1.0e8:
        raise CaptureError(f"poorly identifiable parameters (Jacobian condition number {condition:.3g})")
    dof = max(1, len(result.fun) - len(vals))
    covariance = np.linalg.pinv(jac.T @ jac) * float(np.dot(result.fun, result.fun) / dof)
    stderr_log = np.sqrt(np.maximum(0.0, np.diag(covariance)))
    intervals = np.stack((vals * np.exp(-1.96 * stderr_log), vals * np.exp(1.96 * stderr_log)), axis=-1)
    return {
        "values": {k: float(v) for k, v in zip(PARAMETERS, vals)},
        "local_statistical_95pct_intervals": {k: [float(l), float(h)] for k, (l,h) in zip(PARAMETERS, intervals)},
        "train": train_metrics,
        "holdout": held_metrics,
        "jacobian_condition_number": condition,
        "disclaimer": "Intervals quantify local fit statistics only; they exclude fixture/calibration uncertainty, model discrepancy and speaker manufacturing variation.",
    }


def make_delta(template: Path, result: dict, provenance: dict) -> dict:
    delta = json.loads(template.read_text(encoding="utf-8"))
    if delta.get("schema") != "cd57-specimen-delta-v1":
        raise CaptureError("incorrect specimen delta template schema")
    if not isinstance(delta.get("specimen"), dict) or not isinstance(delta.get("parameters"), dict):
        raise CaptureError("specimen delta template lacks specimen or parameter definitions")
    # The generated file is a speaker-only measurement delta. In particular,
    # never carry forward a pre-populated, unverified measurement in a custom
    # template and make it appear to share this capture's provenance.
    if any(not isinstance(entry, dict) or entry.get("measured") is not None
           for entry in delta["parameters"].values()):
        raise CaptureError("speaker fit requires a clean specimen delta template (every measured entry null)")
    for key in PARAMETERS:
        entry = delta["parameters"].get(key)
        if not isinstance(entry, dict) or entry.get("unit") != PARAMETER_UNITS[key]:
            raise CaptureError(f"template lacks expected speaker parameter/unit: {key}")
    delta["specimen"]["serial"] = provenance["specimen_serial"]
    delta["specimen"]["speaker_label"] = provenance["speaker_label"]
    for key, value in result["values"].items():
        entry = delta["parameters"][key]
        entry["measured"] = {
            "value": value,
            "unit": entry["unit"],
            "instrument": provenance["instrument"],
            "instrument_range": provenance["instrument_range"],
            "lead_compensation": provenance["calibration_method"],
            "date": provenance["date"],
            "measurement_mounting": "free_air",
            "temperature_C": provenance["temperature_C"],
            "source_sha256": provenance["source_sha256"],
            "fit_heldout_relative_rms_percent": result["holdout"]["complex_relative_rms_percent"],
            "fit_local_statistical_95pct_interval": result["local_statistical_95pct_intervals"][key],
        }
    return delta


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--csv", required=True, type=Path, help="calibrated free-air complex impedance CSV")
    p.add_argument("--delta", required=True, type=Path, help="output specimen delta JSON")
    p.add_argument("--report", type=Path, help="optional fit metrics/provenance JSON")
    p.add_argument("--overwrite", action="store_true", help="explicitly permit replacing existing output JSON files")
    p.add_argument("--mounting", choices=("free_air", "in_cabinet"), required=True,
                   help="only free-air sweeps identify free-air fs/Q parameters")
    p.add_argument("--specimen-serial", required=True, help="identity of the measured amplifier or speaker")
    p.add_argument("--speaker-label", required=True, help="literal markings observed on the speaker")
    p.add_argument("--instrument", required=True, help="measurement equipment and current/voltage sensing chain")
    p.add_argument("--instrument-range", required=True, help="voltage/current range and small-signal excitation level")
    p.add_argument("--calibration-method", required=True, help="fixture/lead and phase calibration method")
    p.add_argument("--date", required=True, help="measurement date YYYY-MM-DD")
    p.add_argument("--temperature-c", required=True, type=float, help="voice-coil/ambient temperature in degrees Celsius")
    p.add_argument("--max-holdout-rms-percent", type=float, default=10.0,
                   help="refuse to emit a measured profile if held-out complex relative RMS exceeds this")
    here = Path(__file__).resolve().parents[1]
    p.add_argument("--template", type=Path, default=here / "data/parameters/specimen_delta_template.json")
    args = p.parse_args(argv)
    try:
        if args.mounting != "free_air":
            raise CaptureError("in-cabinet impedance cannot be labelled a free-air fs/Q measurement; use a free-air fixture")
        if not all(x.strip() for x in (args.specimen_serial, args.speaker_label, args.instrument, args.instrument_range, args.calibration_method)):
            raise CaptureError("specimen ID, speaker markings and complete instrument/calibration provenance are mandatory")
        try:
            if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", args.date):
                raise ValueError("ISO date must be extended YYYY-MM-DD")
            date.fromisoformat(args.date)
        except ValueError as exc:
            raise CaptureError("--date must be a valid YYYY-MM-DD calendar date") from exc
        if not math.isfinite(args.temperature_c) or not -20 <= args.temperature_c <= 80:
            raise CaptureError("temperature must be a plausible finite value (-20 to 80 C)")
        if not 0 < args.max_holdout_rms_percent <= 100:
            raise CaptureError("max holdout RMS must be within (0,100]")
        inputs = {args.csv.resolve(), args.template.resolve()}
        outputs = [args.delta.resolve()] + ([args.report.resolve()] if args.report else [])
        if len(set(outputs)) != len(outputs) or any(path in inputs for path in outputs):
            raise CaptureError("output paths must be distinct and may not replace input CSV or template")
        if not args.overwrite and any(path.exists() for path in outputs):
            raise CaptureError("output JSON already exists; choose a fresh path or explicitly pass --overwrite")
        freq, z, digest = load_capture(args.csv)
        fitted = fit_capture(freq, z, args.max_holdout_rms_percent)
        provenance = {"specimen_serial": args.specimen_serial, "speaker_label": args.speaker_label,
                      "instrument": args.instrument, "instrument_range": args.instrument_range,
                      "calibration_method": args.calibration_method, "date": args.date,
                      "temperature_C": args.temperature_c, "source_sha256": digest}
        delta = make_delta(args.template, fitted, provenance)
        report = {"schema": "cd57-speaker-impedance-fit-v1", "measurement": provenance,
                  "input_rows": len(freq), "input_frequency_range_Hz": [float(freq[0]), float(freq[-1])],
                  "model": "linear electromechanical equivalent; NOT an authenticated Eminence OEM curve",
                  "fit": fitted,
                  "not_identified": ["speaker_Mms", "speaker_Sd", "speaker_Bl_of_x_and_Kms_of_x", "cabinet/microphone transfer"]}
        args.delta.parent.mkdir(parents=True, exist_ok=True)
        args.delta.write_text(json.dumps(delta, indent=2, allow_nan=False) + "\n", encoding="utf-8")
        if args.report:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
        print(f"PASS: fitted calibrated {args.mounting} sweep; held-out RMS {fitted['holdout']['complex_relative_rms_percent']:.3f}% ; {len(freq)} points")
        for k,v in fitted["values"].items():
            print(f"  {k}: {v:.8g}")
        print("  Mms, Sd, nonlinearities and cabinet acoustics were NOT inferred")
        return 0
    except (CaptureError, OSError, json.JSONDecodeError) as exc:
        print(f"REFUSED: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
