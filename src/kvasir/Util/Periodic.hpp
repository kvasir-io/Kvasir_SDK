#pragma once
// "Do this every N ms" and "give up after N ms" on a std::chrono clock, in one place instead of a
// `next` variable and an `if(now >= next)` per period.
//
//     Kvasir::Every<Clock> tick{500ms};           // build it in main: the default first due() reads
//     while(true) {                               // the clock, which does not run during static init
//         if(tick.due()) { apply(toggle(HW::Pin::led{})); }
//     }
//
// What happens when the loop was late - pick per call:
//
//   due()        catch-up: true once per elapsed period. The next deadline is the previous one
//                plus the period, so the average rate stays exact; after a stall of k periods
//                the next k calls are true (while(tick.due(now)) { step(); } runs every missed
//                tick).
//   dueCount()   skip to the grid and report: the number of periods passed (0 = not yet, 1 = on
//                time, n = n - 1 missed); the schedule jumps past all of them and keeps its phase.
//                if(x.dueCount()) where every tick matters loses ticks - that is what due() is for.
//   skipTo(now)  rebase: the next due() is one period from now, whatever was missed (no burst
//                after a flash erase or a log flood).
//
// Deadline is a one-shot: stopped, or armed until an end time. Every32 / Deadline32 are 8-byte
// forms for structs kept in many copies; they wrap (see there). An ISR and the main loop must not
// share one object. On a per-core clock (CoreSystickClock) build and poll on the same core, or
// synchronise the clocks.

#include <cassert>
#include <chrono>
#include <concepts>
#include <cstdint>

namespace Kvasir {

// A clock with std::chrono's shape and a static now() returning its time_point. AonTimer::now()
// returns a duration, not a time_point, and is no PollClock.
template<typename C>
concept PollClock = requires {
    typename C::time_point;
    typename C::duration;
    { C::now() } -> std::same_as<typename C::time_point>;
};

template<PollClock ClockT>
class Every {
public:
    using clock      = ClockT;
    using time_point = typename ClockT::time_point;
    using duration   = typename ClockT::duration;

    /// A period, and when the first `due()` is true: at construction by default, so the
    /// first turn of the loop does the work once (`first = ClockT::now() + period` delays it
    /// by one period).
    template<typename Rep,
             typename P>
    constexpr explicit Every(std::chrono::duration<Rep,
                                                   P> period,
                             time_point               first = ClockT::now())
      : period_{std::chrono::duration_cast<duration>(period)}
      , next_{first} {}

    /// True once per elapsed period; the deadline advances by one period each time
    /// (catch-up).
    [[nodiscard]] constexpr bool due(time_point now = ClockT::now()) {
        if(now < next_) { return false; }
        next_ += period_;
        return true;
    }

    /// The periods passed, 0 if none; the schedule moves past all of them, on the old grid.
    /// One 64-bit division, on the due path only.
    [[nodiscard]] constexpr std::uint32_t dueCount(time_point now = ClockT::now()) {
        if(now < next_) { return 0; }
        auto const k = static_cast<std::uint32_t>((now - next_) / period_) + 1U;
        next_ += period_ * k;
        return k;
    }

    /// Restart from `now`: the next `due()` is one period from now, whatever was missed.
    constexpr void skipTo(time_point now) { next_ = now + period_; }

    /// A new period, counted from the last grid point: a shorter one fires `p` after the last
    /// due(), a longer one never fires early.
    template<typename Rep,
             typename P>
    constexpr void setPeriod(std::chrono::duration<Rep,
                                                   P> p) {
        auto const np = std::chrono::duration_cast<duration>(p);
        next_         = next_ - period_ + np;
        period_       = np;
    }

    /// When `due()` becomes true next.
    [[nodiscard]] constexpr time_point next() const { return next_; }

    /// The period, in the clock's own duration.
    [[nodiscard]] constexpr duration period() const { return period_; }

private:
    duration   period_;
    time_point next_;
};

// One-shot: stopped (the default), or armed until an end time. 8 bytes: the time_point it
// replaces, with "stopped" as time_point::max(), which a 64-bit clock never reaches - so a
// `bool busy` + `time_point started` pair converts without growing.
template<PollClock ClockT>
class Deadline {
public:
    using clock      = ClockT;
    using time_point = typename ClockT::time_point;
    using duration   = typename ClockT::duration;

    constexpr Deadline() = default;

    template<typename Rep,
             typename P>
    constexpr explicit Deadline(std::chrono::duration<Rep,
                                                      P> in,
                                time_point               now = ClockT::now())
      : end_{now + std::chrono::duration_cast<duration>(in)} {}

    /// Armed, ending `in` from `now` (also moves the end of an armed one).
    template<typename Rep,
             typename P>
    constexpr void restart(std::chrono::duration<Rep,
                                                 P> in,
                           time_point               now = ClockT::now()) {
        end_ = now + std::chrono::duration_cast<duration>(in);
    }

    constexpr void stop() { end_ = Stopped; }

    [[nodiscard]] constexpr bool stopped() const { return end_ == Stopped; }

    [[nodiscard]] constexpr bool expired(time_point now = ClockT::now()) const {
        return !stopped() && now >= end_;
    }

    [[nodiscard]] constexpr bool armed(time_point now = ClockT::now()) const {
        return !stopped() && now < end_;
    }

    /// Signed: negative once expired, zero when stopped.
    [[nodiscard]] constexpr duration remaining(time_point now = ClockT::now()) const {
        return stopped() ? duration{} : end_ - now;
    }

    /// True exactly once, when expired; the deadline is stopped after it.
    [[nodiscard]] constexpr bool due(time_point now = ClockT::now()) {
        if(!expired(now)) { return false; }
        stop();
        return true;
    }

    /// The end time; time_point::max() when stopped.
    [[nodiscard]] constexpr time_point end() const { return end_; }

private:
    static constexpr time_point Stopped = time_point::max();
    time_point                  end_{Stopped};
};

namespace detail {
    // the clock's time in Unit, truncated to 32 bits: wraps every 2^32 Units
    template<PollClock ClockT,
             typename Unit>
    constexpr std::uint32_t ticks32(typename ClockT::time_point t) {
        return static_cast<std::uint32_t>(
          std::chrono::duration_cast<std::chrono::duration<std::int64_t, Unit>>(
            t.time_since_epoch())
            .count());
    }

    template<typename Unit,
             typename Rep,
             typename P>
    constexpr std::uint32_t units32(std::chrono::duration<Rep,
                                                          P> d) {
        auto const n
          = std::chrono::duration_cast<std::chrono::duration<std::int64_t, Unit>>(d).count();
        assert(n >= 0 && n < (std::int64_t{1} << 31));   // below half the range
        return static_cast<std::uint32_t>(n);
    }
}   // namespace detail

// Every in 8 bytes: the last grid point and the period, both in Unit, 32 bits. It stores no
// position to order: due() compares the unsigned distance `now - last` with the period, which
// is right for any gap below 2^32 Units. Limits: the period below 2^31 Units, and a poll at
// least every 2^32 - period Units (std::milli: 49.7 days, std::micro: 71 min); after a longer
// stall it fires up to one wrap late and nothing can tell. Converting the clock's time to Unit
// is a 64-bit division per poll unless Unit is the clock's own period (RP TIMER + std::micro).
template<PollClock ClockT, typename Unit = std::milli>
class Every32 {
public:
    using clock      = ClockT;
    using time_point = typename ClockT::time_point;
    using ticks      = std::chrono::duration<std::uint32_t, Unit>;

    template<typename Rep,
             typename P>
    constexpr explicit Every32(std::chrono::duration<Rep,
                                                     P> period,
                               time_point               first = ClockT::now())
      : last_{detail::ticks32<ClockT,
                              Unit>(first)
              - detail::units32<Unit>(period)}
      , period_{detail::units32<Unit>(period)} {}

    /// Catch-up, as Every::due().
    [[nodiscard]] constexpr bool due(time_point now = ClockT::now()) {
        if(elapsed(now) < period_) { return false; }
        last_ += period_;
        return true;
    }

    /// As Every::dueCount().
    [[nodiscard]] constexpr std::uint32_t dueCount(time_point now = ClockT::now()) {
        std::uint32_t const k = elapsed(now) / period_;
        last_ += k * period_;
        return k;
    }

    /// As Every::skipTo(): the next due() is one period from now.
    constexpr void skipTo(time_point now) { last_ = detail::ticks32<ClockT, Unit>(now); }

    /// As Every::setPeriod(): counted from the last grid point.
    template<typename Rep,
             typename P>
    constexpr void setPeriod(std::chrono::duration<Rep,
                                                   P> p) {
        period_ = detail::units32<Unit>(p);
    }

    [[nodiscard]] constexpr ticks period() const { return ticks{period_}; }

private:
    constexpr std::uint32_t elapsed(time_point now) const {
        return static_cast<std::uint32_t>(detail::ticks32<ClockT, Unit>(now) - last_);
    }

    std::uint32_t last_;
    std::uint32_t period_;
};

// Deadline in 8 bytes: the start and the interval in Unit, 32 bits (no end position without an
// ordering), armed in the interval's bit 31. Same limits as Every32: interval below 2^31 Units,
// polled at least every 2^32 - interval Units while armed.
template<PollClock ClockT, typename Unit = std::milli>
class Deadline32 {
public:
    using clock      = ClockT;
    using time_point = typename ClockT::time_point;
    using ticks      = std::chrono::duration<std::uint32_t, Unit>;

    constexpr Deadline32() = default;

    template<typename Rep,
             typename P>
    constexpr explicit Deadline32(std::chrono::duration<Rep,
                                                        P> in,
                                  time_point               now = ClockT::now()) {
        restart(in, now);
    }

    template<typename Rep,
             typename P>
    constexpr void restart(std::chrono::duration<Rep,
                                                 P> in,
                           time_point               now = ClockT::now()) {
        start_    = detail::ticks32<ClockT, Unit>(now);
        interval_ = detail::units32<Unit>(in) | ArmedBit;
    }

    constexpr void stop() { interval_ = 0; }

    [[nodiscard]] constexpr bool stopped() const { return (interval_ & ArmedBit) == 0; }

    [[nodiscard]] constexpr bool expired(time_point now = ClockT::now()) const {
        return !stopped() && elapsed(now) >= interval();
    }

    [[nodiscard]] constexpr bool armed(time_point now = ClockT::now()) const {
        return !stopped() && elapsed(now) < interval();
    }

    /// Signed, in Unit: negative once expired, zero when stopped.
    [[nodiscard]] constexpr std::chrono::duration<std::int32_t,
                                                  Unit>
    remaining(time_point now = ClockT::now()) const {
        if(stopped()) { return {}; }
        return std::chrono::duration<std::int32_t, Unit>{
          static_cast<std::int32_t>(interval() - elapsed(now))};
    }

    /// True exactly once, when expired; stopped after it.
    [[nodiscard]] constexpr bool due(time_point now = ClockT::now()) {
        if(!expired(now)) { return false; }
        stop();
        return true;
    }

private:
    static constexpr std::uint32_t ArmedBit = 1U << 31;

    constexpr std::uint32_t interval() const { return interval_ & ~ArmedBit; }

    constexpr std::uint32_t elapsed(time_point now) const {
        return static_cast<std::uint32_t>(detail::ticks32<ClockT, Unit>(now) - start_);
    }

    std::uint32_t start_{};
    std::uint32_t interval_{};
};

}   // namespace Kvasir
