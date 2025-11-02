import sys
import subprocess
import os
from linker_utils import get_memory_regions, find_region_for_address, parse_size

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

# Parse memory regions from linker script
memory_regions = []
if linker_script:
    memory_regions = get_memory_regions(linker_script)
    if not memory_regions:
        print(f"[two_stage_link.py] WARNING: Could not parse memory regions from linker script", file=sys.stderr)
else:
    print(f"[two_stage_link.py] WARNING: No linker script found in arguments", file=sys.stderr)

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

# Calculate RAM usage by checking which sections are actually in RAM
# This works for any configuration (flash+RAM, RAM-only, custom layouts)
size = 0

for s in sections:
    # Skip .stack and .heap - these are what we're calculating
    if s.name in [".stack", ".heap"]:
        continue

    # Parse section address
    try:
        addr = int(s.addr, 0)  # base 0 auto-detects 0x prefix
    except (ValueError, AttributeError):
        # Skip sections without valid addresses
        continue

    # Check which memory region this section is in
    region = find_region_for_address(memory_regions, addr)

    # Only count sections that are in RAM
    if region and region.name == "ram":
        section_size = int(s.size)
        size = size + section_size


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
