// Kvasir::Atomic::Queue built with NDEBUG (asserts compiled out).
//
// OverflowPolicyAssert is [[noreturn]]: with assert(false) gone it must still not return (it
// traps). Each case runs in a forked child that _exit(0)s if the call came back; the parent
// wants the child killed by a signal (SIGILL on x86-64's ud2, SIGTRAP on AArch64's brk - any
// signal counts, the test prints which). Built with -Werror, the binary also proves the header
// has no "noreturn function does return" warning under NDEBUG.
#ifndef NDEBUG
    #error "this test is built with NDEBUG"
#endif

#include "kvasir/Atomic/Queue.hpp"
#include "test_harness.hpp"

#include <array>
#include <csignal>
#include <print>
#include <sys/wait.h>
#include <unistd.h>

namespace {

// runs f in a child; true if the child died by a signal instead of returning
template<typename F>
bool diesBySignal(char const* what,
                  F           f) {
    pid_t const pid = ::fork();
    if(pid == 0) {
        f();
        ::_exit(0);   // the call returned: the bug
    }
    if(pid < 0) {
        std::print("{}: fork failed\n", what);
        return false;
    }
    int status = 0;
    if(::waitpid(pid, &status, 0) != pid) { return false; }
    if(WIFSIGNALED(status)) {
        std::print("{}: child ended by signal {}\n", what, WTERMSIG(status));
        return true;
    }
    std::print("{}: child exited with {} (the call returned)\n", what, WEXITSTATUS(status));
    return false;
}

void overflowTraps() {
    Kvasir::Test::test("overflowTraps");
    CHECK(diesBySignal("push into a full queue", [] {
        Kvasir::Atomic::Queue<int, 4> q;   // default policy: OverflowPolicyAssert
        for(int i = 0; i != 5; ++i) { q.push(i); }
    }));
    CHECK(diesBySignal("range push that does not fit", [] {
        Kvasir::Atomic::Fifo<int, 3> q;
        std::array<int, 4> const     in{1, 2, 3, 4};
        q.push(in);
    }));
}

void frontOnEmptyTraps() {
    Kvasir::Test::test("frontOnEmptyTraps");
    // front()'s precondition is !empty(); with the default policy a breach does not return
    CHECK(diesBySignal("front() on an empty queue", [] {
        Kvasir::Atomic::Queue<int, 4> const q;
        static_cast<void>(q.front());
    }));
}

void fullQueueDoesNotTrapBeforeItIsFull() {
    Kvasir::Test::test("fullQueueDoesNotTrapBeforeItIsFull");
    Kvasir::Atomic::Fifo<int, 4> q;
    for(int i = 0; i != 4; ++i) { q.push(i); }
    CHECK_EQ(q.size(), 4U);
    CHECK(q.peek() != nullptr);
    CHECK_EQ(*q.peek(), 0);
    Kvasir::Atomic::Fifo<int, 4> const empty;
    CHECK(empty.peek() == nullptr);   // peek() on empty: no policy, no trap
}

}   // namespace

int main() {
    overflowTraps();
    frontOnEmptyTraps();
    fullQueueDoesNotTrapBeforeItIsFull();
    return Kvasir::Test::report();
}
