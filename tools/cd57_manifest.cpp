// cd57_manifest -- validate, summarise and merge parameter manifests (dossier Sec. 04, 28, 30).
//
//   cd57_manifest check MANIFEST.json
//       validates the manifest (schema, provenance fields, measured blocks), lists the evidence mix and the branches, shows every MEASURED value beside its
//       NOMINAL value with the relative difference, and states which entries drive the model.  Exit 0 = valid.
//   cd57_manifest merge BASE.json DELTA.json OUT.json
//       merges a specimen delta (schema cd57-specimen-delta-v1) onto the base manifest: measured blocks are stored beside the nominal values, entries are
//       labelled branch "specimen", nothing nominal is overwritten; unknown keys, unit mismatches and missing instrument/date are rejected.  The result is
//       validated before it is written.
#include <fstream>
#include <iostream>
#include <sstream>

#include "Manifest.h"

static std::string slurp(const std::string& p) { std::ifstream f(p); return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }

static int check(const std::string& path)
{
    const std::string text = slurp(path);
    if (text.empty()) { std::cerr << "cannot read " << path << "\n"; return 2; }
    cd57::Manifest m;
    const std::string err = m.load(text);
    if (!err.empty()) { std::cerr << "INVALID: " << err << "\n"; return 1; }
    cd57::AmpConfig c; const std::string aerr = m.apply(c);
    if (!aerr.empty()) { std::cerr << "INVALID: " << aerr << "\n"; return 1; }
    if (!m.missingKeys().empty()) { std::cerr << "INVALID: the model reads '" << m.missingKeys().front() << "' which is absent or null\n"; return 1; }
    std::cout << m.summary() << "\nschema_version " << m.schemaVersion << ", " << m.readKeys().size() << " entries drive the model\n";
    int n = 0;
    for (auto& e : m.entries) {
        if (!e.hasMeasured) continue;
        if (!n++) std::cout << "\nmeasured beside nominal:\n";
        if (e.measuredIsArray) { std::cout << "  " << e.name << " [" << e.unit << "]: measured map of " << e.measuredArray.size() << " points (nominal: none)"; }
        else {
            std::cout << "  " << e.name << " [" << e.unit << "]: nominal " << e.value << ", measured " << e.measured;
            if (e.hasValue && e.value != 0.0) std::cout << " (" << (100.0 * (e.measured - e.value) / e.value) << " %)";
            if (e.hasInterval) std::cout << (e.measured >= e.lo && e.measured <= e.hi ? ", inside" : ", OUTSIDE") << " the assumed interval [" << e.lo << ", " << e.hi << "]";
        }
        std::cout << "; " << e.instrument << ", " << e.date << "\n";
    }
    if (!n) std::cout << "\nno measured values: this is the nominal (documented approximation) profile\n";
    return 0;
}

int main(int argc, char** argv)
{
    if (argc >= 3 && std::string(argv[1]) == "check") return check(argv[2]);
    if (argc >= 5 && std::string(argv[1]) == "merge") {
        const std::string base = slurp(argv[2]), delta = slurp(argv[3]);
        if (base.empty() || delta.empty()) { std::cerr << "cannot read the base or the delta\n"; return 2; }
        std::string err; int n = 0;
        const std::string merged = cd57::Manifest::mergeDelta(base, delta, err, &n);
        if (merged.empty()) { std::cerr << "MERGE REFUSED: " << err << "\n"; return 1; }
        cd57::Manifest probe; const std::string verr = probe.load(merged);
        cd57::AmpConfig c; const std::string aerr = verr.empty() ? probe.apply(c) : "";
        if (!verr.empty() || !aerr.empty()) { std::cerr << "MERGE REFUSED: the merged manifest does not validate: " << (verr.empty() ? aerr : verr) << "\n"; return 1; }
        std::ofstream(argv[4]) << merged;
        std::cout << "merged " << n << " measured parameter(s) onto '" << probe.profile << "' (branch specimen" << (probe.specimenSerial.empty() ? "" : ", serial " + probe.specimenSerial) << "); wrote " << argv[4] << "\n";
        return 0;
    }
    std::cerr << "usage: cd57_manifest check MANIFEST.json | cd57_manifest merge BASE.json DELTA.json OUT.json\n";
    return 2;
}
