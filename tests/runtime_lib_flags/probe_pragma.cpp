// What libc++ 23's _LIBCPP_BEGIN/END_NAMESPACE_STD expand to, spelled out in a file that is not a system header: a gcc
// that suppresses diagnostics from system-header macros (gcc 16) stays silent on libc++'s own sources, older ones warn.
// libcxx.cmake's flags must keep it silent on every compiler.
#pragma clang attribute _LibcxxExplicitABIAnnotations.push( \
  __attribute__((__visibility__("hidden"))),                \
  apply_to = function)

namespace probe {
int answer();

int answer() { return 42; }
}   // namespace probe

#pragma clang attribute _LibcxxExplicitABIAnnotations.pop
