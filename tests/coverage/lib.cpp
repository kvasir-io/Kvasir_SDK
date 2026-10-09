// instrumented code of the coverage host test: one branch the driver never takes, one function it never calls
int classify(int x) {
    if(x < 0) { return -1; }
    return x % 2 == 0 ? 10 : 11;
}

int never(int x) { return x * 3; }
