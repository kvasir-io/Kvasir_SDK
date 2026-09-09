// Tests for Kvasir::StaticFunction, a type erased callable with fixed inline storage.
#include "kvasir/Util/StaticFunction.hpp"
#include "test_harness.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

using Kvasir::StaticFunction;

namespace {

int freeFunction(int a,
                 int b) {
    return a + b;
}

void voidFunction() {}

struct Functor {
    int offset;

    int operator()(int v) const { return v + offset; }
};

}   // namespace

static void emptyState() {
    Kvasir::Test::test("emptyState");

    StaticFunction<int(int), 16> f;
    CHECK(!f);
    CHECK(!static_cast<bool>(f));

    // assigning a callable makes it engaged again
    f = [](int v) { return v * 2; };
    CHECK(static_cast<bool>(f));
    CHECK_EQ(f(21), 42);

    // reset clears the target
    f.reset();
    CHECK(!f);
}

static void capturelessLambda() {
    Kvasir::Test::test("capturelessLambda");

    StaticFunction<int(int), 16> f{[](int v) { return v + 1; }};
    CHECK(static_cast<bool>(f));
    CHECK_EQ(f(1), 2);
    CHECK_EQ(f(41), 42);
}

static void capturingLambda() {
    Kvasir::Test::test("capturingLambda");

    // deliberately not const: a const local is a constant expression and would be
    // folded into the lambda rather than captured, which is not what we want to test
    int                          offset = 10;
    StaticFunction<int(int), 16> f{[offset](int v) { return v + offset; }};
    CHECK_EQ(f(1), 11);

    // several captured values
    int                          a = 1;
    int                          b = 2;
    int                          c = 3;
    StaticFunction<int(int), 32> g{[a, b, c](int v) { return v + a + b + c; }};
    CHECK_EQ(g(10), 16);

    // a capture by reference: the referenced object must outlive the StaticFunction
    int                        counter = 0;
    StaticFunction<void(), 16> h{[&counter] { ++counter; }};
    h();
    h();
    CHECK_EQ(counter, 2);
}

static void functionPointerAndFunctor() {
    Kvasir::Test::test("functionPointerAndFunctor");

    StaticFunction<int(int, int), 16> f{&freeFunction};
    CHECK_EQ(f(20, 22), 42);

    StaticFunction<void(), 16> v{&voidFunction};
    CHECK(static_cast<bool>(v));
    v();

    StaticFunction<int(int), 16> g{Functor{5}};
    CHECK_EQ(g(37), 42);
}

static void returnAndArgumentTypes() {
    Kvasir::Test::test("returnAndArgumentTypes");

    // void return
    int                           sideEffect = 0;
    StaticFunction<void(int), 16> v{[&sideEffect](int x) { sideEffect = x; }};
    v(7);
    CHECK_EQ(sideEffect, 7);

    // no arguments
    StaticFunction<int(), 16> n{[] { return 3; }};
    CHECK_EQ(n(), 3);

    // several arguments of mixed types
    StaticFunction<int(int, char, bool), 16> m{
      [](int i, char c, bool flag) { return flag ? i + c : i - c; }};
    CHECK_EQ(m(10, char{5}, true), 15);
    CHECK_EQ(m(10, char{5}, false), 5);

    // reference arguments
    StaticFunction<void(int&), 16> r{[](int& out) { out = 99; }};
    int                            target = 0;
    r(target);
    CHECK_EQ(target, 99);
}

static void reassignment() {
    Kvasir::Test::test("reassignment");

    StaticFunction<int(int), 24> f{[](int v) { return v + 1; }};
    CHECK_EQ(f(1), 2);

    // assigning a different callable replaces the target
    f = [](int v) { return v * 10; };
    CHECK_EQ(f(3), 30);

    // ... including one with captures
    int offset = 100;
    f          = [offset](int v) { return v + offset; };
    CHECK_EQ(f(1), 101);

    // ... and a plain function pointer
    f = +[](int v) { return v - 1; };
    CHECK_EQ(f(10), 9);
}

static void copyAndMove() {
    Kvasir::Test::test("copyAndMove");

    int                                offset = 7;
    StaticFunction<int(int), 16> const original{[offset](int v) { return v + offset; }};

    // copy construction from a const lvalue
    StaticFunction<int(int), 16> copy{original};
    CHECK(static_cast<bool>(copy));
    CHECK_EQ(copy(1), 8);
    CHECK_EQ(original(1), 8);   // the source still works

    // copy assignment
    StaticFunction<int(int), 16> assigned;
    assigned = original;
    CHECK_EQ(assigned(2), 9);

    // move construction and assignment (the target is trivially copyable, so a move is a
    // copy and the source stays usable)
    StaticFunction<int(int), 16> source{[](int v) { return v * 3; }};
    StaticFunction<int(int), 16> moved{std::move(source)};
    CHECK_EQ(moved(4), 12);

    StaticFunction<int(int), 16> moveAssigned;
    StaticFunction<int(int), 16> source2{[](int v) { return v * 4; }};
    moveAssigned = std::move(source2);
    CHECK_EQ(moveAssigned(4), 16);

    // an empty function copies as empty
    StaticFunction<int(int), 16> const emptyOriginal;
    StaticFunction<int(int), 16>       emptyCopy{emptyOriginal};
    CHECK(!emptyCopy);
}

// copying from a *non const* lvalue must select the copy constructor rather than the
// generic StaticFunction(F&&) one, which would otherwise try to store the StaticFunction
// inside itself
static void copyFromNonConstLvalue() {
    Kvasir::Test::test("copyFromNonConstLvalue");

    StaticFunction<int(int), 16> source{[](int v) { return v + 1; }};

    StaticFunction<int(int), 16> copy{source};
    CHECK(static_cast<bool>(copy));
    CHECK_EQ(copy(1), 2);

    StaticFunction<int(int), 16> assigned;
    assigned = source;
    CHECK_EQ(assigned(10), 11);

    // the source is still callable
    CHECK_EQ(source(100), 101);
}

// a StaticFunction may be widened into one with a bigger storage buffer
static void conversionToLargerStorage() {
    Kvasir::Test::test("conversionToLargerStorage");

    int offset = 5;

    StaticFunction<int(int), 8> const small{[offset](int v) { return v + offset; }};
    CHECK_EQ(small(1), 6);

    // converting construction
    StaticFunction<int(int), 32> wide{small};
    CHECK(static_cast<bool>(wide));
    CHECK_EQ(wide(1), 6);
    CHECK_EQ(wide(37), 42);

    // converting assignment
    StaticFunction<int(int), 32> assigned;
    assigned = small;
    CHECK_EQ(assigned(2), 7);

    // the original is unaffected
    CHECK_EQ(small(3), 8);

    // widening an empty function keeps it empty
    StaticFunction<int(int), 8> const  emptySmall;
    StaticFunction<int(int), 24> const emptyWide{emptySmall};
    CHECK(!emptyWide);
}

// storage size accounting: a callable must fit, and the type must stay usable at the
// smallest size that can hold it
static void storageSizes() {
    Kvasir::Test::test("storageSizes");

    // a captureless lambda is an empty type, but its size is still 1
    StaticFunction<int(int), 1> tiny{[](int v) { return v; }};
    CHECK_EQ(tiny(5), 5);

    // exactly the size of the captured state
    std::int32_t                                value = 0x1234;
    StaticFunction<int(), sizeof(std::int32_t)> exact{[value] { return int(value); }};
    CHECK_EQ(exact(), 0x1234);

    // a larger capture in a larger buffer
    struct Big {
        std::uint64_t a;
        std::uint64_t b;
        std::uint64_t c;
    };

    Big const                           big{1, 2, 3};
    StaticFunction<std::uint64_t(), 32> fat{[big] { return big.a + big.b + big.c; }};
    CHECK_EQ(fat(), 6U);
}

int main() {
    emptyState();
    capturelessLambda();
    capturingLambda();
    functionPointerAndFunctor();
    returnAndArgumentTypes();
    reassignment();
    copyAndMove();
    copyFromNonConstLvalue();
    conversionToLargerStorage();
    storageSizes();

    return Kvasir::Test::report();
}
