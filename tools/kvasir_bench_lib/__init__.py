"""kvasir_bench's layers (the entry and its reference text: tools/kvasir_bench.py). Bottom up: trees (a build tree),
printer (the printer process and its control socket, the guards), image (the tree's ELF, each tool run once), records
(the .noInit records and rings a firmware leaves), gdb_stub, profiling, coverage_data, svd, session (one command's
world, the chip plugins' Bench), commands, cli (the registry and main)."""
import sys
import typing
from pathlib import Path

sys.dont_write_bytecode = True  # no __pycache__ next to the imported tools

SDK = Path(__file__).resolve().parents[2]

# The printer's side of things - its control protocol and log history - is uc_log's own client module; this tool adds
# what a Kvasir build tree and a Kvasir firmware know. elf_image is the post-link script's ELF reader; tools/ holds
# cxx_names, the C++-name shortener size_report shares.
for _tools in (SDK / "tools", SDK / "uc_log" / "tools", SDK / "uc_log" / "remote_fmt" / "tools", SDK / "cmake" / "tools"):
    if str(_tools) not in sys.path:
        sys.path.insert(0, str(_tools))


def die(msg: str, code: int = 1) -> typing.NoReturn:
    print(f"kvasir_bench: {msg}", file=sys.stderr)
    sys.exit(code)


def refuse(msg: str) -> typing.NoReturn:
    """A guard said no (exit 4): the command would do harm or give wrong answers as asked."""
    die(msg, 4)


def note(msg: str) -> None:
    print(f"kvasir_bench: {msg}", file=sys.stderr)
