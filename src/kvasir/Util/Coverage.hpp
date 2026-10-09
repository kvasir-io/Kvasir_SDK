#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>

/// Source coverage of a `coverage` image (kvasir_executable_variants COVERAGE: clang's -fprofile-instr-generate
/// -fcoverage-mapping and compiler-rt's bare-metal profile runtime). In a Startup list, Kvasir::Coverage lets the
/// runtime lay out the .profraw once at boot and records where each piece of it lives: the header copied here, the
/// counters, records and names by address. `kvasir_bench.py coverage` reads the pieces through the printer and writes a
/// file llvm-profdata takes - nothing is copied on the target, the counters are read live, as often as wanted.
///
/// In every other variant Kvasir::Coverage is an empty entry: the same Startup list builds everywhere.
namespace Kvasir {
#if defined(KVASIR_COVERAGE)
namespace Coverage_detail {
    // compiler-rt/lib/profile/InstrProfilingInternal.h: the writer interface of the runtime
    struct IoVec {
        void const* data;
        std::size_t elmSize;
        std::size_t numElm;
        int         useZeroPadding;
    };
    struct Writer;
    using WriterCallback = std::uint32_t (*)(Writer*,
                                             IoVec*,
                                             std::uint32_t);

    struct Writer {
        WriterCallback write;
        void*          ctx;
    };
}   // namespace Coverage_detail
}   // namespace Kvasir

extern "C" {
int         lprofWriteData(Kvasir::Coverage_detail::Writer*,
                           void* valueProfileReader,
                           int   skipNames);
char const* __llvm_profile_begin_counters();
char const* __llvm_profile_end_counters();
char const* __llvm_profile_begin_bitmap();
char const* __llvm_profile_end_bitmap();
char const* __llvm_profile_begin_names();
char const* __llvm_profile_end_names();
void const* __llvm_profile_begin_data();
void const* __llvm_profile_end_data();

// The bare-metal runtime has no InstrProfilingRuntime.cpp: an instrumented object's reference to the hook variable
// lands here.
inline int __llvm_profile_runtime{};
}

namespace Kvasir {
struct Coverage {
    static constexpr std::uint32_t Magic = 0x4B43'4F56U;   // "KCOV", what kvasir_bench looks for

    enum class Kind : std::uint32_t { memory, zeros, inlined };

    struct Piece {
        std::uintptr_t
          address;   // memory: where on the target; inlined: offset into header (12 bytes a piece on the
                     // 32-bit targets, what kvasir_bench reads)
        std::uint32_t length;
        Kind          kind;
    };

    struct Manifest {
        std::uint32_t magic;
        std::uint32_t status;   // lprofWriteData's result, 0 = ok
        std::uint32_t count;
        std::uint32_t headerUsed;
        Piece         pieces[24];
        alignas(8) std::uint8_t
          header[256];   // the raw header (19 x uint64_t in format version 11) and anything
                         // else the runtime hands over by value
    };

    // the host finds it by name; rebuilt at every boot
    static inline Manifest manifest{};

    static void runtimeInit() {
        manifest = {};
        Coverage_detail::Writer w{record, nullptr};
        manifest.status = static_cast<std::uint32_t>(lprofWriteData(&w, nullptr, 0));
        manifest.magic  = Magic;
    }

private:
    static bool in(void const* p,
                   void const* begin,
                   void const* end) {
        auto const a = reinterpret_cast<std::uintptr_t>(p);
        return a >= reinterpret_cast<std::uintptr_t>(begin)
            && a < reinterpret_cast<std::uintptr_t>(end);
    }

    // a piece that lives in one of the profile sections stays where it is (the counters change while the board runs)
    static bool inSection(void const* p) {
        return in(p, __llvm_profile_begin_counters(), __llvm_profile_end_counters())
            || in(p, __llvm_profile_begin_bitmap(), __llvm_profile_end_bitmap())
            || in(p, __llvm_profile_begin_data(), __llvm_profile_end_data())
            || in(p, __llvm_profile_begin_names(), __llvm_profile_end_names());
    }

    // the runtime hands over the file as pieces: the section ones by address, anything else (the header, a local of
    // lprofWriteDataImpl) copied while it still exists
    static std::uint32_t record(Coverage_detail::Writer*,
                                Coverage_detail::IoVec* v,
                                std::uint32_t           n) {
        auto& m = manifest;
        for(std::uint32_t i = 0; i != n; ++i) {
            auto const length = v[i].elmSize * v[i].numElm;
            if(length == 0) { continue; }
            if(m.count == std::size(m.pieces)) { return 1; }
            Piece& p = m.pieces[m.count++];
            p.length = static_cast<std::uint32_t>(length);
            if(v[i].data == nullptr) {
                p.kind = Kind::zeros;
            } else if(inSection(v[i].data)) {
                p.kind    = Kind::memory;
                p.address = reinterpret_cast<std::uintptr_t>(v[i].data);
            } else {
                if(m.headerUsed + length > sizeof(m.header)) { return 1; }
                p.kind    = Kind::inlined;
                p.address = m.headerUsed;
                std::memcpy(m.header + m.headerUsed, v[i].data, length);
                m.headerUsed += static_cast<std::uint32_t>(length);
            }
        }
        return 0;
    }
};
#else
struct Coverage {
    static void runtimeInit() {}
};
#endif
}   // namespace Kvasir
