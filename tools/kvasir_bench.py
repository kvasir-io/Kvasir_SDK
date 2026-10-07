#!/usr/bin/env python3
"""kvasir_bench: drive a board on the bench from scripts - and from an AI assistant's tool calls.

Non-interactive, short output, and nothing left running that it did not say it started. Every
command takes a firmware's BUILD directory and a TARGET (`sanitize`, `tty_release_log`) and finds
the rest itself - probe, J-Link device, ELF, log directory, the printer's socket. BUILD may be a
repo with a single build tree; a target that is not built lists the ones that are.

RULES (the tool enforces what it can; the rest is on you)
  1. `status BUILD TARGET` before any hardware step: which probe, whose printer, running or halted,
     and whether the board runs the tree's build.
  2. One printer per board. If one runs - dominic's `just log` window, or one from `printer start`
     - use it: every command goes through its socket. `printer stop` refuses to end a printer that
     has a window (exit 4) unless asked with --confirm.
  3. Symbols come from the tree's ELF. `peek`, `trace`, `stack`, `ub`, `crash`, `snapshot`, `profile`
     refuse (exit 4) when the printer says the board runs another build or the tree was rebuilt
     since the flash: flash first, or --stale-ok if you know the symbol did not move.
  4. `reset` and `flash` refuse (exit 4) where the repo's .kvasir_bench.json says they do something
     physical (water_mix: the valve's motor calibrates at boot) - ask dominic, then --confirm.
  5. Judge a change with the `sanitize` target and end with `ub`.

THE USUAL SESSION
  kvasir_bench.py status   B T                      # rule 1
  kvasir_bench.py printer start B T                 # only if `status` shows none for this board
  kvasir_bench.py flash    B T                      # builds, flashes through the printer (1-4 s)
  kvasir_bench.py wait-for B T 'Reset cause' --timeout 10
  kvasir_bench.py log      B T --since-mark --last 40
  kvasir_bench.py peek     B T 'App::state::counter' --watch 5 --interval-ms 200
  kvasir_bench.py ub       B T                      # 0 reports, or exit 3 with the place

COMMANDS
 look (no effect on the board; the core keeps running)
  status   [B T]          probes, who holds them, per printer: running/halted, firmware check
  log      B T [--last N] [--grep RE] [--level warn] [--since-ms MS] [--since-mark]
           [--module NAME] [--not-module NAME]
                          the running printer's log history (none without a printer), one line
                          per entry: `uc_time level [module] file:line message`. --module i2c
                          also takes i2c.bus (prefix, repeatable)
  messages B T [--last N] [--errors] [--grep RE]
                          the printer's Status tab (firmware check, RTT overflow, core halted,
                          J-Link) - none of that is in the log
  metrics  B T [--seconds S] [--name RE] [--raw] [--until 'NAME OP NUMBER']
                          uc_log::metric values live: min/mean/max/last and rate per metric
  peek     B T SYMBOL... [--watch N --interval-ms MS] [--typed] [--direct]
                          statics read while the core runs (~2 ms a read through the printer,
                          stamped on the log's clock; without a printer JLinkExe, a second a call)
  trace    B T [NAME] [--last N] [--delta]    Kvasir::Trace rings, oldest record first
  stack    B T                                the stack's high-water mark (Kvasir::StackUsage)
  ub       B T                                a sanitize build's report counter
  crash    B T                                the last halt the printer caught, symbolised
  profile  B T [--seconds S] [--top N] [--inclusive] [--addresses N]
                          where the core spends its time: DWT_PCSR sampled through the printer
                          (the core keeps running), ranked by function (the debug info's and the
                          ELF symbol's - they differ where the linker folded bodies, --icf) and
                          source line. PC samples have no callers: a hot shared leaf is found
                          here, who calls it is not
  chip     B T [CMD ARGS...]                  commands the chip package defines
                                              (<CHIP_ROOT>/tools/kvasir_bench_chip.py); no CMD: list
 wait
  wait-for B T REGEX [--timeout S] [--from-now|--from-start] [--module ...]
                          the printer's log since the mark (the last flash/reset of this tool),
                          history then live; the matching line, or the last lines seen
  peek ... --until 'changed' | '== N' | '!= N' | '< N' | '> N' [--timeout S]
                          the printer watches the value (~1 ms a look) and answers the moment
 act (changes the board)
  flash    B T [--no-build] [--confirm]       build, then download through the printer; its log
                                              session survives. No printer: the tree's JLinkExe.
                                              A RAM_ONLY image needs a printer with --ram_image
                                              (it loads and starts it, no reset; printer start
                                              passes it): any other one is refused
  reset    B T [--confirm]                    through the printer; answered when the log is back
                                              (a RAM_ONLY image: loaded again and started)
  panic    B T [--cause NAME] [--confirm]     makes the running firmware call
                                              Kvasir::Panic::raise(cause) (default user): the core
                                              is halted for a moment, R0, PC and xPSR (IT/ICI
                                              cleared) set, and resumed - through the printer (its
                                              debug-register writes; the log goes on) or GDB. The
                                              record's site is the halted code's lr, no call site.
                                              Tests the firmware's panic
                                              handler; it ends in whatever that does (a reboot).
                                              The printer reports the halt ('core halted', a false
                                              crash) and answers the reset's announcement late
  snapshot B T [--var SYMBOL]... [--frames N] HALTS about a second: registers, backtrace, vars.
                                              For a hang or a fault, never under host traffic
  printer start|stop|status [B T]             a headless printer that outlives the shell. The
                                              probe is the tree's (JLINK_IP / JLINK_PROBE in its
                                              cache); the environment overrides it, empty too:
                                              JLINK_IP= JLINK_PROBE=<serial> for a USB probe
                                              in a tree set up for a network J-Link

EXIT CODES (the same for every command)
  0  done / found / condition met
  1  error: bad arguments, no printer where one is needed, the target not connected, a build that
     fails. The message says what to do
  2  timeout: wait-for, peek --until, metrics --until and friends did not see it in time (the
     last lines / values are printed)
  3  the board is in trouble: `ub` found sanitizer reports, `wait-for` saw the core halt
  4  refused by a guard (rules 2-4): nothing was done

SYMBOL is a regular expression over the demangled names and has to match exactly one object
(`TestControl::State::busResets`, `'Device<.*>::configuration$'`); the error lists candidates.

The printer is reached through its control socket (control.sock in the log directory): JSON
lines as uc_log's src/uc_log/detail/ControlProtocol.hpp defines them. uc_log's
tools/uc_log_client.py speaks it (use that module for anything this tool does not do), its
test_uc_log_client.py holds it to the golden files in uc_log/doc/control_protocol/.
"""
import argparse
import contextlib
import json
import os
import re
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import typing
import zlib
from pathlib import Path

sys.dont_write_bytecode = True  # no __pycache__ next to the imported tools

# The printer's side of things - its control protocol and log history - is uc_log's own client
# module; this tool adds what a Kvasir build tree and a Kvasir firmware know.
sys.path.insert(
    0, str(Path(__file__).resolve().parent.parent / "uc_log" / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent /
                "uc_log" / "remote_fmt" / "tools"))
from uc_log_client import (LEVELS, PROTOCOL, Connection, ControlError,  # noqa: E402
                           control_socket_path, history, log_line_text, message_line,
                           metric_key, read_bytes, req_flash, req_messages, req_ping, req_read,
                           req_reset, req_status, req_subscribe, req_wait, req_write, request,
                           status_text)
from extract_sites import (abbreviated, demangle, demanglers,  # noqa: E402
                           find_demangler, operator_end)

PRINTER = "kvasir_uc_log/uc_log_printer_host_build/uc_log_printer"


def die(msg: str, code: int = 1) -> typing.NoReturn:
    print(f"kvasir_bench: {msg}", file=sys.stderr)
    sys.exit(code)


def refuse(msg: str) -> typing.NoReturn:
    """A guard said no (exit 4): the command would do harm or give wrong answers as asked."""
    die(msg, 4)


def note(msg: str) -> None:
    print(f"kvasir_bench: {msg}", file=sys.stderr)


# ---- what a build tree knows ---------------------------------------------------------------


def build_dir(given: str) -> Path:
    """The build directory: as given, or a repo's only build tree (`build/`, `build_<board>/`)."""
    path = Path(given).resolve()
    if (path / "CMakeCache.txt").is_file():
        return path
    trees = sorted(p.parent for p in path.glob("build*/CMakeCache.txt"))
    if len(trees) == 1:
        note(f"using {trees[0]}")
        return trees[0]
    if trees:
        die(f"{path} holds several build trees, name one: "
            + ", ".join(str(t.relative_to(path)) for t in trees))
    die(f"{path} is not a configured build directory (no CMakeCache.txt)")


def targets_in(build: Path) -> list[str]:
    return sorted(e.stem for e in build.glob("*.elf") if not e.stem.endswith("_flash"))


class Tree:
    def __init__(self, build: str, target: str):
        self.build = build_dir(build)
        self.target = target
        cache = self.build / "CMakeCache.txt"
        self.cache = cache.read_text(errors="replace")
        self.elf = self.build / f"{target}.elf"
        if not self.elf.is_file():
            there = targets_in(self.build)
            die(f"no target '{target}' built in {self.build}"
                + (f"; built here: {', '.join(there)}" if there else "; build it first"))
        # The environment wins whenever it sets the variable, empty included: `JLINK_IP=` is how
        # one call reaches a USB probe in a tree whose cache names a network J-Link.
        self.probe = self._setting("JLINK_PROBE")
        self.host = self._setting("JLINK_IP")   # a network J-Link
        self.chip_root = self._cached("CHIP_ROOT")
        # kHz; empty means the probe's maximum, which the DLL clamps 100000 to (cmake/jlink.cmake)
        self.swd_speed = self._cached("SWD_SPEED") or "100000"
        self.device = os.environ.get("JLINK_DEVICE") or self._device()
        self.log_dir = self.build / "rtt_log" / target

    def _setting(self, name: str) -> str:
        return os.environ[name] if name in os.environ else self._cached(name)

    def _cached(self, name: str) -> str:
        m = re.search(rf"^{name}:[A-Za-z]*=(.*)$", self.cache, re.M)
        return m.group(1).strip() if m else ""

    def _device(self) -> str:
        # build.ninja first: chip.cmake's TARGET_MPU is only the family default
        ninja = self.build / "build.ninja"
        if ninja.is_file():
            m = re.search(r"\s--?[Dd]evice[= ]+([A-Za-z0-9_]+)",
                          ninja.read_text(errors="replace"))
            if m:
                return m.group(1)
        # The flash script the SDK generated names the device the way J-Link wants it.
        for script in sorted(self.build.glob("*_flash.jlink")):
            m = re.search(r"^\s*device\s+(\S+)",
                          script.read_text(errors="replace"), re.M | re.I)
            if m:
                return m.group(1)
        chip_root = self._cached("CHIP_ROOT")
        for cmake in Path(chip_root, "cmake").rglob("*.cmake") if chip_root else []:
            m = re.search(
                r"set\(TARGET_MPU\s+([A-Za-z0-9_]+)\)", cmake.read_text(errors="replace"))
            if m:
                return m.group(1)
        die("cannot tell the J-Link device name; set JLINK_DEVICE")

    def _log_command_words(self) -> list[str]:
        """The tree's own `log_<target>` command (build.ninja), split into words."""
        ninja = self.build / "build.ninja"
        if not ninja.is_file():
            return []
        m = re.search(rf"^# Custom command for CMakeFiles/log_{re.escape(self.target)}\n"
                      r"(?:.*\n)*?  COMMAND = (.*)$", ninja.read_text(errors="replace"), re.M)
        return shlex.split(m.group(1)) if m else []

    def _log_command_values(self, flag: str) -> list[str]:
        """The values of `flag` in the tree's own `log_<target>` command (build.ninja)."""
        words = self._log_command_words()
        return [words[i + 1] for i, w in enumerate(words[:-1]) if w == flag]

    def ram_image(self) -> bool:
        """A RAM_ONLY image: a reset boots flash, never it, so the printer has to load and start
        it by hand (uc_log_printer --ram_image). The tree's log command says so (uc_log's
        RAM_IMAGE), and so does its J-Link script, which starts it with SetPC (jlink.cmake) - the
        script also in a tree whose cmake ran before the log command had the flag."""
        if "--ram_image" in self._log_command_words():
            return True
        script = self.build / f"{self.target}_flash.jlink"
        return script.is_file() and re.search(r"^SetPC ", script.read_text(errors="replace"),
                                              re.M) is not None

    def pre_reset_commands(self) -> list[str]:
        """The --pre_reset_command lines of the tree's own `log_<target>` command: what the chip
        package wants written before every reset and download (TARGET_JLINK_CONNECT_COMMANDS; the
        RP chips park core 1 with them). The printer knows no chip, so a printer started without
        them resets an RP2350 with the old image's core 1 still running."""
        return self._log_command_values("--pre_reset_command")

    def log_filter(self) -> str | None:
        """The compile-time log filter file the target was built with (uc_log LogFilter.hpp), if
        any: the printer shows which modules it compiled out."""
        values = self._log_command_values("--log_filter")
        return values[0] if values else None

    def probe_serial(self) -> str:
        """The serial number of the probe whose nickname (or serial) is self.probe."""
        if not self.probe or self.probe.isdigit():
            return self.probe
        out = subprocess.run(["JLinkExe", "-NoGui", "1"], input="ShowEmuList USB\nexit\n",
                             capture_output=True, text=True, timeout=30).stdout
        for line in out.splitlines():
            m = re.search(r"Serial number: (\d+).*Nickname: (.*?)\s*$", line)
            if m and m.group(2) == self.probe:
                return m.group(1)
        die(f"no J-Link with the nickname '{self.probe}' is connected")


def printers() -> list[tuple[int, str, float]]:
    """(pid, command line, age in seconds) of every uc_log_printer running."""
    found = []
    for proc in Path("/proc").iterdir():
        if not proc.name.isdigit():
            continue
        try:
            cmd = (proc / "cmdline").read_bytes().replace(b"\0",
                                                          b" ").decode(errors="replace")
            if "uc_log_printer" in cmd.split(" ")[0]:
                found.append(
                    (int(proc.name), cmd, time.time() - proc.stat().st_mtime))
        except OSError:
            pass
    return found


def probe_of(cmd: str) -> str:
    """Which J-Link a printer sits on: a probe's nickname, a network J-Link, or whichever one
    USB probe there is."""
    m = re.search(r"--host (\S+)", cmd)
    if m:
        return f"ip:{m.group(1)}"
    m = re.search(r"--probe (.+?)(?= --|$)", cmd)
    return m.group(1).strip() if m else "(any usb)"


# ---- printer ---------------------------------------------------------------------------------


def printer_status(_args) -> None:
    rows = printers()
    if not rows:
        print("no uc_log_printer is running")
    for pid, cmd, age in rows:
        m = re.search(r"--log_dir (\S+)", cmd)
        print(
            f"pid {pid}  probe {probe_of(cmd):<12} up {age:6.0f} s  {m.group(1) if m else ''}")


def printer_start(args) -> None:
    tree = Tree(args.build, args.target)
    exe = tree.build / PRINTER
    if not exe.is_file():
        subprocess.run(["cmake", "--build", str(tree.build), "--target", "uc_log_printer"],
                       capture_output=True)
    if not exe.is_file():
        die(f"{exe} does not build: cmake --build {tree.build} --target uc_log_printer")
    if control_socket(tree) is not None:
        die(f"a printer already serves {tree.target} ({control_socket_path(tree.log_dir)}): use it "
            "- flash, log, peek ... go through it, whoever started it")
    for pid, cmd, _age in printers():
        other = probe_of(cmd)
        if other.startswith("ip:") or tree.host:
            if tree.host and other == f"ip:{tree.host}":
                die(
                    f"the J-Link at {tree.host} is held by printer {pid}: {cmd[:120]}")
            continue   # a network J-Link is another probe
        if other in (tree.probe, "(any usb)") or not tree.probe:
            die(f"probe '{tree.probe or 'any'}' is held by printer {pid}: {cmd[:120]}\n"
                "  stop it first (kvasir_bench.py printer status / stop): two printers on one "
                "target fight over its RTT buffer")
    tree.log_dir.mkdir(parents=True, exist_ok=True)
    cmd = [str(exe), "--device", tree.device, "--speed", "100000",
           "--map_file", f"{tree.target}.map", "--hex_file", f"{tree.target}_flash.hex",
           "--string_constants_file", f"{tree.target}_string_constants.json",
           "--build_command", "true", "--log_dir", str(tree.log_dir), "--disable_ui"]
    for line in tree.pre_reset_commands():
        cmd += ["--pre_reset_command", line]
    if tree.ram_image():
        cmd += ["--ram_image"]
    if tree.log_filter():
        cmd += ["--log_filter", tree.log_filter()]
    if tree.host:
        cmd += ["--host", tree.host]
    elif tree.probe:
        cmd += ["--probe", tree.probe]
    out = open(tree.log_dir / "printer.out", "w")
    # own session: the printer outlives the shell that started it
    proc = subprocess.Popen(cmd, cwd=tree.build, stdout=out, stderr=subprocess.STDOUT,
                            stdin=subprocess.DEVNULL, start_new_session=True)
    time.sleep(1.5)
    if proc.poll() is not None:
        die(f"the printer exited at once; {tree.log_dir / 'printer.out'}:\n"
            + (tree.log_dir / "printer.out").read_text()[-600:])
    control = control_socket(tree)
    print(f"printer {proc.pid} on {'J-Link ' + tree.host if tree.host else 'probe ' + repr(tree.probe or 'any')}, device {tree.device}, "
          f"log {tree.log_dir}" + (f", control socket {control}" if control else
                                   " (no control socket: rebuild uc_log_printer in this tree)"))


def printer_stop(args) -> None:
    tree = Tree(args.build, args.target)
    hit = [(pid, cmd)
           for pid, cmd, _ in printers() if str(tree.log_dir) in cmd]
    if not hit:
        print("no printer of this target is running")
    # a printer with a window is somebody's `just log`: not ours to end unasked
    windows = [pid for pid, cmd in hit if "--disable_ui" not in cmd]
    if windows and not args.confirm:
        refuse(f"printer {', '.join(map(str, windows))} has a window - dominic's `just log`, most "
               "likely. Use it instead (every command goes through it); stop it only when asked, "
               "with --confirm")
    for pid, _ in hit:
        os.kill(pid, signal.SIGINT)
    # gone before we return: a dying printer still answers on its socket
    end = time.monotonic() + 10
    for pid, _ in hit:
        while time.monotonic() < end and Path(f"/proc/{pid}").exists():
            time.sleep(0.05)
        alive = Path(f"/proc/{pid}").exists()
        print(f"stopped printer {pid}" +
              (" (still exiting after 10 s)" if alive else ""))


# ---- guards ----------------------------------------------------------------------------------


def guard_file(tree: Tree) -> Path | None:
    """The repo's .kvasir_bench.json: the build dir, its parent or grandparent."""
    for d in (tree.build, tree.build.parent, tree.build.parent.parent):
        if (d / ".kvasir_bench.json").is_file():
            return d / ".kvasir_bench.json"
    return None


def confirm_needed(tree: Tree, action: str, confirmed: bool) -> None:
    """A repo can say that an action does something physical ({"confirm": {"reset": "why"}}):
    then it runs only with --confirm, and the reason is shown either way."""
    path = guard_file(tree)
    if path is None:
        return
    try:
        why = json.loads(path.read_text()).get("confirm", {}).get(action)
    except (OSError, ValueError) as e:
        die(f"{path}: {e}")
    if why is None:
        return
    if not confirmed:
        refuse(f"{action} on {path.parent.name} needs --confirm ({path}): {why}")
    note(f"{action} confirmed: {why}")


def check_build(tree: Tree, stale_ok: bool) -> None:
    """Symbols come from the tree's ELF: on a board that runs another build their addresses are
    wrong, and every value read there is plausible nonsense. The printer knows what the board
    runs (its firmware check at session start); refuse unless it is this tree's hex."""
    control = control_socket(tree)
    if control is None:
        note("the board's firmware is not checked: no printer runs for this target")
        return
    firmware = request(control, req_status())["firmware"]
    tree_build = hex_build_id(tree)
    board = f"{firmware['build']:08x}" if firmware.get(
        "build") is not None else "?"
    problem = None
    if firmware["state"] == "different":
        problem = f"the board does not run this tree's {tree.target} (its flash differs from the hex file)"
    elif firmware["state"] == "match" and board != tree_build:
        problem = (f"the tree's {tree.target} was rebuilt since the board was flashed (board "
                   f"{board}, tree {tree_build}): symbols may have moved")
    elif firmware["state"] == "unchecked":
        note("the printer could not check the board's firmware; the symbols are taken on trust")
    if problem and not stale_ok:
        refuse(
            f"{problem} - flash it (kvasir_bench.py flash), or pass --stale-ok to read anyway")
    if problem:
        note(f"{problem}; reading anyway (--stale-ok)")


# ---- the printer's control socket (uc_log_client speaks it) ------------------------------------


def control_socket(tree: Tree) -> Path | None:
    """The control socket of this target's printer, if one runs and serves it. A printer that
    answers in another protocol stops the tool: it has to be rebuilt, not worked around."""
    control = control_socket_path(tree.log_dir)
    if not control.is_socket():
        return None
    try:
        with Connection(control, 2) as c:
            c.send(req_ping())
            answer = c.next()
    except ControlError as e:
        die(f"{control}: {e}")
    except OSError:
        return None   # a killed printer's file
    if answer.get("protocol") != PROTOCOL:
        die(f"the printer on {control} speaks control protocol {answer.get('protocol')}, this tool "
            f"{PROTOCOL}: rebuild uc_log_printer in {tree.build} and restart it")
    return control


def status_messages(tree: Tree, count: int) -> list[str]:
    """The printer's Status tab, newest `count` lines, oldest first: from the running printer's
    control socket, else from the newest <stamp>.status.log next to the .rttlog."""
    control = control_socket(tree)
    if control:
        try:
            return [message_line(m) for m in request(control, req_messages(count))["messages"]]
        except (OSError, ControlError):
            pass
    files = sorted(tree.log_dir.glob("*.status.log"),
                   key=lambda f: f.stat().st_mtime)
    if not files:
        return []
    return files[-1].read_text(errors="replace").splitlines()[-count:]


def messages(args) -> None:
    lines = status_messages(Tree(args.build, args.target), args.last)
    if args.errors:
        lines = [l for l in lines if not re.search(
            r"^\S+ \[(status|tool)\] ", l)]
    if args.grep:
        lines = [l for l in lines if re.search(args.grep, l)]
    print("\n".join(lines) if lines else "no status messages")


def set_mark(tree: Tree, control: Path | None) -> None:
    """Remember where the printer's log is now: wait-for and `log --since-mark` start here."""
    mark = tree.log_dir / "mark"
    try:
        status = request(control, req_status()) if control else None
    except (OSError, ControlError):
        status = None
    if status is None or "log_seq" not in status:
        # a printer started later holds only lines after this
        mark.unlink(missing_ok=True)
        return
    mark.write_text(json.dumps(
        {"started_us": status["started_us"], "seq": status["log_seq"]}))


def mark_seq(tree: Tree, control: Path) -> int:
    """The seq of the mark in this printer's log; 0 when the printer started after it."""
    try:
        mark = json.loads((tree.log_dir / "mark").read_text())
        status = request(control, req_status())
        return mark["seq"] if mark["started_us"] == status.get("started_us") else 0
    except (OSError, ValueError, KeyError, ControlError):
        return 0


def need_printer(tree: Tree, what: str) -> Path:
    """The printer's control socket; the log exists only there."""
    control = control_socket(tree)
    if control is None:
        refuse(f"no printer with a control socket runs for this target: {what} comes from the "
               "printer's log history - start one (printer start, or just log)")
    return control


def old_printer(tree: Tree, e: ControlError) -> typing.NoReturn:
    die(f"the printer refused a history request ({e}) - a uc_log_printer from before the log "
        f"history? rebuild it (cmake --build {tree.build} --target uc_log_printer) and restart it")


def hex_build_id(tree: Tree) -> str:
    """The CRC-32 the printer names a build by: over the hex file's data bytes, in file order."""
    path = tree.build / f"{tree.target}_flash.hex"
    if not path.is_file():
        return "?"
    crc = 0
    for line in path.read_text().splitlines():
        if line.startswith(":") and line[7:9] == "00":
            crc = zlib.crc32(bytes.fromhex(line[9:-2]), crc)
    return f"{crc:08x}"


def firmware_line(tree: Tree, control: Path) -> str:
    answer = request(control, req_status())
    tree_id = hex_build_id(tree)
    build = answer["firmware"].get("build")
    note = ""
    if build is not None and f"{build:08x}" != tree_id:
        note = f"  (the tree's hex is now {tree_id}: built since, not flashed)"
    return f"target {status_text(answer)}{note}"


def need_ram_image_printer(tree: Tree, control: Path | None, what: str) -> None:
    """A RAM_ONLY image goes through a printer only if that printer starts it by hand
    (--ram_image: download, then VTOR/MSP/xPSR/PC from its vector table, no reset). Any other
    printer resets the target after the download, the boot ROM then boots flash, and the image is
    lost; its `status` has no `ram_image` (from before the flag) or says false."""
    if control is None or not tree.ram_image():
        return
    try:
        status = request(control, req_status())
    except ControlError as e:
        die(f"status of the printer: {e}")
    if status.get("ram_image"):
        return
    why = (f"a uc_log_printer from before --ram_image: rebuild it (cmake --build {tree.build} "
           "--target uc_log_printer) and restart it" if "ram_image" not in status else
           f"started without --ram_image: restart it (printer stop / printer start {tree.build} "
           f"{tree.target}, or just log after cmake has run again in {tree.build})")
    refuse(f"{tree.target} is a RAM_ONLY image and the running printer would {what} it with a "
           f"reset (the boot ROM then runs flash, not the image) - it is {why}")


def flash(args) -> None:
    tree = Tree(args.build, args.target)
    confirm_needed(tree, "flash", args.confirm)
    if not args.no_build:
        built = subprocess.run(["cmake", "--build", str(tree.build), "--target", tree.target],
                               capture_output=True, text=True)
        if built.returncode != 0:
            bad = [l for l in (built.stdout + built.stderr).splitlines()
                   if re.search(r"error|FAILED|warning", l)]
            die("the build failed:\n" +
                "\n".join((bad or built.stdout.splitlines())[-15:]))
    control = control_socket(tree)
    need_ram_image_printer(tree, control, "flash")
    set_mark(tree, control)
    if control is None:
        done = subprocess.run(["cmake", "--build", str(tree.build), "--target", f"flash_{tree.target}"],
                              capture_output=True, text=True)
        if done.returncode != 0:
            die("flashing failed:\n" +
                "\n".join((done.stdout + done.stderr).splitlines()[-8:]))
        print(
            f"flashed {tree.target} with JLinkExe (no printer runs: no log; build {hex_build_id(tree)})")
        return
    t0 = time.time()
    try:
        request(control, req_flash(), timeout=150)
    except ControlError as e:
        die(f"flash through the printer: {e}")
    print(
        f"flashed {tree.target} through the printer in {time.time() - t0:.1f} s; the log goes on")
    print(firmware_line(tree, control))


def reset(args) -> None:
    tree = Tree(args.build, args.target)
    confirm_needed(tree, "reset", args.confirm)
    control = control_socket(tree)
    if control is None:
        die("no printer with a control socket runs for this target (printer start)")
    need_ram_image_printer(tree, control, "reset")
    set_mark(tree, control)
    try:
        request(control, req_reset(), timeout=40)
    except ControlError as e:
        die(f"reset through the printer: {e}")
    print("reset; the log goes on")
    print(firmware_line(tree, control))


# ---- panic -----------------------------------------------------------------------------------

PANIC_RAISE = "Kvasir::Panic::raise(Kvasir::Panic::Cause)"

# Armv7-M / Armv8-M debug registers (ARM DDI0553B.y D1.2.33 DCRDR, D1.2.34 DCRSR, D1.2.39 DHCSR;
# Armv6-M DDI0419E C1.6 the same): memory mapped, so a printer's memory writes reach them. A DCRSR
# write while the core is not halted is ignored (D1.2.34): wait for S_HALT first
DHCSR, DCRSR, DCRDR = 0xE000EDF0, 0xE000EDF4, 0xE000EDF8
DBGKEY, C_DEBUGEN, C_HALT, S_REGRDY, S_HALT = 0xA05F0000, 1 << 0, 1 << 1, 1 << 16, 1 << 17
REGWNR = 1 << 16
# DCRSR.REGSEL: R0, DebugReturnAddress (the PC the core resumes at), xPSR (D1.2.34)
REG_R0, REG_LR, REG_PC, REG_XPSR = 0, 14, 15, 16
# EPSR.ICI/IT, xPSR[26:25] and [15:10] (B3.5): a halt can land inside an IT block or an
# interrupted LDM/STM, and "the Debugger must ensure that the EPSR.IT and EPSR.ICI bits are
# consistent with DebugReturnAddress, otherwise instruction execution will be UNPREDICTABLE"
# (rule RQLRN, B13.5) - so the new PC gets them cleared. IPSR, T and the flags stay. Armv6-M has
# neither: the bits read as 0 there
EPSR_IT_ICI = (0b11 << 25) | (0x3F << 10)


def function_address(tree: Tree, name: str) -> int:
    """A function's address in the tree's ELF, by its exact demangled name."""
    for line in nm(tree).splitlines():
        m = re.match(r"([0-9a-f]{8}) [0-9a-f]{8} [tTwW] (.+)$", line)
        if m and m.group(2) == name:
            return int(m.group(1), 16)
    die(f"{name} is not in {tree.elf.name}: a firmware without Kvasir::Panic (an older SDK)?")


def raise_causes() -> dict[str, int]:
    """Kvasir::Panic::Cause as the command line names it: PANIC_CAUSES (the record's names, in
    the enum's order) with dashes, and KVASIR_PANIC's enumerator, user."""
    return {("user" if n == "KVASIR_PANIC" else n.replace(" ", "-")): i
            for i, n in enumerate(PANIC_CAUSES)}


def panic_register_writes(address: int, cause: int, xpsr: int, pc: int) -> list[list[tuple[int, int]]]:
    """With the core halted and its xPSR and PC read: R0 = cause; LR = "called from where the
    core stood" (raise()'s return address is the place the record keeps - otherwise whatever LR
    happened to hold); xPSR without IT/ICI; PC = raise (a Thumb address without its bit 0:
    xPSR.T stays set). One list per register transfer; after each DHCSR.S_REGRDY says it is done."""
    return [[(DCRDR, cause), (DCRSR, REGWNR | REG_R0)],
            [(DCRDR, ((pc + 2) | 1) & 0xFFFFFFFF), (DCRSR, REGWNR | REG_LR)],
            [(DCRDR, xpsr & ~EPSR_IT_ICI), (DCRSR, REGWNR | REG_XPSR)],
            [(DCRDR, address & ~1), (DCRSR, REGWNR | REG_PC)]]


def panic_gdb_commands(address: int, cause: int) -> list[str]:
    """The same in one GDB session (no printer: JLinkExe cannot read xPSR and write it back in
    one session, and it lets the core run between two). `monitor go` before detach: detach
    alone leaves the core halted."""
    return ["monitor halt", f"set $r0 = {cause}", "set $lr = ($pc + 2) | 1",
            f"set $xpsr = $xpsr & ~{EPSR_IT_ICI:#x}",
            f"set $pc = {address & ~1:#x}", "monitor go", "detach"]


def panic(args) -> None:
    tree = Tree(args.build, args.target)
    # a panic handler resets as a reset does
    confirm_needed(tree, "reset", args.confirm)
    check_build(tree, args.stale_ok)
    cause = raise_causes()[args.cause]
    address = function_address(tree, PANIC_RAISE)
    control = control_socket(tree)
    set_mark(tree, control)
    if control is None:
        with gdb_server(tree) as port:
            debugger = arm_gdb()
            if debugger is None:
                die("no gdb with ARM support (arm-none-eabi-gdb, gdb-multiarch, gdb)")
            cmds = [f"target remote :{port}"] + \
                panic_gdb_commands(address, cause)
            gdb = subprocess.run([debugger, "-batch", "-nx", str(tree.elf)] +
                                 [a for c in cmds for a in ("-ex", c)],
                                 capture_output=True, text=True, timeout=60)
        if gdb.returncode != 0 or "Remote communication error" in gdb.stderr:
            die("gdb failed:\n" +
                (gdb.stdout[-800:] + gdb.stderr[-800:]).strip())
        print(
            f"raise({args.cause}) called through GDB (no printer runs: no log)")
        return

    with Connection(control, 5) as c:
        def dhcsr() -> int:
            return int.from_bytes(read_bytes(c.ask(req_read([(DHCSR, 4)])))[1][0], "little")

        def wait(bit: int, what: str) -> None:
            deadline = time.time() + 1
            while not dhcsr() & bit:
                if time.time() > deadline:
                    c.ask(req_write([(DHCSR, DBGKEY | C_DEBUGEN)]))
                    die(f"{what}; resumed")

        c.ask(req_write([(DHCSR, DBGKEY | C_DEBUGEN | C_HALT)]))
        wait(S_HALT, "the core did not halt (DHCSR.S_HALT)")
        c.ask(req_write([(DCRSR, REG_XPSR)]))
        wait(S_REGRDY, "the core did not hand out xPSR (DHCSR.S_REGRDY)")
        xpsr = int.from_bytes(read_bytes(
            c.ask(req_read([(DCRDR, 4)])))[1][0], "little")
        c.ask(req_write([(DCRSR, REG_PC)]))
        wait(S_REGRDY, "the core did not hand out the PC (DHCSR.S_REGRDY)")
        pc = int.from_bytes(read_bytes(
            c.ask(req_read([(DCRDR, 4)])))[1][0], "little")
        for step in panic_register_writes(address, cause, xpsr, pc):
            c.ask(req_write(step))
            wait(S_REGRDY, "the core did not take the register write (DHCSR.S_REGRDY)")
        c.ask(req_write([(DHCSR, DBGKEY | C_DEBUGEN)]))
    print(f"raise({args.cause}) called through the printer; the log goes on "
          "(wait-for 'ended in a panic' to see the next boot name it)")


# ---- log -------------------------------------------------------------------------------------

def add_module_arguments(p) -> None:
    p.add_argument("--module", action="append", metavar="NAME",
                   help="only lines of this log module and those under it (i2c: i2c.bus too); repeatable")
    p.add_argument("--not-module", action="append", metavar="NAME",
                   help="leave out lines of this log module and those under it; repeatable")


def wait_for(args) -> None:
    """The printer's log from the mark (the last flash/reset of this tool) on: its history, then
    the live lines, in one stream from the printer, filtered there. A `core halted` status
    message ends the wait at once (exit 3): that line is not coming."""
    tree = Tree(args.build, args.target)
    control = need_printer(tree, "wait-for")
    pattern = re.compile(args.regex)
    seen: list[str] = []
    since = None if args.from_now else 0 if args.from_start else mark_seq(
        tree, control)
    try:
        stream = Connection(control, args.timeout + 5)
    except OSError as e:
        die(f"{control}: {e}")
    with stream:
        try:
            stream.ask(req_subscribe(["log", "messages"], modules=args.module,
                                     not_modules=args.not_module, since_seq=since))
        except ControlError as e:
            old_printer(tree, e)
        end = time.monotonic() + args.timeout
        while True:
            left = end - time.monotonic()
            if left <= 0:
                break
            stream.sock.settimeout(left)
            try:
                event = stream.next()
            except (TimeoutError, socket.timeout):
                break
            if event.get("cmd") == "message":
                if event["level"] == "error" and event["text"].startswith("core halted"):
                    print(
                        f"CORE HALTED while waiting for /{args.regex}/: {event['text'][:args.width]}")
                    print(
                        f"  (kvasir_bench.py crash {args.build} {args.target})")
                    sys.exit(3)
                continue
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


def log(args) -> None:
    """The printer's log history, filtered by the printer (level, module) and here (grep, ms)."""
    tree = Tree(args.build, args.target)
    control = need_printer(tree, "the log")
    filters = {"min_level": args.level,
               "modules": args.module, "not_modules": args.not_module}
    local = args.grep is not None or args.since_ms is not None
    if args.since_mark:
        filters["since_seq"] = mark_seq(tree, control)
    elif local:
        filters["since_seq"] = 0
    else:
        filters["last"] = args.last
    try:
        lines, end = history(control, **filters)
    except ControlError as e:
        old_printer(tree, e)
    except OSError as e:
        die(f"{control}: {e}")
    pattern = re.compile(args.grep) if args.grep else None
    rows = []
    for e in lines:
        if args.since_ms is not None and e["uc_time_ns"] / 1e6 < args.since_ms:
            continue
        text = log_line_text(e)
        if pattern and not pattern.search(text):
            continue
        rows.append(text)
    for text in rows[-args.last:]:
        print(text[:args.width])
    lost = f", {end['lost']} lines no longer held" if end.get("lost") else ""
    print(f"({len(rows)} matching lines from the printer's history, the last "
          f"{min(len(rows), args.last)} shown{lost})")


# ---- symbols ---------------------------------------------------------------------------------


def nm(tree: Tree, demangle: bool = True) -> str:
    # llvm-nm demangles the kilobyte-long template names GNU nm gives up on. Demangled, the
    # whole table can be huge (i2c_testing hwtest_sanitize: 253 MB, names up to 2 MB).
    tool = shutil.which("llvm-nm") or "arm-none-eabi-nm"
    return subprocess.run([tool, "-C" if demangle else "--no-demangle", "-S", "--defined-only",
                           str(tree.elf)], capture_output=True, text=True).stdout


def symbols(tree: Tree) -> list[tuple[int, int, str]]:
    table = []
    for line in nm(tree).splitlines():
        m = re.match(r"([0-9a-f]{8}) ([0-9a-f]{8}) [bBdDrRvV] (.+)$", line)
        if m:
            table.append(
                (int(m.group(1), 16), int(m.group(2), 16), m.group(3)))
    return table


def lookup(table, pattern: str) -> tuple[int, int, str]:
    rx = re.compile(pattern)
    hits = [s for s in table if rx.search(s[2])]
    exact = [s for s in hits if s[2] == pattern]
    hits = exact or hits
    if len(hits) != 1:
        shown = "\n  ".join(h[2][:140] for h in hits[:8])
        die(f"'{pattern}' matches {len(hits)} objects, it has to match one" +
            (f":\n  {shown}" if hits else ""))
    return hits[0]


def jlink(tree: Tree, script: str) -> str:
    cmd = ["JLinkExe", "-NoGui", "1", "-device", tree.device, "-if", "SWD", "-speed", tree.swd_speed,
           "-autoconnect", "1"]
    serial = tree.probe_serial()
    if tree.host:
        cmd += ["-ip", tree.host]
    elif serial:
        cmd += ["-USB", serial]
    with tempfile.NamedTemporaryFile("w", suffix=".jlink", delete=False) as f:
        f.write(script)
    try:
        out = subprocess.run(cmd + ["-CommandFile", f.name], capture_output=True, text=True,
                             timeout=60).stdout
    finally:
        os.unlink(f.name)
    if re.search(r"Could not connect|Cannot connect|Failed to (connect|halt)", out, re.I):
        die("J-Link could not reach the target:\n" +
            "\n".join(out.splitlines()[-6:]))
    return out


def short(name: str) -> str:
    """`Device<a kilobyte>::configuration` as `Device<>::configuration`."""
    out, depth = [], 0
    for ch in name:
        if ch == "<":
            depth += 1
            if depth == 1:
                out.append("<>")
        elif ch == ">":
            depth -= 1
        elif depth == 0:
            out.append(ch)
    return "".join(out)[:90]


def show(name: str, size: int, raw: bytes) -> str:
    number = int.from_bytes(raw, "little") if raw and size <= 8 else None
    value = f"{number} (0x{number:x})" if number is not None else raw.hex(" ")
    return f"{short(name)} = {value}"


TYPED_SCRIPT = r"""
import gdb, json
gdb.execute("set pagination off"); gdb.execute("set width 0"); gdb.execute("set print pretty off")
def unwrap(v):
    # std::atomic<T> and friends: one data member, no point in showing the wrapper
    t = v.type.strip_typedefs()
    while t.code == gdb.TYPE_CODE_STRUCT:
        members = [f for f in t.fields() if not f.is_base_class and hasattr(f, "bitpos")]
        bases = [f for f in t.fields() if f.is_base_class]
        if len(members) == 1 and not bases and ("atomic" in str(t) or members[0].name in ("_M_i", "__a_value", "__a_")):
            v = v[members[0]]
        elif not members and len(bases) == 1 and "atomic" in str(t):
            v = v[bases[0]]
        else:
            break
        t = v.type.strip_typedefs()
    return v
out = []
for addr, raw in json.load(open(%(jobs)r)):
    try:
        name = gdb.execute("info symbol 0x%%x" %% addr, to_string=True).split(" in section")[0].strip()
        t = gdb.parse_and_eval("'" + name + "'").type
        data = bytes.fromhex(raw)
        if len(data) < t.sizeof:
            raise ValueError("%%d of %%d bytes" %% (len(data), t.sizeof))
        v = unwrap(gdb.Value(data[:t.sizeof], t))
        out.append(v.format_string(max_depth=4, static_members=False, symbols=False)[:600])
    except Exception as e:
        out.append(None)
json.dump(out, open(%(result)r, "w"))
"""


_gdb_found: list[str | None] = []


def arm_gdb() -> str | None:
    """The first gdb that runs Python and knows the ARM M profile. arm-none-eabi-gdb has no
    Python directory of its own and loads the system gdb's (/usr/share/gdb/python): after an
    update of only the system gdb its Python fails on import, and every script here with it."""
    if not _gdb_found:
        found = None
        for name in ("arm-none-eabi-gdb", "gdb-multiarch", "gdb"):
            if not shutil.which(name):
                continue
            try:
                probe = subprocess.run(
                    [name, "-batch", "-nx", "-ex", "set architecture armv6-m",
                     "-ex", "python import gdb; print('kvasir-gdb-ok')"],
                    capture_output=True, text=True, timeout=30)
            except (OSError, subprocess.TimeoutExpired):
                continue
            if "kvasir-gdb-ok" in probe.stdout and "Undefined item" not in probe.stderr:
                found = name
                break
        _gdb_found.append(found)
    return _gdb_found[0]


def decode_typed(tree: Tree, jobs: list[tuple[int, bytes]]) -> list[str | None]:
    """Each (address of a symbol, its bytes) as the ELF's debug info reads it; None where it
    cannot. gdb runs without a target: half a second for the lot."""
    gdb = arm_gdb()
    if not jobs or gdb is None:
        return [None] * len(jobs)
    with tempfile.TemporaryDirectory() as tmp:
        jobs_file, result, script = (os.path.join(tmp, n)
                                     for n in ("jobs", "result", "s.py"))
        json.dump([[a, raw.hex()] for a, raw in jobs], open(jobs_file, "w"))
        open(script, "w").write(TYPED_SCRIPT %
                                {"jobs": jobs_file, "result": result})
        subprocess.run([gdb, "-batch", "-nx", str(tree.elf), "-x", script],
                       capture_output=True, text=True, timeout=120)
        try:
            return json.load(open(result))
        except (OSError, ValueError):
            return [None] * len(jobs)


def wants_types(args, wanted) -> bool:
    return args.typed or any(size not in (1, 2, 4, 8) for _, size, _ in wanted)


def read_target(control: Path, address: int, size: int) -> bytes:
    """Any amount of target memory through the printer, 4 KB a request."""
    out = b""
    with Connection(control, 5) as c:
        while len(out) < size:
            n = min(4096, size - len(out))
            try:
                _, data = read_bytes(
                    c.ask(req_read([(address + len(out), n)])))
            except ControlError as e:
                die(f"read through the printer: {e}")
            out += data[0]
    return out


UNTIL = {"==": "eq", "!=": "ne", "<": "lt",
         ">": "gt"}   # to a wait's condition


def peek_until(tree: Tree, control: Path, wanted, args) -> None:
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
        print(f"{short(name)}: {first} -> {last} (0x{last:x}) after {answer['waited_us'] / 1000:.3f} ms, "
              f"at unix {answer['unix_us'] / 1e6:.6f} s (the log's recv_time clock)")
        return
    print(f"TIMEOUT after {args.timeout:g} s: {short(name)} {args.until} never held; "
          f"it was {first}, is {last} (0x{last:x})")
    sys.exit(2)


def peek_via_printer(tree: Tree, control: Path, wanted, watch: int, interval_ms: int,
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
    decoded = decode_typed(tree, [(a, raw)
                           for _, a, _, _, raw in samples]) if typed else []
    for i, (stamp, addr, size, name, raw) in enumerate(samples):
        text = decoded[i] if typed and decoded[i] is not None else None
        print(f"[{(stamp - first) / 1000:9.3f} ms] "
              + (f"{short(name)} = {text}" if text is not None else show(name, size, raw)))
    print(f"(via the printer; first read at unix {first / 1e6:.6f} s - the log's recv_time clock; "
          "the core kept running)")


def peek(args) -> None:
    tree = Tree(args.build, args.target)
    check_build(tree, args.stale_ok)
    table = symbols(tree)
    wanted = [lookup(table, p) for p in args.symbol]
    control = None if args.direct else control_socket(tree)
    if args.until:
        if control is None:
            die("--until needs this target's printer (printer start): it does the watching")
        peek_until(tree, control, wanted, args)
        return
    if control is not None:
        peek_via_printer(tree, control, wanted, args.watch,
                         args.interval_ms, wants_types(args, wanted))
        return
    reads = "".join(
        f"mem8 0x{addr:08x} {min(size, 64)}\n" for addr, size, _ in wanted)
    script = "connect\n" + \
        (reads + f"sleep {args.interval_ms}\n") * max(args.watch, 1) + "exit\n"
    out = jlink(tree, script)
    dumps = re.findall(
        r"^([0-9A-Fa-f]{8}) = ((?:[0-9A-Fa-f]{2} ?)+)", out, re.M)
    values: dict[int, list[int]] = {}
    rounds: list[dict[int, list[int]]] = []
    for addr, data in dumps:
        a = int(addr, 16)
        base = next(
            (w for w in wanted if w[0] <= a < w[0] + max(w[1], 1)), None)
        if base is None:
            continue
        if base[0] == a and base[0] in values:
            if len(values) == len(wanted):
                rounds.append(values)
                values = {}
        values.setdefault(base[0], []).extend(int(b, 16) for b in data.split())
    if values:
        rounds.append(values)
    for i, r in enumerate(rounds):
        for addr, size, name in wanted:
            raw = r.get(addr, [])[:size]
            number = int.from_bytes(
                bytes(raw), "little") if raw and size <= 8 else None
            shown = f"{number} (0x{number:x})" if number is not None else " ".join(
                f"{b:02x}" for b in raw)
            print(f"[{i}] {name[:70]} = {shown}")
    print("(the core kept running)" if "is not halted" in jlink(tree, "connect\nIsHalted\nexit\n") else
          "(WARNING: the core is halted)")


# ---- crash, trace, stack, status -------------------------------------------------------------------


def addr2line(tree: Tree, addresses: list[int]) -> dict[int, str]:
    tool = shutil.which("llvm-addr2line") or "arm-none-eabi-addr2line"
    out = subprocess.run([tool, "-f", "-C", "-e", str(tree.elf)] + [f"0x{a:x}" for a in addresses],
                         capture_output=True, text=True).stdout.splitlines()
    result = {}
    for i, a in enumerate(addresses):
        if 2 * i + 1 < len(out):
            fn = re.sub(
                r"<[^<>]*(?:<[^<>]*(?:<[^<>]*>[^<>]*)*>[^<>]*)*>", "<>", out[2 * i])[:90]
            where = out[2 * i + 1].split("/kvasir_work/")[-1]
            result[a] = f"{fn}  {where}"
    return result


def code_ranges(tree: Tree) -> list[tuple[int, int]]:
    out = subprocess.run(["arm-none-eabi-objdump", "-h", str(tree.elf)], capture_output=True,
                         text=True).stdout
    ranges = []
    lines = out.splitlines()
    for i, line in enumerate(lines):
        m = re.match(r"\s*\d+\s+(\S+)\s+([0-9a-f]{8})\s+([0-9a-f]{8})", line)
        if m and i + 1 < len(lines) and "CODE" in lines[i + 1]:
            ranges.append((int(m.group(3), 16), int(
                m.group(3), 16) + int(m.group(2), 16)))
    return ranges


# kvasir/Util/Panic.hpp: Record::Magic and the Cause enumerators, in order
PANIC_MAGIC = 0x9A41C0DE
PANIC_CAUSES = ["assertion", "abort", "stack smash", "division by zero", "allocation without a heap",
                "unhandled interrupt", "undefined behaviour", "fault", "KVASIR_PANIC", "check failed",
                "register wait timed out", "boot loop", "health check starved",
                "image CRC mismatch"]


# kvasir/Util/Persistent.hpp: id, version | size << 16, the value, zlib's crc32 over all before it
def persistent_id(tag: str) -> int:
    """Kvasir::persistentId("KFLT"): the four characters as a little-endian word."""
    return int.from_bytes(tag.encode("ascii"), "little")


def decode_persistent(raw: bytes, tag: str, version: int) -> list[int] | None:
    """The value words of a Kvasir::Persistent record, or None (another tag, version, size, a
    bad CRC: never stored, cleared, from another build, corrupt)."""
    import zlib
    if len(raw) < 12:
        return None
    words = [int.from_bytes(raw[i:i + 4], "little")
             for i in range(0, len(raw) - 3, 4)]
    if words[0] != persistent_id(tag) or (words[1] & 0xFFFF) != version:
        return None
    n = (words[1] >> 16) // 4
    if len(words) < 2 + n + 1 or zlib.crc32(raw[:4 * (2 + n)]) != words[2 + n]:
        return None
    return words[2:2 + n]


# kvasir/Util/FaultHandler.hpp Fault::FullRecord, in order (33 words)
FAULT_V2_FIELDS = ["count", "flags", "r0", "r1", "r2", "r3", "r12", "lr", "pc", "xpsr",
                   "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11",
                   "exc_return", "msp", "psp", "control", "sp",
                   "cfsr", "hfsr", "mmfar", "bfar", "shcsr", "icsr", "sfsr", "sfar", "msplim", "psplim"]
FAULT_V2_FLAGS = ["frameValid", "calleeSaved", "faultRegs", "fpFrame", "stackLimits", "secureRegs",
                  "stackCopied"]
# Kvasir::Panic::FullRecord
PANIC_V2_FIELDS = ["count", "cause", "pc", "detail", "core", "sp"]
# CFSR bit names (Armv8-M ARM DDI0553B.y D1.2.11: MMFSR 7:0, BFSR 15:8, UFSR 31:16)
CFSR_BITS = {0: "IACCVIOL", 1: "DACCVIOL", 3: "MUNSTKERR", 4: "MSTKERR", 5: "MLSPERR", 7: "MMARVALID",
             8: "IBUSERR", 9: "PRECISERR", 10: "IMPRECISERR", 11: "UNSTKERR", 12: "STKERR", 13: "LSPERR",
             15: "BFARVALID", 16: "UNDEFINSTR", 17: "INVSTATE", 18: "INVPC", 19: "NOCP", 20: "STKOF",
             24: "UNALIGNED", 25: "DIVBYZERO"}


def cfsr_text(cfsr: int) -> str:
    return " ".join(n for b, n in sorted(CFSR_BITS.items()) if cfsr >> b & 1) or "-"


def full_records(tree: "Tree", control, table) -> None:
    """The CrashRecord::Full records, where the image has them: lastFaultV2, lastStack<N>, lastPanicV2."""
    def find(pred):
        hits = [sym for sym in table if pred(sym[2])]
        return hits[0] if hits else None
    fault = find(lambda n: n == "Kvasir::Fault::lastFaultV2")
    fault_valid = False
    if fault:
        words = decode_persistent(read_target(
            control, fault[0], fault[1]), "KFLT", 2)
        if words is None or len(words) != len(FAULT_V2_FIELDS):
            print("  the full fault record (lastFaultV2) is empty or not valid")
        else:
            fault_valid = True
            f = dict(zip(FAULT_V2_FIELDS, words))
            flags = [n for b, n in enumerate(
                FAULT_V2_FLAGS) if f["flags"] >> b & 1]
            where = addr2line(tree, [f["pc"] & ~1, f["lr"] & ~1])
            print(f"  the full fault record (the first of {f['count']} since a boot line last reported one), "
                  f"core {f['flags'] >> 8 & 0xF}, Armv{f['flags'] >> 12 & 0xF}-M, {' '.join(flags)}:")
            print(f"    cfsr {f['cfsr']:#010x} ({cfsr_text(f['cfsr'])}) hfsr {f['hfsr']:#010x} "
                  f"mmfar {f['mmfar']:#010x} bfar {f['bfar']:#010x} icsr {f['icsr']:#010x}")
            if "frameValid" in flags:
                print(
                    f"    faulting pc {f['pc']:#010x}  {where.get(f['pc'] & ~1, '')}")
                print(
                    f"    its lr      {f['lr']:#010x}  {where.get(f['lr'] & ~1, '')}")
            else:
                print("    no stacked frame (it was not pushed, or lies outside RAM)")
            print(
                "    " + " ".join(f"{k}={f[k]:#x}" for k in FAULT_V2_FIELDS[2:7] + FAULT_V2_FIELDS[9:23]))
            print(
                "    " + " ".join(f"{k}={f[k]:#x}" for k in FAULT_V2_FIELDS[27:]))
    # the snapshot belongs to the fault record; once that is taken (a boot line reported it) the
    # snapshot left in RAM is that old fault's
    stack = find(lambda n: n.startswith(
        "Kvasir::Fault::lastStack<")) if fault_valid else None
    if stack:
        words = decode_persistent(read_target(
            control, stack[0], stack[1]), "KSTK", 1)
        if words:
            start, count = words[0], words[1]
            body = words[2:2 + count // 4]
            ranges = code_ranges(tree)
            code = [(i, w) for i, w in enumerate(body) if any(
                lo <= (w & ~1) < hi for lo, hi in ranges)]
            where = addr2line(tree, [w & ~1 for _, w in code])
            print(
                f"  the stack snapshot: {count} bytes from {start:#010x}; the words that point into code:")
            for i, w in code:
                print(
                    f"    {start + 4 * i:#010x} {w:#010x}  {where.get(w & ~1, '')}")
    panic = find(lambda n: n == "Kvasir::Panic::lastPanicV2")
    if panic:
        words = decode_persistent(read_target(
            control, panic[0], panic[1]), "KPNC", 2)
        if words and len(words) == len(PANIC_V2_FIELDS):
            p = dict(zip(PANIC_V2_FIELDS, words))
            name = PANIC_CAUSES[p["cause"]] if p["cause"] < len(
                PANIC_CAUSES) else f"cause {p['cause']}"
            site = (p["pc"] & ~1) - 2
            where = addr2line(tree, [site]) if p["pc"] else {}
            print(f"  the full panic record (the first of {p['count']}): {name}, detail {p['detail']}, core "
                  f"{p['core']}, sp {p['sp']:#010x}, raised by the call before {p['pc']:#010x}  "
                  f"{where.get(site, '')}")


def crash(args) -> None:
    tree = Tree(args.build, args.target)
    check_build(tree, args.stale_ok)
    control = need_printer(tree, "the halt")
    entry = None
    for m in request(control, req_messages(1000))["messages"]:
        if m["level"] == "error" and m["text"].startswith("core halted:"):
            entry = {"recv_time_utc": m["time"], "message": m["text"]}
    if entry is None:
        print("no 'core halted' message: the printer did not see the core stop")
        return
    text = entry["message"]
    regs = dict(re.findall(r"(\w+)=(0x[0-9a-f]+|\d+)", text))
    stack = [int(w, 16) for w in text.split("stack:")
             [-1].split()] if "stack:" in text else []
    exception = int(regs.get("exception", "0"), 0)
    names = {0: "thread mode (a breakpoint or a debugger halted it)", 2: "NMI", 3: "HardFault",
             4: "MemManage", 5: "BusFault", 6: "UsageFault", 11: "SVCall", 14: "PendSV", 15: "SysTick"}
    print(f"core halted at {entry['recv_time_utc']} in "
          f"{names.get(exception, f'IRQ {exception - 16}')}")
    ranges = code_ranges(tree)
    def is_code(a): return any(lo <= (a & ~1) < hi for lo, hi in ranges)
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
    # Kvasir::Fault::lastFault holds the faulting registers
    record = [sym for sym in symbols(
        tree) if sym[2] == "Kvasir::Fault::lastFault"]
    control = control_socket(tree)
    if record and control is not None:
        raw = read_target(control, record[0][0], 44)
        magic, count, pc, lr, xpsr, exc_return, r0, r1, r2, r3, r12 = (
            int.from_bytes(raw[i:i + 4], "little") for i in range(0, 44, 4))
        if magic == 0xFA17C0DE:
            where = addr2line(tree, [pc & ~1, lr & ~1])
            print(
                f"  the fault record in RAM (the first of {count} fault(s) since a boot line last reported one):")
            print(f"    faulting pc {pc:#010x}  {where.get(pc & ~1, '')}")
            print(f"    its lr      {lr:#010x}  {where.get(lr & ~1, '')}")
            print(f"    r0={r0:#x} r1={r1:#x} r2={r2:#x} r3={r3:#x} r12={r12:#x} xpsr={xpsr:#x} "
                  f"exc_return={exc_return:#x}")
        else:
            print(
                "  the fault record in RAM is empty: this halt was not Kvasir's fault handler")
    # Kvasir::Panic::lastPanic (kvasir/Util/Panic.hpp): an assert, abort, stack smash, ... ends in
    # the panic handler, whose halt pc names the handler, not the site; the record has the site
    panic = [sym for sym in symbols(
        tree) if sym[2] == "Kvasir::Panic::lastPanic"]
    if panic and control is not None:
        # 16 bytes in older images, 20 with `detail` (an unhandled interrupt's exception
        # number, exit()'s status); the symbol's size says which
        size = 20 if panic[0][1] >= 20 else 16
        raw = read_target(control, panic[0][0], size)
        magic, count, cause, pc = (int.from_bytes(
            raw[i:i + 4], "little") for i in range(0, 16, 4))
        detail = int.from_bytes(raw[16:20], "little") if size == 20 else None
        if magic == PANIC_MAGIC:
            name = PANIC_CAUSES[cause] if cause < len(
                PANIC_CAUSES) else f"cause {cause}"
            # pc is raise()'s return address; after a call that does not return it may point
            # past the caller's end, into the next function: symbolise the call itself
            extra = ""
            if detail is not None and name == "unhandled interrupt":
                extra = f", exception {detail} = IRQ {detail - 16}"
            elif detail:
                extra = f", detail {detail}"
            if pc == 0:
                print(f"  the panic record in RAM (the first of {count} panic(s) since a boot line last "
                      f"reported one): {name}{extra}, no program site")
            else:
                site = (pc & ~1) - 2
                where = addr2line(tree, [site])
                print(f"  the panic record in RAM (the first of {count} panic(s) since a boot line last "
                      f"reported one): {name}{extra}, raised by the call before {pc:#010x}  "
                      f"{where.get(site, '')}")
    if control is not None:
        full_records(tree, control, symbols(tree))
    # this run's fault line only (since the last flash/reset): an older boot's COREFAULT in the
    # printer's history is not this halt's
    fault_lines = subprocess.run([sys.executable, __file__, "log", str(tree.build), tree.target,
                                  "--grep", "COREFAULT", "--since-mark", "--last", "1",
                                  "--width", "600"],
                                 capture_output=True, text=True).stdout.splitlines()[:-1]
    for line in fault_lines:
        print("  log: " + line.strip())


def ub(args) -> None:
    """The sanitize variant's report counter (kvasir/Util/ubsan.hpp): works without a printer,
    which is how the bench suites run."""
    tree = Tree(args.build, args.target)
    check_build(tree, args.stale_ok)
    hit = [sym for sym in symbols(tree) if sym[2]
           == "Kvasir::Ubsan::ubsanReports"]
    if not hit:
        die(f"{tree.target} has no sanitizer report counter: it is not a sanitize build")
    address = hit[0][0]
    control = control_socket(tree)
    if control is not None:
        raw = read_target(control, address, 8)
    else:
        out = jlink(tree, f"connect\nmem32 0x{address:08x} 2\nexit\n")
        m = re.search(
            rf"^{address:08X} = ([0-9A-Fa-f]{{8}}) ([0-9A-Fa-f]{{8}})", out, re.M)
        if not m:
            die("could not read the counter:\n" +
                "\n".join(out.splitlines()[-6:]))
        raw = int(m.group(1), 16).to_bytes(4, "little") + \
            int(m.group(2), 16).to_bytes(4, "little")
    count, last = int.from_bytes(
        raw[0:4], "little"), int.from_bytes(raw[4:8], "little")
    if count == 0:
        print("0 sanitizer reports since boot")
        return
    where = addr2line(tree, [last & ~1]).get(last & ~1, "")
    print(f"{count} SANITIZER REPORT(S) since boot; the last one returned to {last:#010x}  {where}\n"
          "  (the log has a crit line 'UB: <kind> at <address>' per report, if a printer ran)")
    sys.exit(3)


TRACE_MAGIC = 0x4352544B


def trace_rings(all_symbols) -> list:
    """(descriptor address or None, storage address, storage size) of every Trace::Ring. Since
    2026-10-06 a ring is two objects, a constant `descriptor` and the RAM `storage` (count, then
    records); before, `storage` alone, starting with the header (descriptor address None)."""
    descriptors = {name[:-len("descriptor")]: a for a, _, name in all_symbols
                   if re.search(r"Trace::Ring<.*>::descriptor$", name)}
    return [(descriptors.get(name[:-len("storage")]), a, size) for a, size, name in all_symbols
            if re.search(r"Trace::Ring<.*>::storage$", name)]


def trace_layout(descriptor: bytes | None, storage: bytes):
    """-> (fields, capacity, count, layout address, offset of the count, offset of the records)
    of a ring in either layout, or None if the bytes are no ring."""
    def u(raw, at, n):
        return int.from_bytes(raw[at:at + n], "little")
    if descriptor is None:      # the old layout: magic, fields, capacity, count, layout, records
        if len(storage) < 16:
            return None
        magic, fields, capacity, count, layout_at = (
            u(storage, 0, 4), u(storage, 4, 2), u(storage, 6, 2), u(storage, 8, 4), u(storage, 12, 4))
        count_at, records_at = 8, 16
    else:                       # descriptor: magic, fields, capacity, layout; storage: count, records
        if len(descriptor) < 12 or len(storage) < 4:
            return None
        magic, fields, capacity, layout_at = (
            u(descriptor, 0, 4), u(descriptor, 4, 2), u(descriptor, 6, 2), u(descriptor, 8, 4))
        count, count_at, records_at = u(storage, 0, 4), 0, 4
    if magic != TRACE_MAGIC or fields == 0 or records_at + 4 * fields * capacity > len(storage):
        return None
    return fields, capacity, count, layout_at, count_at, records_at


def trace(args) -> None:
    tree = Tree(args.build, args.target)
    check_build(tree, args.stale_ok)
    control = control_socket(tree)
    if control is None:
        die("no printer with a control socket runs for this target (printer start)")
    rings = trace_rings(symbols(tree))
    if not rings:
        die("no Kvasir::Trace::Ring in this firmware (kvasir/Util/Trace.hpp)")
    shown = 0
    for descriptor_at, address, size in rings:
        raw = read_target(control, address, size)
        descriptor = None if descriptor_at is None else read_target(
            control, descriptor_at, 12)
        ring = trace_layout(descriptor, raw)
        if ring is None:
            print(
                f"ring at {address:#x}: no valid header (does the board run this build?)")
            continue
        fields, capacity, count, layout_at, count_at, records_at = ring
        layout = read_target(control, layout_at, 256).split(b"\0")[
            0].decode(errors="replace")
        name, _, field_text = layout.partition(":")
        if args.name and args.name != name:
            continue
        shown += 1
        names = field_text.split(",")
        again = int.from_bytes(read_target(
            control, address + count_at, 4), "little")
        kept = min(count, capacity)
        rows = []
        for i in range(count - kept, count):
            at = records_at + 4 * fields * (i % capacity)
            rows.append((i, [int.from_bytes(
                raw[at + 4 * k:at + 4 * k + 4], "little") for k in range(fields)]))
        # what record() wrote while the ring was read is overwritten, not torn: drop it
        rows = [r for r in rows if r[0] >= again - capacity]
        rows = rows[-args.last:] if args.last else rows
        print(f"trace '{name}': {count} records written, capacity {capacity}, {len(rows)} shown"
              + (f" ({again - count} more were written while reading)" if again != count else ""))
        widths = [max(len(n), 10) for n in names]
        print("        # " +
              " ".join(f"{n:>{w}}" for n, w in zip(names, widths)))
        previous = None
        for i, values in rows:
            cells = []
            for k, (v, w) in enumerate(zip(values, widths)):
                cells.append(f"{v:>{w}}" if not args.hex else f"{v:>#{w}x}")
            delta = f"   (+{values[0] - previous})" if previous is not None and args.delta else ""
            previous = values[0]
            print(f"{i:9} " + " ".join(cells) + delta)
    if shown == 0:
        die(f"no ring named '{args.name}'")


STACK_PATTERN = 0x5AC35AC3


def metrics(args) -> None:
    """What the firmware logs through uc_log::metric<"name"_sc, "unit"_sc, "scope"_sc>(value):
    the printer takes every such value out of the log and streams it to the subscribers of its
    control socket's metrics stream, one {"cmd":"metric","name":..,"scope":..,"unit":..,
    "time":<uc seconds>,"value":..} a sample - numbers on the target's clock, no text to parse."""
    tree = Tree(args.build, args.target)
    control = control_socket(tree)
    if control is None:
        die("no printer with a control socket runs for this target (printer start)")
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
    end = time.monotonic() + args.seconds
    with Connection(control, 2) as stream:
        stream.ask(req_subscribe(["metrics"]))
        while time.monotonic() < end:
            stream.sock.settimeout(max(end - time.monotonic(), 0.01))
            try:
                sample = stream.next()
            except (TimeoutError, socket.timeout):
                break
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


def stack(args) -> None:
    tree = Tree(args.build, args.target)
    check_build(tree, args.stale_ok)
    control = control_socket(tree)
    if control is None:
        die("no printer with a control socket runs for this target (printer start)")
    found = {n: int(a, 16) for a, n in
             re.findall(r"^([0-9a-f]{8}) (?:[0-9a-f]{8} )?. (_LINKER_stack_(?:start|end)_)$", nm(tree), re.M)}
    if len(found) != 2:
        die("the ELF has no _LINKER_stack_start_/_end_")
    low, high = found["_LINKER_stack_start_"], found["_LINKER_stack_end_"]
    raw = read_target(control, low, high - low)
    words = [int.from_bytes(raw[i:i + 4], "little")
             for i in range(0, len(raw), 4)]
    at = 0
    while at < len(words) and words[at] != STACK_PATTERN:
        at += 1   # the StackProtector's sentinel
    free = 0
    while at + free < len(words) and words[at + free] == STACK_PATTERN:
        free += 1
    if free == 0:
        die("the stack is not painted: put Kvasir::StackUsage into the Startup list "
            "(kvasir/Util/StackUsage.hpp)")
    size = high - low
    used = size - 4 * (at + free)
    print(f"stack {low:#010x}..{high:#010x}: {size} bytes, deepest use so far {used} bytes "
          f"({100 * used / size:.1f} %), {4 * free} bytes never touched")


def probes_on_usb() -> list[tuple[str, str, str]]:
    out = subprocess.run(["JLinkExe", "-NoGui", "1"], input="ShowEmuList USB\nexit\n",
                         capture_output=True, text=True, timeout=30).stdout
    return re.findall(r"Serial number: (\d+), ProductName: (.*?), Nickname: (.*?)\s*$", out, re.M)


def status(args) -> None:
    """Everything worth knowing before touching the hardware, on one screen."""
    running = printers()
    found = probes_on_usb()
    print(f"{len(found)} J-Link probe(s) on USB, {len(running)} printer(s) running")
    held: dict[str, list] = {}
    for pid, cmd, age in running:
        held.setdefault(probe_of(cmd), []).append((pid, cmd, age))
    for serial, product, nick in found:
        mine = held.pop(nick, []) + held.pop(serial, [])
        print(f"probe '{nick or serial}' ({product}, {serial}): "
              + ("free" if not mine else ""))
        for pid, cmd, age in mine:
            print_printer(pid, cmd, age)
    for where, rows in held.items():
        print(f"probe {where}:")
        for pid, cmd, age in rows:
            print_printer(pid, cmd, age)
    if args.build and args.target:
        tree = Tree(args.build, args.target)
        control = control_socket(tree)
        print(f"{tree.target} in {tree.build.name}: hex build {hex_build_id(tree)}, "
              + (firmware_line(tree, control) if control else "no printer with a control socket"))


def print_printer(pid: int, cmd: str, age: float) -> None:
    m = re.search(r"--log_dir (\S+)", cmd)
    log_dir = Path(m.group(1)) if m else None
    line = f"    printer {pid}, up {age:.0f} s, {log_dir or ''}"
    control = control_socket_path(log_dir) if log_dir else None
    if control and control.is_socket():
        try:
            with Connection(control, 2) as c:
                c.send(req_ping())
                protocol = c.next().get("protocol")
                if protocol != PROTOCOL:
                    raise ControlError(f"protocol {protocol}, this tool {PROTOCOL}: rebuild its "
                                       "uc_log_printer and restart it")
                line += "\n        " + status_text(c.ask(req_status()))
        except ControlError as e:
            line += f"\n        (speaks another control protocol: {e})"
        except OSError:
            line += "\n        (its control socket does not answer)"
    else:
        line += "\n        (no control socket: a uc_log_printer built before it had one)"
    if log_dir:
        logs = sorted(log_dir.glob("*.rttlog"),
                      key=lambda f: f.stat().st_mtime)
        if logs:
            line += f"\n        last log line {time.time() - logs[-1].stat().st_mtime:.0f} s ago"
    print(line)


# ---- snapshot --------------------------------------------------------------------------------

GDB_SCRIPT = r'''
import gdb, re
def short(name):
    depth, out = 0, []
    for ch in name or "?":
        if ch == "<":
            depth += 1
            if depth == 1: out.append("<>")
        elif ch == ">":
            depth -= 1
        elif depth == 0:
            out.append(ch)
    return "".join(out)[:90]
gdb.execute("set pagination off"); gdb.execute("set confirm off"); gdb.execute("set width 0")
gdb.execute("target remote :%(port)d")
for reg in ("pc", "sp", "lr", "xpsr"):
    print("%%-5s %%s" %% (reg, gdb.parse_and_eval("$" + reg).format_string(format="x")))
xpsr = int(gdb.parse_and_eval("$xpsr")) & 0x1ff
print("mode  %%s" %% ("thread" if xpsr == 0 else "exception %%d (%%s)" %% (xpsr, {3: "HardFault", 15: "SysTick"}.get(xpsr, "IRQ %%d" %% (xpsr - 16)))))
f, n = gdb.newest_frame(), 0
while f is not None and n < %(frames)d:
    sal = f.find_sal()
    where = sal.symtab.filename.split("/kvasir_work/")[-1] if sal.symtab else "?"
    print("#%%-2d %%s  %%s:%%s" %% (n, short(f.name()), where, sal.line))
    f, n = f.older(), n + 1
for addr, size, name in %(vars)r:
    raw = gdb.selected_inferior().read_memory(addr, min(size, 64)).tobytes()
    value = int.from_bytes(raw, "little")
    print("%%s = %%s" %% (name[:70], "%%d (0x%%x)" %% (value, value) if size <= 8 else raw.hex(" ")))
gdb.execute("detach")
'''


@contextlib.contextmanager
def gdb_server(tree: Tree):
    """A J-Link GDB server on a free local port for one gdb session; it does not halt the core
    and ends with that session (-singlerun). Yields the port."""
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    server = ["JLinkGDBServerCLExe", "-device", tree.device, "-if", "SWD", "-speed", tree.swd_speed,
              "-port", str(port), "-swoport", str(port +
                                                  1), "-telnetport", str(port + 2),
              "-nohalt", "-noir", "-silent", "-singlerun", "-nogui"]
    serial = tree.probe_serial()
    if serial:
        server += ["-select", f"USB={serial}"]
    log_path = Path(tempfile.gettempdir()) / f"kvasir_gdbserver_{port}.log"
    with open(log_path, "w") as out:
        srv = subprocess.Popen(
            server, stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    try:
        for _ in range(80):
            text = log_path.read_text(errors="replace")
            if "Waiting for GDB connection" in text:
                break
            if srv.poll() is not None or re.search(r"ERROR|Could not connect", text):
                die("the GDB server did not come up:\n" +
                    "\n".join(text.splitlines()[-8:]))
            time.sleep(0.1)
        yield port
    finally:
        try:
            srv.wait(timeout=5)
        except subprocess.TimeoutExpired:
            srv.kill()
        log_path.unlink(missing_ok=True)


def snapshot(args) -> None:
    tree = Tree(args.build, args.target)
    check_build(tree, args.stale_ok)
    table = symbols(tree)
    wanted = [lookup(table, p) for p in args.var]
    with gdb_server(tree) as port:
        with tempfile.NamedTemporaryFile("w", suffix=".py", delete=False) as f:
            f.write(GDB_SCRIPT %
                    {"port": port, "frames": args.frames, "vars": wanted})
        t0 = time.time()
        debugger = arm_gdb()
        if debugger is None:
            die("no gdb with Python and ARM support (arm-none-eabi-gdb, gdb-multiarch, gdb)")
        gdb = subprocess.run([debugger, "-batch", "-nx", str(tree.elf), "-x", f.name],
                             capture_output=True, text=True, timeout=60)
        os.unlink(f.name)
        keep = [l for l in gdb.stdout.splitlines()
                if re.match(r"(pc|sp|lr|xpsr|mode) |#\d|.* = ", l)]
        print("\n".join(keep)
              if keep else gdb.stdout[-1500:] + gdb.stderr[-800:])
        print(
            f"(the core stood still for about {time.time() - t0:.1f} s and runs again)")


# ---- profile ---------------------------------------------------------------------------------

# Armv8-M ARM (DDI0553B.y) D1.2.68 DWT_PCSR, Armv7-M ARM (DDI0403E) C1.6.5 DEMCR / C1.8.14, Armv6-M
# (DDI0419E) the same addresses. Absent, it reads 0 (Cortex-M33 TRM r1p0 C3.2, v6-M/v7-M RAZ).
DEMCR = 0xE000EDFC       # bit 24 TRCENA
DWT_PCSR = 0xE000101C
PCSR_NONE = 0xFFFFFFFF   # halted, no sample, or Secure code without Secure debug
READ_PIECES = 64         # uc_log control::MaxPieces


def strip_templates(name: str) -> str:
    """`a::B<...>::c(int, x<y>)` -> `a::B::c()`; operators, `(anonymous namespace)` and lambda
    names (`{lambda(int)#1}`, `'lambda'`) stay."""
    out, depth, parens, i = [], 0, 0, 0
    while i < len(name):
        c = name[i]
        keep = None
        if name.startswith("[abi:", i) and "]" in name[i:i + 64]:
            i = name.index("]", i) + 1
            continue
        if name.startswith("(anonymous namespace)", i):
            keep = i + len("(anonymous namespace)")
        elif name.startswith(("{lambda", "{unnamed"), i) and "}" in name[i:]:
            keep = name.index("}", i) + 1
        elif c == "'" and "'" in name[i + 1:]:
            keep = name.index("'", i + 1) + 1
        elif (end := operator_end(name, i)) is not None:
            keep = end
        if keep is not None:
            if depth == 0:
                out.append(name[i:keep])
            i = keep
            continue
        # inside a template argument list: `A<(3 > 2)>` has a `>` in parentheses
        if depth:
            parens += c == "("
            parens -= c == ")" and parens > 0
            depth += c == "<" and not parens
            depth -= c == ">" and not parens
        elif c == "<":
            depth = 1
        elif c == "(":   # a parameter list
            level, j = 0, i
            while j < len(name):
                level += name[j] == "("
                level -= name[j] == ")"
                j += 1
                if level == 0:
                    break
            out.append("()")
            i = j
            continue
        else:
            out.append(c)
        i += 1
    return "".join(out)[:120]


def short_function_names(names: set[str]) -> dict[str, str]:
    """Mangled name -> strip_templates() of its demangled form. A demangled template signature can
    run to megabytes (i2c_testing hwtest: 7.8 MB), so each is demangled once, streamed, and
    abbreviated() (remote_fmt, memoised) before it is stripped."""
    def split(n):   # a mangled name behind a prefix: __Thumbv7ABSLongThunk__ZN..., .Lswitch.table._ZN...
        m = re.search(r"(?<=[._])_Z[A-Z]", n)
        return (n[:m.start()], n[m.start():]) if m and not n.startswith("_Z") else ("", n)
    out = {n: strip_templates(n) or "??" for n in names if not split(n)[
        1].startswith("_Z")}
    mangled = sorted({split(n)[1] for n in names} - set(out))
    memo: dict = {}
    unread = []
    if mangled:
        with tempfile.TemporaryFile("w+") as todo:
            todo.write("\n".join(mangled) + "\n")
            todo.seek(0)
            with subprocess.Popen([find_demangler()], stdin=todo, stdout=subprocess.PIPE,
                                  text=True) as demangler:
                assert demangler.stdout is not None
                for name, line in zip(mangled, demangler.stdout):
                    line = line.rstrip("\n")
                    if line.startswith("_Z"):
                        unread.append(name)
                    else:
                        out[name] = strip_templates(
                            abbreviated(line, memo=memo))
    # what the first demangler cannot read, the other one may (remote_fmt extract_sites.py)
    for name, line in zip(unread, demangle(demanglers(find_demangler())[1:], unread)):
        out[name] = strip_templates(abbreviated(line, memo=memo)
                                    if not line.startswith("_Z") else line)
    for n in names - set(out):
        prefix, rest = split(n)
        out[n] = prefix + out[rest]
    return out


def symbolize(elf: Path, addresses: typing.Iterable[int]) -> dict[int, list[dict]]:
    """Address -> inline stack (innermost first) with short function names. The output is read
    line by line and each name kept once: mangled, it is still 0.6 GB for 40 000 addresses of
    i2c_testing hwtest."""
    tool = shutil.which("llvm-symbolizer")
    if not tool:
        die("llvm-symbolizer is needed for the inlined frames")
    frames: dict[int, list[dict]] = {}
    names: dict[str, str] = {}
    with tempfile.TemporaryFile("w+") as todo:
        todo.write("\n".join(f"0x{a:x}" for a in addresses) + "\n")
        todo.seek(0)
        with subprocess.Popen([tool, "--obj", str(elf), "--inlining", "--no-demangle",
                               "--output-style=JSON"], stdin=todo, stdout=subprocess.PIPE,
                              text=True) as symbolizer:
            assert symbolizer.stdout is not None
            for line in symbolizer.stdout:
                entry = json.loads(line)
                frames[int(entry["Address"], 16)] = [
                    {"FunctionName": names.setdefault(f["FunctionName"], f["FunctionName"]),
                     "FileName": f["FileName"], "Line": f["Line"]}
                    for f in entry.get("Symbol", [])]
    short = short_function_names(set(names))
    for stack in frames.values():
        for f in stack:
            f["FunctionName"] = short[f["FunctionName"]]
    return frames


def unique_suffixes(paths: list[str]) -> dict[str, str]:
    """Each path shortened to its fewest trailing components that no other path ends in."""
    parts = {p: Path(p).parts or ("",) for p in paths}
    out = {}
    for p, mine in parts.items():
        n = 1
        while n < len(mine) and any(q != p and parts[q][-n:] == mine[-n:] for q in parts):
            n += 1
        out[p] = "/".join(mine[-n:])
    return out


def rank_samples(counts: dict[int, int], frames: dict[int, list[dict]], top: int) -> dict[str, list[tuple[float, str]]]:
    """Samples per address + inline stacks (innermost first, llvm-symbolizer JSON) -> the top
    (percent, name) by self time, inclusive time and source line."""
    import collections
    total = sum(counts.values()) or 1
    short: dict[str, str] = {}

    def strip(n):
        if n not in short:
            short[n] = strip_templates(n)
        return short[n]

    own, inclusive, lines = collections.Counter(
    ), collections.Counter(), collections.Counter()
    for address, n in counts.items():
        stack = frames.get(address) or [
            {"FunctionName": "??", "FileName": "", "Line": 0}]
        own[strip(stack[0]["FunctionName"])] += n
        lines[(stack[0]["FileName"], stack[0]["Line"])] += n
        for name in {strip(f["FunctionName"]) for f in stack}:
            inclusive[name] += n

    def pct(c): return [(100 * n / total, k) for k, n in c.most_common(top)]
    top_lines = pct(lines)
    short = unique_suffixes([f for _, (f, _) in top_lines])
    return {"self": pct(own), "inclusive": pct(inclusive),
            "line": [(p, f"{short[f]}:{line}") for p, (f, line) in top_lines]}


def code_symbols(tree: Tree) -> list[tuple[int, int, list[str]]]:
    """The ELF's functions as (start, size, mangled names), sorted by start. Several names at one
    start are one body the linker folded (--icf) - often unrelated functions that compiled alike.
    Mangled: demangling the whole table costs hundreds of MB; symbol_names() does the hit ones."""
    sized: dict[int, tuple[int, list[str]]] = {}
    # size 0: linker labels (`_LINKER_INTERN_*`), asm without .size
    labels: dict[int, list[str]] = {}
    for line in nm(tree, demangle=False).splitlines():
        m = re.match(r"([0-9a-f]{8}) ([0-9a-f]{8}) [tTwW] (.+)$", line)
        if not m:
            continue
        start, size, name = int(m.group(1), 16), int(
            m.group(2), 16), m.group(3)
        if size == 0:
            labels.setdefault(start, []).append(name)
        else:
            old_size, names = sized.get(start, (0, []))
            sized[start] = (max(old_size, size), names + [name])
    # A label only names a body where no function of its own starts: next to one it is not a
    # second function folded into it (main's body shares its start with two linker labels).
    for start, names in labels.items():
        sized.setdefault(start, (0, names))
    return sorted((start, size, names) for start, (size, names) in sized.items())


def symbol_at(table: list[tuple[int, int, list[str]]], address: int) -> tuple[int, list[str]] | None:
    """(start, names) of the function whose body holds @p address, if any."""
    import bisect
    i = bisect.bisect_right(table, (address, float("inf"), [])) - 1
    if i >= 0 and table[i][0] <= address < table[i][0] + max(table[i][1], 2):
        return table[i][0], table[i][2]
    return None


def symbol_names(table: list[tuple[int, int, list[str]]], addresses: typing.Iterable[int]) -> dict[str, str]:
    """Mangled -> short name, for the names of every body one of @p addresses lies in only."""
    wanted = {n for a in addresses if (
        hit := symbol_at(table, a)) for n in hit[1]}
    return short_function_names(wanted) if wanted else {}


def symbol_name(names: list[str], short: dict[str, str], limit: int = 2) -> str:
    """A body's names, shortened (@p short, from symbol_names()), the first @p limit of them; a
    folded one says how many functions share it (counted by full name - shortened, `f<int>` and
    `f<bool>` are one)."""
    shown_names = sorted({short.get(n, n) for n in names}, key=len)
    shown = ", ".join(shown_names[:limit]) + \
        (", ..." if len(shown_names) > limit else "")
    return shown + (f"  [{len(set(names))} folded]" if len(set(names)) > 1 else "")


def rank_by_symbol(counts: dict[int, int], table: list[tuple[int, int, list[str]]], top: int,
                   short: dict[str, str] | None = None) -> list[tuple[float, str]]:
    """Self time by the ELF symbol the PC lies in - the linker's view, which the inline stack of
    the debug info does not give: under --icf a folded body's line table names whichever of its
    functions the linker kept, `FPBits::is_neg` for an `optional::has_value`."""
    import collections
    if short is None:
        short = symbol_names(table, counts)
    total = sum(counts.values()) or 1
    own: collections.Counter = collections.Counter()
    for address, n in counts.items():
        hit = symbol_at(table, address)
        own[symbol_name(hit[1], short) if hit else "??"] += n
    return [(100 * n / total, k) for k, n in own.most_common(top)]


def profile(args) -> None:
    """DWT_PCSR sampled through the printer while the core runs, symbolised with inlined frames."""
    tree = Tree(args.build, args.target)
    check_build(tree, args.stale_ok)
    control = need_printer(tree, "the samples")
    import collections
    counts: collections.Counter = collections.Counter()
    with Connection(control, 5) as c:
        _, (demcr,) = read_bytes(c.ask(req_read([(DEMCR, 4)])))
        if not int.from_bytes(demcr, "little") & (1 << 24):
            die("DEMCR.TRCENA is clear: the DWT is off, DWT_PCSR reads nothing")
        request_pcs = req_read([(DWT_PCSR, 4)] * READ_PIECES)
        end = time.monotonic() + args.seconds
        none = 0
        while time.monotonic() < end:
            _, data = read_bytes(c.ask(request_pcs))
            for d in data:
                pc = int.from_bytes(d, "little")
                if pc == PCSR_NONE:
                    none += 1
                else:
                    counts[pc & ~1] += 1
    total = sum(counts.values())
    if not total:
        die("every sample read 0xFFFFFFFF: the core is halted, or runs Secure code without Secure "
            "debug" if none else "no samples")
    if set(counts) == {0}:
        die("DWT_PCSR reads 0: this core has no DWT_PCSR")
    ranges = code_ranges(tree)
    outside = sum(n for a, n in counts.items() if not any(
        lo <= a < hi for lo, hi in ranges))
    frames = symbolize(tree.elf, counts)
    ranked = rank_samples(counts, frames, args.top)
    table = code_symbols(tree)
    short = symbol_names(table, counts)
    ranked["symbol"] = rank_by_symbol(counts, table, args.top, short)
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
            hit = symbol_at(table, address)
            where = f"{symbol_name(hit[1], short, 1)}+{address - hit[0]:#x}" if hit else "??"
            frame = (frames.get(address) or [
                     {"FunctionName": "??", "FileName": "", "Line": 0}])[0]
            print(f"{100 * n / total:6.1f} %  {address:#010x}  {where} | "
                  f"{frame['FunctionName']} {Path(frame['FileName']).name}:{frame['Line']}")


# ---- chip commands ---------------------------------------------------------------------------
# A chip package may ship <CHIP_ROOT>/tools/kvasir_bench_chip.py with
#     def commands() -> {name: (help, add_arguments(parser), run(bench, args))}
# `run` gets a Bench; a command that reads symbols calls bench.check_build itself.


def svd_registers(tree: Tree, peripheral: str) -> tuple[int, dict[str, tuple[int, dict[str, tuple[int, int]]]]]:
    """A peripheral's base address and registers from chip.svd: name -> (offset, field -> (lsb, width))."""
    import xml.etree.ElementTree as ET
    chip_root = tree.chip_root
    svd = Path(chip_root, "chip.svd") if chip_root else None
    if svd is None or not svd.is_file():
        die(
            f"no chip.svd under the tree's CHIP_ROOT ({chip_root or 'not set'})")
    for p in ET.parse(svd).getroot().iter("peripheral"):
        if p.findtext("name") != peripheral:
            continue
        regs = {}
        for r in p.iter("register"):
            fields = {}
            for f in r.iter("field"):
                m = re.fullmatch(r"\[(\d+):(\d+)\]",
                                 f.findtext("bitRange") or "")
                if m:
                    fields[f.findtext("name")] = (
                        int(m.group(2)), int(m.group(1)) - int(m.group(2)) + 1)
                elif f.findtext("bitOffset") is not None:
                    fields[f.findtext("name")] = (int(f.findtext("bitOffset"), 0),
                                                  int(f.findtext("bitWidth"), 0))
            regs[r.findtext("name")] = (
                int(r.findtext("addressOffset"), 0), fields)
        return int(p.findtext("baseAddress"), 0), regs
    die(f"{svd} has no peripheral {peripheral}")


def field(value: int, fields: dict[str, tuple[int, int]], name: str) -> int:
    lsb, width = fields[name]
    return (value >> lsb) & ((1 << width) - 1)


class Bench:
    """What a chip command works with: the tree, the printer's socket, the chip's SVD."""
    note, die, refuse, field = (staticmethod(note), staticmethod(die), staticmethod(refuse),
                                staticmethod(field))

    def __init__(self, tree: Tree):
        self.tree = tree
        self._connection: Connection | None = None

    def __enter__(self) -> "Bench":
        return self

    def __exit__(self, *_) -> None:
        if self._connection is not None:
            self._connection.close()

    def _ask(self, req: dict) -> dict:
        if self._connection is None:
            control = control_socket(self.tree)
            if control is None:
                refuse("no printer with a control socket runs for this target: chip commands go "
                       "through it - start one (printer start, or just log)")
            self._connection = Connection(control, 5)
        return self._connection.ask(req)

    def read(self, addresses: list[int]) -> tuple[int, list[int]]:
        """32-bit words while the core runs: (unix µs on the log's clock, values)."""
        stamp, data = read_bytes(
            self._ask(req_read([(a, 4) for a in addresses])))
        return stamp, [int.from_bytes(d, "little") for d in data]

    def write(self, words: list[tuple[int, int]]) -> list[int]:
        """(address, value) words written in order; each one's value read back after it."""
        _, data = read_bytes(self._ask(req_write(words)))
        return [int.from_bytes(d, "little") for d in data]

    def svd(self, peripheral: str) -> tuple[int, dict[str, tuple[int, dict[str, tuple[int, int]]]]]:
        return svd_registers(self.tree, peripheral)

    def check_build(self, stale_ok: bool) -> None:
        check_build(self.tree, stale_ok)


def load_chip_plugin(tree: Tree) -> typing.Any:
    """The chip package's kvasir_bench_chip.py module, or None."""
    import importlib.util
    path = Path(tree.chip_root, "tools",
                "kvasir_bench_chip.py") if tree.chip_root else None
    if path is None or not path.is_file():
        return None
    spec = importlib.util.spec_from_file_location("kvasir_bench_chip", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def chip_cmd(args) -> None:
    tree = Tree(args.build, args.target)
    plugin = load_chip_plugin(tree)
    commands = plugin.commands() if plugin else {}
    if not args.rest:
        if not commands:
            print(f"no chip commands ({tree.chip_root or 'no CHIP_ROOT'})")
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
    with Bench(tree) as bench:
        run(bench, parser.parse_args(args.rest[1:]))


# ---- command line ----------------------------------------------------------------------------


def main() -> None:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    pr = sub.add_parser("printer").add_subparsers(dest="action", required=True)
    for name, fn in (("start", printer_start), ("stop", printer_stop)):
        p = pr.add_parser(name)
        p.add_argument("build")
        p.add_argument("target")
        if name == "stop":
            p.add_argument("--confirm", action="store_true",
                           help="stop a printer that has a window (somebody's `just log`) too")
        p.set_defaults(fn=fn)
    p = pr.add_parser("status")
    p.add_argument("build", nargs="?")
    p.add_argument("target", nargs="?")
    p.set_defaults(fn=printer_status)

    for name, fn in (("flash", flash), ("reset", reset), ("crash", crash), ("stack", stack),
                     ("ub", ub)):
        p = sub.add_parser(name)
        p.add_argument("build")
        p.add_argument("target")
        if name == "flash":
            p.add_argument("--no-build", action="store_true")
        if name in ("flash", "reset"):
            p.add_argument("--confirm", action="store_true",
                           help="the repo's .kvasir_bench.json says this does something physical")
        else:
            p.add_argument("--stale-ok", action="store_true",
                           help="read symbols even if the board runs another build")
        p.set_defaults(fn=fn)

    p = sub.add_parser("panic")
    p.add_argument("build")
    p.add_argument("target")
    p.add_argument("--cause", choices=list(raise_causes()), default="user",
                   help="the Kvasir::Panic::Cause raise() is called with")
    p.add_argument("--confirm", action="store_true",
                   help="the repo's .kvasir_bench.json says a reset does something physical")
    p.add_argument("--stale-ok", action="store_true",
                   help="take raise()'s address from the ELF even if the board runs another build")
    p.set_defaults(fn=panic)

    p = sub.add_parser("messages")
    p.add_argument("build")
    p.add_argument("target")
    p.add_argument("--last", type=int, default=50)
    p.add_argument("--errors", action="store_true", help="errors only")
    p.add_argument("--grep", help="lines matching this regex")
    p.set_defaults(fn=messages)

    p = sub.add_parser("metrics")
    p.add_argument("build")
    p.add_argument("target")
    p.add_argument("--seconds", type=float, default=3,
                   help="how long to listen (the timeout of --until)")
    p.add_argument("--name", help="regex on scope::name")
    p.add_argument("--raw", action="store_true",
                   help="every sample, not the summary")
    p.add_argument(
        "--until", help="'NAME OP NUMBER': stop when a sample satisfies it")
    p.set_defaults(fn=metrics)

    p = sub.add_parser("status")
    p.add_argument("build", nargs="?")
    p.add_argument("target", nargs="?")
    p.set_defaults(fn=status)

    p = sub.add_parser("wait-for")
    p.add_argument("build")
    p.add_argument("target")
    p.add_argument("regex")
    p.add_argument("--timeout", type=float, default=10)
    p.add_argument("--from-now", action="store_true",
                   help="ignore what is already in the log")
    p.add_argument("--from-start", action="store_true",
                   help="search the whole file first")
    p.add_argument("--last", type=int, default=15,
                   help="lines shown on a timeout")
    p.add_argument("--width", type=int, default=200)
    add_module_arguments(p)
    p.set_defaults(fn=wait_for)

    p = sub.add_parser("trace")
    p.add_argument("build")
    p.add_argument("target")
    p.add_argument("name", nargs="?")
    p.add_argument("--last", type=int, default=0)
    p.add_argument("--hex", action="store_true")
    p.add_argument("--delta", action="store_true",
                   help="the first field's step from row to row")
    p.add_argument("--stale-ok", action="store_true",
                   help="read symbols even if the board runs another build")
    p.set_defaults(fn=trace)

    p = sub.add_parser("log")
    p.add_argument("build")
    p.add_argument("target")
    p.add_argument("--last", type=int, default=40)
    p.add_argument("--grep")
    p.add_argument("--level", choices=LEVELS)
    p.add_argument("--since-ms", type=float)
    p.add_argument("--since-mark", action="store_true",
                   help="only what came after this tool's last flash or reset")
    p.add_argument("--width", type=int, default=200)
    add_module_arguments(p)
    p.set_defaults(fn=log)

    p = sub.add_parser("peek")
    p.add_argument("build")
    p.add_argument("target")
    p.add_argument("symbol", nargs="+")
    p.add_argument("--watch", type=int, default=1, help="read this many times")
    p.add_argument("--interval-ms", type=int, default=200)
    p.add_argument(
        "--until", help="'changed', '== N', '!= N', '< N', '> N': the printer watches")
    p.add_argument("--timeout", type=float, default=10,
                   help="seconds, for --until")
    p.add_argument("--typed", action="store_true",
                   help="decode with the ELF's debug info (enums, structs, atomics)")
    p.add_argument("--direct", action="store_true",
                   help="attach with JLinkExe even if this target's printer has a peek server")
    p.add_argument("--stale-ok", action="store_true",
                   help="read symbols even if the board runs another build")
    p.set_defaults(fn=peek)

    p = sub.add_parser("profile")
    p.add_argument("build")
    p.add_argument("target")
    p.add_argument("--seconds", type=float, default=10)
    p.add_argument("--top", type=int, default=30)
    p.add_argument("--inclusive", action="store_true",
                   help="also rank by inclusive time over the inline stack")
    p.add_argument("--addresses", type=int, default=0, metavar="N",
                   help="also list the N hottest PCs, each with its ELF symbol and debug-info frame")
    p.add_argument("--stale-ok", action="store_true",
                   help="read symbols even if the board runs another build")
    p.set_defaults(fn=profile)

    p = sub.add_parser("chip")
    p.add_argument("build")
    p.add_argument("target")
    p.add_argument("rest", nargs=argparse.REMAINDER)
    p.set_defaults(fn=chip_cmd)

    p = sub.add_parser("snapshot")
    p.add_argument("build")
    p.add_argument("target")
    p.add_argument("--var", action="append", default=[])
    p.add_argument("--frames", type=int, default=10)
    p.add_argument("--stale-ok", action="store_true",
                   help="read symbols even if the board runs another build")
    p.set_defaults(fn=snapshot)

    args = ap.parse_args()
    # one exit code per kind of outcome (the table at the top): no tracebacks for what the
    # printer or the socket answered
    try:
        args.fn(args)
    except ControlError as e:
        die(f"the printer answered: {e}")
    except (TimeoutError, socket.timeout):
        die("the printer did not answer in time (is it busy with a flash? `status` shows)")
    except (ConnectionError, FileNotFoundError) as e:
        die(f"the printer's socket went away: {e} (`status` shows what runs)")


if __name__ == "__main__":
    main()
