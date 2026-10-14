// One warning libcxx.cmake does not silence: gcc then names every -Wno-* option it does not know as a note
// ("unrecognized command-line option ... may have been intended to silence earlier diagnostics"), clang warns on
// them anyway. The test expects exactly this warning and nothing else.
#warning "runtime_lib_flags probe"
