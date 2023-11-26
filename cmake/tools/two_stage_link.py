import sys
import subprocess
import os

cnt = 0
ocnt = 0
rcnt = 0
mcnt = 0
ecnt = 0
hcnt = 0

for a in sys.argv:
    if a.startswith("-o"):
        ocnt = cnt+1
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

process = subprocess.Popen(sys.argv[2:])
process.wait()
if process.returncode != 0:
    os.remove(name + ".step1");
    exit(1)

objdump_res = subprocess.check_output([sys.argv[1], "--format=sysv", sys.argv[ocnt]])
lines = objdump_res.splitlines()

del lines[-1]
del lines[-1]
del lines[0]
del lines[0]

class Section:
    def __init__(self, n, si):
        self.name = n
        self.size = si

sections = []

for l in lines:
    sl = l.split()
    sections.append(Section(sl[0].decode('cp437'), sl[1].decode('cp437')))

size = 0

for s in sections:
    if s.name == ".data":
        size = size + int(s.size)
    if s.name == ".bss":
        size = size + int(s.size)
    if s.name == ".noInit":
        size = size + int(s.size)
    if s.name == ".noInitLowRam":
        size = size + int(s.size)
def parseSize(sie):
    si = sie.split("=")[-1]
    if "k" in si:
        return int(si[:-1])*1024
    return int(si)

ramsize = parseSize(sys.argv[rcnt])
heapsize = parseSize(sys.argv[hcnt])
minstacksize = parseSize(sys.argv[mcnt])

stack_size_extra = (ramsize - (size+minstacksize+heapsize+ int(ramsize/256)))

earg = sys.argv[ecnt].split("=")

earg[-1] = str(stack_size_extra)

sys.argv[ecnt] = '='.join(earg)
sys.argv[ocnt] = name

process = subprocess.Popen(sys.argv[2:])
process.wait()

os.remove(name + ".step1");
if process.returncode != 0:
    exit(1)
