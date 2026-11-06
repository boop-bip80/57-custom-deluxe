#!/usr/bin/env python3
"""check_compare.py -- self-check of the Sec. 26 comparison pipeline (cd57_capture_matrix + cd57_compare).

A comparison tool that cannot fail proves nothing, so this renders a small capture set several ways and asserts what the pipeline MUST say:

  identical   the same programme rendered twice (different thread count): every metric is exactly zero and every criterion passes
              (the plumbing -- file layout, fixed delay, windowing, metric code -- introduces no error of its own)
  perturbed   the volume pots given a different taper (--taper 1): the pipeline must FAIL the clean-response, onset and harmonic criteria
              (the metrics are sensitive to a change that a listener would hear as a different control law)
  1x vs 2x    the same programme at 1x and 2x oversampling: response, onset, harmonics, IMD and burst envelope must stay inside the
              Sec. 26 limits (numerical equivalence of the two realtime candidates for the supported stimuli)

usage: check_compare.py --capture EXE --compare EXE [--tmp DIR]
"""
import argparse, json, os, shutil, subprocess, sys, tempfile

def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode not in (0,):
        sys.stdout.write(r.stdout + r.stderr); raise SystemExit(f"command failed: {' '.join(cmd)}")
    return r

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--capture", required=True); ap.add_argument("--compare", required=True); ap.add_argument("--tmp", default="")
    a = ap.parse_args()
    tmp = a.tmp or tempfile.mkdtemp(prefix="cd57_cmp_")
    shutil.rmtree(tmp, ignore_errors=True); os.makedirs(tmp)
    common = ["--quick", "--wav", "--only", "-i6m", "--timestamp", "2026-11-06T00:00:00Z"]
    for name, extra, th in (("ref", [], "2"), ("same", [], "1"), ("taper", ["--taper", "1"], "2"), ("os1", ["--os", "0"], "2")):
        run([a.capture, "--out", f"{tmp}/{name}", "--threads", th] + common + extra)
    ok = True
    def check(cond, msg):
        nonlocal ok
        print(("  PASS  " if cond else "  FAIL  ") + msg); ok = ok and cond
    def cmp(name):
        run([a.compare, "--ref", f"{tmp}/ref", "--test", f"{tmp}/{name}", "--json", f"{tmp}/{name}.json"])
        j = json.load(open(f"{tmp}/{name}.json"))
        return {c["name"].split(",")[0].split(" (")[0]: c for c in j["criteria"]}, j

    c, j = cmp("same")
    check(j["fixed_delay_samples"] == 0, "identical renders: the estimated fixed delay is 0 samples")
    check(all(x["worst"] == 0.0 for x in c.values() if x["n"] > 0), "identical renders: every metric is exactly zero (no error from the pipeline itself)")
    check(all(x["verdict"] in ("pass", "not_evaluated") for x in c.values()), "identical renders: every criterion passes")
    check(sum(1 for x in c.values() if x["n"] > 0) >= 7, "identical renders: at least 7 of the 8 criteria were actually evaluated (held-out needs the heldout set)")
    check(j["reported_not_limited"]["whole_capture_nrmse_worst"] == 0.0, "identical renders: whole-capture NRMSE is exactly zero")

    c, j = cmp("taper")
    for key in ("clean magnitude response", "clipping onset", "significant harmonics"):
        k = [n for n in c if n.startswith(key)][0]
        check(c[k]["verdict"] == "fail", f"different pot taper: '{key}' criterion FAILS (worst {c[k]['worst']:.3g} {c[k]['unit']}, limit {c[k]['limit']})")
    check(j["reported_not_limited"]["whole_capture_nrmse_worst"] > 0.05, f"different pot taper: whole-capture NRMSE exceeds the 5 % waveform limit ({100 * j['reported_not_limited']['whole_capture_nrmse_worst']:.1f} %)")

    c, j = cmp("os1")
    for key in ("clean magnitude response", "clipping onset", "significant harmonics", "intermodulation", "burst/probe envelope"):
        k = [n for n in c if n.startswith(key)][0]
        check(c[k]["verdict"] == "pass", f"1x vs 2x oversampling: '{key}' passes (worst {c[k]['worst']:.3g} {c[k]['unit']}, limit {c[k]['limit']})")
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
