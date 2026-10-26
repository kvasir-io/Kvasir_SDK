#!/usr/bin/env python3
"""cxx_names: the one short form of a demangled C++ name (kvasir_bench's crash, stack, peek, snapshot, profile) and
size_report's bucket key - including the names the earlier shorteners broke on (operators, deep templates)."""
import sys
import unittest
from pathlib import Path

sys.dont_write_bytecode = True  # no __pycache__ next to the imported tools
HERE = Path(__file__).resolve().parent
# also under python3 -I, which leaves the script's folder off sys.path
sys.path.insert(0, str(HERE))
import cxx_names  # noqa: E402


class StripTemplates(unittest.TestCase):
    def test_templates_and_parameters_go(self):
        name = "a::B<c<int>, 3>::f(d<e>, int) const"
        self.assertEqual(cxx_names.strip_templates(name), "a::B<>::f() const")
        self.assertEqual(cxx_names.strip_templates(
            name, marker="", params=""), "a::B::f const")

    def test_operators_namespaces_and_lambdas_stay(self):
        for name, short in [("(anonymous namespace)::f(int)", "(anonymous namespace)::f()"),
                            ("a::operator<<<int>(x)", "a::operator<<<>()"),
                            ("a::operator<(A<int>, int)", "a::operator<()"),
                            ("B<c>::operator->() const",
                             "B<>::operator->() const"),
                            ("f()::{lambda(int)#1}::operator()(int) const",
                             "f()::{lambda(int)#1}::operator()() const"),
                            ("A<(3 > 2)>::h(int)", "A<>::h()"),
                            ("auto std::as_bytes[abi:de230103]<int, 4u>(std::span<int, 4u>)",
                             "auto std::as_bytes<>()"),
                            ("'lambda'(int)", "'lambda'()")]:
            self.assertEqual(cxx_names.strip_templates(name), short, name)


class DisplayName(unittest.TestCase):
    """The names the old shorteners broke: a regex that stopped after three template levels, a bracket counter
    that took an operator's `<` for a list."""

    def test_an_operator_in_a_location_line(self):
        self.assertEqual(cxx_names.display_name("bool a::operator<(A<int>, int) const"),
                         "bool a::operator<() const")

    def test_five_template_levels_are_one_marker(self):
        self.assertEqual(cxx_names.display_name("void uc_log::detail::Log<a<b<c<d<e>>>>>::record<>(int)"),
                         "void uc_log::detail::Log<>::record<>()")

    def test_conversion_and_new_operators(self):
        self.assertEqual(cxx_names.display_name("a::B<int>::operator unsigned int() const"),
                         "a::B<>::operator unsigned int() const")
        self.assertEqual(cxx_names.display_name(
            "operator new(unsigned int)"), "operator new()")

    def test_the_width(self):
        long = "a::" + "x" * 200 + "(int)"
        self.assertEqual(len(cxx_names.display_name(long)), 90)
        self.assertEqual(len(cxx_names.display_name(long, 120)), 120)


class PlainName(unittest.TestCase):
    """size_report's bucket key (its buckets are pinned by test_size_report)."""

    def test_plain_name(self):
        for name, plain in [
            ("memcpy", "memcpy"),
            ("void a::B<c<int>, 3>::f<x>(d<e>, int) const", "a::B::f"),
            ("(anonymous namespace)::f(int)", "(anonymous namespace)::f"),
            ("__llvm_libc::(anonymous namespace)::shared_errno",
             "__llvm_libc::(anonymous namespace)::shared_errno"),
            ("a::B<int>::operator()(int) const", "a::B::operator()"),
            ("bool operator<(A const&, A const&)", "operator<"),
            ("operator new(unsigned int)", "operator new"),
            ("Kvasir::Startup::StartupImpl<X<1>>::ResetISR()",
             "Kvasir::Startup::StartupImpl::ResetISR"),
            ("main::{lambda(int)#1}::operator()(int) const",
             "main::{lambda(int)#1}::operator()"),
            ("std::__2::array<int, 3ul> x", "x"),
        ]:
            self.assertEqual(cxx_names.plain_name(name), plain, name)


if __name__ == "__main__":
    unittest.main()
