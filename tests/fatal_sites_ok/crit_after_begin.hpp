// crit after the start, in the same function: a fatal path (its end is the caller's; marked)
inline void dump(int code) {
    ::Kvasir::Panic::beginFatal();   // fatal end: the caller raises right after
    UC_LOG_C("dump {}", code);
}

// the primitive's own line is the start and the crit line together
[[noreturn]] inline void fail(int code) {
    KVASIR_DETAIL_FATAL_AT(Cause::user, 0, code, "fail {}", code);
}

// error and warn lines on paths that go on are no business of rule 3
inline bool tryRead(int ret) {
    if(ret != 0) {
        UC_LOG_E("read failed: {}", ret);
        return false;
    }
    UC_LOG_W("slow");
    return true;
}
