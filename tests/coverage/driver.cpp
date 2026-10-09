// The coverage host test's driver: runs lib.cpp, lets Kvasir::Coverage build its manifest, assembles the .profraw from
// the manifest as kvasir_bench does (memory pieces read where they are) and compares it with what the runtime's own
// buffer writer produces. Writes manifest.profraw; exit 0 only when the two are byte for byte the same.
#include "kvasir/Util/Coverage.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

int classify(int x);

extern "C" {
std::uint64_t __llvm_profile_get_size_for_buffer();
int           __llvm_profile_write_buffer(char* buffer);
}

int main(int    argc,
         char** argv) {
    int sum = 0;
    for(int i = 0; i != argc + 4; ++i) { sum += classify(i); }

    Kvasir::Coverage::runtimeInit();
    auto const& m = Kvasir::Coverage::manifest;
    if(m.magic != Kvasir::Coverage::Magic || m.status != 0) {
        std::printf("manifest: magic %x status %u\n", m.magic, m.status);
        return 1;
    }
    std::vector<char> fromManifest;
    for(std::uint32_t i = 0; i != m.count; ++i) {
        auto const& p  = m.pieces[i];
        auto const* at = p.kind == Kvasir::Coverage::Kind::inlined
                         ? reinterpret_cast<char const*>(m.header) + p.address
                         : reinterpret_cast<char const*>(p.address);
        if(p.kind == Kvasir::Coverage::Kind::zeros) {
            fromManifest.insert(fromManifest.end(), p.length, '\0');
        } else {
            fromManifest.insert(fromManifest.end(), at, at + p.length);
        }
    }
    std::vector<char> fromRuntime(__llvm_profile_get_size_for_buffer());
    if(__llvm_profile_write_buffer(fromRuntime.data()) != 0) { return 1; }

    std::FILE* f = std::fopen(argv[1], "wb");
    std::fwrite(fromManifest.data(), 1, fromManifest.size(), f);
    std::fclose(f);
    std::printf("%zu pieces, %zu bytes, runtime %zu bytes, sum %d\n",
                static_cast<std::size_t>(m.count),
                fromManifest.size(),
                fromRuntime.size(),
                sum);
    return fromManifest == fromRuntime ? 0 : 1;
}
