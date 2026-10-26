#pragma once
// The common log backend of an image, for its config header (kvasir_config.hpp, UC_LOG_CONFIG_HEADER): the image keeps
// its uc_log::ComBackend<uc_log::Tag::User> specialisation, one line over this
//
//   template<> struct uc_log::ComBackend<uc_log::Tag::User> : Kvasir::Log::RttSink<> {};
//
// and a config of another shape (other rings, an ISR policy, a duplex channel, a second core) spells its backend out.
// An alias: RttSink<> IS the uc_log::RttLog<...> it names. Its clock: kvasir/Log/SystickLogClock.hpp.
//
// Include it after the chip's DebuggerPresent.hpp (Kvasir::Chip::DebuggerPresent) and WaitBounds.hpp
// (Register::CpuHzCeiling<Tag::User>, the loss policy's CPU ceiling): both are named here. It never includes
// uc_log/uc_log.hpp: the config header must not.

#include "kvasir/Common/Tags.hpp"

#include <cstddef>
#include <uc_log/RttLog.hpp>

namespace Kvasir::Log {

/// One RTT up ring of RingBytes for every context, written while the chip's debugger check says one is attached, a
/// dropped record counted and told in the next line on its ring ("⚠ N log records lost"), a panic's lines waiting for
/// the printer (uc_log::LossPolicy::Counted at the chip's CPU ceiling).
template<std::size_t RingBytes = 16384>
using RttSink = uc_log::RttLog<
  Kvasir::Chip::DebuggerPresent,
  uc_log::Rings<RingBytes>,
  uc_log::LossPolicy::Counted<Kvasir::Register::CpuHzCeiling<Kvasir::Tag::User>::value>>;

}   // namespace Kvasir::Log
