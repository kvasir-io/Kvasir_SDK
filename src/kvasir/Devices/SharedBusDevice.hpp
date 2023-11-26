#pragma once
#include <cstdint>
#include <tuple>

namespace Kvasir {
template<typename SharedBus_>
struct SharedBusDevice {
    using SharedBus = SharedBus_;
    bool         owner_{};
    std::uint8_t error_count{};
    std::uint8_t reset_count{1};

    bool isOwner() const { return owner_; }

    bool acquire() {
        if(owner_) {
            return true;
        }

        owner_ = SharedBus::acquire();
        return owner_;
    }

    void release() {
        if(owner_) {
            SharedBus::release();
            owner_ = false;
        }
    }

    bool needsPowerReset() const { return reset_count > 5; }

    void reset() {
        reset_count = 0;
        release();
    }

    void incementErrorCount() { ++error_count; }

    void resetErrorCount() {
        error_count = 0;
        reset_count = 1;
    }
    template<typename T>
    struct Fail {
        static_assert(std::is_void_v<T>, "invalid resetter");
    };

    template<typename... Resetter>
    bool resetHandler(Resetter&&... resetter) {
        auto impl = [&]<typename T>(T&& v) {
            if constexpr(std::is_same_v<bool, std::remove_cvref_t<T>>) {
                v = true;
            } else if constexpr(requires { v.reset(); }) {
                v.reset();
            } else if constexpr(requires { v(); }) {
                v();
            } else {
                Fail<T>{};
            }
        };

        if(error_count > 5) {
            error_count = 0;
            ++reset_count;
            (impl(std::forward<Resetter>(resetter)), ...);
            return true;
        }

        if(reset_count == 0) {
            ++reset_count;
            (impl(std::forward<Resetter>(resetter)), ...);
            return true;
        }

        return false;
    }
};
}   // namespace Kvasir
