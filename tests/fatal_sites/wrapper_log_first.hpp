// A fixture for tools/check_fatal_sites.py: a fatal site whose line goes through a wrapper (UBSAN_REPORT) before
// beginFatal(). Never compiled.
[[noreturn]] void fixtureWrapperLogsFirst() {
    UBSAN_REPORT();
    ::Kvasir::Panic::beginFatal();
    ::Kvasir::Panic::raiseAt(::Kvasir::Panic::Cause::undefinedBehaviour, 0);
}
