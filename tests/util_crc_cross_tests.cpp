// Kvasir::Crc::Crc32 against the CRC-32s that stay outside the SDK and must agree with it byte for byte:
// uc_log's (the printer's firmware check, which ImageCheck's CRC must match) and fs's log
// store (records written by firmwares in the field). Each is compiled in only where its checkout is.
#include "kvasir/Util/Crc.hpp"
#include "test_harness.hpp"

#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

#if KVASIR_CRC_CROSS_UC_LOG
    #include "uc_log/detail/HexImage.hpp"
#endif
#if KVASIR_CRC_CROSS_FS
    #include "fs/log/Store.hpp"
#endif

int main() {
    using Kvasir::Crc::Crc32;
    using Kvasir::Test::test;

    std::mt19937           rng{3};
    std::vector<std::byte> data(70'000);
    for(auto& b : data) { b = static_cast<std::byte>(rng()); }

    for(std::size_t const n :
        {std::size_t{0}, std::size_t{1}, std::size_t{9}, std::size_t{4096}, data.size()})
    {
        std::span<std::byte const> const d{data.data(), n};
        std::span<std::byte const> const half = d.first(n / 2);
        std::span<std::byte const> const rest = d.subspan(n / 2);
#if KVASIR_CRC_CROSS_UC_LOG
        test("uc_log::detail::crc32");
        CHECK_EQ(uc_log::detail::crc32(d), Crc32::compute(d));
        CHECK_EQ(uc_log::detail::crc32(rest, uc_log::detail::crc32(half)), Crc32::compute(d));
#endif
#if KVASIR_CRC_CROSS_FS
        test("fs::log::detail::crc32");
        CHECK_EQ(fs::log::detail::crc32(0, d), Crc32::compute(d));
        CHECK_EQ(fs::log::detail::crc32(fs::log::detail::crc32(0, half), rest),
                 Crc32::resume(Crc32::compute(half)).update(rest).finish());
#endif
        static_cast<void>(half);
        static_cast<void>(rest);
    }
    return Kvasir::Test::report();
}
