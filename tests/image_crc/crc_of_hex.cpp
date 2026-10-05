// The log printer's number for a hex file (uc_log detail/HexImage.hpp): parseIntelHex + imageCrc, printed in hex.
#include "uc_log/detail/HexImage.hpp"

#include <cstdio>
#include <fstream>

int main(int    argc,
         char** argv) {
    if(argc != 2) { return 2; }
    std::ifstream in{argv[1]};
    auto const    segments = uc_log::detail::parseIntelHex(in);
    if(!segments) { return 1; }
    std::printf("%08x %zu\n", uc_log::detail::imageCrc(*segments), segments->size());
    return 0;
}
