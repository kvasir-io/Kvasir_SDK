"""The command line: the parser built from the registry (session.command), and the exit code of every outcome."""
import argparse
import socket

from uc_log_client import ControlError  # noqa: E402

from . import commands, die
from .session import COMMON, REGISTRY, run

_ = commands   # registers every command


def build_parser(description: str | None) -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        description=description, formatter_class=argparse.RawDescriptionHelpFormatter)
    top = ap.add_subparsers(dest="cmd", required=True)
    groups: dict[str, argparse._SubParsersAction] = {}
    for cmd in REGISTRY:
        sub = top
        if len(cmd.path) == 2:
            if cmd.path[0] not in groups:
                groups[cmd.path[0]] = top.add_parser(
                    cmd.path[0]).add_subparsers(dest="action", required=True)
            sub = groups[cmd.path[0]]
        p = sub.add_parser(cmd.path[-1])
        if cmd.target:
            p.add_argument("build")
            p.add_argument("target")
        for a in cmd.args:
            names, options = COMMON[a] if isinstance(a, str) else a
            p.add_argument(*names, **options)
        p.set_defaults(command=cmd)
    return ap


def main(description: str | None) -> None:
    args = build_parser(description).parse_args()
    # one exit code per kind of outcome (the table at the top): no tracebacks for what the
    # printer or the socket answered
    try:
        run(args.command, args)
    except ControlError as e:
        die(f"the printer answered: {e}")
    except (TimeoutError, socket.timeout):
        die("the printer did not answer in time (is it busy with a flash? `status` shows)")
    except FileNotFoundError as e:
        # a program or file, not the socket (a unix connect names none)
        if e.filename is not None:
            die(f"{e.filename}: {e.strerror}")
        die(f"the printer's socket went away: {e} (`status` shows what runs)")
    except ConnectionError as e:
        die(f"the printer's socket went away: {e} (`status` shows what runs)")
