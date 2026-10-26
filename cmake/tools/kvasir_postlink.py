#!/usr/bin/env python3
"""The post-link pipeline of a Kvasir image: every SDK step on the linked ELF, in one fixed order, with one failure policy.
util.cmake (target_configure_kvasir) registers it as the image's one SDK POST_BUILD command.

    kvasir_postlink.py <elf> --linker-file <ld> --objcopy <tool> --objdump <tool>
                       [--image-crc [--exclude START END]...] [--uf2-family <id>] [--delete-on-failure]

The steps, in this order (checks, then the CRC, then the artefacts, then the listing, then the size):
  ram_funcs       check_ram_funcs.py        every RAM function in RAM, nothing it reaches in flash
  reset_entry     check_reset_entry.py      ResetISR touches no stack before its bl into the body
  undefined_refs  find_undefined_refs.py    no undefined or absolute-zero symbol
  image_crc       patch_image_crc.py        with --image-crc: the descriptor written into the ELF (before every
                                            artefact, so each one carries it)
  images          elf_image.py              <name>_flash.{bin,hex,uf2,elf}, _eeprom*, from the load view
  lst             objdump | beautify_lst.py <name>.lst (streamed: no raw listing) of every section the image loads
  size            pretty_size.py            the memory table
The ELF is read and parsed once (kvasir_elf.Elf over a bytearray); each step is the tool's run() on that Elf, in this
process (every tool's main() reads the file itself and stays runnable alone). image_crc patches the buffer and the
file, so the steps after it see the patched bytes. The listing disassembles every section of the image's load view
(kvasir_elf: what the flash artefacts are cut from), read from the ELF: a chip's own section is in it without a name
list here. Not only the executable ones: the RP2040's .boot2 is code in a section without SHF_EXECINSTR, and the vector
table, .data and the scratch banks would leave the listing's section table too. The first step that
fails ends the run with its exit code, after the tool's own message; with --delete-on-failure the ELF is removed then
(whichever step failed), so the next build fails again instead of passing on a stale image. The driver itself prints
nothing on success: what is printed is what the tools print.
"""

import argparse
import io
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True      # no __pycache__ in cmake/tools
sys.path.insert(0, str(Path(__file__).resolve().parent))
import beautify_lst  # noqa: E402
import check_ram_funcs  # noqa: E402
import check_reset_entry  # noqa: E402
import elf_image  # noqa: E402
import find_undefined_refs  # noqa: E402
import patch_image_crc  # noqa: E402
import pretty_size  # noqa: E402
from kvasir_elf import Elf  # noqa: E402


class Failed(Exception):
    def __init__(self, code):
        super().__init__(code)
        self.code = code


def run_tool(call):
    """call() (a tool's run(...)); Failed with its exit code when that is not 0 (None counts as 0, SystemExit as its
    code, sys.exit("text") as that message and exit 1)."""
    try:
        code = call()
    except SystemExit as e:
        code = e.code
    if code not in (None, 0):
        # sys.exit("message"): the message is the tool's own text
        if not isinstance(code, int):
            print(code, file=sys.stderr)
            code = 1
        raise Failed(code)


def read_image(path):
    """The file as one bytearray, read into it (bytearray(read_bytes()) holds the image twice for a moment: a 150 MB
    debug image peaked at 304 MB that way)."""
    with open(path, 'rb') as f:
        data = bytearray(Path(path).stat().st_size)
        if f.readinto(data) != len(data):
            raise OSError(f'{path}: short read')
    return data


def listing_sections(elf):
    """The names the listing disassembles: every section the image loads (Elf.loaded()), each once."""
    return list(dict.fromkeys(name for (name, _address, _data) in elf.loaded()))


def steps(args, elf):
    path = str(args.elf)
    prefix = str(args.elf.with_suffix(''))
    yield 'ram_funcs', lambda: run_tool(lambda: check_ram_funcs.run(elf, args.elf))
    yield 'reset_entry', lambda: run_tool(lambda: check_reset_entry.run(elf, path))
    yield 'undefined_refs', lambda: run_tool(lambda: find_undefined_refs.run(elf, args.elf))
    if args.image_crc:
        yield 'image_crc', lambda: run_tool(lambda: patch_image_crc.run(elf, args.elf, [tuple(p) for p in args.exclude]))
    uf2 = int(args.uf2_family, 0) if args.uf2_family else None
    yield 'images', lambda: run_tool(lambda: elf_image.run(elf, args.elf, prefix, uf2, args.objcopy))

    def lst():
        # objdump's output goes through the shortener as it comes: the raw listing of a big image is tens of GB, held
        # neither in memory nor on disk
        cmd = [args.objdump, *(f'--section={s}' for s in listing_sections(elf)), '--disassemble', '--demangle',
               '--all-headers', path]
        sys.stdout.flush()
        with subprocess.Popen(cmd, stdout=subprocess.PIPE) as objdump, \
                open(f'{prefix}.lst', 'w', encoding='utf-8') as listing:
            beautify_lst.beautify(io.TextIOWrapper(
                objdump.stdout, encoding='utf-8', errors='replace'), listing)
        if objdump.returncode != 0:
            print(
                f'kvasir_postlink: {" ".join(cmd)} exited with {objdump.returncode}', file=sys.stderr)
            raise Failed(objdump.returncode)
    yield 'lst', lst

    yield 'size', lambda: run_tool(lambda: pretty_size.run(elf, path, args.linker_file))


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('elf', type=Path)
    ap.add_argument('--linker-file', required=True)
    ap.add_argument('--objcopy', required=True)
    ap.add_argument('--objdump', required=True)
    ap.add_argument('--image-crc', action='store_true')
    ap.add_argument('--exclude', nargs=2, action='append',
                    default=[], metavar=('START', 'END'))
    ap.add_argument('--uf2-family')
    ap.add_argument('--delete-on-failure', action='store_true')
    args = ap.parse_args(argv)
    try:
        try:
            elf = Elf(read_image(args.elf))
        except (OSError, ValueError) as e:
            print(f'kvasir_postlink: {args.elf}: {e}', file=sys.stderr)
            raise Failed(2) from e
        for (_name, step) in steps(args, elf):
            step()
            sys.stdout.flush()
    except Failed as failure:
        if args.delete_on_failure and args.elf.exists():
            args.elf.unlink()
            print(f'kvasir_postlink: removed {args.elf.name}', file=sys.stderr)
        return failure.code
    return 0


if __name__ == '__main__':
    sys.exit(main())
