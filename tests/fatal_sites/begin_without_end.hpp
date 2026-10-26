// A fixture for tools/check_fatal_sites.py: a beginFatal() on a path that goes on (no raise, not noreturn, no
// marker). Never compiled.
static void fixtureBeginsAndGoesOn(unsigned got) {
    ::Kvasir::Panic::beginFatal();
    UC_LOG_C("only a report: {}", got);
}
