"""The printer: the uc_log_printer processes (from their argv), starting and stopping one, its control socket, the
build guard, and reading target memory through it. kvasir_bench never opens a probe itself."""
import atexit
import json
import os
import signal
import socket
import subprocess
import time
import typing
from pathlib import Path

from uc_log_client import (READ_BYTES, Connection, ControlError, alive, control_socket_path,  # noqa: E402
                           message_line, read_bytes, req_messages, req_read, req_status, request,
                           status_text)

from . import die, note, refuse
from .trees import Tree, hex_build_id

PRINTER = "kvasir_uc_log/uc_log_printer_host_build/uc_log_printer"


class Printer(typing.NamedTuple):
    """A running uc_log_printer, from its argv (the NUL-split /proc/<pid>/cmdline, so an argument
    with a space stays one)."""
    pid: int
    argv: list[str]
    age: float          # seconds
    cwd: Path | None    # /proc/<pid>/cwd, to resolve a relative --log_dir; None when unreadable

    @classmethod
    def from_proc(cls, proc: Path) -> "Printer | None":
        """The printer behind a /proc/<pid> directory; None for any other process. Raises OSError
        for one that is gone or unreadable."""
        argv = [a.decode(errors="replace") for a in (
            proc / "cmdline").read_bytes().split(b"\0")[:-1]]
        # a substring of the program path, not its basename: a printer built elsewhere counts too
        if not argv or "uc_log_printer" not in argv[0]:
            return None
        try:
            cwd = Path(os.readlink(proc / "cwd"))
        except OSError:
            cwd = None
        return cls(int(proc.name), argv, time.time() - proc.stat().st_mtime, cwd)

    def value(self, flag: str) -> str | None:
        """The last value of `flag`, given as `flag value` or `flag=value` (cxxopts takes both)."""
        found = None
        for i, a in enumerate(self.argv[1:], 1):
            if a == flag and i + 1 < len(self.argv):
                found = self.argv[i + 1]
            elif a.startswith(flag + "="):
                found = a[len(flag) + 1:]
        return found

    @property
    def log_dir(self) -> Path | None:
        v = self.value("--log_dir")
        if not v:
            return None
        p = Path(v)
        if not p.is_absolute() and self.cwd is not None:
            p = self.cwd / p
        return p.resolve()      # not strict: a directory that is gone still compares

    @property
    def host(self) -> str:
        return self.value("--host") or ""

    @property
    def probe(self) -> str:
        """Which J-Link it sits on: `ip:<host>`, a probe's nickname or serial, or whichever one
        USB probe there is."""
        return f"ip:{self.host}" if self.host else (self.value("--probe") or "(any usb)")

    @property
    def has_window(self) -> bool:
        return "--disable_ui" not in self.argv

    @property
    def command_line(self) -> str:
        """For messages only, never for matching."""
        return " ".join(self.argv)

    def alive(self) -> bool:
        return Path(f"/proc/{self.pid}").exists()


def printers() -> list[Printer]:
    """Every uc_log_printer running."""
    found = []
    for proc in Path("/proc").iterdir():
        if not proc.name.isdigit():
            continue
        try:
            if (p := Printer.from_proc(proc)) is not None:
                found.append(p)
        except OSError:
            pass
    return found


def printers_of(tree: "Tree") -> list[Printer]:
    """The printers that serve this target: the same log directory, compared as paths (a substring
    test took x_release_log's printer for x_release's)."""
    mine = tree.log_dir.resolve()
    return [p for p in printers() if p.log_dir == mine]


def start_printer(tree: Tree) -> int:
    """A printer for the tree's target, built first; its pid. Refuses where another printer holds
    the probe."""
    # UC_LOG_PRINTER_BIN: a printer built elsewhere (an A/B from source snapshots, a frozen printer for a long run),
    # used as it is - not rebuilt
    override = os.environ.get("UC_LOG_PRINTER_BIN")
    exe = Path(override) if override else tree.build / PRINTER
    # Always built, not only when missing: the binary must take every option the tree's log command passes. Up to
    # date this is a ~0.1 s no-op; `just log` does the same through log_<target>'s dependency.
    built = subprocess.run(["true"] if override else
                           ["cmake", "--build",
                               str(tree.build), "--target", "uc_log_printer"],
                           capture_output=True, text=True)
    if built.returncode != 0:
        die(f"the printer does not build: cmake --build {tree.build} --target uc_log_printer\n"
            + (built.stdout + built.stderr)[-2000:])
    if not exe.is_file():
        die(f"{exe} does not build: cmake --build {tree.build} --target uc_log_printer")
    if control_socket(tree) is not None:
        die(f"a printer already serves {tree.target} ({control_socket_path(tree.log_dir)}): use it "
            "- flash, log, peek ... go through it, whoever started it")
    for p in printers():
        other = p.probe
        if other.startswith("ip:") or tree.host:
            if tree.host and other == f"ip:{tree.host}":
                die(
                    f"the J-Link at {tree.host} is held by printer {p.pid}: {p.command_line[:120]}")
            continue   # a network J-Link is another probe
        if other in (tree.probe, "(any usb)") or not tree.probe:
            die(f"probe '{tree.probe or 'any'}' is held by printer {p.pid}: {p.command_line[:120]}\n"
                "  stop it first (kvasir_bench.py printer status / stop): two printers on one "
                "target fight over its RTT buffer")
    tree.log_dir.mkdir(parents=True, exist_ok=True)
    cmd = tree.printer_command(exe)
    # own session: the printer outlives the shell that started it (and keeps its own copy of printer.out)
    with open(tree.log_dir / "printer.out", "w") as out:
        proc = subprocess.Popen(cmd, cwd=tree.build, stdout=out, stderr=subprocess.STDOUT,
                                stdin=subprocess.DEVNULL, start_new_session=True)
    time.sleep(1.5)
    if proc.poll() is not None:
        die(f"the printer exited at once; {tree.log_dir / 'printer.out'}:\n"
            + (tree.log_dir / "printer.out").read_text()[-600:])
    return proc.pid


def stop_pid(pid: int) -> bool:
    """SIGINT, then up to 10 s for it to go; whether it went."""
    try:
        os.kill(pid, signal.SIGINT)
    except ProcessLookupError:
        return True
    end = time.monotonic() + 10
    while time.monotonic() < end and Path(f"/proc/{pid}").exists():
        time.sleep(0.05)
    return not Path(f"/proc/{pid}").exists()


_started_here: list[int] = []


def _stop_started_here() -> None:
    for pid in _started_here:
        stop_pid(pid)
        note(f"stopped printer {pid} again (started for this command)")


def check_build(tree: Tree, control: Path, stale_ok: bool) -> None:
    """Symbols come from the tree's ELF: on a board that runs another build their addresses are
    wrong, and every value read there is plausible nonsense. The printer knows what the board
    runs (its firmware check at session start); refuse unless it is this tree's hex."""
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


def check_map(tree: Tree, answer: dict) -> None:
    """A records answer comes from the printer's map: another printer's (another target's) map would decode any
    address as plausibly as this one. Not a stale board (check_build's --stale-ok), a wrong printer: no way past."""
    used = Path(answer["map"]["path"])
    if used.resolve() != tree.map_file.resolve():
        die(f"the printer decodes with {used}, not this tree's map {tree.map_file}")


# ---- the printer's control socket (uc_log_client speaks it) ------------------------------------


def control_socket(tree: Tree) -> Path | None:
    """The control socket of this target's printer, if one runs and answers a ping there. A malformed answer stops
    the tool."""
    control = control_socket_path(tree.log_dir)
    try:
        # not alive: no socket, or a killed printer's file
        return control if alive(control) else None
    except ControlError as e:
        die(f"{control}: {e}")


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
    """The printer's control socket. kvasir_bench never opens a probe itself: every access goes
    through the printer that holds it. With none running for this target, one is started for this
    command and stopped again at its end (a printer from `printer start` or `just log` stays)."""
    control = control_socket(tree)
    if control is not None:
        return control
    # a printer of this target that does not answer yet (starting, or restarting its connection): wait for it and use
    # it - never a second printer beside it (two on one probe fight over its RTT buffer)
    serving = printers_of(tree)
    if serving:
        end = time.monotonic() + 15
        while time.monotonic() < end and any(p.alive() for p in serving):
            if (control := control_socket(tree)) is not None:
                return control
            time.sleep(0.1)
        if any(p.alive() for p in serving):
            refuse(f"printer {', '.join(str(p.pid) for p in serving)} serves {tree.target} but its control socket does not "
                   f"answer after 15 s - not starting a second one beside it; see {tree.log_dir / 'printer.out'}, or "
                   "stop it (kvasir_bench.py printer stop)")
    pid = start_printer(tree)
    if not _started_here:
        atexit.register(_stop_started_here)
    _started_here.append(pid)
    end = time.monotonic() + 15
    while (control := control_socket(tree)) is None:
        if time.monotonic() > end or not Path(f"/proc/{pid}").exists():
            die(f"the printer started for {what} has no control socket; "
                f"{tree.log_dir / 'printer.out'}:\n" + (tree.log_dir / "printer.out").read_text()[-600:])
        time.sleep(0.1)
    note(
        f"started printer {pid} for {what} (none ran for {tree.target}); it stops again at the end")
    return control


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
    lost; its `status` says `ram_image` false."""
    if control is None or not tree.ram_image():
        return
    try:
        status = request(control, req_status())
    except ControlError as e:
        die(f"status of the printer: {e}")
    if status.get("ram_image"):
        return
    refuse(f"{tree.target} is a RAM_ONLY image and the running printer would {what} it with a "
           f"reset (the boot ROM then runs flash, not the image) - it is started without --ram_image: restart it "
           f"(printer stop / printer start {tree.build} {tree.target}, or just log after cmake has run again in "
           f"{tree.build})")


def read_chunked(ask: typing.Callable[[dict], dict], address: int, size: int, on_error: str = "die") -> bytes | None:
    """Any amount of target memory through one connection's `ask`, the most a request may carry (READ_BYTES) at a
    time. A refused read dies, or gives None with on_error="none" (the gdb stub answers E01)."""
    out = b""
    while len(out) < size:
        n = min(READ_BYTES, size - len(out))
        try:
            _, data = read_bytes(ask(req_read([(address + len(out), n)])))
        except ControlError as e:
            if on_error == "none":
                return None
            die(f"read through the printer: {e}")
        out += data[0]
    return out


def events(stream: Connection, seconds: float) -> typing.Iterator[dict]:
    """A subscription's events until `seconds` have passed, or the printer stays quiet for the rest of them; what a
    `dropped` event means is the caller's (wait-for fails, metrics only notes it)."""
    end = time.monotonic() + seconds
    while (left := end - time.monotonic()) > 0:
        stream.sock.settimeout(max(left, 0.01))
        try:
            yield stream.next()
        except (TimeoutError, socket.timeout):
            return


def read_target(control: Path, address: int, size: int) -> bytes:
    """Any amount of target memory through the printer, on a connection of its own (a Session keeps one:
    Session.read_bytes)."""
    with Connection(control, 5) as c:
        return read_chunked(c.ask, address, size)


def probes_on_usb(root: Path = Path("/sys/bus/usb/devices")) -> list[tuple[str, str, str]]:
    """(serial, product, nickname) of every SEGGER J-Link on USB, from sysfs: no J-Link tool, so
    nothing touches a probe a printer holds. A probe with a nickname names itself
    `J-Link_<nickname>` in its USB product string."""
    found = []
    for vendor in sorted(root.glob("*/idVendor")):
        try:
            if vendor.read_text().strip() != "1366":   # SEGGER
                continue
            serial = (vendor.parent / "serial").read_text().strip().lstrip("0")
            product = (vendor.parent / "product").read_text().strip()
        except OSError:
            continue
        nick = product.removeprefix(
            "J-Link_") if product.startswith("J-Link_") else ""
        found.append((serial, product.replace("_", " "), nick))
    return found


def print_printer(p: Printer) -> None:
    log_dir = p.log_dir
    line = f"    printer {p.pid}, up {p.age:.0f} s, {log_dir or ''}"
    control = control_socket_path(log_dir) if log_dir else None
    if control and control.is_socket():
        try:
            line += "\n        " + \
                status_text(request(control, req_status(), 2))
        except ControlError as e:
            line += f"\n        (its control socket answered: {e})"
        except OSError:
            line += "\n        (its control socket does not answer)"
    else:
        line += "\n        (no control socket)"
    if log_dir:
        logs = sorted(log_dir.glob("*.rttlog"),
                      key=lambda f: f.stat().st_mtime)
        if logs:
            line += f"\n        last log line {time.time() - logs[-1].stat().st_mtime:.0f} s ago"
    print(line)
