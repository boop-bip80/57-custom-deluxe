// DigitizedCsv.h -- loader for data/tubes/digitized/*.csv (written by scripts/digitize_jj_sheets.py).
// Columns: panel,parameter,x,y ; '#' lines are comments ; the parameter field is quoted when it contains a comma.
#pragma once
#include <algorithm>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace cd57tools {

using CurveKey = std::pair<std::string, std::string>;                       // (panel, parameter)
using CurveMap = std::map<CurveKey, std::vector<std::pair<double, double>>>;  // sorted by x

inline CurveMap loadDigitized(const std::string& path)
{
    CurveMap out;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#' || line.rfind("panel,", 0) == 0) continue;
        std::vector<std::string> fld;
        for (size_t i = 0; i <= line.size();) {
            std::string v;
            if (i < line.size() && line[i] == '"') {
                size_t j = i + 1;
                while (j < line.size() && line[j] != '"') v += line[j++];
                i = j + 2;
            } else {
                size_t j = line.find(',', i);
                if (j == std::string::npos) j = line.size();
                v = line.substr(i, j - i);
                i = j + 1;
            }
            fld.push_back(v);
        }
        if (fld.size() != 4) continue;
        out[{fld[0], fld[1]}].push_back({std::stod(fld[2]), std::stod(fld[3])});
    }
    for (auto& kv : out) std::sort(kv.second.begin(), kv.second.end());
    return out;
}

// numeric value of a parameter label such as "Ug=-0.5V", "Ua=100V", "Ug=+20V", "Ig@Ug=+5V", "Utr=350V,solid"
inline double paramValue(const std::string& par)
{
    const size_t eq = par.rfind('=');
    return std::stod(par.substr(eq + 1));
}

} // namespace cd57tools
