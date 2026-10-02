// The executable: collect argv and hand it to the library.
//
// The allocation trap is inert unless a binary links it. In debug builds this
// one does (see CMakeLists.txt), which is what makes the guard around every
// audio callback mean anything: if one ever allocates, this process dies
// instead of quietly glitching.

#include <iostream>
#include <string>
#include <vector>

#include "cli/run.hpp"

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + (argc > 0 ? 1 : 0), argv + argc);
    return analyzer::cli::run(args, std::cout, std::cerr);
}
