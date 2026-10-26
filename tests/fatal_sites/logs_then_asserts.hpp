// A fixture for tools/check_fatal_sites.py: an assert is a fatal site too, its line comes first. Never compiled.
static void fixtureLogsThenAsserts(bool up) {
    if(!up) { UC_LOG_C("the partner did not come up"); }
    assert(up);
}
