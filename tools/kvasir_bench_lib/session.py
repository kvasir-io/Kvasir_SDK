"""One command's world: the tree, its image, the printer (asked for once, started on demand), the guards - and the
registry that builds it for every command. Chip plugins get the same object."""
import typing
from pathlib import Path

from uc_log_client import Connection, req_read_words, req_records, req_write, words  # noqa: E402

from . import die, note, printer, refuse, svd, trees
from .image import Image


class Session:
    """What a command (and a chip plugin's `run(bench, args)`) works with. Nothing reaches the board before it is
    asked for: `control` finds the target's printer, or starts one that stops again at the command's end (rule 0)."""
    die = staticmethod(die)
    note = staticmethod(note)
    refuse = staticmethod(refuse)

    def __init__(self, tree: trees.Tree, what: str = "this command"):
        self.tree, self.what = tree, what
        self._control: Path | None = None
        self._connection: Connection | None = None

    def __enter__(self) -> "Session":
        return self

    def __exit__(self, *_) -> None:
        if self._connection is not None:
            self._connection.close()
            self._connection = None

    @property
    def image(self) -> Image:
        return self.tree.image

    def need(self, what: str) -> Path:
        """The printer's control socket, found or started once; `what` names the reason when one is started."""
        if self._control is None:
            self._control = printer.need_printer(self.tree, what)
        return self._control

    @property
    def control(self) -> Path:
        return self.need(self.what)

    def existing_control(self) -> Path | None:
        """The socket of a printer that runs for the target now; never starts one."""
        return printer.control_socket(self.tree)

    def check_build(self, stale_ok: bool) -> None:
        """Rule 3: the board runs this tree's build (the printer's firmware check), else refuse."""
        if self._control is None:
            self._control = printer.need_printer(
                self.tree, "the firmware check")
        printer.check_build(self.tree, self._control, stale_ok)

    def confirm(self, action: str, confirmed: bool) -> None:
        """Rule 4: the repo's .kvasir_bench.json may say that `action` does something physical."""
        trees.confirm_needed(self.tree, action, confirmed)

    def ask(self, req: dict, timeout: float | None = None) -> dict:
        """One request on a connection kept for the session (`timeout`: from this request on, else 5 s)."""
        if self._connection is None:
            self._connection = Connection(self.control, 5)
        return self._connection.ask(req, timeout)

    def records(self, kinds: list[str]) -> dict:
        """What the firmware leaves for a debugger, read and decoded by the printer (its `records` answer: uc_log's
        TargetRecords.hpp is the one decoder) with the map it decodes the log with - which must be this tree's."""
        answer = self.ask(req_records(kinds), timeout=60)
        printer.check_map(self.tree, answer)
        return answer

    def read_bytes(self, address: int, size: int) -> bytes:
        """Any amount of target memory while the core runs, on the session's one connection."""
        return printer.read_chunked(self.ask, address, size)

    # the chip plugin contract: commands() -> {name: (help, add_arguments(parser), run(bench, args))}, and `run`
    # calls bench.read / write / svd / die / check_build
    def read(self, addresses: list[int]) -> tuple[int, list[int]]:
        """32-bit words while the core runs: (unix µs on the log's clock, values)."""
        return words(self.ask(req_read_words(addresses)))

    def write(self, writes: list[tuple[int, int]]) -> list[int]:
        """(address, value) words written in order; each one's value read back after it."""
        return words(self.ask(req_write(writes)))[1]

    def svd(self, peripheral: str) -> tuple[int, dict[str, tuple[int, dict[str, tuple[int, int]]]]]:
        return svd.svd_registers(self.tree, peripheral)


# ---- the command registry --------------------------------------------------------------------


def arg(*names: str, **options) -> tuple[tuple[str, ...], dict]:
    """One argparse argument, declared as data."""
    return names, options


# the options several commands share with the same text; a command whose text differs declares its own
COMMON = {
    "stale_ok": arg("--stale-ok", action="store_true", help="read symbols even if the board runs another build"),
    "confirm": arg("--confirm", action="store_true",
                   help="the repo's .kvasir_bench.json says this does something physical"),
}


class Command(typing.NamedTuple):
    path: tuple[str, ...]           # ("printer", "start")
    fn: typing.Callable
    # arg(...) tuples or COMMON names, in declaration order
    args: list
    # BUILD TARGET given: the command gets a Session; else None (status, printer status)
    target: bool
    what: str                       # need_printer's reason when it starts one
    confirm: str | None             # rule 4's action, checked first
    checks_build: bool              # rule 3, checked before the command's own work


REGISTRY: list[Command] = []


# The guards run in this order, before the command's own code (the order the commands had by hand):
#   confirm (flash, reset, panic as "reset")  ->  the build check (crash, ub, stack, trace, peek, snapshot,
#   coverage, profile, panic)  ->  the command, which asks for the printer when it first needs it.
# poke checks the build itself, and only for a symbol (an address needs no build).
def command(*path: str, args=(), target: bool = True, what: str = "this command", confirm: str | None = None,
            checks_build: bool = False):
    """Registers `fn(session, args)` under `path`; without a target the session is None (the command reads
    args.build / args.target itself, if it has them)."""
    def wrap(fn):
        REGISTRY.append(Command(path, fn, list(args),
                        target, what, confirm, checks_build))
        return fn
    return wrap


def run(cmd: Command, args) -> None:
    if not cmd.target:
        cmd.fn(None, args)
        return
    with Session(trees.Tree(args.build, args.target), cmd.what) as s:
        if cmd.confirm:
            s.confirm(cmd.confirm, args.confirm)
        if cmd.checks_build:
            s.check_build(args.stale_ok)
        cmd.fn(s, args)
