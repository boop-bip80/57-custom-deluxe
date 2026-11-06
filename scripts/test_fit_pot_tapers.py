#!/usr/bin/env python3
"""Deterministic positive and refusal checks for the measured-pot importer."""
from __future__ import annotations

import csv
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TEMPLATE = ROOT / "data/parameters/specimen_delta_template.json"
SPEC = importlib.util.spec_from_file_location("cd57_fit_pot_tapers", Path(__file__).with_name("fit_pot_tapers.py"))
assert SPEC is not None and SPEC.loader is not None
POT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(POT)
CalibrationError, make_delta, main, read_resistances = (
    POT.CalibrationError, POT.make_delta, POT.main, POT.read_resistances
)


def write_csv(path: Path, *, bad: str = "", tone: bool = False) -> None:
    fields = ["rotation", "instrument_ohm", "mic_ohm"] + (["tone_ohm"] if tone else [])
    with path.open("w", newline="", encoding="utf-8") as file:
        writer = csv.DictWriter(file, fieldnames=fields)
        writer.writeheader()
        for i in range(11):
            alpha_a = (i / 10) ** 2.3
            alpha_b = (i / 10) ** 1.6
            if bad == "nonmonotone" and i == 6:
                alpha_a = 0.01
            if bad == "endpoint":
                alpha_b = 0.15 + 0.85 * alpha_b
            if bad == "over" and i == 5:
                alpha_a = 1.2
            row = {"rotation": i/10, "instrument_ohm": 1000000 * alpha_a, "mic_ohm": 960000 * alpha_b}
            if tone:
                row["tone_ohm"] = 1000000 * (i/10)
            writer.writerow(row)


class PotTaperTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.csv = self.base / "pot.csv"
        self.delta = self.base / "delta.json"
        self.totals = {"instrument_ohm": 1e6, "mic_ohm": 960000.0}
        self.metadata = {"specimen_serial": "TEST-SPECIMEN-001", "instrument": "four-wire digital meter",
                         "instrument_range": "2 Mohm range", "lead_compensation": "meter leads nulled",
                         "date": "2026-11-06", "source_sha256": "abc123"}

    def args(self) -> list[str]:
        return ["--csv", str(self.csv), "--delta", str(self.delta),
                "--instrument-total-ohm", "1000000", "--mic-total-ohm", "960000",
                "--specimen-serial", "TEST-SPECIMEN-001", "--instrument", "four-wire digital meter",
                "--instrument-range", "2 Mohm range", "--lead-compensation", "meter leads nulled",
                "--date", "2026-11-06", "--template", str(TEMPLATE)]

    def test_independent_tapers_and_delta_schema(self) -> None:
        write_csv(self.csv)
        curves, digest = read_resistances(self.csv, self.totals)
        self.assertEqual(len(digest), 64)
        self.assertEqual(len(curves["instrument_ohm"]), 11)
        self.assertAlmostEqual(curves["instrument_ohm"][5], 0.5**2.3, places=10)
        self.assertAlmostEqual(curves["mic_ohm"][5], 0.5**1.6, places=10)
        result = make_delta(TEMPLATE, curves, self.totals, self.metadata)
        self.assertEqual(result["schema"], "cd57-specimen-delta-v1")
        self.assertEqual(result["specimen"]["serial"], "TEST-SPECIMEN-001")
        a = result["parameters"]["volume_pot_taper_instrument"]["measured"]
        b = result["parameters"]["volume_pot_taper_mic"]["measured"]
        self.assertNotEqual(a["value"], b["value"])
        self.assertEqual(a["unit"], "fraction_vs_angle_11pt")
        self.assertEqual(a["measured_total_resistance_ohm"], 1e6)
        self.assertIsNone(result["parameters"]["tone_pot_taper"]["measured"])

    def test_main_and_output_collision(self) -> None:
        write_csv(self.csv)
        self.assertEqual(main(self.args()), 0)
        self.assertTrue(self.delta.is_file())
        self.assertEqual(main(self.args()), 2)

    def test_optional_tone(self) -> None:
        write_csv(self.csv, tone=True)
        totals = dict(self.totals, tone_ohm=1000000)
        curves, _ = read_resistances(self.csv, totals)
        self.assertEqual(curves["tone_ohm"][5], 0.5)
        result = make_delta(TEMPLATE, curves, totals, self.metadata)
        self.assertIsNotNone(result["parameters"]["tone_pot_taper"]["measured"])

    def test_reject_nonmonotonic_curve(self) -> None:
        write_csv(self.csv, bad="nonmonotone")
        with self.assertRaisesRegex(CalibrationError, "non-monotonic"):
            read_resistances(self.csv, self.totals)

    def test_reject_bad_endstop(self) -> None:
        write_csv(self.csv, bad="endpoint")
        with self.assertRaisesRegex(CalibrationError, "end stops"):
            read_resistances(self.csv, self.totals)

    def test_reject_above_measured_total(self) -> None:
        write_csv(self.csv, bad="over")
        with self.assertRaisesRegex(CalibrationError, "above-total"):
            read_resistances(self.csv, self.totals)

    def test_reject_unspecified_tone_total(self) -> None:
        write_csv(self.csv, tone=True)
        with self.assertRaisesRegex(CalibrationError, "each CSV channel"):
            read_resistances(self.csv, self.totals)

    def test_reject_fake_date_and_blank_serial(self) -> None:
        write_csv(self.csv)
        args = self.args()
        args[args.index("--date") + 1] = "2026-99-99"
        self.assertEqual(main(args), 2)
        args = self.args()
        args[args.index("--specimen-serial") + 1] = "   "
        self.assertEqual(main(args), 2)

    def test_reject_input_overwrite_even_when_explicit(self) -> None:
        write_csv(self.csv)
        args = self.args()
        args[args.index("--delta") + 1] = str(self.csv)
        args.append("--overwrite")
        self.assertEqual(main(args), 2)

    def test_accepts_repo_manifest_merge(self) -> None:
        tool = ROOT / "build-audit/cd57_manifest"
        if not tool.is_file():
            self.skipTest("cd57_manifest has not been built in local build-audit")
        write_csv(self.csv)
        self.assertEqual(main(self.args()), 0)
        merged = self.base / "merged.json"
        run = subprocess.run([str(tool), "merge", str(ROOT / "data/parameters/cd57_nominal.json"),
                              str(self.delta), str(merged)], capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertTrue(merged.is_file())


if __name__ == "__main__":
    unittest.main(verbosity=2)
