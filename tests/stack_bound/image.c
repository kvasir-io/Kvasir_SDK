// The stack bound test's crafted image (tests/stack_bound/test_stack_bound.py): a reset entry with a deep chain, a
// self-recursion and an indirect call through a pointer the analysis cannot see, two interrupt handlers with frames
// of known size. -DWITH_FP adds one VFP instruction (the extended exception frame); -DWITH_LEVELS a level table in
// .kvasir.isr_levels: IRQ 0..2 all at level 1 (StartUp.hpp's layout: core, n, (index, level) x n).
typedef void (*fn)(void);
int volatile idx;
extern unsigned stack_top;

__attribute__((noinline)) int deep3(int x) {
    char volatile b[200];
    b[idx] = (char)x;
    return b[3];
}

__attribute__((noinline)) int deep2(int x) {
    char volatile b[64];
    b[idx] = (char)x;
    return b[1] + deep3(x);
}

__attribute__((noinline)) int deep1(int x) {
    int volatile b[4];
    b[idx] = x;
    return b[0] + deep2(x);
}

__attribute__((noinline)) int rec(int n) {
    int volatile k = n;
    return n ? rec(n - 1) + k : 0;
}

fn volatile hook;

__attribute__((noinline)) void bigIsr(void) {
    char volatile b[400];
    b[idx] = 1;
    (void)b[idx];
}

__attribute__((noinline)) void smallIsr(void) {
    int volatile v = 1;
#ifdef WITH_FP
    float volatile f = 1.5f;
    f                = f + (float)v;
#endif
    (void)v;
}

__attribute__((noreturn)) void ResetISR(void) {
    int volatile r = deep1(5) + rec(3);
    (void)r;
    hook();
    for(;;) {}
}

__attribute__((used, section(".vectors"))) void const* const vectors[19] = {&stack_top,
                                                                            (void*)ResetISR,
                                                                            0,
                                                                            0,
                                                                            0,
                                                                            0,
                                                                            0,
                                                                            0,
                                                                            0,
                                                                            0,
                                                                            0,
                                                                            0,
                                                                            0,
                                                                            0,
                                                                            0,
                                                                            0,
                                                                            (void*)smallIsr,
                                                                            (void*)bigIsr,
                                                                            (void*)smallIsr};

#ifdef WITH_LEVELS
__attribute__((used, section(".kvasir.isr_levels"))) short const isr_levels[]
  = {0, 3, 0, 1, 1, 1, 2, 1};
#endif
