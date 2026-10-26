// Kvasir::Crc::Crc32 against uc_log's CRC-32, which stays outside the SDK and must agree with it byte for byte: the
// printer's firmware check, which ImageCheck's CRC must match. (fs's log store checks its copy against the textbook
// loop util_crc_tests.cpp checks Crc32 against: fs/test/crc_test.cpp.)
#include "kvasir/Util/Crc.hpp"
#include "test_harness.hpp"
#include "uc_log/printer/HexImage.hpp"

#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

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
        test("uc_log::printer::crc32");
        CHECK_EQ(uc_log::printer::crc32(d), Crc32::compute(d));
        CHECK_EQ(uc_log::printer::crc32(rest, uc_log::printer::crc32(half)), Crc32::compute(d));
    }
    return Kvasir::Test::report();
}
