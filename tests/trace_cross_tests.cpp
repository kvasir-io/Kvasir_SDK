// Kvasir::Trace::Ring against the reader that stays outside the SDK: uc_log's records::decodeRing, what the
// printer's trace page decodes from the bytes a probe reads (uc_log is its own project and cannot include
// Trace.hpp). A real ring records; its descriptor and storage are copied out as a 32-bit little-endian target holds
// them and decoded. uc_log is a submodule of every SDK checkout, so this always builds. kvasir_bench.py has no reader
// of its own: it asks the printer (its `records` request), so this decoder is the only host copy of the layout.
#include "kvasir/Util/Trace.hpp"

#include "test_harness.hpp"
#include "uc_log/printer/TargetRecords.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace {
namespace records = uc_log::printer::records;

static_assert(std::endian::native == std::endian::little,
              "the copy below assumes a little-endian host");
static_assert(records::traceMagic == Kvasir::Trace::Descriptor::magicValue);
// on the target the descriptor is magic, fields, capacity and a 4-byte pointer
static_assert(records::traceDescriptorSize == 4 + 2 + 2 + 4);
static_assert(offsetof(Kvasir::Trace::Descriptor,
                       fields)
                == 4
              && offsetof(Kvasir::Trace::Descriptor,
                          capacity)
                   == 6
              && offsetof(Kvasir::Trace::Descriptor,
                          layout)
                   == 8);

using Ring = Kvasir::Trace::Ring<"cross", 4, "a", "b">;
static_assert(
  offsetof(Ring::Storage,
           records)
      == 4
    && sizeof(Ring::Storage) == 4 + 4 * 2 * 4,
  "the storage is the count, then the records: no pointer, the same bytes on the target");

constexpr std::uint32_t layoutAt = 0x1000'A884U;   // where the target would keep the layout string

void put(std::vector<std::byte>& out,
         std::uint32_t           v,
         std::size_t             bytes) {
    for(std::size_t i = 0; i != bytes; ++i) {
        out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFU));
    }
}

// the 12 bytes a probe reads from a target's descriptor (the host's has an 8-byte pointer)
std::vector<std::byte> targetDescriptor() {
    std::vector<std::byte> out;
    put(out, Ring::descriptor.magic, 4);
    put(out, Ring::descriptor.fields, 2);
    put(out, Ring::descriptor.capacity, 2);
    put(out, layoutAt, 4);
    return out;
}

std::vector<std::byte> targetStorage() {
    std::vector<std::byte> out(sizeof(Ring::Storage));
    std::memcpy(out.data(), &Ring::storage, sizeof(Ring::Storage));
    return out;
}

void decodesWhatRecords() {
    Kvasir::Test::test("uc_log decodes Trace::Ring");
    for(std::uint32_t i = 0; i != 6; ++i) { Ring::record(100 + i, 200 + i); }
    auto const descriptor = targetDescriptor();
    auto const storage    = targetStorage();
    auto const h          = records::decodeRing(descriptor, storage);
    CHECK(h.has_value());
    if(!h) { return; }
    CHECK_EQ(h->fields, Ring::fieldCount);
    CHECK_EQ(h->capacity, 4);
    CHECK_EQ(h->count, Ring::count());
    CHECK_EQ(h->layout, layoutAt);
    CHECK_EQ(h->storageSize(), sizeof(Ring::Storage));

    std::vector<Ring::Record> kept;
    Ring::forEach([&](Ring::Record const& r) { kept.push_back(r); });
    auto const rows = records::decodeRingRows(storage, *h, h->count);
    CHECK_EQ(rows.size(), kept.size());
    for(std::size_t i = 0; i != rows.size() && i != kept.size(); ++i) {
        CHECK_EQ(rows[i].index, 2 + i);   // 6 written, capacity 4: records 2..5, oldest first
        CHECK_EQ(rows[i].values.size(), 2);
        CHECK_EQ(rows[i].values[0], kept[i][0]);
        CHECK_EQ(rows[i].values[1], kept[i][1]);
    }

    auto const layout = records::decodeRingLayout(std::as_bytes(std::span{Ring::layout}));
    CHECK(layout.name == "cross");
    CHECK(layout.fields.size() == 2 && layout.fields[0] == "a" && layout.fields[1] == "b");

    // what record() wrote while the probe read is dropped, as the printer does with the count read again
    CHECK_EQ(records::decodeRingRows(storage, *h, h->count + 2).size(), 2);
}

}   // namespace

int main() {
    decodesWhatRecords();
    return Kvasir::Test::report();
}
