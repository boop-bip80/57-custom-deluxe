#!/usr/bin/env python3
"""Graph-level diff of the solver netlist against the independent schematic register.

    check_netlist_against_register.py [--netlist FILE] [--register FILE] [--self-test]

The register (data/netlist/schematic_register.json) is a transcription of dossier Fig. 7 made without looking
at the netlist.  Its 'netlist_mapping' section declares how register net names and element ids map onto the
netlist's node and element names.  This script then checks, up to that declared net-name mapping:

  1. the mapping is a bijection and covers every register net and every netlist node (or waives it with a reason);
  2. every register element is either matched to a netlist element or waived with a reason;
  3. each matched element connects the same (mapped) nets, in the same roles for tubes, transformer windings and
     pot tracks, and carries the same value (declared dossier-over-drawing overrides are reported, not hidden);
  4. every netlist element that the register does not account for is declared as an extra, carries an evidence tag
     from the allowed set for its category (never a drawn-circuit tag such as HIST/CONF for something that is not
     on the drawing) and touches only the nets its category allows;
  5. net by net, the multiset of attached parts is identical on both sides (independent of the pairwise matching).

Exit status 0 = identical graph and values, 1 = at least one discrepancy.  --self-test applies deliberate
mutations to the netlist text in memory and requires every one to be detected (no broken file is stored).
Standard library only.
"""
import argparse
import collections
import json
import math
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def parse_value(s):
    m = re.match(r"^[-+]?(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?", s)
    if not m:
        raise ValueError("bad number %r" % s)
    v = float(m.group(0))
    suf = s[m.end():].lower()
    if suf.startswith("meg"):
        return v * 1e6
    mult = {"t": 1e12, "g": 1e9, "k": 1e3, "m": 1e-3, "u": 1e-6, "n": 1e-9, "p": 1e-12, "f": 1e-15}
    return v * (mult.get(suf[0], 1.0) if suf else 1.0)


class Elem:
    def __init__(self, kind, name, nodes, value, attrs, roles=None):
        self.kind, self.name, self.nodes, self.value, self.attrs = kind, name, nodes, value, attrs
        self.roles = roles or []

    @property
    def ev(self):
        return self.attrs.get("ev", "")

    @property
    def dyn(self):
        return "dyn" in self.attrs


def parse_netlist(text):
    elems = collections.OrderedDict()
    for raw in text.splitlines():
        line = raw.split("#", 1)[0]
        tok = line.split()
        if not tok or tok[0].startswith("."):
            continue
        attrs, pos = {}, []
        for t in tok[1:]:
            if "=" in t:
                k, v = t.split("=", 1)
                attrs[k] = v
            elif t in ("dyn", "novar"):
                attrs[t] = "1"
            else:
                pos.append(t)
        kind, name = tok[0], pos[0]
        if kind in ("R", "C", "L"):
            e = Elem(kind, name, pos[1:3], parse_value(pos[3]), attrs, ["a", "b"])
        elif kind == "TRIODE":
            e = Elem(kind, name, pos[1:4], None, attrs, ["plate", "grid", "cathode"])
        elif kind == "PENTODE":
            e = Elem(kind, name, pos[1:5], None, attrs, ["plate", "grid", "screen", "cathode"])
        elif kind == "DIODE":
            e = Elem(kind, name, pos[1:3], None, attrs, ["anode", "cathode"])
        elif kind == "XFMR":
            keys = ["a1", "a2", "b1", "b2", "s1", "s2"]
            e = Elem(kind, name, [attrs[k] for k in keys], None, attrs, keys)
        else:
            raise ValueError("unknown element type " + kind)
        if name in elems:
            raise ValueError("duplicate element " + name)
        elems[name] = e
    return elems


def close(a, b):
    return math.isclose(a, b, rel_tol=1e-9, abs_tol=0.0)


def fmt(x):
    return "%.6g" % x


def check(reg, elems):
    """Return (problems, notes)."""
    problems, notes = [], []
    mp = reg["netlist_mapping"]
    nets_map = mp["nets"]
    inv = {}
    for rn, nn in nets_map.items():
        if nn in inv:
            problems.append("net mapping is not injective: %s and %s both map to %s" % (inv[nn], rn, nn))
        inv[nn] = rn

    # 1. net coverage -----------------------------------------------------------------------------------------
    for rn in reg["nets"]:
        if rn not in nets_map and rn not in mp["unmapped_nets"]:
            problems.append("register net %s is neither mapped nor waived" % rn)
    for rn in list(nets_map) + list(mp["unmapped_nets"]):
        if rn not in reg["nets"]:
            problems.append("mapping names unknown register net %s" % rn)
    nl_nodes = {n for e in elems.values() for n in e.nodes}
    for n in sorted(nl_nodes):
        if n not in inv and n not in mp["extra_nodes"]:
            problems.append("netlist node %s is neither mapped to a register net nor declared extra" % n)
    for rn, nn in nets_map.items():
        if nn not in nl_nodes:
            problems.append("register net %s maps to %s which no netlist element uses" % (rn, nn))

    def m(rnet):
        if rnet not in nets_map:
            raise KeyError(rnet)
        return nets_map[rnet]

    claimed = {}

    def claim(name, why):
        if name not in elems:
            problems.append("%s: netlist element %s does not exist" % (why, name))
            return None
        if name in claimed:
            problems.append("%s: netlist element %s already claimed by %s" % (why, name, claimed[name]))
        claimed[name] = why
        return elems[name]

    def ev_ok(e, why):
        allowed = mp["allowed_evidence"]["overrides"].get(e.name, mp["allowed_evidence"]["default_RC"])
        if e.ev not in allowed:
            problems.append("%s: %s carries ev=%s, expected one of %s" % (why, e.name, e.ev or "(none)", allowed))

    def same_pair(e, ra, rb, why):
        want = frozenset((m(ra), m(rb)))
        got = frozenset(e.nodes)
        if want != got:
            problems.append("%s: %s connects %s but the schematic connects %s (%s-%s)"
                            % (why, e.name, "-".join(e.nodes), "-".join(sorted(want)), ra, rb))

    def value_ok(e, rid, regval, why):
        ovr = mp["value_overrides"].get(rid)
        if ovr:
            if not close(e.value, ovr["netlist_value"]):
                problems.append("%s: %s = %s, declared dossier-default is %s" % (why, e.name, fmt(e.value), fmt(ovr["netlist_value"])))
            else:
                notes.append("VALUE OVERRIDE %s: netlist %s = %s (dossier default); drawing prints %s. %s"
                             % (rid, e.name, fmt(e.value), fmt(ovr["drawing_value"]), ovr["reason"]))
            return
        if not close(e.value, regval):
            problems.append("%s: %s = %s but the schematic prints %s" % (why, e.name, fmt(e.value), fmt(regval)))

    # 2./3. element-by-element ------------------------------------------------------------------------------------
    accounted = set()
    for part in ("resistors", "capacitors"):
        kind = "R" if part == "resistors" else "C"
        for c in reg[part]:
            rid = c["id"]
            accounted.add(rid)
            if rid not in mp["elements"]:
                problems.append("register element %s has no netlist mapping" % rid)
                continue
            e = claim(mp["elements"][rid], rid)
            if e is None:
                continue
            if e.kind != kind:
                problems.append("%s: %s is a %s in the netlist, a %s on the schematic" % (rid, e.name, e.kind, kind))
                continue
            same_pair(e, c["nets"][0], c["nets"][1], rid)
            value_ok(e, rid, c["value"], rid)
            ev_ok(e, rid)

    for p in reg["potentiometers"]:
        rid = p["id"]
        accounted.add(rid)
        tr = mp["pots"].get(rid)
        if not tr:
            problems.append("register pot %s has no netlist mapping" % rid)
            continue
        total = 0.0
        for end in ("end_a", "end_b"):
            e = claim(tr[end], rid + "." + end)
            if e is None:
                continue
            same_pair(e, p["wiper"], p[end], rid + "." + end)
            ev_ok(e, rid + "." + end)
            total += e.value
        if not close(total, p["value"]):
            problems.append("%s: track resistances add to %s, the schematic prints %s" % (rid, fmt(total), fmt(p["value"])))

    for t in reg["tubes"]:
        rid = t["id"]
        accounted.add(rid)
        ev = t["electrodes"]
        if rid == "V5":
            rect = mp["rectifier"]["V5"]
            for pl, net in (("plate_1", "plate_1"), ("plate_2", "plate_2")):
                e = claim(rect[pl], "V5." + pl)
                if e is None:
                    continue
                if e.kind != "DIODE":
                    problems.append("V5.%s: %s is not a DIODE" % (pl, e.name))
                    continue
                if e.nodes[0] != m(ev[net]) or e.nodes[1] != m(rect["cathode_dc_net"]):
                    problems.append("V5.%s: %s runs %s->%s, schematic plate %s -> cathode/%s (%s -> %s)"
                                    % (pl, e.name, e.nodes[0], e.nodes[1], ev[net], rect["cathode_dc_net"], m(ev[net]), m(rect["cathode_dc_net"])))
                ev_ok(e, "V5." + pl)
            if ev["cathode_dc"] != rect["cathode_dc_net"]:
                problems.append("V5: register cathode_dc net %s differs from the mapped cathode net" % ev["cathode_dc"])
            continue
        if rid not in mp["tubes"]:
            problems.append("register tube %s has no netlist mapping" % rid)
            continue
        e = claim(mp["tubes"][rid], rid)
        if e is None:
            continue
        want_kind = "PENTODE" if "screen" in ev else "TRIODE"
        if e.kind != want_kind:
            problems.append("%s: %s is a %s, the schematic tube has %s electrodes" % (rid, e.name, e.kind, "4" if "screen" in ev else "3"))
            continue
        for role, node in zip(e.roles, e.nodes):
            if role not in ev:
                problems.append("%s: netlist role %s has no register electrode" % (rid, role))
            elif m(ev[role]) != node:
                problems.append("%s: netlist %s of %s is node %s, schematic %s is %s (%s)" % (rid, role, e.name, node, role, ev[role], m(ev[role])))
        if e.kind == "TRIODE" and e.attrs.get("type") != t["type"]:
            problems.append("%s: netlist tube type %s, schematic %s" % (rid, e.attrs.get("type"), t["type"]))
        ev_ok(e, rid)

    for x in reg["transformers"]:
        accounted.add(x["id"])
        if x["id"] == "OT":
            o = mp["transformers"]["OT"]
            e = claim(o["element"], "OT")
            if e is None:
                continue
            w = {w["name"]: w for w in x["windings"]}
            pri, sec = w["primary"], w["secondary"]
            want = {o["primary_a"]: pri["ends"][0], o["primary_ct"]: pri["centre_tap"], o["primary_ct_b"]: pri["centre_tap"],
                    o["primary_b"]: pri["ends"][1], o["secondary_hot"]: sec["ends"][0], o["secondary_cold"]: sec["ends"][1]}
            for role, rnet in want.items():
                got = e.attrs.get(role)
                if got != m(rnet):
                    problems.append("OT: winding terminal %s is node %s, schematic net %s is %s" % (role, got, rnet, m(rnet)))
            ev_ok(e, "OT")
        else:
            pt = mp["transformers"]["PT_HV"]
            hv = [w for w in x["windings"] if w["name"] == "HV secondary"][0]
            for name, rend in zip(pt["elements"], hv["ends"]):
                e = claim(name, "PT.HV secondary")
                if e is None:
                    continue
                if frozenset(e.nodes) != frozenset((m(rend), m(hv["centre_tap"]))):
                    problems.append("PT.HV secondary: %s connects %s, schematic half-winding runs %s to the centre tap (%s-%s)"
                                    % (name, "-".join(e.nodes), rend, m(rend), m(hv["centre_tap"])))
                ev_ok(e, "PT.HV secondary")
            for w in x["windings"]:
                if w["name"] != "HV secondary":
                    key = "PT." + w["name"]
                    if key not in mp["not_modelled"]:
                        problems.append("%s is neither mapped nor waived" % key)

    jmap = mp["jacks"]
    for j in reg["jacks"]:
        rid = j["id"]
        accounted.add(rid)
        if rid in jmap:
            e = claim(jmap[rid], rid)
            if e is None:
                continue
            same_pair(e, j["tip"], j["switch"], rid + " switch lug")
            ev_ok(e, rid)
        elif rid == "J_XSPK":
            e = claim(mp["speaker_equivalent"]["J_XSPK"], rid)
            if e is not None:
                same_pair(e, j["tip"], j["sleeve"], rid)
                ev_ok(e, rid)
        elif rid not in mp["not_modelled"]:
            problems.append("register jack %s is neither mapped nor waived" % rid)
    accounted.add("SPK")
    spk = mp["speaker_equivalent"]["SPK"]
    for name in spk:
        e = claim(name, "SPK")
        if e is not None:
            ev_ok(e, "SPK")
    if all(n in elems for n in spk):
        d = {n: elems[n] for n in spk}
        sp, gnd = m(reg["speaker"]["nets"][0]), m(reg["speaker"]["nets"][1])
        if sp not in d["Re"].nodes:
            problems.append("SPK: voice-coil resistance %s does not start at the speaker hot net %s" % ("Re", sp))
        sx = [n for n in d["Re"].nodes if n != sp]
        if not sx or sx[0] not in d["Le"].nodes:
            problems.append("SPK: Re and Le are not in series")
        else:
            sy = [n for n in d["Le"].nodes if n != sx[0]]
            for n in ("Res", "Cmes", "Lces"):
                if frozenset(d[n].nodes) != frozenset((sy[0], gnd)):
                    problems.append("SPK: motional element %s does not connect %s to ground" % (n, sy[0]))

    for sid in ("SW_AC", "F1", "PLUG"):
        accounted.add(sid)
    for key, why in mp["not_modelled"].items():
        notes.append("WAIVED %s: %s" % (key, why))
    all_ids = set()
    for sect in ("resistors", "capacitors", "potentiometers", "tubes", "transformers", "jacks", "switches", "fuses"):
        for it in reg[sect]:
            all_ids.add(it["id"])
    all_ids.update(("PLUG", "SPK"))
    for rid in sorted(all_ids - accounted):
        problems.append("register element %s is not accounted for by the checker" % rid)
    for key in mp["not_modelled"]:
        if key not in all_ids and not key.startswith("PT."):
            problems.append("not_modelled names unknown register element %s" % key)

    # 4. extras --------------------------------------------------------------------------------------------------
    for ex in mp["extras"]:
        names = list(ex.get("names", []))
        if "pattern" in ex:
            names += [n for n in elems if re.search(ex["pattern"], n)]
        for n in names:
            if n not in elems:
                problems.append("extra %s (%s) does not exist in the netlist" % (n, ex["category"]))
                continue
            if n in claimed:
                problems.append("extra %s (%s) is also claimed as %s" % (n, ex["category"], claimed[n]))
            claimed[n] = "extra/" + ex["category"]
            e = elems[n]
            if e.ev not in ex["allowed_evidence"]:
                problems.append("EVIDENCE TAG: %s (%s, not on the drawing) carries ev=%s, allowed %s"
                                % (n, ex["category"], e.ev or "(none)", ex["allowed_evidence"]))
            if "touches_register_nets_only" in ex:
                for node in e.nodes:
                    r = inv.get(node)
                    if r is not None and r not in ex["touches_register_nets_only"]:
                        problems.append("extra %s (%s) touches drawn net %s (%s); allowed %s"
                                        % (n, ex["category"], r, node, ex["touches_register_nets_only"]))
    for n in elems:
        if n not in claimed:
            problems.append("netlist element %s is not on the schematic and not declared as an extra" % n)

    # interelectrode caps must sit between two electrodes of one tube
    tube_nodes = [(e.name, set(e.nodes)) for e in elems.values() if e.kind in ("TRIODE", "PENTODE")]
    for ex in mp["extras"]:
        if ex["category"] != "interelectrode":
            continue
        for n in [n for n in elems if re.search(ex["pattern"], n)]:
            if not any(set(elems[n].nodes) <= s and len(set(elems[n].nodes)) == 2 for _, s in tube_nodes):
                problems.append("interelectrode cap %s (%s) does not connect two electrodes of one tube" % (n, "-".join(elems[n].nodes)))

    # 5. net-by-net signature (independent of the pairwise matching) -------------------------------------------
    def sig_reg():
        s = collections.defaultdict(list)
        vo = mp["value_overrides"]
        for c in reg["resistors"]:
            for n in c["nets"]:
                s[n].append("R %s" % fmt(c["value"]))
        for c in reg["capacitors"]:
            v = vo[c["id"]]["netlist_value"] if c["id"] in vo else c["value"]
            for n in c["nets"]:
                s[n].append("C %s" % fmt(v))
        for p in reg["potentiometers"]:
            for end in ("end_a", "end_b"):
                for n in (p["wiper"], p[end]):
                    s[n].append("R track")
        for t in reg["tubes"]:
            if t["id"] == "V5":
                for pl in ("plate_1", "plate_2"):
                    s[t["electrodes"][pl]].append("DIODE anode")
                s[t["electrodes"]["cathode_dc"]].append("DIODE cathode")
                s[t["electrodes"]["cathode_dc"]].append("DIODE cathode")
                continue
            kind = "PENTODE" if "screen" in t["electrodes"] else "TRIODE"
            for role, n in t["electrodes"].items():
                s[n].append("%s %s" % (kind, role))
        for x in reg["transformers"]:
            if x["id"] == "OT":
                w = {w["name"]: w for w in x["windings"]}
                for role, n in (("a1", w["primary"]["ends"][0]), ("a2", w["primary"]["centre_tap"]), ("b1", w["primary"]["centre_tap"]),
                                ("b2", w["primary"]["ends"][1]), ("s1", w["secondary"]["ends"][0]), ("s2", w["secondary"]["ends"][1])):
                    s[n].append("XFMR " + role)
            else:
                hv = [w for w in x["windings"] if w["name"] == "HV secondary"][0]
                for end in hv["ends"]:
                    s[end].append("R track")
                    s[hv["centre_tap"]].append("R track")
        for j in reg["jacks"]:
            if j["id"] in jmap:
                s[j["tip"]].append("R track")
                s[j["switch"]].append("R track")
            elif j["id"] == "J_XSPK":
                s[j["tip"]].append("R track")
                s[j["sleeve"]].append("R track")
        return s

    def sig_nl():
        s = collections.defaultdict(list)
        for e in elems.values():
            if claimed.get(e.name, "").startswith("extra/"):
                continue
            if e.name in mp["speaker_equivalent"]["SPK"]:
                continue
            for role, n in zip(e.roles, e.nodes):
                r = inv.get(n)
                if r is None:
                    continue
                if e.kind in ("R", "C"):
                    if e.name in mp["dynamic_values"] or e.name == mp["speaker_equivalent"]["J_XSPK"]:
                        s[r].append("R track")
                    elif e.name in mp["transformers"]["PT_HV"]["elements"]:
                        s[r].append("R track")
                    else:
                        s[r].append("%s %s" % (e.kind, fmt(e.value)))
                elif e.kind == "XFMR":
                    s[r].append("XFMR " + role)
                elif e.kind == "DIODE":
                    s[r].append("DIODE " + role)
                else:
                    s[r].append("%s %s" % (e.kind, role))
        return s

    sr, sn = sig_reg(), sig_nl()
    # speaker equivalent stands in for the register's SPK part: one leg on SPK_HOT, one on GND
    sr["SPK_HOT"].append("SPK leg")
    sr["GND"].append("SPK leg")
    for rn in sorted(nets_map):
        a = sorted(sr.get(rn, []))
        b = sorted(sn.get(rn, []))
        if rn == "SPK_HOT" or rn == "GND":
            a = [x for x in a if x != "SPK leg"]
        if a != b:
            only_s = sorted((collections.Counter(a) - collections.Counter(b)).elements())
            only_n = sorted((collections.Counter(b) - collections.Counter(a)).elements())
            problems.append("NET SIGNATURE %s (%s): only on the schematic %s, only in the netlist %s" % (rn, nets_map[rn], only_s, only_n))
    return problems, notes


def mutations(text):
    """Deliberately broken variants of the netlist text; every one must be detected."""
    def sub(old, new):
        assert old in text, old
        return text.replace(old, new, 1)
    return [
        ("wrong value (Rk1 820 -> 680)", sub("R Rk1    k1 0   820 ", "R Rk1    k1 0   680 ")),
        ("grid leak returned to cathode instead of ground (Rgl_3)", sub("R Rgl_3  c3 0   220k", "R Rgl_3  c3 k34 220k")),
        ("cathodyne grid return grounded instead of bootstrapped", sub("R Rgl_2b g2b j  1Meg", "R Rgl_2b g2b 0  1Meg")),
        ("6V6 screens on B+3 instead of B+2", sub("PENTODE V3 p3 g3 b2 k34", "PENTODE V3 p3 g3 b3 k34")),
        ("V2B plate load on B+2 instead of B+3", sub("R Rpl_2b b3 p2b", "R Rpl_2b b2 p2b")),
        ("tone cap to ground replaced by a direct ground on tone end tb", sub("C Ctone   tb 0  5n ", "C Ctone   tb tb 5n ")),
        ("bright cap on the mix node instead of the Bright wiper", sub("C Cbright ta wb 500p", "C Cbright ta m 500p")),
        ("jack 2 switch ties to ground instead of the Hi node", sub("R Sw_b2   b_t2 b_t1", "R Sw_b2   b_t2 0   ")),
        ("missing element (Rgs_4 removed)", sub("R Rgs_4  c4 g4  1.5k tol=0.05 ev=HIST src=S03 note=grid_stopper\n", "")),
        ("extra element on a drawn net", sub("R Rdrop2 b2 b3 22k", "R Rstray b3 k1 1Meg ev=HIST\nR Rdrop2 b2 b3 22k")),
        ("interelectrode cap mislabelled HIST", sub("C Cgk_1b b_g k1  1.6p ev=DATASHEET", "C Cgk_1b b_g k1  1.6p ev=HIST")),
        ("source chain mislabelled CONF", sub("R Rsrc   pu 0   100   dyn ev=PROP", "R Rsrc   pu 0   100   dyn ev=CONF")),
        ("OT secondary not grounded", sub("s1=sp s2=0 ", "s1=sp s2=sp ")),
        ("V1 cathodes split", sub("TRIODE V1A p1n n_g k1", "TRIODE V1A p1n n_g kb2")),
    ]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--netlist", default=None)
    ap.add_argument("--register", default=os.path.join(ROOT, "data", "netlist", "schematic_register.json"))
    ap.add_argument("--self-test", action="store_true", help="require every deliberate mutation to be detected")
    args = ap.parse_args()
    with open(args.register) as f:
        reg = json.load(f)
    netlist = args.netlist or os.path.join(ROOT, reg["netlist_mapping"]["netlist"])
    with open(netlist) as f:
        text = f.read()

    if args.self_test:
        base_problems, _ = check(reg, parse_netlist(text))
        if base_problems:
            print("self-test needs a clean baseline; the unmodified netlist has %d discrepancies" % len(base_problems))
            return 1
        bad = 0
        for label, broken in mutations(text):
            problems, _ = check(reg, parse_netlist(broken))
            if problems:
                print("detected  %-62s (%d findings, e.g. %s)" % (label, len(problems), problems[0][:90]))
            else:
                print("MISSED    %s" % label)
                bad += 1
        print("self-test: %d mutations, %d missed" % (len(mutations(text)), bad))
        return 1 if bad else 0

    problems, notes = check(reg, parse_netlist(text))
    for n in notes:
        print("note: " + n)
    for p in problems:
        print("DISCREPANCY: " + p)
    n_el = len(parse_netlist(text))
    print("%s: %d netlist elements vs register %s: %s" % (os.path.relpath(netlist, ROOT) if netlist.startswith(ROOT) else netlist, n_el,
                                                       os.path.basename(args.register), "%d discrepancies" % len(problems) if problems else "graph and values identical (declared overrides and waivers listed above)"))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
