"""What a Kvasir firmware's records say, as text. The printer finds, reads and decodes them (its `records` answer:
uc_log's printer/TargetRecords.hpp is the one host decoder, held to the SDK's headers by tests/records_cross_tests.cpp);
here are only the sentences over that answer, and the check names an image's strings hold (a record keeps a name's
hash)."""
import re

from .image import addr2line


# the fault record's register lines, by the printer's names of its words (the answer's `words`)
FAULT_REGISTERS = ("r0", "r1", "r2", "r3", "r12", "xpsr", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11",
                   "exc_return", "msp", "psp", "control", "sp")
FAULT_SYSTEM = ("shcsr", "icsr", "sfsr", "sfar", "msplim", "psplim")


def full_records(s, answer: dict) -> None:
    """The crash records (kvasir/Runtime/CrashRecord.hpp) of a records answer, where the image has them: lastFault,
    lastStack<N>, lastPanic."""
    tree = s.tree
    f = answer.get("fault")
    if f is not None and not f["valid"]:
        print("  the fault record (lastFault) is empty or not valid: no fault since a boot line reported one")
    elif f is not None:
        w = {word["name"]: word["value"] for word in f["words"]}
        flags = f["flag_names"]
        where = addr2line(tree, [w["pc"] & ~1, w["lr"] & ~1])
        print(f"  the fault record (the first of {w['count']} since a boot line last reported one), "
              f"core {f['core']}, Armv{f['arch']}-M, {' '.join(flags)}:")
        print(f"    cfsr {w['cfsr']:#010x} ({' '.join(f['cfsr_names']) or '-'}) hfsr {w['hfsr']:#010x} "
              f"mmfar {w['mmfar']:#010x} bfar {w['bfar']:#010x} icsr {w['icsr']:#010x}")
        if "frameValid" in flags:
            print(
                f"    faulting pc {w['pc']:#010x}  {where.get(w['pc'] & ~1, '')}")
            print(
                f"    its lr      {w['lr']:#010x}  {where.get(w['lr'] & ~1, '')}")
        else:
            print("    no stacked frame (it was not pushed, or lies outside RAM)")
        print("    " + " ".join(f"{k}={w[k]:#x}" for k in FAULT_REGISTERS))
        print("    " + " ".join(f"{k}={w[k]:#x}" for k in FAULT_SYSTEM))
        # the snapshot belongs to the fault record; once that is taken (a boot line reported it) the
        # snapshot left in RAM is that old fault's
        stack = answer.get("stack")
        if stack is not None and stack["valid"]:
            code = [(i, v) for i, v in enumerate(
                stack["body"]) if s.image.is_code(v)]
            where = addr2line(tree, [v & ~1 for _, v in code])
            print(f"  the stack snapshot: {stack['bytes']} bytes from {stack['from']:#010x}; the words that point "
                  "into code:")
            for i, v in code:
                print(
                    f"    {stack['from'] + 4 * i:#010x} {v:#010x}  {where.get(v & ~1, '')}")
    p = answer.get("panic")
    if p is not None and not p["valid"]:
        print("  the panic record (lastPanic) is empty or not valid: no panic since a boot line reported one")
    elif p is not None:
        r, name = p["record"], p["cause_name"]
        extra = ""
        if name == "unhandled interrupt":
            extra = f", exception {r['detail']} = IRQ {r['detail'] - 16}"
        elif r["detail"]:
            extra = f", detail {r['detail']}"
        # the chip's free-running counter (Port::FreeRunningUs; RP: the TIMER since its release in that boot)
        when = f"at {p['time_us']} us" if p[
            "time_us"] else "time 0 (no counter, or it did not run)"
        head = (f"  the panic record (the first of {r['count']} since a boot line last reported one): "
                f"{name}{extra}, core {r['core']}, sp {r['sp']:#010x}, {when}, ")
        if "site" not in p:
            print(head + "no program site")
        else:
            # pc is raise()'s return address; after a call that does not return it may point past the caller's
            # end, into the next function: the printer's site is the call itself
            where = addr2line(tree, [p["site"]])
            print(
                head + f"raised by the call before {r['pc']:#010x}  {where.get(p['site'], '')}")


def fnv1a(text: bytes) -> int:
    """Kvasir::Health::fnv1a: what a record stores instead of the check's name."""
    h = 0x811C9DC5
    for c in text:
        h = ((h ^ c) * 0x01000193) & 0xFFFFFFFF
    return h


def names_by_hash(resident, wanted: set[int]) -> dict[int, str]:
    """The check names behind FNV-1a hashes, found among the strings of the image (`resident`, as
    image_reader takes it): a check's name is a string_view the supervisor keeps in flash, without its
    own symbol and maybe merged into a longer string - so whole strings first, then their parts."""
    found: dict[int, str] = {}
    runs = [m.group(0) for _name, _start, data in resident for m in re.finditer(
        rb"[\x20-\x7e]+", data)]
    for run in runs:
        h = fnv1a(run)
        if h in wanted and h not in found:
            found[h] = run.decode()
    for run in runs:
        if len(found) == len(wanted):
            break
        for i in range(len(run)):
            h = 0x811C9DC5
            for j in range(i, min(len(run), i + 64)):
                h = ((h ^ run[j]) * 0x01000193) & 0xFFFFFFFF
                if h in wanted and h not in found:
                    found[h] = run[i:j + 1].decode()
    return found


def health_lines(answer: dict, resident) -> list[str]:
    """What the health supervisor's and the boot guard's .noInit records say (Kvasir::Health::lastStarvation,
    lastRecovery, Kvasir::BootGuard::state) in a records answer of kind health, read without taking them: they survive
    the reset a starvation ends in, until the next boot's report takes them. `resident`: the image's strings, where
    the check names are."""
    found = {key: answer.get(key)
             for key in ("starvation", "recoveries", "guard")}
    starved, recovered, guard = (
        r["record"] if r is not None and r["valid"] else None for r in found.values())
    has_starved, has_recovered = found["starvation"] is not None, found["recoveries"] is not None
    # a record holds the check's index and the hash of its name; the index alone (a recovery cut short, the guard's
    # detail) borrows the name from a record that pairs the same index with a hash
    pairs = {}
    if starved:
        pairs[starved["check"]] = starved["nameHash"]
    if recovered and recovered["total"]:
        pairs.setdefault(recovered["lastCheck"], recovered["lastNameHash"])
    names = names_by_hash(resident, set(pairs.values())) if pairs else {}

    def check(index, name_hash=None):
        name_hash = pairs.get(index) if name_hash is None else name_hash
        if name_hash is None:
            return f"check #{index}"
        name = names.get(name_hash)
        return (f"check '{name}' (#{index})" if name is not None
                else f"check #{index} (name hash {name_hash:#010x}: no such name in this image)")
    lines = []
    if starved:
        lines.append(f"  the health starvation record: {check(starved['check'], starved['nameHash'])} starved, "
                     f"silent {starved['silentMs']} ms of the {starved['allowedMs']} ms allowed, "
                     f"{starved['uptimeMs']} ms after boot; checks past half their window "
                     f"{starved['silentMask']:#x}")
    elif has_starved:
        lines.append("  the health starvation record (lastStarvation) is empty: no check starved, or the boot "
                     "report took it")
    if recovered:
        r = recovered
        text = f"  the health recovery record: {r['total']} recover{'y' if r['total'] == 1 else 'ies'} this run"
        if r["total"]:
            text += (f", the last {check(r['lastCheck'], r['lastNameHash'])} at {r['lastUptimeMs']} ms after "
                     f"boot")
        lines.append(text)
        cut = found["recoveries"]
        if "in_progress_check" in cut:
            lines.append(f"    {check(cut['in_progress_check'])}'s onStarved was running, attempt "
                         f"{cut['in_progress_attempt']}: a reset inside it (the watchdog bit during the recovery) - "
                         "the boot guard counts it as a starvation")
        if r["escalatedAttempts"]:
            lines.append(
                f"    escalated to the policy after {r['escalatedAttempts']} recovery attempt(s)")
    elif has_recovered:
        lines.append("  the health recovery record (lastRecovery) is empty: no onStarved ran, or the boot report "
                     "took it")
    if guard:
        g, failure = guard, found["guard"]["failure_name"]
        causes = answer["panic_causes"]
        if failure == "panic":
            detail = causes[g["lastDetail"]] if g["lastDetail"] < len(
                causes) else f"cause {g['lastDetail']}"
            failure += f" ({detail})"
        elif failure == "starvation":
            failure += f" ({check(g['lastDetail'])})"
        elif failure == "external":
            failure += " (the reset pin, an external watchdog)"
        flags = found["guard"]["guard_flag_names"]
        lines.append(f"  the boot guard: boot {g['bootCount']}, {g['consecutive']} failed run(s) in a row, "
                     f"{g['total']} in all, the last failure {failure}; {' '.join(flags) or 'no flags'}")
    return lines


def print_health_records(s, answer: dict) -> None:
    for line in health_lines(answer, s.image.resident()):
        print(line)
