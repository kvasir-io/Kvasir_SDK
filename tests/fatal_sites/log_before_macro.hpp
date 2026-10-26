// A fixture for tools/check_fatal_sites.py: a warning line, then the fatal-site macro in the same function - the
// warning comes before the fatal start. Never compiled.
[[noreturn]] void fixtureLogsBeforeMacro(int status) {
    UC_LOG_W("about to give up: {}", status);
    KVASIR_DETAIL_FATAL_AT(Kvasir::Panic::Cause::abort, KVASIR_DETAIL_CALLER_PC(), 0, "gave up");
}
