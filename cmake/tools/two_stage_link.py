import sys
import subprocess
import os
from linker_utils import get_flash_address_range, parse_size

cnt = 0
ocnt = 0
rcnt = 0
mcnt = 0
ecnt = 0
hcnt = 0
linker_script = None

for a in sys.argv:
    if a.startswith("-o"):
        ocnt = cnt+1
    if a.startswith("--script="):
        linker_script = a.split("=", 1)[1]
    if a == "-T":
        # Also support -T format
        if cnt+1 < len(sys.argv):
            linker_script = sys.argv[cnt+1]
    if "--defsym=cmake_ram_size=" in a:
        rcnt = cnt
    if "--defsym=cmake_min_stack_size=" in a:
        mcnt = cnt
    if "--defsym=cmake_stack_size_extra=" in a:
        ecnt = cnt
    if "--defsym=cmake_heap_size=" in a:
        hcnt = cnt
    cnt = cnt+1

name = sys.argv[ocnt]
sys.argv[ocnt] = name + ".step1"

# Detect RAM-only configuration by checking if .text is in flash address range
# Get flash address range from linker script
flash_range = None
if linker_script:
    flash_range = get_flash_address_range(linker_script)
else:
    print(f"[two_stage_link.py] WARNING: No linker script found in arguments, assuming flash-based", file=sys.stderr)

process = subprocess.Popen(sys.argv[2:])
process.wait()
if process.returncode != 0:
    os.remove(name + ".step1")
    exit(1)

objdump_res = subprocess.check_output(
    [sys.argv[1], "--format=sysv", sys.argv[ocnt]])
lines = objdump_res.splitlines()

del lines[-1]
del lines[-1]
del lines[0]
del lines[0]


class Section:
    def __init__(self, n, si, addr):
        self.name = n
        self.size = si
        self.addr = addr


sections = []

for l in lines:
    sl = l.split()
    if len(sl) >= 3:
        sections.append(Section(sl[0].decode(
            'cp437'), sl[1].decode('cp437'), sl[2].decode('cp437')))

# Determine if RAM-only by checking if .text is in flash address range
is_ram_only = False
text_addr = None

for s in sections:
    if s.name == ".text":
        # Parse address - could be hex (0x...) or decimal
        text_addr = int(s.addr, 0)  # base 0 auto-detects 0x prefix

        if flash_range:
            flash_start, flash_end = flash_range
            is_in_flash = (flash_start <= text_addr < flash_end)
            is_ram_only = not is_in_flash
        else:
            # No flash region defined, must be RAM-only
            is_ram_only = True
        break

if text_addr is None:
    print(f"[two_stage_link.py] WARNING: .text section not found, assuming flash-based", file=sys.stderr)
    is_ram_only = False

size = 0

for s in sections:
    if s.name == ".text" and is_ram_only:
        # In RAM-only config, .text is in RAM and must be counted
        size = size + int(s.size)
    if s.name == ".data":
        size = size + int(s.size)
    if s.name == ".bss":
        size = size + int(s.size)
    if s.name == ".noInit":
        size = size + int(s.size)
    if s.name == ".noInitLowRam":
        size = size + int(s.size)


def parseSize(sie):
    """Parse size from command line argument (format: --defsym=name=value)"""
    si = sie.split("=")[-1]
    return parse_size(si)


ramsize = parseSize(sys.argv[rcnt])
heapsize = parseSize(sys.argv[hcnt])
minstacksize = parseSize(sys.argv[mcnt])

stack_size_extra = (ramsize - (size+minstacksize+heapsize + int(ramsize/256)))

earg = sys.argv[ecnt].split("=")

earg[-1] = str(stack_size_extra)

sys.argv[ecnt] = '='.join(earg)
sys.argv[ocnt] = name

process = subprocess.Popen(sys.argv[2:])
process.wait()

os.remove(name + ".step1")
if process.returncode != 0:
    exit(1)
