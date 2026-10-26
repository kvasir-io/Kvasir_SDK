#!/usr/bin/env python3
"""kvasir_bench: drive a board on the bench from scripts - and from an AI assistant's tool calls.

Non-interactive, short output, and nothing left running that it did not say it started. Every
command takes a firmware's BUILD directory and a TARGET (`sanitize`, `tty_release_log`) and finds
the rest itself - probe, J-Link device, ELF, log directory, the printer's socket. BUILD may be a
repo with a single build tree; a target that is not built lists the ones that are.

RULES (the tool enforces what it can; the rest is on you)
  0. kvasir_bench never opens a probe itself (no J-Link tool, GDB server or J-Link DLL): every
     command goes through the printer that holds the probe. With none running for the target,
     a command starts one, uses it and stops it again at its end.
  1. `status BUILD TARGET` before any hardware step: which probe, whose printer, running or halted,
     and whether the board runs the tree's build.
  2. One printer per board. If one runs - a `just log` window, or one from `printer start`
     - use it: every command goes through its socket. `printer stop` refuses to end a printer that
     has a window (exit 4) unless asked with --confirm.
  3. Symbols come from the tree's ELF. `peek`, `trace`, `stack`, `ub`, `crash`, `snapshot`, `profile`
     refuse (exit 4) when the printer says the board runs another build or the tree was rebuilt
     since the flash: flash first, or --stale-ok if you know the symbol did not move.
  4. `reset` and `flash` refuse (exit 4) where the repo's .kvasir_bench.json says they do something
     physical (a motor that calibrates at boot) - ask the owner, then --confirm.
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
  peek     B T SYMBOL... [--watch N --interval-ms MS] [--typed]
                          statics read while the core runs (~2 ms a read through the printer,
                          stamped on the log's clock)
  trace    B T [NAME] [--last N] [--delta]    Kvasir::Trace rings, oldest record first
  stack    B T                                the stack's high-water mark (Kvasir::StackUsage)
  ub       B T                                a sanitize build's report counter
  coverage B T [--merge P.profdata]... [-- LLVM-COV-REPORT-ARGS]
                          a `coverage` build's source coverage, read live (Kvasir::Coverage in its
                          Startup list): writes <T>.profraw/.profdata in the tree, prints
                          llvm-cov's report (runtime libraries filtered out). Through the printer
  crash    B T                                the last halt the printer caught, symbolised
                                              and the .noInit records: fault, panic, health
                                              (starvation, recoveries), boot guard - read and
                                              decoded by the printer (its records request), with
                                              this tree's map
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
                          history then live; the matching line, or the last lines seen.
                          Exit 2 timeout, 3 core halted, 5 lines dropped for this client (it
                          read too slowly: the line may have been among them)
  peek ... --until 'changed' | '== N' | '!= N' | '< N' | '> N' [--timeout S]
                          the printer watches the value (~1 ms a look) and answers the moment
 act (changes the board)
  flash    B T [--no-build] [--confirm]       build, then download through the printer; its log
                                              session survives.
                                              A RAM_ONLY image needs a printer with --ram_image
                                              (it loads and starts it, no reset; printer start
                                              passes it): any other one is refused
  poke     B T WHERE VALUE [--confirm]        writes one 32-bit word while the core runs and
                                              reads it back: WHERE is an address (0x4005_0004, a
                                              register) or a SYMBOL (needs this tree's build on
                                              the board). Through the printer. Nothing
                                              reads-modify-writes for you: a register
                                              the firmware writes too is the chip's atomic alias
                                              (RP2xxx: +0x2000 sets, +0x3000 clears bits)
  reset    B T [--confirm]                    through the printer; answered when the log is back
                                              (a RAM_ONLY image: loaded again and started)
  panic    B T [--cause NAME] [--confirm]     makes the running firmware call
                                              Kvasir::Panic::raise(cause) (default user; NAME a
                                              cause with dashes, check-failed - the printer lists
                                              them): the printer's raise_panic halts the core for
                                              a moment, sets R0, LR, PC and xPSR (IT/ICI cleared)
                                              and resumes it (the probe's own calls; the log goes
                                              on). The record's site is where the core stood.
                                              Tests the firmware's panic
                                              handler; it ends in whatever that does (a reboot).
                                              The printer reports the requested halt as a tool
                                              message, not as a crash
  snapshot B T [--var SYMBOL]... [--frames N] HALTS a few seconds: registers, backtrace, vars,
                                              through the printer (gdb on a remote stub that reads
                                              through it). For a hang or a fault
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
lines as uc_log's host/uc_log/printer/ControlProtocol.hpp defines them. uc_log's
tools/uc_log_client.py speaks it (use that module for anything this tool does not do), its
test_uc_log_client.py holds it to the golden files in uc_log/doc/control_protocol/.

The code lives in tools/kvasir_bench_lib/ (one module per layer, its __init__ lists them); a frozen copy of the
tool needs this file and that directory, with uc_log/tools, uc_log/remote_fmt/tools and cmake/tools beside them.
"""
import sys
from pathlib import Path

# no __pycache__ next to the imported tools: before the package import
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from kvasir_bench_lib import cli  # noqa: E402


def build_parser():
    return cli.build_parser(__doc__)


def main() -> None:
    cli.main(__doc__)


if __name__ == "__main__":
    main()
