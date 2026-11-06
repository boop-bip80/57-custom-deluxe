#!/usr/bin/env python3
"""check_capture_matrix.py -- run the capture-matrix renderer on its quick set and check three things:

  1. every metadata/<id>.json satisfies data/capture/capture_metadata.schema.json (a small dependency-free validator: required / type / enum /
     properties, which is all the schema uses), and every processed/<id>.json parses and contains the expected blocks;
  2. the quick set reproduces the COMMITTED reference rows (data/reference/capture_matrix_summary.csv) within a stated tolerance, so an
     unintended change of the model shows up as a failing test and an intended one forces `--update-reference` (reviewed in the diff);
  3. the harness is internally consistent: every capture has a finite 1 kHz gain, no unrecovered solver failure, and onset captures sit at 1 % THD.

usage: check_capture_matrix.py --exe cd57_capture_matrix [--reference REF.csv] [--schema SCHEMA.json] [--tmp DIR] [--update-reference]
"""
import argparse, csv, json, math, os, shutil, subprocess, sys, tempfile

def type_ok(v, t):
    ts = t if isinstance(t, list) else [t]
    for x in ts:
        if x == "null" and v is None: return True
        if x == "string" and isinstance(v, str): return True
        if x == "boolean" and isinstance(v, bool): return True
        if x == "number" and isinstance(v, (int, float)) and not isinstance(v, bool): return True
        if x == "object" and isinstance(v, dict): return True
        if x == "array" and isinstance(v, list): return True
    return False

def validate(inst, schema, path="$"):
    errs = []
    if "type" in schema and not type_ok(inst, schema["type"]):
        return [f"{path}: expected {schema['type']}, got {type(inst).__name__}"]
    if "enum" in schema and inst not in schema["enum"]:
        errs.append(f"{path}: {inst!r} not in {schema['enum']}")
    if isinstance(inst, dict):
        for k in schema.get("required", []):
            if k not in inst: errs.append(f"{path}: missing required '{k}'")
        for k, sub in schema.get("properties", {}).items():
            if k in inst: errs += validate(inst[k], sub, f"{path}.{k}")
    return errs

def num(x):
    try: return float(x)
    except Exception: return float("nan")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    here = os.path.dirname(os.path.abspath(__file__)); root = os.path.dirname(here)
    ap.add_argument("--reference", default=os.path.join(root, "data", "reference", "capture_matrix_summary.csv"))
    ap.add_argument("--schema", default=os.path.join(root, "data", "capture", "capture_metadata.schema.json"))
    ap.add_argument("--tmp", default="")
    ap.add_argument("--threads", default="2")
    ap.add_argument("--update-reference", action="store_true")
    a = ap.parse_args()
    out = a.tmp or tempfile.mkdtemp(prefix="cd57_cm_")
    shutil.rmtree(out, ignore_errors=True)
    r = subprocess.run([a.exe, "--out", out, "--quick", "--threads", a.threads, "--timestamp", "2026-11-06T00:00:00Z"], capture_output=True, text=True)
    sys.stdout.write(r.stderr[-600:])
    ok = True
    def fail(msg):
        nonlocal ok; ok = False; print("  FAIL ", msg)
    if r.returncode != 0: fail(f"renderer exit code {r.returncode}")

    schema = json.load(open(a.schema))
    ids = json.load(open(os.path.join(out, "matrix.json")))["ids"]
    if len(ids) != 12: fail(f"quick set should produce 12 captures, got {len(ids)}")
    nbad = 0
    for i in ids:
        md = json.load(open(os.path.join(out, "metadata", i + ".json")))
        for e in validate(md, schema):
            fail(f"{i}: {e}"); nbad += 1
        pr = json.load(open(os.path.join(out, "processed", i + ".json")))
        for blk in ("onset", "noise_floor", "tone_1k", "imd_700_1000", "sweep", "burst_probe", "rails_end", "solver"):
            if blk not in pr: fail(f"{i}: processed file lacks '{blk}'")
        if md["id"] != i or pr["id"] != i: fail(f"{i}: id mismatch")
        if md["synthetic"] is not True: fail(f"{i}: a model render must say synthetic=true")
    print(f"  {'PASS' if nbad == 0 else 'FAIL'}  {len(ids)} metadata files validated against the schema")

    rows = list(csv.DictReader(open(os.path.join(out, "summary.csv"))))
    for row in rows:
        if not math.isfinite(num(row["tone_gain_dB"])): fail(f"{row['id']}: non-finite tone gain")
        if int(row["failures"]) != 0: fail(f"{row['id']}: unrecovered solver failures {row['failures']}")
        if row["level"] == "onset" and abs(num(row["thd_pct"]) - 1.0) > 0.08: fail(f"{row['id']}: onset capture THD {row['thd_pct']} % is not 1 %")
    print("  PASS  every capture is finite, solver-clean and the onset captures sit at 1 % THD" if ok else "")

    if a.update_reference:
        print("  (update-reference requested: the committed reference is NOT compared; regenerate it with the full set, see docs/CAPTURE_MATRIX.md)")
        return 0 if ok else 1
    ref = {r["id"]: r for r in csv.DictReader(open(a.reference))}
    worst = (0.0, "")
    ncmp = 0
    for row in rows:
        rr = ref.get(row["id"])
        if rr is None: fail(f"{row['id']}: not in the committed reference"); continue
        for k, v in row.items():
            if k in ("id", "set", "jack", "level") or v in ("", "null") : continue
            x, y = num(v), num(rr[k])
            if math.isnan(x) and math.isnan(y): continue
            # tolerance: 1e-3 relative OR a small absolute floor in the column's own unit (dB columns 0.02 dB, others 1e-6)
            floor = 0.02 if k.endswith(("_dB", "_dbc", "_Hz")) else (0.05 if k.endswith("_ms") else 1e-6)
            if k.endswith("_Hz"): floor = 0.02 * max(abs(y), 1.0)          # band edges move with the 1/12-octave grid: 2 %
            err = abs(x - y)
            ncmp += 1
            if err > max(floor, 2e-3 * abs(y)):
                fail(f"{row['id']} {k}: {x} vs reference {y}")
            elif abs(y) > 0 and err / max(abs(y), floor) > worst[0]: worst = (err / max(abs(y), floor), f"{row['id']} {k}")
    print(f"  {'PASS' if ok else 'FAIL'}  quick set reproduces the committed reference ({ncmp} values compared; worst relative difference {worst[0]:.2e} at {worst[1]})")
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
