#!/usr/bin/env python3
"""
Memory Usage Analysis Tool for Embedded Systems
Analyzes binary files and generates formatted memory usage reports
"""

import subprocess
import sys
import re
from typing import List, Union, Dict
from linker_utils import get_memory_regions, find_region_for_address, parse_size as parse_size_util


class MemoryRegionUsage:
    """Represents memory region usage for display."""

    def __init__(self, name: str, size: int) -> None:
        self.name = name
        self.size = size
        self.used = 0  # Will be accumulated from sections


class PrintRecord:
    """Represents a formatted record for memory usage display."""

    def __init__(self, name: str, used: Union[int, str], size: Union[int, str], perc: Union[float, str]) -> None:
        self.name = name
        self.used = used
        self.size = size
        self.perc = perc


class Section:
    """Represents a binary section with its name, size, and address."""

    def __init__(self, name: str, size: str, addr: str = None) -> None:
        self.name = name
        self.size = size
        self.addr = addr


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

    size_tool = sys.argv[1]
    binary = sys.argv[2]
    linker_file = sys.argv[6]

    # Parse memory regions from linker script
    memory_regions = get_memory_regions(linker_file)
    if not memory_regions:
        print(
            f"Warning: Could not parse memory regions from linker file '{linker_file}'", file=sys.stderr)

    # Create usage tracking for each memory region
    region_usage: Dict[str, MemoryRegionUsage] = {}
    for region in memory_regions:
        region_usage[region.name] = MemoryRegionUsage(
            region.name, region.length)

    # Get section information from binary
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

    # Parse sections with addresses
    sections: List[Section] = []
    for line in lines:
        try:
            sl = line.split()
            if len(sl) >= 3:
                section_name = sl[0].decode('cp437')
                section_size = sl[1].decode('cp437')
                section_addr = sl[2].decode('cp437')
                sections.append(
                    Section(section_name, section_size, section_addr))
        except (UnicodeDecodeError, IndexError) as e:
            print(f"Error parsing section line: {e}", file=sys.stderr)
            continue

    # For each section, determine which memory region it belongs to
    # Skip .stack and .heap as they're allocated space, not "used"
    for section in sections:
        if section.name in [".stack", ".heap"]:
            continue

        # Parse section address
        try:
            addr = int(section.addr, 0)  # base 0 auto-detects 0x prefix
        except (ValueError, AttributeError):
            # Skip sections without valid addresses
            continue

        # Find which memory region contains this section
        region = find_region_for_address(memory_regions, addr)

        if region and region.name in region_usage:
            try:
                size = int(section.size)
                region_usage[region.name].used += size
            except ValueError:
                print(
                    f"Warning: Could not parse section size '{section.size}' for section '{section.name}'", file=sys.stderr)

    # Create print records only for regions that exist
    print_records: List[PrintRecord] = []
    for usage in region_usage.values():
        print_records.append(PrintRecord(
            usage.name, usage.used, usage.size, 0))

    if not print_records:
        print("No memory regions found", file=sys.stderr)
        return

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
