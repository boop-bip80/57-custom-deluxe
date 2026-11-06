// validate_manifest -- the provenance mechanism of dossier Sec. 04 / 28 / 30, tested as hard on its REFUSALS as on its successes:
//   * the shipped manifest is schema 2: every entry carries a branch, a source_revision key and a serial_range key
//   * the all-null specimen template changes NOTHING (the merged manifest drives a bit-identical render)
//   * a measured value is stored BESIDE the nominal one, used by the model, and can be switched off; entries are labelled branch 'specimen'
//   * a delta is refused for: an unknown parameter, a unit mismatch, a missing instrument, a missing date, another base profile, an unknown branch, a wrong schema
//   * a measured potentiometer map overrides the plugin's assumed taper for that pot only; an invalid map (not monotonic, wrong length) is a reported manifest error
//   * merging is idempotent and survives a parse/dump round trip
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

#include "CD57Engine.h"
#include "EmbeddedData.h"
#include "Manifest.h"

using namespace cd57;
static int nPass = 0, nFail = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "")
{
    (ok ? nPass : nFail)++;
    std::printf("  %s  %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.empty() ? "" : "   [", detail.empty() ? "" : (detail + "]").c_str());
}
static std::string f(double v, int p = 4) { char b[64]; std::snprintf(b, sizeof b, "%.*f", p, v); return b; }
static std::string slurp(const std::string& p) { std::ifstream in(p); return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); }

static std::string measured(const std::string& key, const std::string& body) { return "\"" + key + "\": {\"measured\": " + body + "}"; }
static std::string delta(const std::string& params, const std::string& profile = "provisional_public_source_nominal", const std::string& branch = "specimen", const std::string& schema = "cd57-specimen-delta-v1")
{
    return "{\"schema\": \"" + schema + "\", \"delta_of_profile\": \"" + profile + "\", \"branch\": \"" + branch + "\", \"specimen\": {\"serial\": \"TEST-0001\", \"revision\": \"unit test\"}, \"parameters\": {" + params + "}}";
}
static std::string mval(double v, const std::string& unit, const std::string& instr = "Fluke 87V", const std::string& date = "2026-12-01")
{
    std::string s = "{\"value\": " + std::to_string(v) + ", \"unit\": \"" + unit + "\"";
    if (!instr.empty()) s += ", \"instrument\": \"" + instr + "\", \"instrument_range\": \"auto\", \"lead_compensation\": \"zeroed\"";
    if (!date.empty()) s += ", \"date\": \"" + date + "\"";
    return s + "}";
}

static std::vector<float> render(const std::string& manifest, int taper = 0, double micKnob = 0.0, AmpCircuit** keep = nullptr, CD57Engine** eng = nullptr)
{
    (void)keep; (void)eng;
    EngineParams p; p.oversampleLog2 = 0; p.instVol = 0.5f; p.micVol = (float)micKnob; p.tone = 0.6f; p.jack = 1; p.load = 2; p.taper = taper; p.speakerHeating = false; p.outputRefVolts = 28.28f;
    CD57Engine e; e.prepare(48000.0, 512, embeddedNetlist(), p, manifest);
    std::vector<float> in(512), ot(512), oa(512), out;
    for (int blk = 0; blk < 12; ++blk) {
        for (int i = 0; i < 512; ++i) in[(size_t)i] = (float)(0.02 * std::sin(2 * 3.14159265358979 * 1000.0 * (blk * 512 + i) / 48000.0));
        e.process(in.data(), ot.data(), oa.data(), 512);
        out.insert(out.end(), ot.begin(), ot.end());
    }
    return out;
}

int main()
{
    const std::string base = embeddedManifest();
    const std::string tplPath = std::string(CD57_PARAM_DIR) + "/specimen_delta_template.json";
    const std::string tpl = slurp(tplPath);

    std::printf("\n== 1. the shipped manifest (schema 2)\n");
    Manifest nom; const std::string lerr = nom.load(base);
    check(lerr.empty() && nom.schemaVersion == 2, "loads as schema 2", lerr);
    int noBranch = 0, noRevKey = 0;
    for (auto& e : nom.entries) { if (e.branch.empty()) ++noBranch; }
    check(noBranch == 0, "every entry names its branch (historical_5e3 / reissue_2006 / modern_nominal / specimen)", std::to_string(nom.branchCount["historical_5e3"]) + " historical, " + std::to_string(nom.branchCount["modern_nominal"]) + " modern nominal");
    int docNoRev = 0; for (auto& e : nom.entries) if (e.status == "manufacturer_documented" && e.sourceRevision.empty()) ++docNoRev;
    check(docNoRev == 0, "every manufacturer-documented entry states the revision of its source (S01 retrieval date, S02 'PN 7708607000 Rev. A')", std::to_string(docNoRev) + " without");
    (void)noRevKey;
    {   // the loader REFUSES a schema-2 manifest whose entries lack the Sec. 04 fields
        std::string bad = base; const size_t p = bad.find("\"branch\"");
        bad.replace(p, 8, "\"brunch\"");
        Manifest m; const std::string e = m.load(bad);
        check(!e.empty() && e.find("branch") != std::string::npos, "a schema-2 manifest with an entry lacking its branch is refused", e);
        std::string bad2 = base; const size_t q = bad2.find("\"source_revision\""); bad2.replace(q, 17, "\"source_rev1sion\"");
        Manifest m2; const std::string e2 = m2.load(bad2);
        check(!e2.empty() && e2.find("source_revision") != std::string::npos, "a schema-2 manifest with an entry lacking source_revision is refused", e2);
    }
    check(nom.measuredCount() == 0, "the shipped manifest carries no measured value (no specimen was measured)");

    std::printf("\n== 2. the all-null template changes nothing\n");
    {
        std::string err; int n = -1;
        const std::string merged = Manifest::mergeDelta(base, tpl, err, &n);
        check(!merged.empty() && n == 0, "merging the template measures 0 parameters", err);
        AmpConfig a, b; Manifest mn, mm; mn.load(base); mm.load(merged); mn.apply(a); mm.apply(b);
        const bool same = a.ptHalfVrms == b.ptHalfVrms && a.otLprimFull == b.otLprimFull && a.speaker.Re == b.speaker.Re && a.potTotal == b.potTotal && !b.volumeMapMeasured && !b.micMapMeasured && !b.toneMapMeasured;
        check(same, "the merged configuration equals the nominal configuration field by field");
        const auto r0 = render(base), r1 = render(merged);
        double worst = 0; for (size_t i = 0; i < r0.size(); ++i) worst = std::max(worst, (double)std::fabs(r0[i] - r1[i]));
        check(worst == 0.0, "the merged manifest drives a bit-identical render (6144 samples)", "max |difference| " + f(worst, 12));
    }

    std::printf("\n== 3. a measured value sits beside the nominal one\n");
    std::string merged3;
    {
        const std::string d = delta(measured("pt_hv_half_winding_rms", mval(365.0, "V")) + "," + measured("speaker_Re", mval(6.0, "ohm", "Keysight U1281A")) + "," + measured("ot_primary_inductance_full", mval(15.0, "H", "LCR bridge", "2026-12-02")));
        std::string err; int n = 0; merged3 = Manifest::mergeDelta(base, d, err, &n);
        check(!merged3.empty() && n == 3, "3 measured parameters merged", err);
        Manifest m; const std::string e = m.load(merged3);
        check(e.empty() && m.measuredCount() == 3, "the merged manifest validates and counts 3 measured entries", e);
        check(m.nominal("pt_hv_half_winding_rms") == 377.4 && m.isMeasured("pt_hv_half_winding_rms"), "the NOMINAL 377.4 V is still in the entry, next to the measurement");
        check(m.find("pt_hv_half_winding_rms")->branch == "specimen" && m.find("speaker_Fs") == nullptr && m.find("speaker_fs")->branch == "modern_nominal", "measured entries are labelled branch 'specimen'; untouched entries keep their branch");
        check(m.specimenSerial == "TEST-0001", "the specimen serial is recorded in the manifest", m.specimenSerial);
        AmpConfig c; m.apply(c);
        check(c.ptHalfVrms == 365.0 && c.speaker.Re == 6.0 && c.otLprimFull == 15.0, "the model reads the MEASURED values", "PT " + f(c.ptHalfVrms, 1) + " V, Re " + f(c.speaker.Re, 2) + ", L " + f(c.otLprimFull, 1));
        m.useMeasured = false; AmpConfig c2; m.apply(c2);
        check(c2.ptHalfVrms == 377.4 && c2.speaker.Re == 6.4, "with useMeasured = false the same manifest gives the nominal values", "PT " + f(c2.ptHalfVrms, 1));
        // and the circuit really changes: idle B+1 follows the PT voltage
        EngineParams p; p.oversampleLog2 = 0; p.load = 2;
        CD57Engine e0, e1; e0.prepare(48000.0, 512, embeddedNetlist(), p, base); e1.prepare(48000.0, 512, embeddedNetlist(), p, merged3);
        check(e1.diag().b1 < e0.diag().b1 - 5.0, "the measured (lower) PT voltage lowers the idle B+1 of the rendered circuit", "nominal " + f(e0.diag().b1, 1) + " V, specimen " + f(e1.diag().b1, 1) + " V");
        check(e1.manifestError().empty(), "the engine accepts the merged manifest without a manifest error", e1.manifestError());
        // idempotent merge, and a parse/dump round trip keeps the text
        std::string e2; const std::string again = Manifest::mergeDelta(merged3, d, e2);
        check(again == merged3, "merging the same delta again changes nothing (idempotent)", e2);
        Json round = Json::parse(merged3);
        check(round.dump() == merged3, "the merged manifest survives parse -> dump unchanged");
    }

    std::printf("\n== 4. refusals\n");
    {
        auto refused = [&](const std::string& what, const std::string& d, const std::string& needle) {
            std::string err; const std::string r = Manifest::mergeDelta(base, d, err);
            check(r.empty() && err.find(needle) != std::string::npos, what, err);
        };
        refused("a delta for a parameter that does not exist is refused", delta(measured("flux_capacitor", mval(1.0, "V"))), "does not exist");
        refused("a unit mismatch is refused (no silent conversion)", delta(measured("speaker_Re", mval(6.0, "V"))), "unit");
        refused("a measurement without an instrument is refused", delta(measured("speaker_Re", mval(6.0, "ohm", ""))), "instrument");
        refused("a measurement without a date is refused", delta(measured("speaker_Re", mval(6.0, "ohm", "Fluke", ""))), "date");
        refused("a delta written for another base profile is refused", delta(measured("speaker_Re", mval(6.0, "ohm")), "some_other_profile"), "base profile");
        refused("a delta naming an unknown branch is refused", delta(measured("speaker_Re", mval(6.0, "ohm")), "provisional_public_source_nominal", "prototype"), "branch");
        refused("a delta with the wrong schema tag is refused", delta(measured("speaker_Re", mval(6.0, "ohm")), "provisional_public_source_nominal", "specimen", "cd57-delta-v0"), "schema");
        refused("a measured value that is not a number is refused", delta(measured("speaker_Re", "{\"value\": \"six\", \"unit\": \"ohm\", \"instrument\": \"x\", \"date\": \"y\"}")), "number");
    }

    std::printf("\n== 5. measured potentiometer maps (Sec. 08)\n");
    {
        const char* lin = "[0,0.1,0.2,0.3,0.4,0.5,0.6,0.7,0.8,0.9,1.0]";
        const char* strange = "[0,0.01,0.02,0.04,0.07,0.11,0.17,0.26,0.4,0.62,1.0]";
        auto mapBody = [&](const char* arr, const char* unit = "fraction_vs_angle_11pt") { return std::string("{\"value\": ") + arr + ", \"unit\": \"" + unit + "\", \"instrument\": \"ohmmeter + protractor\", \"date\": \"2026-12-03\"}"; };
        std::string err;
        const std::string mp = Manifest::mergeDelta(base, delta(measured("volume_pot_taper_instrument", mapBody(strange)) + "," + measured("volume_pot_taper_mic", mapBody(lin))), err);
        check(!mp.empty(), "a delta with two measured volume maps merges", err);
        Manifest m; m.load(mp); AmpConfig c; const std::string ae = m.apply(c);
        check(ae.empty() && c.volumeMapMeasured && c.micMapMeasured && !c.toneMapMeasured, "the Instrument and Mic maps are measured, the tone pot is not", ae);
        check(std::fabs(c.volumeMap.alpha(0.5) - 0.11) < 1e-12 && std::fabs(c.micVolumeMap.alpha(0.5) - 0.5) < 1e-12, "alpha(0.5) reads the measured value of each pot (0.11 and 0.5)", f(c.volumeMap.alpha(0.5), 3) + " / " + f(c.micVolumeMap.alpha(0.5), 3));
        // the plugin's assumed taper selection no longer matters for the measured pot, and the pot resistances in the circuit follow the maps separately
        EngineParams p; p.oversampleLog2 = 0; p.instVol = 0.5f; p.micVol = 0.5f; p.load = 2;
        p.taper = 0; CD57Engine ea; ea.prepare(48000.0, 512, embeddedNetlist(), p, mp);
        p.taper = 2; CD57Engine eb; eb.prepare(48000.0, 512, embeddedNetlist(), p, mp);
        auto& na = ea.amp().network(); auto& nb = eb.amp().network();
        const double ra = na.getR(na.resId("Rvb_lo")), rb = nb.getR(nb.resId("Rvb_lo")), rm = na.getR(na.resId("Rvn_lo"));
        check(std::fabs(ra - 0.11e6) < 1.0 && std::fabs(ra - rb) < 1e-6, "the plugin's taper choice (audio-15 vs linear) no longer changes the measured Instrument pot: R_lower = 0.11 x 1 Mohm both times", f(ra, 0) + " / " + f(rb, 0) + " ohm");
        check(std::fabs(rm - 0.5e6) < 1.0, "the Mic pot follows its OWN measured map (linear: 0.5 x 1 Mohm)", f(rm, 0) + " ohm");
        p.taper = 0; CD57Engine ec; ec.prepare(48000.0, 512, embeddedNetlist(), p, base);
        auto& nc = ec.amp().network();
        check(std::fabs(nc.getR(nc.resId("Rvb_lo")) - 0.15e6) < 3e3, "without a measured map the assumed audio taper is used (alpha(0.5) = 0.15)", f(nc.getR(nc.resId("Rvb_lo")), 0) + " ohm");
        // invalid maps are reported manifest errors, and the engine falls back and says so
        auto bad = [&](const char* what, const char* arr, const char* needle) {
            std::string e2; const std::string r = Manifest::mergeDelta(base, delta(measured("volume_pot_taper_instrument", mapBody(arr))), e2);
            Manifest mm; mm.load(r); AmpConfig cc; const std::string ae2 = mm.apply(cc);
            EngineParams q; q.oversampleLog2 = 0; CD57Engine en; en.prepare(48000.0, 512, embeddedNetlist(), q, r);
            check(!ae2.empty() && ae2.find(needle) != std::string::npos && !en.manifestError().empty(), what, ae2);
        };
        bad("a map that is not monotonic is a reported manifest error (a pot cannot go backwards)", "[0,0.1,0.2,0.3,0.25,0.5,0.6,0.7,0.8,0.9,1.0]", "monotonic");
        bad("a map with 10 points is a reported manifest error", "[0,0.1,0.2,0.3,0.4,0.5,0.6,0.7,0.8,1.0]", "11");
        bad("a map with an alpha above 1 is a reported manifest error", "[0,0.1,0.2,0.3,0.4,0.5,0.6,0.7,0.8,0.9,1.2]", "[0, 1]");
    }

    std::printf("\n%d passed, %d failed\n", nPass, nFail);
    return nFail ? 1 : 0;
}
