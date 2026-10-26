// the allow-list: a core's COREFAULT dump logs crit for the fault handler, which began the fatal path
static inline void Log(unsigned const* stack,
                       unsigned        lr) {
    UC_LOG_C("COREFAULT {} {}", stack[0], lr);
}
