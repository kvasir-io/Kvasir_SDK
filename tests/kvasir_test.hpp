// Shared test infrastructure for the Kvasir register tests.
//
// Provides the Kvasir::Test::read/write mock (enabled via KVASIR_REGISTER_MOCK, see
// src/kvasir/Register/Utility.hpp), a Recorder that captures every register access and
// injects read values, and a small CHECK based test harness in the style of uc_log/tests.
#pragma once

#ifndef KVASIR_REGISTER_MOCK
    #error "the Kvasir register tests must be compiled with KVASIR_REGISTER_MOCK defined"
#endif

#include <cstdint>
#include <deque>
#include <map>
#include <print>
#include <source_location>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace Kvasir { namespace Test {

    struct Recorder {
        struct Read {
            unsigned address;
            unsigned value;   // value returned by this read

            bool operator==(Read const&) const = default;
        };

        struct Write {
            unsigned address;
            unsigned value;

            bool operator==(Write const&) const = default;
        };

        using Action = std::variant<Read, Write>;

        std::vector<Action> actions;
        std::map<unsigned, std::deque<unsigned>>
          readValues;   // address -> values returned in sequence

        void setReadValue(unsigned address,
                          unsigned value) {
            readValues[address] = {value};
        }

        void setReadValues(unsigned             address,
                           std::deque<unsigned> values) {
            readValues[address] = std::move(values);
        }

        void reset() {
            actions.clear();
            readValues.clear();
        }

        template<typename T,
                 unsigned A>
        T read() {
            unsigned returnedValue = 0;
            // return the next injected value if available, otherwise 0
            if(auto it = readValues.find(A); it != readValues.end() && !it->second.empty()) {
                returnedValue = it->second.front();
                it->second.pop_front();
            }
            actions.push_back(Read{A, returnedValue});
            return static_cast<T>(returnedValue);
        }

        template<typename T,
                 unsigned A>
        void write(T v) {
            actions.push_back(Write{A, static_cast<unsigned>(v)});
        }
    };

    inline Recorder recorder{};

    template<typename TRegType,
             unsigned Address>
    TRegType read() {
        return recorder.read<TRegType, Address>();
    }

    template<typename TRegType,
             unsigned Address>
    void write(TRegType v) {
        recorder.write<TRegType, Address>(v);
    }

    inline int              failures = 0;
    inline std::string_view currentTest{};

    inline void printAction(Recorder::Action const& action) {
        if(auto const* r = std::get_if<Recorder::Read>(&action)) {
            std::print("    Read  0x{:02X} -> 0x{:X}\n", r->address, r->value);
        } else {
            auto const& w = std::get<Recorder::Write>(action);
            std::print("    Write 0x{:02X} <- 0x{:X}\n", w.address, w.value);
        }
    }

    inline void printRecordedActions() {
        std::print("  recorded actions:\n");
        for(auto const& action : recorder.actions) { printAction(action); }
    }

    inline void fail(std::string_view     msg,
                     std::source_location loc) {
        ++failures;
        std::print("FAIL [{}] {} ({}:{})\n", currentTest, msg, loc.file_name(), loc.line());
        printRecordedActions();
    }

    // begin a new test case: names failure output and clears the recorder
    inline void test(std::string_view name) {
        currentTest = name;
        recorder.reset();
    }

    template<typename T>
    constexpr unsigned long long asUnsigned(T v) {
        if constexpr(std::is_enum_v<T>) {
            return static_cast<unsigned long long>(std::to_underlying(v));
        } else {
            return static_cast<unsigned long long>(v);
        }
    }

    // launder a value through a volatile so the compiler cannot constant fold it and
    // the runtime (indexed) apply path is exercised
    inline unsigned runtimeValue(unsigned v) {
        unsigned volatile x = v;
        return x;
    }

    // the recorded actions must match exactly (kind, address and value, in order)
    inline void checkActions(std::vector<Recorder::Action> const& expected,
                             std::source_location loc = std::source_location::current()) {
        if(recorder.actions != expected) {
            ++failures;
            std::print("FAIL [{}] recorded actions differ ({}:{})\n",
                       currentTest,
                       loc.file_name(),
                       loc.line());
            std::print("  expected actions:\n");
            for(auto const& action : expected) { printAction(action); }
            printRecordedActions();
        }
    }

    // the recorded actions must match the given kinds in order: 'r' = read, 'w' = write
    inline void checkActionKinds(std::string_view     kinds,
                                 std::source_location loc = std::source_location::current()) {
        bool ok = recorder.actions.size() == kinds.size();
        if(ok) {
            for(std::size_t i = 0; i < kinds.size(); ++i) {
                bool const isRead = std::holds_alternative<Recorder::Read>(recorder.actions[i]);
                if(isRead != (kinds[i] == 'r')) {
                    ok = false;
                    break;
                }
            }
        }
        if(!ok) {
            ++failures;
            std::print("FAIL [{}] expected action kinds \"{}\" ({}:{})\n",
                       currentTest,
                       kinds,
                       loc.file_name(),
                       loc.line());
            printRecordedActions();
        }
    }

    inline std::size_t writeCount(unsigned address) {
        std::size_t count = 0;
        for(auto const& action : recorder.actions) {
            if(auto const* w = std::get_if<Recorder::Write>(&action); w && w->address == address) {
                ++count;
            }
        }
        return count;
    }

    inline std::size_t readCount(unsigned address) {
        std::size_t count = 0;
        for(auto const& action : recorder.actions) {
            if(auto const* r = std::get_if<Recorder::Read>(&action); r && r->address == address) {
                ++count;
            }
        }
        return count;
    }

    // value of the single write to address; fails the test if there is not exactly one
    inline unsigned writtenValue(unsigned             address,
                                 std::source_location loc = std::source_location::current()) {
        unsigned    value = 0;
        std::size_t count = 0;
        for(auto const& action : recorder.actions) {
            if(auto const* w = std::get_if<Recorder::Write>(&action); w && w->address == address) {
                value = w->value;
                ++count;
            }
        }
        if(count != 1) {
            ++failures;
            std::print("FAIL [{}] expected exactly one write to 0x{:02X}, got {} ({}:{})\n",
                       currentTest,
                       address,
                       count,
                       loc.file_name(),
                       loc.line());
            printRecordedActions();
        }
        return value;
    }

}}   // namespace Kvasir::Test

#define CHECK(...)                                                               \
    do {                                                                         \
        if(!(__VA_ARGS__)) {                                                     \
            ::Kvasir::Test::fail(#__VA_ARGS__, std::source_location::current()); \
        }                                                                        \
    } while(false)

#define CHECK_EQ(a_, b_)                                                 \
    do {                                                                 \
        auto const checkEqA_ = ::Kvasir::Test::asUnsigned(a_);           \
        auto const checkEqB_ = ::Kvasir::Test::asUnsigned(b_);           \
        if(checkEqA_ != checkEqB_) {                                     \
            ++::Kvasir::Test::failures;                                  \
            std::print("FAIL [{}] {} == {}: 0x{:X} != 0x{:X} ({}:{})\n", \
                       ::Kvasir::Test::currentTest,                      \
                       #a_,                                              \
                       #b_,                                              \
                       checkEqA_,                                        \
                       checkEqB_,                                        \
                       std::source_location::current().file_name(),      \
                       std::source_location::current().line());          \
            ::Kvasir::Test::printRecordedActions();                      \
        }                                                                \
    } while(false)

#include "kvasir/Register/Register.hpp"
#include "kvasir/Register/Types.hpp"
#include "kvasir/Register/Utility.hpp"
