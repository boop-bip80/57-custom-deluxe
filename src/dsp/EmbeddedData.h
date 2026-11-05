// EmbeddedData.h -- data files compiled into the binaries (single source of truth; see cmake/EmbedText.cmake)
#pragma once
#include <string>
namespace cd57 {
std::string embeddedNetlist();   // data/netlist/cd57_nominal.net
std::string embeddedManifest();  // data/parameters/cd57_nominal.json
}
