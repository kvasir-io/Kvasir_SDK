"""The programs that run inside gdb's embedded Python (gdb_stub.gdb_python imports this file as a top-level module,
with this folder and tools/ on sys.path): `typed` decodes symbols' bytes with the ELF's debug info (peek --typed),
`snapshot` prints registers, frames and variables over the printer's gdb stub. Each takes a JSON job file. No
relative import: inside gdb this is no package member. The print formats are plain functions the host tests call
without gdb."""
import json

from cxx_names import display_name

# gdb's own module exists only inside gdb; the formats below need none of it
try:
    import gdb
except ImportError:
    gdb = None


def mode_text(xpsr: int) -> str:
    """The IPSR field (xPSR[8:0]) as a word: thread mode, or the exception."""
    ipsr = xpsr & 0x1FF
    if ipsr == 0:
        return "thread"
    name = {3: "HardFault", 15: "SysTick"}.get(ipsr, f"IRQ {ipsr - 16}")
    return f"exception {ipsr} ({name})"


def frame_line(n: int, name: str | None, file: str, line: int, root: str) -> str:
    """One backtrace line, the file as the tree shows it (without the source root)."""
    where = file[len(root):] if root and file.startswith(root) else file
    return f"#{n:<2d} {display_name(name or '?')}  {where}:{line}"


def var_line(name: str, size: int, raw: bytes) -> str:
    """A --var: a number up to 8 bytes, else the bytes read (64 at most)."""
    value = int.from_bytes(raw, "little")
    return f"{name[:70]} = " + (f"{value} (0x{value:x})" if size <= 8 else raw.hex(" "))


def unwrap(v):
    """std::atomic<T> and friends: one data member, no point in showing the wrapper."""
    t = v.type.strip_typedefs()
    while t.code == gdb.TYPE_CODE_STRUCT:
        members = [f for f in t.fields(
        ) if not f.is_base_class and hasattr(f, "bitpos")]
        bases = [f for f in t.fields() if f.is_base_class]
        if len(members) == 1 and not bases and ("atomic" in str(t)
                                                or members[0].name in ("_M_i", "__a_value", "__a_")):
            v = v[members[0]]
        elif not members and len(bases) == 1 and "atomic" in str(t):
            v = v[bases[0]]
        else:
            break
        t = v.type.strip_typedefs()
    return v


def typed(job_path: str) -> None:
    """Job {"jobs": [[address, hex bytes]], "result": path}: writes one decoded text (or null) per job."""
    with open(job_path) as f:
        job = json.load(f)
    gdb.execute("set pagination off")
    gdb.execute("set width 0")
    gdb.execute("set print pretty off")
    out = []
    for addr, raw in job["jobs"]:
        try:
            name = gdb.execute(f"info symbol 0x{addr:x}", to_string=True).split(
                " in section")[0].strip()
            t = gdb.parse_and_eval("'" + name + "'").type
            data = bytes.fromhex(raw)
            if len(data) < t.sizeof:
                raise ValueError(f"{len(data)} of {t.sizeof} bytes")
            v = unwrap(gdb.Value(data[:t.sizeof], t))
            out.append(v.format_string(
                max_depth=4, static_members=False, symbols=False)[:600])
        except Exception:
            out.append(None)
    with open(job["result"], "w") as f:
        json.dump(out, f)


def snapshot(job_path: str) -> None:
    """Job {"port", "frames", "vars": [[address, size, name]], "root"}: over the printer's gdb stub on `port`, the
    registers, the mode, up to `frames` frames and each variable, then detach."""
    with open(job_path) as f:
        job = json.load(f)
    gdb.execute("set pagination off")
    gdb.execute("set confirm off")
    gdb.execute("set width 0")
    gdb.execute(f"target remote :{job['port']}")
    for reg in ("pc", "sp", "lr", "xpsr"):
        print(f"{reg:<5} {gdb.parse_and_eval('$' + reg).format_string(format='x')}")
    print(f"mode  {mode_text(int(gdb.parse_and_eval('$xpsr')))}")
    frame, n = gdb.newest_frame(), 0
    while frame is not None and n < job["frames"]:
        sal = frame.find_sal()
        print(frame_line(n, frame.name(),
              sal.symtab.filename if sal.symtab else "?", sal.line, job["root"]))
        frame, n = frame.older(), n + 1
    for addr, size, name in job["vars"]:
        print(var_line(name, size, gdb.selected_inferior(
        ).read_memory(addr, min(size, 64)).tobytes()))
    gdb.execute("detach")
