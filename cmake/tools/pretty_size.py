#!/usr/bin/env python3
"""
Memory Usage Analysis Tool for Embedded Systems
Analyzes binary files and generates formatted memory usage reports

The sections and their addresses come from the ELF itself (kvasir_elf.Elf), the regions from the linker script's MEMORY
block. Flash rows count every section's load image: a section the image loads (kvasir_elf's load view, what the flash
artefacts are cut from) is booked to the region of its load address, and also to the region of its run address when
that is another one (.data and RAM functions: flash and ram). A NOBITS section counts at its run address only.
"""

from linker_utils import get_memory_regions, find_region_for_address
import struct
import sys
from pathlib import Path
from typing import List, Union, Dict

sys.dont_write_bytecode = True      # no __pycache__ in cmake/tools
sys.path.insert(0, str(Path(__file__).resolve().parent))
from kvasir_elf import Elf, SHF_ALLOC  # noqa: E402


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


def main() -> None:
    """Main function to analyze memory usage and generate report."""
    if len(sys.argv) != 3:
        print("Usage: pretty_size.py <binary> <linker_file>", file=sys.stderr)
        sys.exit(1)

    binary = sys.argv[1]
    try:
        elf = Elf(Path(binary).read_bytes())
    except (OSError, ValueError, struct.error) as e:
        print(f"Error reading '{binary}': {e}", file=sys.stderr)
        sys.exit(1)
    run(elf, binary, sys.argv[2])


def run(elf: Elf, binary: str, linker_file: str) -> None:
    """The memory table of an Elf already read from `binary`, printed."""
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

    try:
        load_address = elf.load_addresses()
    except (ValueError, struct.error) as e:
        print(f"Error reading '{binary}': {e}", file=sys.stderr)
        sys.exit(1)

    # Every allocated section with a size; .stack and .heap are reserved space, not "used"
    for section in elf.sections:
        if not section['flags'] & SHF_ALLOC or section['size'] == 0 or section['name'] in (".stack", ".heap"):
            continue
        load = None
        if section['name'] in load_address:
            load = find_region_for_address(
                memory_regions, load_address[section['name']])
        run = find_region_for_address(memory_regions, section['addr'])
        if load:
            region_usage[load.name].used += section['size']
        if run and run is not load:
            region_usage[run.name].used += section['size']

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
