// A fixture for tools/check_fatal_sites.py: a #define body is a region of its own - this one logs through
// KVASIR_DETAIL_CHECK_LOG before beginFatal(), then ends through Check::Detail::failed(). Never compiled.
#define FIXTURE_CHECK(cond)                                   \
    do {                                                      \
        if(!(cond)) {                                         \
            KVASIR_DETAIL_CHECK_LOG(crit, "check failed"_sc); \
            ::Kvasir::Panic::beginFatal();                    \
            ::Kvasir::Check::Detail::failed();                \
        }                                                     \
    } while(false)
