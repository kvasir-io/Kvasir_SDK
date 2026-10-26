// a #define body that logs crit without starting the fatal path first
#define REPORT_BAD(x)          \
    do {                       \
        prepare(x);            \
        UC_LOG_C("bad {}", x); \
    } while(false)
