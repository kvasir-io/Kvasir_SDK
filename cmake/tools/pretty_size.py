import subprocess
import sys
import re
class MemoryRegion:
    def __init__(self, n, si, se):
        self.name = n
        self.size = si
        self.sections = se

class PrintRecord:
    def __init__(self, n, u, si, p):
        self.name = n
        self.used = u
        self.size = si
        self.perc = p

class Section:
    def __init__(self, n, si):
        self.name = n
        self.size = si

def humanbytes(B):
    B = float(B)
    KB = float(1024)

    if B < KB*64:
        return "{0:.0f}  B".format(B)
    return "{0:.2f} KB".format(B/KB)

def machinebytes(size_string):
    match_end = re.search(r'^\d+', size_string).end()
    int_size = int(size_string[0:match_end])
    extension_part = size_string[match_end:]
    if "K" in extension_part:
        return int_size*1024

    return int_size

memory_regions = []

size_tool = sys.argv[1]
binary = sys.argv[2]
f_size = sys.argv[3]
r_size = sys.argv[4]
e_size = sys.argv[5]
linker_file = sys.argv[6]

#TODO make more resillient
linker_file_content = open(linker_file)
linker_file_content = linker_file_content.readlines()
for l in linker_file_content:
    key = "LENGTH"
    pos = l.find(key)
    if pos != -1:
        try:
          region = l.split()[0]
          list = l[pos+len(key):].split()
          size = machinebytes(list[1])
          if len(list) > 3:
              if list[2] == "-":
                  size = size - machinebytes(list[3])
              if list[2] == "+":
                  size = size + machinebytes(list[3])
          if region == "flash":
              f_size = size
          if region == "ram":
              r_size = size
          if region == "eeprom":
              e_size = size
        except:
            print("bad")

memory_regions.append(MemoryRegion("flash", f_size, [".text", ".data"]))
memory_regions.append(MemoryRegion("ram", r_size, [".data", ".bss", ".heap", ".noInit", ".noInitLowRam"]))
memory_regions.append(MemoryRegion("eeprom", e_size, [".eeprom"]))

objdump_res = subprocess.check_output([size_tool, "--format=sysv", binary])
lines = objdump_res.splitlines()

del lines[-1]
del lines[-1]
del lines[0]
del lines[0]

sections = []
for l in lines:
    sl = l.split()
    sections.append(Section(sl[0].decode('cp437'), sl[1].decode('cp437')))

print_records = []

for mr in memory_regions:
    print_records.append(PrintRecord(mr.name, 0, mr.size, 0))

for s in sections:
    for mr in memory_regions:
        if s.name in mr.sections:
            for pr in print_records:
                if pr.name == mr.name:
                    pr.used += int(s.size)

for pr in print_records:
    if float(pr.size) != 0.0:
        pr.perc = f'{(float(pr.used)/float(pr.size)*100):.2f}' + "%"
    else:
        pr.perc = "0.00%"
    pr.used = humanbytes(pr.used)
    pr.size = humanbytes(pr.size)

size_name = 14
size_used = 14
size_size = 14
size_perc = 14

for pr in print_records:
    size_name = max(size_name, len(pr.name))
    size_used = max(size_used, len(pr.used))
    size_size = max(size_size, len(pr.size))
    size_prec = max(size_perc, len(pr.perc))

fmt_string = "{{:>{}}} {{:>{}}} {{:>{}}} {{:>{}}}".format(size_name, size_used, size_size, size_perc)

print(fmt_string.format("Memory region", "Used Size", "Region Size", "%age Used"))

for pr in print_records:
    print(fmt_string.format(pr.name, pr.used, pr.size, pr.perc))
