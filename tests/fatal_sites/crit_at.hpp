// a crit line through UC_LOG_AT (a wrapper's caller position) outside a fatal path
inline void check(int x) {
    prepare(x);
    UC_LOG_AT(::uc_log::LogLevel::crit, 7, "f.cpp", "bad {}", x);
}
