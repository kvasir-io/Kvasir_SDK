// A fixture for tools/check_fatal_sites.py: a fatal site that logs before beginFatal(). Never compiled.
[[noreturn]] void fixtureLogsFirst() {
    UC_LOG_C("the line a full ring would drop");
    ::Kvasir::Panic::beginFatal();
    Kvasir::Panic::raiseAt(Kvasir::Panic::Cause::abort, 0, 0);
}
