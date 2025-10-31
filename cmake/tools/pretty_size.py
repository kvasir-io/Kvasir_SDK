#!/usr/bin/env python3
"""
Memory Usage Analysis Tool for Embedded Systems
Analyzes binary files and generates formatted memory usage reports
"""

import subprocess
import sys
import re
from typing import List, Union
from linker_utils import get_flash_address_range, parse_size as parse_size_util


class MemoryRegion:
    """Represents a memory region with its size and associated sections."""

    def __init__(self, name: str, size: Union[int, str], sections: List[str]) -> None:
        self.name = name
        self.size = size
        self.sections = sections


class PrintRecord:
    """Represents a formatted record for memory usage display."""

    def __init__(self, name: str, used: Union[int, str], size: Union[int, str], perc: Union[float, str]) -> None:
        self.name = name
        self.used = used
        self.size = size
        self.perc = perc


class Section:
    """Represents a binary section with its name and size."""

    def __init__(self, name: str, size: str) -> None:
        self.name = name
        self.size = size


def humanbytes(B: Union[int, float]) -> str:
    """Convert bytes to human-readable format."""
    B = float(B)
    KB = float(1024)
    MB = float(1024 * 1024)

    if B >= MB:
        return "{0:.2f} MB".format(B/MB)
    elif B < KB*64:
        return "{0:.0f}  B".format(B)
    return "{0:.2f} KB".format(B/KB)


def machinebytes(size_string: str) -> int:
    """Convert size string with suffixes (K, M) to bytes."""
    return parse_size_util(size_string)


def main() -> None:
    """Main function to analyze memory usage and generate report."""
    if len(sys.argv) != 7:
        print("Usage: pretty_size.py <size_tool> <binary> <flash_size> <ram_size> <eeprom_size> <linker_file>", file=sys.stderr)
        sys.exit(1)

    memory_regions: List[MemoryRegion] = []

    size_tool = sys.argv[1]
    binary = sys.argv[2]
    f_size = sys.argv[3]
    r_size = sys.argv[4]
    e_size = sys.argv[5]
    linker_file = sys.argv[6]

    # Parse linker file to extract actual memory region sizes and flash address range
    has_flash_region = False
    flash_range = None
    try:
        with open(linker_file, 'r') as f:
            linker_file_content = f.read()

        # Get flash address range using shared utility
        flash_range = get_flash_address_range(linker_file)
        if flash_range:
            has_flash_region = True
            flash_start, flash_end = flash_range
            f_size = flash_end - flash_start

        # Parse memory sizes from lines
        for line in linker_file_content.splitlines():
            key = "LENGTH"
            pos = line.find(key)
            if pos != -1:
                try:
                    tokens = line.split()
                    if len(tokens) < 1:
                        continue
                    region = tokens[0]

                    length_tokens = line[pos+len(key):].split()
                    if len(length_tokens) < 2:
                        continue

                    size = machinebytes(length_tokens[1])
                    if len(length_tokens) > 3:
                        if length_tokens[2] == "-":
                            size = size - machinebytes(length_tokens[3])
                        elif length_tokens[2] == "+":
                            size = size + machinebytes(length_tokens[3])

                    if region == "ram":
                        r_size = size
                    elif region == "eeprom":
                        e_size = size
                except (IndexError, ValueError, AttributeError) as e:
                    print(
                        f"Error parsing line '{line.strip()}': {e}", file=sys.stderr)
    except (IOError, OSError) as e:
        print(
            f"Error reading linker file '{linker_file}': {e}", file=sys.stderr)
        # Continue with provided sizes as fallback

    # Convert string sizes to integers if needed
    try:
        if isinstance(f_size, str):
            f_size = machinebytes(f_size)
        if isinstance(r_size, str):
            r_size = machinebytes(r_size)
        if isinstance(e_size, str):
            e_size = machinebytes(e_size)
    except ValueError as e:
        print(f"Error converting size values: {e}", file=sys.stderr)
        sys.exit(1)

    # Get section information from binary first
    # We need this to determine if .text is actually in flash or RAM
    try:
        objdump_res = subprocess.check_output(
            [size_tool, "--format=sysv", binary], stderr=subprocess.STDOUT)
        lines = objdump_res.splitlines()
    except subprocess.CalledProcessError as e:
        print(f"Error running size tool '{size_tool}': {e}", file=sys.stderr)
        sys.exit(1)
    except FileNotFoundError:
        print(f"Size tool '{size_tool}' not found", file=sys.stderr)
        sys.exit(1)

    # Remove header and footer lines
    if len(lines) >= 4:
        del lines[-1]
        del lines[-1]
        del lines[0]
        del lines[0]
    else:
        print("Unexpected output format from size tool", file=sys.stderr)
        sys.exit(1)

    sections: List[Section] = []
    text_addr = None
    for line in lines:
        try:
            sl = line.split()
            if len(sl) >= 2:
                section_name = sl[0].decode('cp437')
                section_size = sl[1].decode('cp437')
                sections.append(Section(section_name, section_size))

                # Get .text address if available
                if section_name == ".text" and len(sl) >= 3:
                    try:
                        # Parse address - could be hex (0x...) or decimal
                        # base 0 auto-detects 0x prefix
                        text_addr = int(sl[2].decode('cp437'), 0)
                    except (ValueError, UnicodeDecodeError):
                        pass
        except (UnicodeDecodeError, IndexError) as e:
            print(f"Error parsing section line: {e}", file=sys.stderr)
            continue

    # Determine if RAM-only by checking if .text is in flash address range
    is_ram_only = False
    if text_addr is not None and flash_range is not None:
        flash_start, flash_end = flash_range
        is_in_flash = (flash_start <= text_addr < flash_end)
        is_ram_only = not is_in_flash
    elif text_addr is not None and flash_range is None:
        # No flash region defined, must be RAM-only
        is_ram_only = True
    elif not has_flash_region:
        # No flash region at all
        is_ram_only = True

    # Adjust section assignments based on actual configuration
    # Note: .stack and .heap are excluded from "used" calculation (they're allocated space)
    if is_ram_only:
        # RAM-only: everything in RAM, no flash
        memory_regions.append(MemoryRegion("flash", 0, []))
        memory_regions.append(MemoryRegion(
            "ram", r_size, [".text", ".data", ".bss", ".noInit"]))
    else:
        # Flash-based: .text in flash, data copied from flash to RAM
        memory_regions.append(MemoryRegion(
            "flash", f_size, [".text", ".data"]))
        memory_regions.append(MemoryRegion(
            "ram", r_size, [".data", ".bss", ".noInit", ".noInitLowRam"]))
    memory_regions.append(MemoryRegion("eeprom", e_size, [".eeprom"]))

    # Calculate memory usage
    print_records: List[PrintRecord] = []
    for mr in memory_regions:
        print_records.append(PrintRecord(mr.name, 0, mr.size, 0))

    for s in sections:
        for mr in memory_regions:
            if s.name in mr.sections:
                for pr in print_records:
                    if pr.name == mr.name:
                        try:
                            pr.used += int(s.size)
                        except ValueError:
                            print(
                                f"Warning: Could not parse section size '{s.size}' for section '{s.name}'", file=sys.stderr)

    # Format output
    for pr in print_records:
        try:
            if float(pr.size) != 0.0:
                pr.perc = f'{(float(pr.used)/float(pr.size)*100):.2f}' + "%"
            else:
                pr.perc = "0.00%"
            pr.used = humanbytes(pr.used)
            pr.size = humanbytes(pr.size)
        except (ValueError, ZeroDivisionError) as e:
            print(
                f"Error formatting record for {pr.name}: {e}", file=sys.stderr)
            pr.perc = "N/A"

    # Calculate column widths
    size_name = max(14, max(len(pr.name) for pr in print_records))
    size_used = max(14, max(len(str(pr.used)) for pr in print_records))
    size_size = max(14, max(len(str(pr.size)) for pr in print_records))
    size_perc = max(14, max(len(str(pr.perc)) for pr in print_records))

    fmt_string = "{{:>{}}} {{:>{}}} {{:>{}}} {{:>{}}}".format(
        size_name, size_used, size_size, size_perc)

    print(fmt_string.format("Memory region",
          "Used Size", "Region Size", "%age Used"))
    for pr in print_records:
        print(fmt_string.format(pr.name, pr.used, pr.size, pr.perc))


if __name__ == "__main__":
    main()
