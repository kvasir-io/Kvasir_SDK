// A fixture for tools/check_fatal_sites.py: policies in a namespace at column 0, as ImageCheck's - a beginFatal() in
// one struct must not count for a raise in another. Never compiled.
namespace Policy {
struct Log {
    static void mismatch(unsigned got,
                         unsigned want) {
        ::Kvasir::Panic::beginFatal();
        UC_LOG_C("check: 0x{:08x} vs 0x{:08x}", got, want);
    }
};

template<unsigned Limit>
struct PanicAfter {
    static void mismatch(unsigned got,
                         unsigned want,
                         unsigned inARow) {
        UC_LOG_C("check: 0x{:08x} vs 0x{:08x} ({} of {})", got, want, inARow, Limit);
        if(inARow >= Limit) { Kvasir::Panic::raiseAt(Kvasir::Panic::Cause::abort, 0, got); }
    }
};
}   // namespace Policy
