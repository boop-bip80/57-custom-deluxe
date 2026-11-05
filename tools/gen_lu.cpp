// gen_lu -- emits generated/LuKernel.h: unrolled sparse-LU factor/solve for the netlist's fixed pattern.
//   gen_lu <netlist.net> <out LuKernel.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include "Network.h"
int main(int argc, char** argv)
{
    if (argc < 3) { std::cerr << "usage: gen_lu netlist.net out.h\n"; return 2; }
    std::ifstream f(argv[1]); std::stringstream ss; ss << f.rdbuf();
    cd57::Network n; n.useKernel = false; n.parse(ss.str()); n.build(1.0 / 96000.0, 0.5);
    std::ofstream o(argv[2]); o << n.emitLuKernel();
    std::cout << "wrote " << argv[2] << " (N=" << n.unknowns() << ", " << n.luOps() << " ops, nnz " << n.luNnz() << ")\n";
    return 0;
}
