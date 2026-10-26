"""gdb without a second probe client: the typed decoder (gdb on the ELF alone) and a gdb remote stub that answers
from the printer's registers and reads."""
import contextlib
import json
import re
import shutil
import socket
import subprocess
import tempfile
import threading
import typing
from pathlib import Path

from uc_log_client import Connection  # noqa: E402

from . import die
from .printer import read_chunked
from .trees import Tree


IN_GDB = Path(__file__).with_name("in_gdb.py")
TOOLS = IN_GDB.parents[1]   # tools/, for cxx_names


def gdb_python(entry: str, job: Path) -> list[str]:
    """gdb -ex arguments that import in_gdb as a top-level module (no __pycache__ in the tools folders) and run one
    of its entries on a job file."""
    path = f"sys.path[:0] = [{str(IN_GDB.parent)!r}, {str(TOOLS)!r}]"
    return ["-ex", f"python import sys; sys.dont_write_bytecode = True; {path}",
            "-ex", f"python import in_gdb; in_gdb.{entry}({str(job)!r})"]


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
        job, result = Path(tmp, "job.json"), Path(tmp, "result.json")
        job.write_text(json.dumps(
            {"jobs": [[a, raw.hex()] for a, raw in jobs], "result": str(result)}))
        subprocess.run([gdb, "-batch", "-nx", str(tree.elf), *gdb_python("typed", job)],
                       capture_output=True, text=True, timeout=120)
        try:
            return json.loads(result.read_text())
        except (OSError, ValueError):
            return [None] * len(jobs)


# GDB's own description of an M-profile core (gdb/features/arm/arm-m-profile.xml): r0..r12, sp, lr, pc, xpsr
GDB_TARGET_XML = ('<?xml version="1.0"?><!DOCTYPE target SYSTEM "gdb-target.dtd"><target>'
                  '<architecture>arm</architecture><feature name="org.gnu.gdb.arm.m-profile">'
                  + "".join(f'<reg name="r{i}" bitsize="32"/>' for i in range(13))
                  + '<reg name="sp" bitsize="32" type="data_ptr"/><reg name="lr" bitsize="32"/>'
                    '<reg name="pc" bitsize="32" type="code_ptr"/><reg name="xpsr" bitsize="32"/>'
                    '</feature></target>')


def gdb_packet(payload: str) -> bytes:
    return f"${payload}#{sum(payload.encode()) & 0xFF:02x}".encode()


def serve_gdb(conn: socket.socket, registers: list[int], read: typing.Callable[[int, int], bytes | None]) -> None:
    """One gdb session of the remote protocol, answered from `registers` (r0..r12, sp, lr, pc,
    xpsr) and `read`: what gdb needs for registers, a backtrace and memory, nothing else."""
    pending = b""
    while True:
        while b"#" not in pending or len(pending) < pending.index(b"#") + 3:
            chunk = conn.recv(65536)
            if not chunk:
                return
            pending += chunk
            pending = pending.lstrip(b"+-\x03")
        start = pending.index(b"$")
        end = pending.index(b"#", start)
        packet = pending[start + 1:end].decode(errors="replace")
        pending = pending[end + 3:].lstrip(b"+-")
        conn.sendall(b"+")
        reply = ""
        if packet.startswith("qSupported"):
            reply = "PacketSize=4000;qXfer:features:read+"
        elif packet.startswith("qXfer:features:read:target.xml:"):
            off, length = (int(x, 16)
                           for x in packet.rsplit(":", 1)[1].split(","))
            part = GDB_TARGET_XML[off:off + length]
            reply = ("l" if off + length >=
                     len(GDB_TARGET_XML) else "m") + part
        elif packet == "?":
            reply = "S05"
        elif packet.startswith(("Hg", "Hc")):
            reply = "OK"
        elif packet == "qAttached":
            reply = "1"
        elif packet == "qfThreadInfo":
            reply = "m1"
        elif packet == "qsThreadInfo":
            reply = "l"
        elif packet == "qC":
            reply = "QC1"
        elif packet == "g":
            reply = "".join(v.to_bytes(4, "little").hex() for v in registers)
        elif packet.startswith("p"):
            n = int(packet[1:], 16)
            reply = registers[n].to_bytes(
                4, "little").hex() if n < len(registers) else "E01"
        elif packet.startswith("m"):
            addr, length = (int(x, 16) for x in packet[1:].split(","))
            data = read(addr, length)
            reply = data.hex() if data is not None else "E01"
        elif packet in ("D", "k") or packet.startswith("D;"):
            conn.sendall(gdb_packet("OK"))
            return
        conn.sendall(gdb_packet(reply))


@contextlib.contextmanager
def printer_gdb_stub(control: Path, registers: list[int]):
    """A gdb remote on a free local port that reads memory through the printer: no second
    debugger on the probe. Yields the port; one session."""
    # one connection for the stub's life, and requests as large as the printer takes: the core stands still
    # while gdb reads, so every round trip saved shortens the halt
    connection = Connection(control, 10)

    def read(addr: int, length: int) -> bytes | None:
        return read_chunked(connection.ask, addr, length, on_error="none")

    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    port = listener.getsockname()[1]

    def run():
        try:
            conn, _ = listener.accept()
            with conn:
                serve_gdb(conn, registers, read)
        except OSError:
            pass

    thread = threading.Thread(target=run, daemon=True)
    thread.start()
    try:
        yield port
    finally:
        listener.close()
        thread.join(timeout=5)
        connection.close()


def run_gdb_script(tree: Tree, port: int, args, wanted) -> None:
    debugger = arm_gdb()
    if debugger is None:
        die("no gdb with Python and ARM support (arm-none-eabi-gdb, gdb-multiarch, gdb)")
    with tempfile.TemporaryDirectory() as tmp:   # gone on every way out, a gdb timeout included
        job = Path(tmp, "job.json")
        job.write_text(json.dumps(
            {"port": port, "frames": args.frames, "vars": wanted, "root": tree.path_root}))
        gdb = subprocess.run([debugger, "-batch", "-nx", str(tree.elf), *gdb_python("snapshot", job)],
                             capture_output=True, text=True, timeout=60)
    keep = [l for l in gdb.stdout.splitlines() if re.match(
        r"(pc|sp|lr|xpsr|mode) |#\d|.* = ", l)]
    print("\n".join(keep)
          if keep else gdb.stdout[-1500:] + gdb.stderr[-800:])
