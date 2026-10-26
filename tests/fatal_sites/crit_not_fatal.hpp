// a crit line on a path that goes on: no beginFatal() before it (rule 3)
inline bool readSomething(int ret) {
    if(ret != 0) {
        UC_LOG_C("read failed: {}", ret);
        return false;
    }
    return true;
}
