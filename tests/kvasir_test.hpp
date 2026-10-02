// Register mock test infrastructure.
//
// Provides the Kvasir::Test::read/write mock (enabled via KVASIR_REGISTER_MOCK, see
// src/kvasir/Register/Utility.hpp) and a Recorder that captures every register access and
// injects read values. The CHECK based harness itself lives in test_harness.hpp; including
// this header additionally routes failure output through the recorded action log.
#pragma once

#ifndef KVASIR_REGISTER_MOCK
    #error "the Kvasir register tests must be compiled with KVASIR_REGISTER_MOCK defined"
#endif

#include "test_harness.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <print>
#include <source_location>
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

        // A register with state: what it holds, and how a read or a write changes that. An address
        // with a model answers its reads from the model instead of the readValues queue; the
        // accesses are recorded either way.
        struct Model {
            unsigned value = 0;   // what the register holds

            // Applied in this order on a write: plain bits take the written value, one-to-* bits
            // are changed by written ones only, read-only bits keep their value, strobes are
            // never stored.
            unsigned readOnlyMask    = 0;
            unsigned oneToClearMask  = 0;
            unsigned oneToSetMask    = 0;
            unsigned oneToToggleMask = 0;
            unsigned selfClearMask   = 0;   // strobes: never stored, always read 0
            unsigned clearOnReadMask = 0;   // cleared after every read

            // Full control. onRead gets the stored value and returns what the read yields (the
            // stored value is then run through clearOnReadMask). onWrite gets (stored, written) and
            // returns the new stored value; when set it replaces the mask logic.
            std::function<unsigned(unsigned)>           onRead;
            std::function<unsigned(unsigned, unsigned)> onWrite;

            Model& readOnly(unsigned m) {
                readOnlyMask |= m;
                return *this;
            }

            Model& oneToClear(unsigned m) {
                oneToClearMask |= m;
                return *this;
            }

            Model& oneToSet(unsigned m) {
                oneToSetMask |= m;
                return *this;
            }

            Model& oneToToggle(unsigned m) {
                oneToToggleMask |= m;
                return *this;
            }

            Model& selfClearing(unsigned m) {
                selfClearMask |= m;
                return *this;
            }

            Model& clearOnRead(unsigned m) {
                clearOnReadMask |= m;
                return *this;
            }

            unsigned read() {
                unsigned const v = onRead ? onRead(value) : value;
                value &= ~clearOnReadMask;
                return v;
            }

            void write(unsigned w) {
                if(onWrite) {
                    value = onWrite(value, w);
                    return;
                }
                unsigned const special
                  = readOnlyMask | oneToClearMask | oneToSetMask | oneToToggleMask | selfClearMask;
                unsigned v = (value & special) | (w & ~special);
                v &= ~(w & oneToClearMask);
                v |= (w & oneToSetMask);
                v ^= (w & oneToToggleMask);
                v &= ~selfClearMask;
                value = v;
            }
        };

        std::vector<Action> actions;
        std::map<unsigned, std::deque<unsigned>>
          readValues;   // address -> values returned in sequence
        std::map<unsigned, Model>
          models;   // address -> model; empty = every read comes from readValues

        // gives the address state from now on (the same model on every call)
        Model& model(unsigned address) { return models[address]; }

        void setReadValue(unsigned address,
                          unsigned value) {
            refuseModelled(address);
            readValues[address] = {value};
        }

        void setReadValues(unsigned             address,
                           std::deque<unsigned> values) {
            refuseModelled(address);
            readValues[address] = std::move(values);
        }

        void reset() {
            actions.clear();
            readValues.clear();
            models.clear();
        }

        template<typename T,
                 unsigned A>
        T read() {
            unsigned returnedValue = 0;
            if(auto m = models.find(A); m != models.end()) {
                returnedValue = m->second.read();
            } else if(auto it = readValues.find(A); it != readValues.end() && !it->second.empty()) {
                // the next injected value if available, otherwise 0
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
            if(auto m = models.find(A); m != models.end()) {
                m->second.write(static_cast<unsigned>(v));
            }
        }

    private:
        // a queued read value on a modelled address would never be returned: a test bug
        void refuseModelled(unsigned address) {
            if(models.contains(address)) {
                ++failures;
                std::print("FAIL [{}] setReadValue on 0x{:02X}, which has a model\n",
                           currentTest,
                           address);
            }
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

    // route harness failures through the recorded action log and clear the recorder for
    // every new test case
    inline bool const registerMockHooksInstalled = [] {
        failureContextPrinter = &printRecordedActions;
        testResetHook         = [] { recorder.reset(); };
        return true;
    }();

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

#include "kvasir/Register/Register.hpp"
#include "kvasir/Register/Types.hpp"
#include "kvasir/Register/Utility.hpp"
