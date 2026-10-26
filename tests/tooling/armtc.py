"""The ARM/LLVM tools the SDK's tool tests use, and when a missing one is a skip or a failure.

KVASIR_TEST_REQUIRE (comma-separated tool groups, unset locally) names the groups a CI job installed: a test that
would skip for a missing tool of a named group fails instead, so a renamed exe or a broken lookup cannot turn a test
into a permanent green skip. Unset, a missing tool is a skip (a lone checkout with nothing installed).

    import armtc                       # sys.path.insert(0, <Kvasir_SDK>/tests/tooling) first
    armtc.need(self, 'clang++', 'ld.lld')
    objcopy = armtc.first(self, 'llvm-objcopy', 'arm-none-eabi-objcopy')
    for tc in armtc.toolchains(self): ...

Every exe belongs to exactly one group; an exe in no group (ninja, a demangler of the host) is never required.
`case` may be None (a setUpClass): the skip or the failure is then raised directly.
"""

import os
import shutil
import unittest

GROUPS = {
    # every CI job: arm-none-eabi-gcc + newlib from pacman
    'arm-gcc': {'arm-none-eabi-gcc', 'arm-none-eabi-g++', 'arm-none-eabi-ld', 'arm-none-eabi-objcopy',
                'arm-none-eabi-nm', 'arm-none-eabi-size', 'arm-none-eabi-objdump', 'arm-none-eabi-readelf'},
    # the clang jobs
    'clang': {'clang', 'clang++', 'ld.lld'},
    'llvm': {'llvm-objcopy', 'llvm-nm', 'llvm-size', 'llvm-readelf', 'llvm-objdump', 'llvm-profdata', 'llvm-cov',
             'llvm-dwarfdump', 'llvm-cxxfilt'},
    # the host's GNU compiler and binutils (base-devel, in every job)
    'host-cxx': {'g++', 'c++filt'},
}

ENV = 'KVASIR_TEST_REQUIRE'

# the ARM C++ toolchains the image tests build with: the driver line, and the programs the build needs (clang without
# lld cannot link: the CI's plain clang job before it installed lld)
TOOLCHAINS = {
    'gcc': (['arm-none-eabi-g++', '-nostartfiles'], ['arm-none-eabi-g++']),
    'clang': (['clang++', '--target=armv8m.main-none-eabi', '-fuse-ld=lld'], ['clang++', 'ld.lld']),
}


def required():
    """The groups KVASIR_TEST_REQUIRE names; an unknown name is an error (a typo would require nothing)."""
    value = os.environ.get(ENV, '')
    names = {n.strip() for n in value.split(',') if n.strip()}
    if 'all' in names:
        return set(GROUPS)
    unknown = names - set(GROUPS)
    if unknown:
        raise ValueError(f'{ENV}: unknown tool group(s) {", ".join(sorted(unknown))}; known: '
                         f'{", ".join(sorted(GROUPS))} (or all)')
    return names


def group_of(exe):
    for name, exes in GROUPS.items():
        if exe in exes:
            return name
    return None


def _missing(case, exes):
    """Skip, or fail when one of the missing exes is in a required group."""
    req = required()
    forced = [(e, group_of(e)) for e in exes if group_of(e) in req]
    if forced:
        exe, group = forced[0]
        msg = f'{exe} not found, but tool group {group} is required ({ENV})'
        if case is None:
            raise AssertionError(msg)
        case.fail(msg)
    msg = f'{" / ".join(exes)} not found'
    if case is None:
        raise unittest.SkipTest(msg)
    case.skipTest(msg)


def need(case, *exes):
    """Every exe must be on PATH: else a skip, or a failure for a required group."""
    for exe in exes:
        if shutil.which(exe) is None:
            _missing(case, [exe])


def first(case, *alternatives):
    """The path of the first alternative found; none found: a skip, or a failure if one of them is required."""
    for exe in alternatives:
        path = shutil.which(exe)
        if path is not None:
            return path
    _missing(case, list(alternatives))
    return None   # not reached: _missing raises


def usable(toolchain):
    return all(shutil.which(e) is not None for e in TOOLCHAINS[toolchain][1])


def toolchains(case, table=None):
    """The keys of `table` (default TOOLCHAINS: key -> (flags, exes)) whose exes are all here. One that is missing a
    tool of a required group fails; none usable at all is a skip."""
    table = TOOLCHAINS if table is None else table
    found = []
    for key, (_, exes) in table.items():
        missing = [e for e in exes if shutil.which(e) is None]
        if not missing:
            found.append(key)
        elif any(group_of(e) in required() for e in missing):
            _missing(case, missing)
    if not found:
        _missing(case, [e for _, (_, exes) in table.items() for e in exes])
    return found
