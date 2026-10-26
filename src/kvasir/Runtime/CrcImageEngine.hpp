#pragma once
// The image's CRC engine: one spelling for "the chip's hardware for this CRC where the image names one, else the
// software" (Kvasir::Crc::Engine, kvasir/Util/Crc.hpp). A library that computes a CRC at run time and takes any engine
// that gives the CRC names Kvasir::Crc::ImageEngine<P>; the image picks the engines once, in the hook
// Kvasir::Crc::ImageEngines<Tag::User> (the table in kvasir/Port/Hooks.hpp). Here and not in Util/Crc.hpp: the hook is
// read through Port::LateT, and the software engines stay a base header that includes nothing of the port.
//
// The hook's `type` has `template<auto P, std::size_t TableSize> using For = <an engine with Engine's shape for P>;`.
// The default is the software. A chip package offers a set over its hardware (chip_rp_common DmaCrc.hpp:
// `Kvasir::DmaCrc::Sniffer<Dma, channel>`, the DMA sniffer on a channel of the firmware's DmaBase - the channel is the
// firmware's, so the image names the set, and lists it in its Startup list, which claims the channel for it;
// chip_atsam_common DMAC.hpp: `Kvasir::DmaCrc::Dmac`, the DMAC's CPU-fed engine, nothing to claim):
//
//     template<>
//     struct Kvasir::Crc::ImageEngines<Kvasir::Tag::User> { using type = Kvasir::DmaCrc::Sniffer<Dma, Dma::Channel::ch2>; };
//
// A set's For keeps the software for a CRC its hardware cannot compute. A set that claims or provides a resource (the
// sniffer's channel) must be in a Startup list: Startup's list rules refuse it otherwise ("the image's CRC engine set
// ... is in no Startup list"). Every translation unit of the image must see the same specialisation (the header they
// all include, before the first library header that instantiates a user); nothing can check that.
//
// A set's engine may fail where its hardware does (a DMA transfer that times out or hits a bus error): it then has
// `bool failed() const`, true once an update() failed - finish() is not the CRC then, and the caller handles it as a
// failed read (ImageEngine::failed() below; CellStore does). Its compute() is the CRC whatever happens: where the
// hardware fails, the engine computes that span in software.
#include "kvasir/Common/Tags.hpp"
#include "kvasir/Port/Hooks.hpp"
#include "kvasir/Util/Crc.hpp"

#include <concepts>
#include <cstddef>
#include <span>
#include <type_traits>

namespace Kvasir::Crc {

struct SoftwareEngines {
    template<auto P, std::size_t TableSize>
    using For = Engine<P, TableSize>;
};

template<typename T, typename... Ts>
struct ImageEngines {
    using type = SoftwareEngines;
};

/// The engine the image chose for P (ImageEngines above), with Engine's shape: update(span) / finish() at run time,
/// compute(span) at run time and in a constant expression - where it is the software Engine<P, TableSize>, so a
/// library's static_asserts over its CRC hold whatever the image chose. TableSize is the software's, used where the
/// image's set leaves P to the software. A CellStore's cells, an SD card's blocks and an aglio packager's frames
/// (through Calc<ImageEngine<P>>) take it. failed(): the selected engine's hardware failed in an update() (always
/// false for the software), so finish() is not the CRC; compute() never fails (header above).
template<auto P, std::size_t TableSize = 16>
class ImageEngine {
    using Software = Engine<P, TableSize>;

public:
    // looked up late (Port::LateT on P's type): the image's specialisation is taken where a user is instantiated
    using Selected =
      typename Port::LateT<ImageEngines<Tag::User>, decltype(P)>::type::template For<P, TableSize>;
    using value_type                       = typename Software::value_type;
    static constexpr auto        params    = P;
    static constexpr std::size_t tableSize = TableSize;
    static_assert(
      std::is_same_v<typename Selected::value_type,
                     value_type>,
      "the image's CRC engine for P has another value type than Kvasir::Crc::Engine<P>");

    ImageEngine() = default;

    ImageEngine& update(std::span<std::byte const> data) {
        engine_.update(data);
        return *this;
    }

    [[nodiscard]] value_type finish() const { return engine_.finish(); }

    [[nodiscard]] bool failed() const {
        if constexpr(requires(Selected const& e) {
                         { e.failed() } -> std::convertible_to<bool>;
                     })
        {
            return engine_.failed();
        } else {
            return false;
        }
    }

    [[nodiscard]] static constexpr value_type compute(std::span<std::byte const> data) {
        if consteval {
            return Software::compute(data);
        } else {
            return Selected::compute(data);
        }
    }

private:
    Selected engine_{};
};

}   // namespace Kvasir::Crc
