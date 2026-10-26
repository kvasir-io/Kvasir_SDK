// Kvasir::Trace::Ring: record order across the wrap, count() and clear(), the layout string and the descriptor the
// bench tools and the printer read, and the conversion of each value to 32 bits. Each case has a ring type of its
// own: `storage` is a static per type. The refusals (capacity, field count, arity) are in trace_must_fail.
#include "kvasir/Util/Trace.hpp"

#include "test_harness.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

using Kvasir::Trace::Descriptor;
using Kvasir::Trace::Ring;

namespace {

template<typename R>
std::vector<typename R::Record> dump() {
    std::vector<typename R::Record> out;
    R::forEach([&](typename R::Record const& r) { out.push_back(r); });
    return out;
}

// records 0 .. n-1 (one field each), then checks the kept ones are the last min(n, Capacity), oldest first
template<typename R,
         std::size_t Capacity>
void checkOrder(std::uint32_t n) {
    R::clear();
    for(std::uint32_t i = 0; i != n; ++i) { R::record(i); }
    CHECK_EQ(R::count(), n);   // the records ever written, not the kept ones
    auto const          rows = dump<R>();
    std::uint32_t const kept = n < Capacity ? n : static_cast<std::uint32_t>(Capacity);
    CHECK_EQ(rows.size(), kept);
    for(std::uint32_t i = 0; i != rows.size(); ++i) { CHECK_EQ(rows[i][0], n - kept + i); }
}

template<typename R>
std::string_view layoutOf() {
    return std::string_view{R::layout.data(), R::layout.size()};
}

void order() {
    Kvasir::Test::test("order");
    using R1 = Ring<"one", 1, "v">;
    checkOrder<R1, 1>(0);
    checkOrder<R1, 1>(1);
    checkOrder<R1, 1>(7);

    using R4 = Ring<"four", 4, "v">;
    checkOrder<R4, 4>(3);    // before the wrap
    checkOrder<R4, 4>(4);    // exactly at the capacity
    checkOrder<R4, 4>(10);   // after 2.5 wraps

    using R64 = Ring<"sixtyfour", 64, "v">;
    checkOrder<R64, 64>(63);
    checkOrder<R64, 64>(64);
    checkOrder<R64, 64>(160);
}

void clearAndCount() {
    Kvasir::Test::test("clear and count");
    using R = Ring<"clr", 4, "a", "b">;
    R::record(1, 2);
    R::record(3, 4);
    CHECK_EQ(R::count(), 2);
    auto rows = dump<R>();
    CHECK_EQ(rows.size(), 2);
    CHECK(rows[0] == R::Record{1, 2});
    CHECK(rows[1] == R::Record{3, 4});
    R::clear();
    CHECK_EQ(R::count(), 0);
    CHECK(dump<R>().empty());
    R::record(5, 6);   // clear() starts over at slot 0
    CHECK_EQ(R::storage.count, 1);
    CHECK(R::storage.records[0] == R::Record{5, 6});
}

void layoutString() {
    Kvasir::Test::test("layout");
    using Usb
      = Ring<"usb", 64, "us", "ints", "sie_status", "buff_status">;   // the header's doc example
    using namespace std::string_view_literals;
    CHECK(layoutOf<Usb>() == "usb:us,ints,sie_status,buff_status\0"sv);
    // name + ':' + the fields + one separator or terminator per field
    CHECK_EQ(Usb::layout.size(), 3 + 1 + (2 + 4 + 10 + 11) + 4);
    CHECK_EQ(Usb::fieldCount, 4);

    using One = Ring<"r", 2, "x">;
    CHECK(layoutOf<One>() == "r:x\0"sv);
    CHECK_EQ(One::layout.size(), 4);
}

void descriptorWords() {
    Kvasir::Test::test("descriptor");
    using R = Ring<"desc", 16, "a", "b", "c">;
    static_assert(Descriptor::magicValue
                  == 0x4352544BU);   // "KTRC" in memory, what the readers look for
    CHECK_EQ(R::descriptor.magic, 0x4352544BU);
    CHECK_EQ(R::descriptor.fields, 3);
    CHECK_EQ(R::descriptor.capacity, 16);
    CHECK(R::descriptor.layout == R::layout.data());
    // the storage the readers decode: count first, then Capacity records of `fields` words
    static_assert(sizeof(R::Storage) == 4 + 4 * 3 * 16);
    static_assert(offsetof(R::Storage, records) == 4);
}

void valuesAreWords() {
    Kvasir::Test::test("values are 32-bit words");
    using R = Ring<"conv", 2, "s", "w", "b">;
    R::clear();
    R::record(std::int8_t{-1}, std::uint64_t{0x1'2345'6789ULL}, true);
    auto const rows = dump<R>();
    CHECK_EQ(rows.size(), 1);
    CHECK_EQ(rows[0][0], 0xFFFFFFFFU);   // sign-extended, then cut to 32 bits
    CHECK_EQ(rows[0][1], 0x23456789U);   // truncated
    CHECK_EQ(rows[0][2], 1);
}

}   // namespace

int main() {
    order();
    clearAndCount();
    layoutString();
    descriptorWords();
    valuesAreWords();
    return Kvasir::Test::report();
}
