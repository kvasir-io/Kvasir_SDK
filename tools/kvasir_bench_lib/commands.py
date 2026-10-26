"""Every command: `fn(s: Session, args)`, registered with its arguments and guards (session.command). The order
here is the order of the subcommands in the help."""
import argparse
import collections
import importlib.util
import os
import re
import signal
import subprocess
import sys
import time
import typing
from pathlib import Path

from cxx_names import display_name  # noqa: E402
from uc_log_client import (LEVELS, READ_PIECES, Connection, ControlError, history, log_line_text, message_kind,  # noqa: E402
                           metric_key, read_bytes, req_flash, req_go, req_halt, req_messages, req_raise_panic, req_read,
                           req_read_words, req_registers, req_reset, req_subscribe, req_wait, req_write, request, words)

from . import die, gdb_stub, printer, profiling, records, refuse, trees
from .coverage_data import assemble_profraw, image_reader
from .image import addr2line
from .session import Session, arg, command


# ---- printer ---------------------------------------------------------------------------------


@command("printer", "start")
def printer_start(s: Session, _args) -> None:
    tree = s.tree
    pid = printer.start_printer(tree)
    control = printer.control_socket(tree)
    print(f"printer {pid} on {'J-Link ' + tree.host if tree.host else 'probe ' + repr(tree.probe or 'any')}, device {tree.device}, "
          f"log {tree.log_dir}" + (f", control socket {control}" if control else " (no control socket)"))


@command("printer", "stop",
         args=[arg("--confirm", action="store_true", help="stop a printer that has a window (somebody's `just log`) too")])
def printer_stop(s: Session, args) -> None:
    hit = printer.printers_of(s.tree)
    if not hit:
        print("no printer of this target is running")
    # a printer with a window is somebody's `just log`: not ours to end unasked
    windows = [p.pid for p in hit if p.has_window]
    if windows and not args.confirm:
        refuse(f"printer {', '.join(map(str, windows))} has a window - a `just log`, most "
               "likely. Use it instead (every command goes through it); stop it only when asked, "
               "with --confirm")
    for p in hit:
        os.kill(p.pid, signal.SIGINT)
    # gone before we return: a dying printer still answers on its socket and holds the probe (its USB handle goes
    # with the process) - a printer started after a stop that returned early fights the old one over the probe
    end = time.monotonic() + 10
    alive = []
    for p in hit:
        while time.monotonic() < end and p.alive():
            time.sleep(0.05)
        if p.alive():
            alive.append(p.pid)
        else:
            print(f"stopped printer {p.pid}")
    if alive:
        die(f"printer {', '.join(map(str, alive))} still exiting after 10 s: it holds the probe until it is gone - "
            "wait for it before starting another")


@command("printer", "status", target=False, args=[arg("build", nargs="?"), arg("target", nargs="?")])
def printer_status(_s, _args) -> None:
    rows = printer.printers()
    if not rows:
        print("no uc_log_printer is running")
    for p in rows:
        print(
            f"pid {p.pid}  probe {p.probe:<12} up {p.age:6.0f} s  {p.log_dir or ''}")


# ---- flash, reset ----------------------------------------------------------------------------


@command("flash", what="the flash", confirm="flash", args=[arg("--no-build", action="store_true"), "confirm"])
def flash(s: Session, args) -> None:
    tree = s.tree
    if not args.no_build:
        built = subprocess.run(["cmake", "--build", str(tree.build), "--target", tree.target],
                               capture_output=True, text=True)
        if built.returncode != 0:
            bad = [l for l in (built.stdout + built.stderr).splitlines()
                   if re.search(r"error|FAILED|warning", l)]
            die("the build failed:\n" +
                "\n".join((bad or built.stdout.splitlines())[-15:]))
    control = s.control
    printer.need_ram_image_printer(tree, control, "flash")
    printer.set_mark(tree, control)
    t0 = time.time()
    try:
        request(control, req_flash(), timeout=150)
    except ControlError as e:
        die(f"flash through the printer: {e}")
    print(
        f"flashed {tree.target} through the printer in {time.time() - t0:.1f} s; the log goes on")
    print(printer.firmware_line(tree, control))


@command("reset", what="the reset", confirm="reset",
         args=[arg("--line", action="store_true", help="through the probe's reset pin (nRESET on RUN): a chip-level reset"),
               "confirm"])
def reset(s: Session, args) -> None:
    tree, control = s.tree, s.control
    printer.need_ram_image_printer(tree, control, "reset")
    printer.set_mark(tree, control)
    try:
        request(control, req_reset(line=args.line), timeout=40)
    except ControlError as e:
        die(f"reset through the printer: {e}")
    print("reset through the reset line; the log goes on" if args.line else "reset; the log goes on")
    print(printer.firmware_line(tree, control))


# ---- crash, stack, ub, coverage --------------------------------------------------------------


def stop_on_unread(answer: dict) -> None:
    """A record whose read failed is no empty record: that is said and the command stops, as a refused read always
    made it stop - after the halt's own lines, which come from the printer's message, not from a read."""
    for key in ("fault", "stack", "panic", "starvation", "recoveries", "guard"):
        r = answer.get(key)
        if r is not None and "error" in r:
            die(f"the {key} record at {r['address']:#010x} could not be read through the printer: {r['error']}")


@command("crash", what="the halt", checks_build=True, args=["stale_ok"])
def crash(s: Session, _args) -> None:
    tree, control = s.tree, s.control
    entry = None
    for m in request(control, req_messages(1000))["messages"]:
        if message_kind(m) == "halt":
            entry = {"recv_time_utc": m["time"], "message": m["text"]}
    if entry is None:
        # a reboot (the default fatal end) or a starvation's watchdog reset is no halt: the records are in RAM
        # until the next boot report takes them
        print("no 'core halted' message: the printer did not see the core stop")
        answer = s.records(["crash", "health"])
        stop_on_unread(answer)
        records.full_records(s, answer)
        records.print_health_records(s, answer)
        return
    text = entry["message"]
    regs = dict(re.findall(r"(\w+)=(0x[0-9a-f]+|\d+)", text))
    stack = [int(w, 16) for w in text.split("stack:")
             [-1].split()] if "stack:" in text else []
    exception = int(regs.get("exception", "0"), 0)
    answer = s.records(["crash", "health"])
    # the printer's names of exceptions 0-15; 16 and up are interrupts
    name = answer["exception_names"][
        exception] if exception < 16 else f"IRQ {exception - 16}"
    if exception == 0:
        name += " (a breakpoint or a debugger halted it)"
    print(f"core halted at {entry['recv_time_utc']} in {name}")
    is_code = s.image.is_code
    wanted = [int(regs[r], 0) for r in ("pc", "lr") if r in regs]
    wanted += [w for w in stack if is_code(w)]
    where = addr2line(tree, [a & ~1 for a in wanted])
    for r in ("pc", "lr", "sp", "xpsr", "msp", "psp", "cfsr", "hfsr", "mmfar", "bfar"):
        if r in regs:
            a = int(regs[r], 0)
            print(f"  {r:5} {a:#010x}" +
                  (f"  {where.get(a & ~1, '')}" if r in ("pc", "lr") and is_code(a) else ""))
    print("  words on the stack that point into code (return addresses, the stacked pc/lr - "
          "Kvasir's fault handler moves sp, so the faulting frame is in its own log line):")
    for i, w in enumerate(stack):
        if is_code(w):
            print(f"    sp+{4 * i:<3} {w:#010x}  {where.get(w & ~1, '')}")
    stop_on_unread(answer)
    records.full_records(s, answer)
    records.print_health_records(s, answer)
    # this run's fault line only (since the last flash/reset): an older boot's COREFAULT in the
    # printer's history is not this halt's; a fault with an unreadable frame logs no line (the record has it)
    try:
        fault_lines, _ = log_rows(
            tree, control, grep="COREFAULT", since_mark=True)
    except (ControlError, OSError):
        # a lost socket or a refused request: the halt above is the answer, the line is extra
        fault_lines = []
    for line in fault_lines[-1:]:
        print("  log: " + line[:600].strip())


@command("stack", checks_build=True, args=["stale_ok"])
def stack(s: Session, _args) -> None:
    """Core 0's painted stack (Kvasir::StackUsage), as the printer measured it."""
    stacks = {st["name"]: st for st in s.records(["stacks"])["stacks"]}
    st = stacks.get("core 0 stack")
    if st is None:
        die("the map has no _LINKER_stack_start_/_end_")
    if not st["valid"]:
        if "error" in st:
            die(f"the stack {st['low']:#010x}..{st['high']:#010x} could not be read: {st['error']}")
        die("the stack is not painted: put Kvasir::StackUsage into the Startup list "
            "(kvasir/Runtime/StackUsage.hpp)")
    low, high, size, used = st["low"], st["high"], st["size"], st["used"]
    print(f"stack {low:#010x}..{high:#010x}: {size} bytes, deepest use so far {used} bytes "
          f"({100 * used / size:.1f} %), {st['never']} bytes never touched")


@command("ub", what="the UB counter", checks_build=True, args=["stale_ok"])
def ub(s: Session, _args) -> None:
    """The sanitize variant's report counter (kvasir/Runtime/ubsan.hpp), read through the printer (one started for
    this read when none runs): exit 3 when it counted a report."""
    tree = s.tree
    counter = s.records(["ubsan"]).get("ubsan")
    if counter is None:
        die(f"{tree.target} has no sanitizer report counter: it is not a sanitize build")
    if not counter["valid"]:
        die(
            f"the sanitizer report counter could not be read: {counter.get('error', '?')}")
    count, last = counter["record"]["count"], counter["record"]["lastReturnAddress"]
    if count == 0:
        print("0 sanitizer reports since boot")
        return
    where = addr2line(tree, [last & ~1]).get(last & ~1, "")
    print(f"{count} SANITIZER REPORT(S) since boot; the last one returned to {last:#010x}  {where}\n"
          "  (the log has an error line 'UB: <kind> at <address>' per report - crit for the two that end the run -\n"
          "  if a printer ran)")
    sys.exit(3)


@command("coverage", what="coverage", checks_build=True,
         args=[arg("--merge", action="append", default=[],
                   help=".profdata to merge the result with (several runs, both boards)"),
               "stale_ok",
               arg("llvm_cov", nargs=argparse.REMAINDER, help="llvm-cov report arguments after --")])
def coverage(s: Session, args) -> None:
    """A coverage image's live counters as a .profraw (Kvasir::Coverage's manifest), and llvm-cov's report."""
    tree = s.tree
    hit = s.image.exact("Kvasir::Coverage::manifest")
    if hit is None:
        die(f"{tree.target} has no coverage manifest: not a coverage build, or Kvasir::Coverage is not in its Startup")
    flash_bytes = image_reader(s.image.resident())

    try:
        raw = assemble_profraw(s.read_bytes(
            hit.address, hit.size), flash_bytes, s.read_bytes)
    except ValueError as e:
        die(str(e))
    profraw = tree.elf.with_suffix(".profraw")
    profraw.write_bytes(raw)
    profdata = profraw.with_suffix(".profdata")
    merge = subprocess.run(["llvm-profdata", "merge", "-sparse", str(profraw), *args.merge, "-o", str(profdata)],
                           capture_output=True, text=True)
    if merge.returncode != 0:
        die(f"llvm-profdata refused {profraw}: {merge.stderr.strip()}")
    report = [a for a in args.llvm_cov if a != "--"]
    subprocess.run(["llvm-cov", "report", str(tree.elf), f"-instr-profile={profdata}",
                    "-ignore-filename-regex=/lib/(libc|libcxx|compiler-rt)/", *report])
    print(f"{profraw}\n{profdata}\n  llvm-cov show {tree.elf} -instr-profile={profdata} -format=html "
          f"-output-dir={tree.elf.parent / (tree.target + '_coverage_html')}")


# ---- panic -----------------------------------------------------------------------------------


# a panic handler resets as a reset does
@command("panic", what="the panic", confirm="reset", checks_build=True,
         args=[arg("--cause", default="user",
                   help="the Kvasir::Panic::Cause raise() is called with: a cause name with dashes for spaces "
                        "(check-failed; KVASIR_PANIC is user) - the printer's error lists them"),
               arg("--confirm", action="store_true",
                   help="the repo's .kvasir_bench.json says a reset does something physical"),
               arg("--stale-ok", action="store_true",
                   help="raise even if the board runs another build than the tree's")])
def panic(s: Session, args) -> None:
    """The printer halts the core and makes it call Kvasir::Panic::raise(cause) (its raise_panic request: the TUI's
    "Raise panic"), raise() found in its map."""
    control = s.control
    printer.set_mark(s.tree, control)
    s.ask(req_raise_panic(args.cause), timeout=10)
    print(f"raise({args.cause}) called through the printer; the log goes on "
          "(wait-for 'ended in a panic' to see the next boot name it)")


# ---- messages, metrics, status ---------------------------------------------------------------


@command("messages", args=[arg("--last", type=int, default=50), arg("--errors", action="store_true", help="errors only"),
                           arg("--grep", help="lines matching this regex")])
def messages(s: Session, args) -> None:
    lines = printer.status_messages(s.tree, args.last)
    if args.errors:
        lines = [l for l in lines if not re.search(
            r"^\S+ \[(status|tool)\] ", l)]
    if args.grep:
        lines = [l for l in lines if re.search(args.grep, l)]
    print("\n".join(lines) if lines else "no status messages")


@command("metrics", args=[arg("--seconds", type=float, default=3, help="how long to listen (the timeout of --until)"),
                          arg("--name", help="regex on scope::name"),
                          arg("--raw", action="store_true",
                              help="every sample, not the summary"),
                          arg("--until", help="'NAME OP NUMBER': stop when a sample satisfies it")])
def metrics(s: Session, args) -> None:
    """What the firmware logs through uc_log::metric<"name"_sc, "unit"_sc, "scope"_sc>(value):
    the printer takes every such value out of the log and streams it to the subscribers of its
    control socket's metrics stream, one {"cmd":"metric","name":..,"scope":..,"unit":..,
    "time":<uc seconds>,"value":..} a sample - numbers on the target's clock, no text to parse."""
    control = s.control
    wanted = re.compile(args.name) if args.name else None
    until = None
    if args.until:
        m = re.fullmatch(
            r"\s*(\S+)\s*(==|!=|<=|>=|<|>)\s*(-?[\d.eE+-]+)\s*", args.until)
        if not m:
            die("--until takes 'NAME OP NUMBER', e.g. 'position >= 50'")
        ops = {"==": lambda a, b: a == b, "!=": lambda a, b: a != b, "<": lambda a, b: a < b,
               ">": lambda a, b: a > b, "<=": lambda a, b: a <= b, ">=": lambda a, b: a >= b}
        until = (m.group(1), ops[m.group(2)], float(m.group(3)))
    seen: dict[str, list[tuple[float, float]]] = {}
    units: dict[str, str] = {}
    with Connection(control, 2) as stream:
        stream.ask(req_subscribe(["metrics"]))
        for sample in printer.events(stream, args.seconds):
            if sample.get("cmd") == "dropped":
                print(f"({sample['lines']} samples dropped for this client: it read too slowly, "
                      "the rates below are short)", file=sys.stderr)
                continue
            if sample.get("cmd") != "metric":
                continue
            key = metric_key(sample)
            if wanted and not wanted.search(key):
                continue
            units[key] = sample["unit"]
            seen.setdefault(key, []).append((sample["time"], sample["value"]))
            if args.raw:
                print(
                    f"{sample['time']:12.6f} s  {key} = {sample['value']:g} {sample['unit']}")
            if until and sample["name"] == until[0] and until[1](sample["value"], until[2]):
                print(f"{key} = {sample['value']:g} {sample['unit']} at "
                      f"{sample['time']:.6f} s (uc_time)")
                return
    if until:
        last = {k: v[-1][1] for k, v in seen.items()}
        print(
            f"TIMEOUT after {args.seconds:g} s: '{args.until}' never held; last values: {last}")
        sys.exit(2)
    if not seen:
        print(f"no metric in {args.seconds:g} s: the firmware logs none (uc_log::metric<...>), "
              "or not in this time")
        sys.exit(2)
    if args.raw:
        return
    for key, samples in sorted(seen.items()):
        values = [v for _, v in samples]
        span = samples[-1][0] - samples[0][0]
        rate = f"{(len(samples) - 1) / span:.1f}/s" if span > 0 else "-"
        print(f"{key:28} [{units[key] or '-':>3}] n={len(samples):<5} rate={rate:<7} "
              f"min={min(values):g} mean={sum(values) / len(values):g} max={max(values):g} "
              f"last={values[-1]:g}")


@command("status", target=False, args=[arg("build", nargs="?"), arg("target", nargs="?")])
def status(_s, args) -> None:
    """Everything worth knowing before touching the hardware, on one screen."""
    running = printer.printers()
    found = printer.probes_on_usb()
    print(f"{len(found)} J-Link probe(s) on USB, {len(running)} printer(s) running")
    held: dict[str, list[printer.Printer]] = {}
    for p in running:
        held.setdefault(p.probe, []).append(p)
    for serial, product, nick in found:
        mine = held.pop(nick, []) + held.pop(serial, [])
        print(f"probe '{nick or serial}' ({product}, {serial}): " +
              ("free" if not mine else ""))
        for p in mine:
            printer.print_printer(p)
    for where, rows in held.items():
        print(f"probe {where}:")
        for p in rows:
            printer.print_printer(p)
    if args.build and args.target:
        tree = trees.Tree(args.build, args.target)
        control = printer.control_socket(tree)
        print(f"{tree.target} in {tree.build.name}: hex build {trees.hex_build_id(tree)}, "
              + (printer.firmware_line(tree, control) if control else "no printer with a control socket"))


# ---- wait-for, trace, log --------------------------------------------------------------------

MODULE_ARGS = [arg("--module", action="append", metavar="NAME",
                   help="only lines of this log module and those under it (i2c: i2c.bus too); repeatable"),
               arg("--not-module", action="append", metavar="NAME",
                   help="leave out lines of this log module and those under it; repeatable")]


@command("wait-for", what="wait-for",
         args=[arg("regex"), arg("--timeout", type=float, default=10),
               arg("--from-now", action="store_true",
                   help="ignore what is already in the log"),
               arg("--from-start", action="store_true",
                   help="search the whole file first"),
               arg("--last", type=int, default=15,
                   help="lines shown on a timeout"),
               arg("--width", type=int, default=200), *MODULE_ARGS])
def wait_for(s: Session, args) -> None:
    """The printer's log from the mark (the last flash/reset of this tool) on: its history, then
    the live lines, in one stream from the printer, filtered there. A halt status
    message ends the wait at once (exit 3): that line is not coming."""
    tree, control = s.tree, s.control
    pattern = re.compile(args.regex)
    seen: list[str] = []
    since = None if args.from_now else 0 if args.from_start else printer.mark_seq(
        tree, control)
    try:
        stream = Connection(control, args.timeout + 5)
    except OSError as e:
        die(f"{control}: {e}")
    with stream:
        stream.ask(req_subscribe(["log", "messages"], modules=args.module,
                                 not_modules=args.not_module, since_seq=since))
        # the subscription stands: a caller that starts the wait before the action it waits for acts only after this
        # line, so no line of that action can come before the wait sees the stream
        print("wait-for: subscribed", file=sys.stderr, flush=True)
        for event in printer.events(stream, args.timeout):
            if event.get("cmd") == "message":
                if message_kind(event) == "halt":
                    print(
                        f"CORE HALTED while waiting for /{args.regex}/: {event['text'][:args.width]}")
                    print(
                        f"  (kvasir_bench.py crash {args.build} {args.target})")
                    sys.exit(3)
                continue
            if event.get("cmd") == "dropped":
                # the printer dropped lines for this connection (its queue was full): the line
                # waited for may have been among them - a failure, not a silent wait
                print(f"DROPPED: {event['lines']} lines dropped for this client while waiting for "
                      f"/{args.regex}/; the last {min(len(seen), args.last)} of {len(seen)} lines seen:")
                for text in seen[-args.last:]:
                    print(text[:args.width])
                sys.exit(5)
            if event.get("cmd") == "backlog_end" and event.get("lost"):
                print(
                    f"({event['lost']} lines since the mark are no longer in the printer's history)")
            if event.get("cmd") != "log":
                continue
            text = log_line_text(event)
            seen.append(text)
            if pattern.search(text):
                print(text[:args.width])
                return
    print(f"TIMEOUT after {args.timeout:g} s: no line matched /{args.regex}/; "
          f"the last {min(len(seen), args.last)} of {len(seen)} lines seen:")
    for text in seen[-args.last:]:
        print(text[:args.width])
    sys.exit(2)


@command("trace", checks_build=True,
         args=[arg("name", nargs="?"), arg("--last", type=int, default=0), arg("--hex", action="store_true"),
               arg("--delta", action="store_true", help="the first field's step from row to row"), "stale_ok"])
def trace(s: Session, args) -> None:
    rings = s.records(["trace"])["rings"]
    if not rings:
        die("no Kvasir::Trace::Ring in this firmware (kvasir/Util/Trace.hpp)")
    shown = 0
    for ring in rings:
        if not ring["valid"]:
            print(f"ring at {ring['address']:#x}: {ring.get('error', '?')}")
            continue
        name = ring["name"]
        if args.name and args.name != name:
            continue
        shown += 1
        names, count, capacity = ring["field_names"], ring["count"], ring["capacity"]
        # the printer dropped what record() may have overwritten while the ring was read
        rows = ring["rows"]
        rows = rows[-args.last:] if args.last else rows
        more = ring["written_while_reading"]
        print(f"trace '{name}': {count} records written, capacity {capacity}, {len(rows)} shown"
              + (f" ({more} more were written while reading)" if more else ""))
        widths = [max(len(n), 10) for n in names]
        print("        # " +
              " ".join(f"{n:>{w}}" for n, w in zip(names, widths)))
        previous = None
        for row in rows:
            values = row["values"]
            cells = []
            for v, w in zip(values, widths):
                cells.append(f"{v:>{w}}" if not args.hex else f"{v:>#{w}x}")
            delta = f"   (+{values[0] - previous})" if previous is not None and args.delta else ""
            previous = values[0]
            print(f"{row['index']:9} " + " ".join(cells) + delta)
    if shown == 0:
        die(f"no ring named '{args.name}'")


def log_rows(tree: trees.Tree, control: Path, *, grep: str | None = None, since_mark: bool = False,
             since_ms: float | None = None, last: int = 40, level: str | None = None,
             modules: list[str] | None = None, not_modules: list[str] | None = None) -> tuple[list[str], dict]:
    """The printer's log history as text lines, filtered by the printer (level, module) and here (grep, ms), and the
    history's end record. Raises the printer's ControlError and OSError."""
    filters = {"min_level": level, "modules": modules,
               "not_modules": not_modules}
    if since_mark:
        filters["since_seq"] = printer.mark_seq(tree, control)
    elif grep is not None or since_ms is not None:
        filters["since_seq"] = 0
    else:
        filters["last"] = last
    lines, end = history(control, **filters)
    pattern = re.compile(grep) if grep else None
    rows = []
    for e in lines:
        if since_ms is not None and e["uc_time_ns"] / 1e6 < since_ms:
            continue
        text = log_line_text(e)
        if pattern and not pattern.search(text):
            continue
        rows.append(text)
    return rows, end


@command("log", what="the log",
         args=[arg("--last", type=int, default=40), arg("--grep"), arg("--level", choices=LEVELS),
               arg("--since-ms", type=float),
               arg("--since-mark", action="store_true",
                   help="only what came after this tool's last flash or reset"),
               arg("--width", type=int, default=200), *MODULE_ARGS])
def log(s: Session, args) -> None:
    """The printer's log history, filtered by the printer (level, module) and here (grep, ms)."""
    tree, control = s.tree, s.control
    try:
        rows, end = log_rows(tree, control, grep=args.grep, since_mark=args.since_mark, since_ms=args.since_ms,
                             last=args.last, level=args.level, modules=args.module, not_modules=args.not_module)
    except OSError as e:
        die(f"{control}: {e}")
    for text in rows[-args.last:]:
        print(text[:args.width])
    lost = f", {end['lost']} lines no longer held" if end.get("lost") else ""
    print(f"({len(rows)} matching lines from the printer's history, the last "
          f"{min(len(rows), args.last)} shown{lost})")


# ---- peek, poke ------------------------------------------------------------------------------


def show(name: str, size: int, raw: bytes) -> str:
    number = int.from_bytes(raw, "little") if raw and size <= 8 else None
    value = f"{number} (0x{number:x})" if number is not None else raw.hex(" ")
    return f"{display_name(name)} = {value}"


def wants_types(args, wanted) -> bool:
    return args.typed or any(size not in (1, 2, 4, 8) for _, size, _ in wanted)


UNTIL = {"==": "eq", "!=": "ne", "<": "lt",
         ">": "gt"}   # to a wait's condition


def peek_until(control: Path, wanted, args) -> None:
    addr, size, name = wanted[0]
    if len(wanted) != 1 or size > 8:
        die("--until watches one symbol of at most 8 bytes")
    m = re.fullmatch(
        r"\s*(changed|(==|!=|<|>)\s*(-?(?:0x)?[0-9a-fA-F]+))\s*", args.until)
    if not m:
        die("--until takes 'changed', '== N', '!= N', '< N' or '> N'")
    if m.group(1) == "changed":
        req = req_wait(addr, size, "changed", int(args.timeout * 1000))
    else:
        value = int(m.group(3), 0) & ((1 << (8 * size)) - 1)
        req = req_wait(addr, size, UNTIL[m.group(2)], int(
            args.timeout * 1000), value=value)
    try:
        answer = request(control, req, timeout=args.timeout + 5)
    except ControlError as e:
        die(f"wait through the printer: {e}")
    first, last = answer["first"], answer["last"]
    if answer["hit"]:
        print(f"{display_name(name)}: {first} -> {last} (0x{last:x}) after {answer['waited_us'] / 1000:.3f} ms, "
              f"at unix {answer['unix_us'] / 1e6:.6f} s (the log's recv_time clock)")
        return
    print(f"TIMEOUT after {args.timeout:g} s: {display_name(name)} {args.until} never held; "
          f"it was {first}, is {last} (0x{last:x})")
    sys.exit(2)


def peek_via_printer(tree: trees.Tree, control: Path, wanted, watch: int, interval_ms: int,
                     typed: bool) -> None:
    """One line out, one line back, on the printer's own probe connection: about a millisecond
    a read, all symbols of a round back to back, stamped with the clock the log file uses."""
    limit = 1024 if typed else 64
    req = req_read([(addr, min(size, limit)) for addr, size, _ in wanted])
    with Connection(control, 2) as c:
        first = None
        samples = []
        next_at = time.monotonic()
        for i in range(max(watch, 1)):
            delay = next_at - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            next_at += interval_ms / 1000
            try:
                stamp, data = read_bytes(c.ask(req))
            except ControlError as e:
                die(f"read through the printer: {e}")
            first = first or stamp
            for (addr, size, name), raw in zip(wanted, data):
                samples.append((stamp, addr, size, name, raw[:size]))
    decoded = gdb_stub.decode_typed(tree, [(a, raw)
                                    for _, a, _, _, raw in samples]) if typed else []
    for i, (stamp, addr, size, name, raw) in enumerate(samples):
        text = decoded[i] if typed and decoded[i] is not None else None
        print(f"[{(stamp - first) / 1000:9.3f} ms] "
              + (f"{display_name(name)} = {text}" if text is not None else show(name, size, raw)))
    print(f"(via the printer; first read at unix {first / 1e6:.6f} s - the log's recv_time clock; "
          "the core kept running)")


@command("peek", what="the peek", checks_build=True,
         args=[arg("symbol", nargs="+"), arg("--watch", type=int, default=1, help="read this many times"),
               arg("--interval-ms", type=int, default=200),
               arg("--until", help="'changed', '== N', '!= N', '< N', '> N': the printer watches"),
               arg("--timeout", type=float, default=10,
                   help="seconds, for --until"),
               arg("--typed", action="store_true",
                   help="decode with the ELF's debug info (enums, structs, atomics)"),
               "stale_ok"])
def peek(s: Session, args) -> None:
    wanted = [s.image.one(p) for p in args.symbol]
    control = s.control
    if args.until:
        peek_until(control, wanted, args)
        return
    peek_via_printer(s.tree, control, wanted, args.watch,
                     args.interval_ms, wants_types(args, wanted))


@command("poke", what="the poke", confirm="poke",
         args=[arg("where", help="an address (0x..., _ allowed) or a SYMBOL regex"),
               arg("value", help="the 32-bit word, 0x... or decimal"), arg("--confirm",
                                                                           action="store_true"),
               arg("--stale-ok", action="store_true", help="write to a symbol even if the board runs another build")])
def poke(s: Session, args) -> None:
    try:
        address = int(args.where.replace("_", ""), 0)
        name = f"0x{address:08x}"
    except ValueError:
        # a symbol's address is only right on this tree's build
        s.check_build(args.stale_ok)
        address, size, name = s.image.one(args.where)
        if size < 4:
            die(f"{name} is {size} bytes: poke writes a whole word, which would hit its neighbours")
    if address % 4:
        die(f"0x{address:08x} is not word aligned")
    value = int(args.value.replace("_", ""), 0)
    if not 0 <= value <= 0xFFFF_FFFF:
        die(f"{args.value} does not fit 32 bits")
    (before,) = words(s.ask(req_read_words([address])))[1]
    (after,) = words(s.ask(req_write([(address, value)])))[1]
    print(f"{name}: 0x{before:08x} -> wrote 0x{value:08x}, reads 0x{after:08x}")


# ---- profile ---------------------------------------------------------------------------------


@command("profile", what="the samples", checks_build=True,
         args=[arg("--seconds", type=float, default=10), arg("--top", type=int, default=30),
               arg("--inclusive", action="store_true",
                   help="also rank by inclusive time over the inline stack"),
               arg("--addresses", type=int, default=0, metavar="N",
                   help="also list the N hottest PCs, each with its ELF symbol and debug-info frame"),
               "stale_ok"])
def profile(s: Session, args) -> None:
    """DWT_PCSR sampled through the printer while the core runs, symbolised with inlined frames."""
    tree = s.tree
    counts: collections.Counter = collections.Counter()
    with Connection(s.control, 5) as c:
        _, (demcr,) = words(c.ask(req_read_words([profiling.DEMCR])))
        if not demcr & (1 << 24):
            die("DEMCR.TRCENA is clear: the DWT is off, DWT_PCSR reads nothing")
        request_pcs = req_read_words([profiling.DWT_PCSR] * READ_PIECES)
        end = time.monotonic() + args.seconds
        none = 0
        while time.monotonic() < end:
            for pc in words(c.ask(request_pcs))[1]:
                if pc == profiling.PCSR_NONE:
                    none += 1
                else:
                    counts[pc & ~1] += 1
    total = sum(counts.values())
    if not total:
        die("every sample read 0xFFFFFFFF: the core is halted, or runs Secure code without Secure "
            "debug" if none else "no samples")
    if set(counts) == {0}:
        die("DWT_PCSR reads 0: this core has no DWT_PCSR")
    outside = sum(n for a, n in counts.items() if not s.image.is_code(a))
    frames = profiling.symbolize(tree.elf, counts)
    ranked = profiling.rank_samples(counts, frames, args.top)
    table = s.image.functions
    short_names = profiling.symbol_names(table, counts)
    ranked["symbol"] = profiling.rank_by_symbol(
        counts, table, args.top, short_names)
    print(f"{total} samples over {args.seconds:g} s ({total / args.seconds:,.0f}/s)"
          + (f", {none} without a PC (0xFFFFFFFF)" if none else "")
          + (f", {outside} outside the ELF's code" if outside else ""))
    for title, key in (("self", "self"), ("self, by ELF symbol (folded bodies say so)", "symbol"),
                       ("inclusive, over the inline stack", "inclusive"), ("source lines", "line")):
        if key == "inclusive" and not args.inclusive:
            continue
        print(f"-- {title}")
        for p, name in ranked[key]:
            print(f"{p:6.1f} %  {name}")
    if args.addresses:
        print("-- addresses: symbol+offset | the debug info's innermost frame")
        for address, n in counts.most_common(args.addresses):
            hit = profiling.symbol_at(table, address)
            where = f"{profiling.symbol_name(hit[1], short_names, 1)}+{address - hit[0]:#x}" if hit else "??"
            frame = (frames.get(address) or [
                     {"FunctionName": "??", "FileName": "", "Line": 0}])[0]
            print(f"{100 * n / total:6.1f} %  {address:#010x}  {where} | "
                  f"{frame['FunctionName']} {Path(frame['FileName']).name}:{frame['Line']}")


# ---- chip commands ---------------------------------------------------------------------------
# A chip package may ship <CHIP_ROOT>/tools/kvasir_bench_chip.py with
#     def commands() -> {name: (help, add_arguments(parser), run(bench, args))}
# `run` gets the command's Session; a command that reads symbols calls bench.check_build itself.


def load_chip_plugin(tree: trees.Tree) -> typing.Any:
    """The chip package's kvasir_bench_chip.py module, or None."""
    path = Path(tree.chip_root, "tools",
                "kvasir_bench_chip.py") if tree.chip_root else None
    if path is None or not path.is_file():
        return None
    spec = importlib.util.spec_from_file_location("kvasir_bench_chip", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@command("chip", what="the chip command", args=[arg("rest", nargs=argparse.REMAINDER)])
def chip_cmd(s: Session, args) -> None:
    plugin = load_chip_plugin(s.tree)
    commands = plugin.commands() if plugin else {}
    if not args.rest:
        if not commands:
            print(f"no chip commands ({s.tree.chip_root or 'no CHIP_ROOT'})")
        for name, (text, _, _) in sorted(commands.items()):
            print(f"{name:10} {text}")
        return
    name = args.rest[0]
    if name not in commands:
        die(f"no chip command '{name}'"
            + (f"; this chip has: {', '.join(sorted(commands))}" if commands else ""))
    text, add_arguments, run = commands[name]
    parser = argparse.ArgumentParser(
        prog=f"kvasir_bench.py chip {args.build} {args.target} {name}", description=text)
    add_arguments(parser)
    run(s, parser.parse_args(args.rest[1:]))


# ---- snapshot --------------------------------------------------------------------------------


@command("snapshot", what="the snapshot", checks_build=True,
         args=[arg("--var", action="append", default=[]), arg("--frames", type=int, default=10), "stale_ok"])
def snapshot(s: Session, args) -> None:
    wanted = [s.image.one(p) for p in args.var]
    # everything through the printer: halt and go through its probe calls, registers and
    # memory through its socket - no second J-Link client (it can take the probe off USB)
    # halt, registers and go on the session's connection; the gdb stub reads on one of its own while gdb runs
    control = s.control
    t0 = time.time()
    if not s.ask(req_halt(), timeout=10).get("halted"):
        die("the printer could not halt the core")
    released = False
    try:
        registers = s.ask(req_registers())["values"]
        with gdb_stub.printer_gdb_stub(control, registers[:17]) as port:
            gdb_stub.run_gdb_script(s.tree, port, args, [
                                    tuple(w) for w in wanted])
        s.ask(req_go())
        released = True
    finally:
        if not released:
            # something above failed, maybe the session's connection itself (a timed-out answer still pending on
            # it): the halted core is released all the same, on a connection of its own
            request(control, req_go(), timeout=10)
    print(f"(through the printer: the core stood still for about {time.time() - t0:.1f} s "
          "and runs again)")
