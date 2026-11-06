#!/usr/bin/env python3
"""Deterministic synthetic/negative tests for the free-air speaker impedance fit.

All generated curves are SYNTHETIC; the tests do not imply that the Fender OEM
Eminence PN 7708599000 has any of the synthetic parameters.
"""
from __future__ import annotations

import csv
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import numpy as np

from fit_speaker_impedance import CaptureError, fit_capture, impedance, load_capture, write_outputs_safely


HERE = Path(__file__).resolve().parent
TOOL = HERE / "fit_speaker_impedance.py"


class SpeakerImpedanceFitTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory(prefix="cd57_speaker_fit_")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.csvpath = self.root / "synthetic_free_air.csv"
        self.delta = self.root / "specimen_delta.json"
        self.report = self.root / "report.json"
        self.values = np.array([6.25, 0.00082, 102.5, 6.1, 0.93], dtype=float)
        self.f = np.geomspace(18.0, 13000.0, 221)
        self.z = impedance(self.f, self.values)

    def write_capture(self, f=None, z=None, path=None):
        f = self.f if f is None else f
        z = self.z if z is None else z
        path = self.csvpath if path is None else path
        with path.open("w", newline="", encoding="utf-8") as out:
            writer = csv.writer(out)
            writer.writerow(("frequency_hz", "real_ohm", "imag_ohm"))
            for hz, v in zip(f, z):
                writer.writerow((f"{hz:.12g}", f"{v.real:.12g}", f"{v.imag:.12g}"))

    def invoke(self, *extra):
        return subprocess.run([sys.executable, str(TOOL), "--csv", str(self.csvpath), "--delta", str(self.delta),
                               "--report", str(self.report), "--mounting", "free_air",
                               "--specimen-serial", "SYNTHETIC-TEST-ONLY", "--speaker-label", "SYNTHETIC DRIVER",
                               "--instrument", "Synthetic exact complex V/I calculator",
                               "--instrument-range", "0.1 V RMS test fixture",
                               "--calibration-method", "analytically zero lead/phase offset",
                               "--date", "2026-11-06", "--temperature-c", "22", *extra],
                              capture_output=True, text=True)

    def test_synthetic_exact_fit_and_manifest_delta(self):
        self.write_capture()
        result = self.invoke()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        delta = json.loads(self.delta.read_text())
        report = json.loads(self.report.read_text())
        self.assertEqual(delta["schema"], "cd57-specimen-delta-v1")
        self.assertEqual(delta["specimen"]["serial"], "SYNTHETIC-TEST-ONLY")
        self.assertEqual(delta["specimen"]["speaker_label"], "SYNTHETIC DRIVER")
        self.assertEqual(report["measurement"]["source_sha256"], hashlib.sha256(self.csvpath.read_bytes()).hexdigest())
        self.assertLess(report["fit"]["holdout"]["complex_relative_rms_percent"], 0.01)
        for key, expected in zip(("speaker_Re", "speaker_Le", "speaker_fs", "speaker_Qms", "speaker_Qes"), self.values):
            measured = delta["parameters"][key]["measured"]
            self.assertAlmostEqual(measured["value"] / float(expected), 1.0, places=3, msg=key)
            self.assertEqual(measured["source_sha256"], report["measurement"]["source_sha256"])
            self.assertEqual(measured["measurement_mounting"], "free_air")
        for unknown in ("speaker_Mms", "speaker_Sd", "speaker_thermal_Cth", "speaker_thermal_Rth"):
            self.assertIsNone(delta["parameters"][unknown]["measured"], unknown)
        self.assertNotEqual(delta["parameters"]["speaker_Re"]["nominal"], delta["parameters"]["speaker_Re"]["measured"]["value"])

    def test_noise_holdout_and_identifiability(self):
        rng = np.random.default_rng(301)
        perturb = 1.0 + (rng.normal(0, 0.001, len(self.z)) + 1j * rng.normal(0, 0.001, len(self.z)))
        self.write_capture(z=self.z * perturb)
        result = self.invoke()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        report = json.loads(self.report.read_text())
        self.assertGreater(report["fit"]["holdout"]["complex_relative_rms_percent"], 0.01)
        self.assertLess(report["fit"]["holdout"]["complex_relative_rms_percent"], 0.5)
        intervals = report["fit"]["local_statistical_95pct_intervals"]
        for key in ("speaker_Re", "speaker_Le", "speaker_fs", "speaker_Qms", "speaker_Qes"):
            self.assertGreater(intervals[key][1], intervals[key][0])

    def test_reject_cabinet_mounting_and_invalid_metadata(self):
        self.write_capture()
        p = self.invoke("--mounting", "in_cabinet")
        self.assertEqual(p.returncode, 2)
        self.assertIn("free-air", p.stderr)
        self.assertFalse(self.delta.exists())
        p = self.invoke("--date", "2026-13-45")
        self.assertEqual(p.returncode, 2)
        self.assertFalse(self.delta.exists())

    def test_reject_missing_complex_phase_and_nonfinite_data(self):
        self.csvpath.write_text("frequency_hz,impedance_ohm\n100,8\n", encoding="utf-8")
        with self.assertRaisesRegex(CaptureError, "requires columns"):
            load_capture(self.csvpath)
        self.csvpath.write_text("frequency_hz,real_ohm,imag_ohm,real_ohm\n100,8,2,9\n", encoding="utf-8")
        with self.assertRaisesRegex(CaptureError, "unique"):
            load_capture(self.csvpath)
        self.write_capture()
        text = self.csvpath.read_text()
        self.csvpath.write_text(text.replace("18,", "18,nan,", 1), encoding="utf-8")
        with self.assertRaises(CaptureError):
            load_capture(self.csvpath)

    def test_reject_insufficient_frequency_span_and_bad_model(self):
        self.write_capture(f=self.f[:20], z=self.z[:20])
        with self.assertRaisesRegex(CaptureError, "at least 32"):
            load_capture(self.csvpath)
        self.write_capture(f=np.linspace(60, 120, 60), z=np.ones(60)*8+1j*np.ones(60))
        with self.assertRaisesRegex(CaptureError, "insufficient sweep span"):
            load_capture(self.csvpath)
        rng = np.random.default_rng(6)
        self.write_capture(z=self.z + (rng.normal(0, 9.0, len(self.z)) + 1j * rng.normal(0, 9.0, len(self.z))))
        p = self.invoke("--max-holdout-rms-percent", "1")
        self.assertEqual(p.returncode, 2)
        self.assertFalse(self.delta.exists())

    def test_output_path_safety_and_explicit_overwrite(self):
        self.write_capture()
        p = self.invoke("--delta", str(self.csvpath))
        self.assertEqual(p.returncode, 2)
        self.assertIn("may not replace input", p.stderr)
        self.assertIn("frequency_hz", self.csvpath.read_text())
        p = self.invoke("--report", str(self.delta))
        self.assertEqual(p.returncode, 2)
        self.assertIn("must be distinct", p.stderr)

        first = self.invoke()
        self.assertEqual(first.returncode, 0, first.stderr)
        output_sha = hashlib.sha256(self.delta.read_bytes()).hexdigest()
        second = self.invoke()
        self.assertEqual(second.returncode, 2)
        self.assertIn("already exists", second.stderr)
        self.assertEqual(output_sha, hashlib.sha256(self.delta.read_bytes()).hexdigest())
        third = self.invoke("--overwrite")
        self.assertEqual(third.returncode, 0, third.stderr)

    def test_reject_template_with_prepopulated_unknown_measurement(self):
        self.write_capture()
        template = json.loads((HERE.parent / "data/parameters/specimen_delta_template.json").read_text())
        template["parameters"]["speaker_Sd"]["measured"] = {"value": 0.05}
        polluted = self.root / "not-a-clean-template.json"
        polluted.write_text(json.dumps(template), encoding="utf-8")
        p = self.invoke("--template", str(polluted))
        self.assertEqual(p.returncode, 2)
        self.assertIn("clean specimen delta", p.stderr)
        self.assertFalse(self.delta.exists())

    def test_separate_repeat_sweep_is_scored_without_refitting(self):
        self.write_capture()
        repeat = self.root / "second_recorded_sweep.csv"
        self.write_capture(path=repeat, z=self.z * (1.0 + 0.0002))
        success = self.invoke("--validation-csv", str(repeat))
        self.assertEqual(success.returncode, 0, success.stderr)
        report = json.loads(self.report.read_text())
        validated = report["independent_repeat"]
        self.assertTrue(validated["fit_parameters_frozen"])
        self.assertLess(validated["fixed_fit_comparison"]["complex_relative_rms_percent"], 0.1)
        self.assertEqual(validated["source_sha256"], hashlib.sha256(repeat.read_bytes()).hexdigest())
        self.assertIn("repeat_source_sha256", json.loads(self.delta.read_text())["parameters"]["speaker_Re"]["measured"])

        self.write_capture(path=repeat, z=self.z * 1.25)
        rejected = self.invoke("--validation-csv", str(repeat), "--overwrite")
        self.assertEqual(rejected.returncode, 2)
        self.assertIn("independent repeat mismatch", rejected.stderr)
        self.assertEqual(report, json.loads(self.report.read_text()), "failed validation may not replace a prior passing report")

    def test_repeat_capture_must_be_distinct_from_training_input(self):
        self.write_capture()
        rejected = self.invoke("--validation-csv", str(self.csvpath))
        self.assertEqual(rejected.returncode, 2)
        self.assertIn("distinct recorded sweep", rejected.stderr)
        duplicate = self.root / "duplicate_bytes.csv"
        duplicate.write_bytes(self.csvpath.read_bytes())
        rejected_copy = self.invoke("--validation-csv", str(duplicate))
        self.assertEqual(rejected_copy.returncode, 2)
        self.assertIn("byte-identical", rejected_copy.stderr)
        self.assertFalse(self.delta.exists())

    def test_staging_and_rollback_on_multi_output_failure(self):
        self.delta.write_bytes(b"ORIGINAL DELTA\n")
        self.report.write_bytes(b"ORIGINAL REPORT\n")
        real_replace = os.replace
        calls = []

        def fail_second_rename(src, dst):
            calls.append(str(dst))
            if len(calls) == 2:
                raise OSError("simulated report publication failure")
            return real_replace(src, dst)

        with mock.patch("fit_speaker_impedance.os.replace", side_effect=fail_second_rename):
            with self.assertRaisesRegex(OSError, "simulated report"):
                write_outputs_safely([(self.delta, {"new": "delta"}), (self.report, {"new": "report"})], overwrite=True)
        self.assertEqual(self.delta.read_bytes(), b"ORIGINAL DELTA\n")
        self.assertEqual(self.report.read_bytes(), b"ORIGINAL REPORT\n")
        self.assertFalse(list(self.root.glob("*.tmp")), "stage and rollback temporary files must be cleaned")

    def test_staging_fails_before_prior_delta_is_replaced(self):
        self.delta.write_bytes(b"EXISTING CALIBRATION\n")
        parent_file = self.root / "parent_is_a_file"
        parent_file.write_text("NOT A DIRECTORY")
        with self.assertRaises(OSError):
            write_outputs_safely([(self.delta, {"new": "delta"}),
                                  (parent_file / "report.json", {"new": "report"})], overwrite=True)
        self.assertEqual(self.delta.read_bytes(), b"EXISTING CALIBRATION\n")


if __name__ == "__main__":
    unittest.main()
