// Manifest.h -- parameter manifest loader with provenance validation (dossier Sec. 30, 33).
//
// "A model loader should fail clearly when a mandatory calibrated field is absent, or require an explicitly selected
//  provisional profile."  This loader: (1) rejects entries without a status; (2) rejects 'assumed' entries that lack a
// sensitivity interval or a value; (3) accepts null values only for status 'unknown'; (4) applies the known numeric
// parameters to the engine configuration and (5) summarises the evidence mix for the UI and validation report.
#pragma once
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "AmpCircuit.h"
#include "Json.h"

namespace cd57 {

struct ManifestEntry {
    std::string name, unit, status, source, note;
    bool hasValue = false;
    double value = 0.0;
    bool hasInterval = false;
    double lo = 0.0, hi = 0.0;
};

class Manifest {
public:
    std::string target, profile, profileNote;
    std::vector<ManifestEntry> entries;
    std::map<std::string, int> statusCount;

    // returns empty string on success, otherwise a human-readable reason (never throws)
    std::string load(const std::string& jsonText)
    {
        entries.clear(); statusCount.clear();
        Json root;
        try { root = Json::parse(jsonText); } catch (const std::exception& e) { return std::string("manifest JSON: ") + e.what(); }
        target = root.string("target"); profile = root.string("profile"); profileNote = root.string("profile_note");
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
            if (e.status == "assumed") {
                const Json* iv = j.get("interval");
                if (iv && iv->type == Json::Array && iv->arr.size() == 2) { e.hasInterval = true; e.lo = iv->arr[0].num; e.hi = iv->arr[1].num; }
                else if (v && v->isNumber()) return "assumed parameter '" + e.name + "' lacks a sensitivity interval (dossier Sec. 28)";
            }
            ++statusCount[e.status];
            entries.push_back(e);
        }
        return "";
    }

    const ManifestEntry* find(const std::string& n) const { for (auto& e : entries) if (e.name == n) return &e; return nullptr; }
    double value(const std::string& n, double dflt) const { auto* e = find(n); return e && e->hasValue ? e->value : dflt; }

    // apply known numeric parameters to the circuit configuration
    void apply(AmpConfig& c) const
    {
        c.ptHalfVrms = value("pt_hv_half_winding_rms", c.ptHalfVrms);
        c.ptRs = value("pt_series_resistance_per_half", c.ptRs);
        c.potTotal = value("pot_total_resistance", c.potTotal);
        c.volumeMap.endpointResistance = value("pot_endpoint_contact_resistance", c.volumeMap.endpointResistance);
        c.toneMap.endpointResistance = c.volumeMap.endpointResistance;
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
    }

    std::string summary() const
    {
        std::ostringstream o;
        o << target << " -- profile '" << profile << "'\n" << entries.size() << " parameters: ";
        for (auto& kv : statusCount) o << kv.second << " " << kv.first << ", ";
        return o.str();
    }
};

} // namespace cd57
