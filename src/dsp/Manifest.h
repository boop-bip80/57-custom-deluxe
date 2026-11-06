// Manifest.h -- parameter manifest loader with provenance validation (dossier Sec. 04, 28, 30, 33).
//
// "A model loader should fail clearly when a mandatory calibrated field is absent, or require an explicitly selected provisional profile."  (Sec. 30)
// "Every parameter should carry a value, units, source, source revision, evidence label, uncertainty and the serial-number range to which it applies.
//  Store a measured value alongside its nominal marking rather than replacing one with the other."                                          (Sec. 04)
// "Keep four separate branches of the model: historical stock 5E3; earlier '57 Deluxe reissue; modern nominal Custom Deluxe; and measured specimen.
//  A parameter change should say which branch it modifies."  "Preserve differences as a versioned delta from the historical netlist."          (Sec. 04)
//
// This loader (1) rejects entries without a status; (2) rejects 'assumed' entries that lack a value or a sensitivity interval; (3) accepts null values
// only for status 'unknown'; (4) for schema_version >= 2 requires branch, source_revision and serial_range on every entry; (5) validates a MEASURED block
// stored beside the nominal value (value, unit, instrument, date) and uses it when 'useMeasured' is set (the default); (6) merges a versioned SPECIMEN
// DELTA onto the nominal manifest (mergeDelta) with explicit rejection of unknown keys, unit mismatches, missing instrument/date and a delta written for
// another base profile; (7) applies the numeric parameters to the engine configuration, including measured potentiometer maps; (8) summarises the
// evidence mix and the branches for the UI and the validation report.
#pragma once
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "AmpCircuit.h"
#include "Json.h"

namespace cd57 {

inline const std::set<std::string>& manifestBranches()
{
    static const std::set<std::string> b = {"historical_5e3", "reissue_2006", "modern_nominal", "specimen"};
    return b;
}

struct ManifestEntry {
    std::string name, unit, status, source, note;
    std::string branch, sourceRevision, serialRange;
    bool hasValue = false;
    double value = 0.0;                         // the NOMINAL value (never overwritten by a measurement)
    bool hasInterval = false;
    double lo = 0.0, hi = 0.0;
    // measured block, stored BESIDE the nominal value (Sec. 04)
    bool hasMeasured = false, measuredIsArray = false;
    double measured = 0.0;
    std::vector<double> measuredArray;
    std::string instrument, instrumentRange, leadCompensation, date;
};

class Manifest {
public:
    std::string target, profile, profileNote, specimenSerial;
    int schemaVersion = 1;
    bool useMeasured = true;                    // false = ignore every measured block and use the nominal values (a lab control)
    std::vector<ManifestEntry> entries;
    std::map<std::string, int> statusCount, branchCount;
private:
    mutable std::vector<std::string> read_, missing_;
public:

    // returns empty string on success, otherwise a human-readable reason (never throws)
    std::string load(const std::string& jsonText)
    {
        entries.clear(); statusCount.clear(); branchCount.clear(); read_.clear(); missing_.clear();
        Json root;
        try { root = Json::parse(jsonText); } catch (const std::exception& e) { return std::string("manifest JSON: ") + e.what(); }
        target = root.string("target"); profile = root.string("profile"); profileNote = root.string("profile_note");
        schemaVersion = (int)root.number("schema_version", 1);
        specimenSerial.clear();
        if (const Json* sp = root.get("specimen_serial")) specimenSerial = sp->type == Json::String ? sp->str : "";
        if (profile.empty()) return "manifest has no explicit 'profile' (a provisional profile must be selected explicitly)";
        const Json* params = root.get("parameters");
        if (!params || params->type != Json::Object) return "manifest has no 'parameters' object";
        for (auto& kv : params->obj) {
            ManifestEntry e; e.name = kv.first;
            const Json& j = kv.second;
            e.status = j.string("status"); e.unit = j.string("unit"); e.source = j.string("source"); e.note = j.string("note");
            if (e.status.empty()) return "parameter '" + e.name + "' has no status (provenance is mandatory)";
            const Json* v = j.get("value");
            if (v && v->isNumber()) { e.hasValue = true; e.value = v->num; }
            const bool isNull = !v || v->isNull();
            if (isNull && e.status != "unknown") return "parameter '" + e.name + "' has a null value but status '" + e.status + "' (only 'unknown' may be null)";
            // a sensitivity interval is read wherever one is given (historical-nominal pots, proposed thermal values, ...) and is MANDATORY for 'assumed'
            {
                const Json* iv = j.get("interval");
                if (iv && iv->type == Json::Array && iv->arr.size() == 2 && iv->arr[0].isNumber() && iv->arr[1].isNumber()) { e.hasInterval = true; e.lo = iv->arr[0].num; e.hi = iv->arr[1].num; }
                else if (e.status == "assumed" && v && v->isNumber()) return "assumed parameter '" + e.name + "' lacks a sensitivity interval (dossier Sec. 28)";
            }
            // Sec. 04 provenance fields: mandatory from schema 2 on (source_revision / serial_range may be null = 'not tied to one', but the KEY must be there)
            e.branch = j.string("branch");
            if (schemaVersion >= 2) {
                if (e.branch.empty()) return "parameter '" + e.name + "' has no branch (Sec. 04: a parameter must say which branch of the model it belongs to)";
                if (!j.get("source_revision")) return "parameter '" + e.name + "' lacks 'source_revision' (Sec. 04; null is allowed, absence is not)";
                if (!j.get("serial_range")) return "parameter '" + e.name + "' lacks 'serial_range' (Sec. 04; null is allowed, absence is not)";
            }
            if (!e.branch.empty() && !manifestBranches().count(e.branch)) return "parameter '" + e.name + "' names the unknown branch '" + e.branch + "'";
            e.sourceRevision = j.string("source_revision"); e.serialRange = j.string("serial_range");
            // measured block, beside the nominal value
            if (const Json* m = j.get("measured"); m && !m->isNull()) {
                const std::string err = parseMeasured(*m, e);
                if (!err.empty()) return "parameter '" + e.name + "': " + err;
            }
            ++statusCount[e.status]; ++branchCount[e.branch.empty() ? "unlabelled" : e.branch];
            entries.push_back(e);
        }
        return "";
    }

    const ManifestEntry* find(const std::string& n) const { for (auto& e : entries) if (e.name == n) return &e; return nullptr; }

    // Reads a numeric entry.  Every lookup is recorded: a key the model asks for that is absent (or null) is listed in missingKeys() instead of
    // silently falling back to a hard-coded default, and the entries the model reads are listed in readKeys() (dossier Sec. 30: provenance
    // must be machine-checkable; an entry that drives nothing must be a declared documentation entry, not an accident).
    // A scalar MEASURED value is used in preference to the nominal one when useMeasured is set; the nominal stays in the entry.
    double value(const std::string& n, double dflt) const
    {
        auto* e = find(n);
        if (e && useMeasured && e->hasMeasured && !e->measuredIsArray) { read_.push_back(n); return e->measured; }
        if (e && e->hasValue) { read_.push_back(n); return e->value; }
        missing_.push_back(n);
        return dflt;
    }
    // the nominal value of an entry irrespective of any measurement (NaN if it has none)
    double nominal(const std::string& n) const { auto* e = find(n); return e && e->hasValue ? e->value : std::nan(""); }
    bool isMeasured(const std::string& n) const { auto* e = find(n); return e && useMeasured && e->hasMeasured; }
    int measuredCount() const { int c = 0; for (auto& e : entries) if (e.hasMeasured) ++c; return c; }

    // A measured potentiometer map (Sec. 08): alpha = electrical fraction (grounded end -> wiper) at knob rotation 0, 0.1, ..., 1.0.
    // Returns true and fills 'out' if the entry carries a valid measured map; 'err' is set if it carries an INVALID one.
    bool potMap(const std::string& n, PotMap& out, std::string& err) const
    {
        auto* e = find(n);
        if (!e || !useMeasured || !e->hasMeasured) return false;
        read_.push_back(n);
        if (!e->measuredIsArray || e->measuredArray.size() != 11) { err = "'" + n + "' must be an array of 11 alpha values (knob 0, 0.1, ..., 1.0)"; return false; }
        for (size_t i = 0; i < 11; ++i) {
            const double a = e->measuredArray[i];
            if (!(a >= 0.0 && a <= 1.0)) { err = "'" + n + "' has an alpha outside [0, 1]"; return false; }
            if (i && a + 1e-12 < e->measuredArray[i - 1]) { err = "'" + n + "' is not monotonic (a pot cannot go backwards)"; return false; }
        }
        out.taper = PotTaper::Custom;
        for (size_t i = 0; i < 11; ++i) out.custom[i] = e->measuredArray[i];
        return true;
    }

    const std::vector<std::string>& readKeys() const { return read_; }
    const std::vector<std::string>& missingKeys() const { return missing_; }
    // entries that exist in the manifest but are not read by apply() (call apply() first)
    std::vector<std::string> unreadEntries() const
    {
        std::vector<std::string> out;
        for (auto& e : entries) { bool r = false; for (auto& k : read_) if (k == e.name) r = true; if (!r) out.push_back(e.name); }
        return out;
    }

    // apply known numeric parameters to the circuit configuration.  Returns an error text if a measured map is invalid (empty on success).
    std::string apply(AmpConfig& c) const
    {
        c.ptHalfVrms = value("pt_hv_half_winding_rms", c.ptHalfVrms);
        c.ptRs = value("pt_series_resistance_per_half", c.ptRs);
        c.otRatioFull = value("ot_turns_ratio_full_primary", c.otRatioFull);
        c.otLprimFull = value("ot_primary_inductance_full", c.otLprimFull);
        c.otLeakFullRef = value("ot_leakage_inductance_full_ref", c.otLeakFullRef);
        c.otDcrPrimHalf = value("ot_dcr_primary_half", c.otDcrPrimHalf);
        c.otDcrSec = value("ot_dcr_secondary", c.otDcrSec);
        c.otSatFlux = value("ot_core_saturation_flux_per_secondary_turn", c.otSatFlux);
        c.potTotal = value("pot_total_resistance", c.potTotal);
        c.volumeMap.endpointResistance = value("pot_endpoint_contact_resistance", c.volumeMap.endpointResistance);
        c.toneMap.endpointResistance = c.volumeMap.endpointResistance;
        c.micVolumeMap.endpointResistance = c.volumeMap.endpointResistance;
        c.speaker.Re = value("speaker_Re", c.speaker.Re);
        c.speaker.Le = value("speaker_Le", c.speaker.Le);
        c.speaker.fs = value("speaker_fs", c.speaker.fs);
        c.speaker.Qms = value("speaker_Qms", c.speaker.Qms);
        c.speaker.Qes = value("speaker_Qes", c.speaker.Qes);
        c.speaker.Mms = value("speaker_Mms", c.speaker.Mms);
        c.speaker.Sd = value("speaker_Sd", c.speaker.Sd);
        c.speaker.alphaT = value("speaker_voice_coil_tempco", c.speaker.alphaT);
        c.speaker.Cth = value("speaker_thermal_Cth", c.speaker.Cth);
        c.speaker.Rth = value("speaker_thermal_Rth", c.speaker.Rth);
        // measured pot maps replace the assumed tapers (Sec. 08: "the default should be a measured mapping when available")
        std::string err;
        PotMap m;
        if (potMap("volume_pot_taper_instrument", m, err)) { const double rc = c.volumeMap.endpointResistance; c.volumeMap = m; c.volumeMap.endpointResistance = rc; c.volumeMapMeasured = true; }
        if (err.empty() && potMap("volume_pot_taper_mic", m, err)) { c.micVolumeMap = m; c.micVolumeMap.endpointResistance = c.volumeMap.endpointResistance; c.micMapMeasured = true; }
        if (err.empty() && potMap("tone_pot_taper", m, err)) { const double rc = c.toneMap.endpointResistance; c.toneMap = m; c.toneMap.endpointResistance = rc; c.toneMapMeasured = true; }
        return err;
    }

    std::string summary() const
    {
        std::ostringstream o;
        o << target << " -- profile '" << profile << "'" << (specimenSerial.empty() ? "" : ", specimen " + specimenSerial) << "\n" << entries.size() << " parameters: ";
        for (auto& kv : statusCount) o << kv.second << " " << kv.first << ", ";
        o << "\nbranches: ";
        for (auto& kv : branchCount) o << kv.second << " " << kv.first << ", ";
        o << measuredCount() << " carry a measured value beside the nominal one";
        return o.str();
    }

    // ---------------------------------------------------------------------------------------------------------------------------------------
    // Specimen DELTA (Sec. 04: "Preserve differences as a versioned delta"; Sec. 28/30: measurements enter as data, never as a code edit).
    //
    //   { "schema": "cd57-specimen-delta-v1", "delta_of_profile": "<profile of the base manifest>", "branch": "specimen",
    //     "specimen": { "serial": ..., "revision": ..., "market": ..., "tube_markings": ..., "speaker_label": ... },
    //     "parameters": { "<existing key>": { "measured": { "value": <number or [11 numbers]>, "unit": "<the entry's unit>", "instrument": "...",
    //                     "instrument_range": "...", "lead_compensation": "...", "date": "...", "photo_ids": [...] } }, ... } }
    //
    // Entries whose "measured" is null are IGNORED (so the all-null template changes nothing).  The merge keeps the nominal value and its status in place,
    // adds the measured block beside it and labels the entry branch "specimen".  Returns the merged manifest text, or "" with 'err' set.
    static std::string mergeDelta(const std::string& baseText, const std::string& deltaText, std::string& err, int* merged = nullptr)
    {
        err.clear(); if (merged) *merged = 0;
        Json base, delta;
        try { base = Json::parse(baseText); } catch (const std::exception& e) { err = std::string("base manifest JSON: ") + e.what(); return ""; }
        try { delta = Json::parse(deltaText); } catch (const std::exception& e) { err = std::string("delta JSON: ") + e.what(); return ""; }
        if (delta.string("schema") != "cd57-specimen-delta-v1") { err = "delta 'schema' must be \"cd57-specimen-delta-v1\""; return ""; }
        if (delta.string("delta_of_profile") != base.string("profile")) { err = "delta is written for base profile '" + delta.string("delta_of_profile") + "' but the base manifest is '" + base.string("profile") + "'"; return ""; }
        const std::string branch = delta.string("branch", "specimen");
        if (!manifestBranches().count(branch)) { err = "delta names the unknown branch '" + branch + "'"; return ""; }
        const Json* dp = delta.get("parameters");
        if (!dp || dp->type != Json::Object) { err = "delta has no 'parameters' object"; return ""; }
        Json* bp = nullptr; for (auto& kv : base.obj) if (kv.first == "parameters") bp = &kv.second;
        if (!bp) { err = "base manifest has no parameters"; return ""; }
        int n = 0;
        for (auto& kv : dp->obj) {
            const Json* m = kv.second.get("measured");
            Json* target = nullptr; for (auto& b : bp->obj) if (b.first == kv.first) target = &b.second;
            if (!target) { err = "delta parameter '" + kv.first + "' does not exist in the base manifest (a delta may only measure existing parameters)"; return ""; }
            if (!m || m->isNull()) continue;
            ManifestEntry probe; probe.name = kv.first; probe.unit = target->string("unit");
            const std::string e2 = parseMeasured(*m, probe);
            if (!e2.empty()) { err = "delta parameter '" + kv.first + "': " + e2; return ""; }
            // set / replace the measured block and the branch label in the base entry, nominal value untouched
            bool had = false; for (auto& f : target->obj) if (f.first == "measured") { f.second = *m; had = true; }
            if (!had) target->obj.emplace_back("measured", *m);
            bool hadB = false; for (auto& f : target->obj) if (f.first == "branch") { f.second.type = Json::String; f.second.str = branch; hadB = true; }
            if (!hadB) { Json b; b.type = Json::String; b.str = branch; target->obj.emplace_back("branch", b); }
            ++n;
        }
        // record the specimen identity and the delta that was applied (provenance of the merged manifest)
        auto setTop = [&](const char* k, const Json& v) { for (auto& f : base.obj) if (f.first == k) { f.second = v; return; } base.obj.emplace_back(k, v); };
        if (const Json* sp = delta.get("specimen")) { setTop("specimen", *sp); if (const Json* ser = sp->get("serial")) setTop("specimen_serial", *ser); }
        Json applied; applied.type = Json::Object;
        { Json a; a.type = Json::String; a.str = delta.string("schema"); applied.obj.emplace_back("schema", a);
          Json c; c.type = Json::Number; c.num = n; applied.obj.emplace_back("measured_parameters", c);
          Json bb; bb.type = Json::String; bb.str = branch; applied.obj.emplace_back("branch", bb); }
        setTop("delta_applied", applied);
        if (merged) *merged = n;
        return base.dump();
    }

private:
    // validate and read a "measured" block into e (value or array of numbers, unit equal to the entry's, instrument and date mandatory)
    static std::string parseMeasured(const Json& m, ManifestEntry& e)
    {
        if (m.type != Json::Object) return "'measured' must be an object";
        const Json* v = m.get("value");
        if (!v) return "'measured' lacks a value";
        if (v->isNumber()) { e.measuredIsArray = false; e.measured = v->num; if (!std::isfinite(v->num)) return "measured value is not finite"; }
        else if (v->type == Json::Array) {
            e.measuredIsArray = true; e.measuredArray.clear();
            for (auto& x : v->arr) { if (!x.isNumber() || !std::isfinite(x.num)) return "measured array holds a non-number"; e.measuredArray.push_back(x.num); }
            if (e.measuredArray.empty()) return "measured array is empty";
        } else return "measured value must be a number or an array of numbers";
        const std::string unit = m.string("unit");
        if (unit.empty()) return "'measured' lacks a unit";
        if (!e.unit.empty() && unit != e.unit) return "measured unit '" + unit + "' differs from the parameter's unit '" + e.unit + "' (no silent conversion)";
        e.instrument = m.string("instrument"); e.date = m.string("date");
        if (e.instrument.empty()) return "'measured' lacks the instrument (Sec. 04: record the instrument range and lead compensation used)";
        if (e.date.empty()) return "'measured' lacks a date";
        e.instrumentRange = m.string("instrument_range"); e.leadCompensation = m.string("lead_compensation");
        e.hasMeasured = true;
        return "";
    }
};

} // namespace cd57
