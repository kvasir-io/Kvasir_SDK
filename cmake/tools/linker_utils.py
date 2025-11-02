"""
Shared utilities for linker script parsing and memory analysis.
Used by two_stage_link.py and pretty_size.py.
"""

import re
import sys


def remove_comments(text):
    """Remove C-style comments from text.

    Handles both /* ... */ block comments and // line comments.

    Args:
        text: String potentially containing comments

    Returns:
        String with comments removed
    """
    # Remove block comments /* ... */
    # Use non-greedy match and DOTALL to handle multiline comments
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.DOTALL)

    # Remove line comments //
    # Match from // to end of line
    text = re.sub(r'//.*?$', '', text, flags=re.MULTILINE)

    return text


def get_flash_address_range(linker_script_path):
    """Parse MEMORY block to get flash region's address range.

    Returns:
        tuple: (flash_start, flash_end) or None if no flash region found
    """
    try:
        with open(linker_script_path, 'r') as f:
            content = f.read()

        # Remove comments before parsing
        content = remove_comments(content)

        # Find MEMORY block
        memory_block_pattern = r'MEMORY\s*\{([^}]+)\}'
        memory_match = re.search(memory_block_pattern,
                                 content, re.DOTALL | re.IGNORECASE)

        if not memory_match:
            return None

        memory_content = memory_match.group(1)

        # Parse flash region: flash (rx) : ORIGIN = 0x10000000, LENGTH = 4M
        flash_pattern = r'flash\s*\([^)]*\)\s*:\s*ORIGIN\s*=\s*([x0-9A-Fa-f]+)\s*,\s*LENGTH\s*=\s*([x0-9A-FKMa-fkm]+)'
        flash_match = re.search(flash_pattern, memory_content, re.IGNORECASE)

        if not flash_match:
            return None

        origin_str = flash_match.group(1)
        length_str = flash_match.group(2)

        # Parse origin (e.g., "0x10000000")
        origin = int(origin_str, 16) if origin_str.startswith(
            '0x') else int(origin_str)

        # Parse length (e.g., "4M", "0x400000")
        length = parse_size(length_str)

        flash_start = origin
        flash_end = origin + length

        return (flash_start, flash_end)

    except Exception as e:
        print(f"Error parsing flash region: {e}", file=sys.stderr)
        return None


def parse_size(size_string):
    """Convert size string with suffixes (k/K, M) to bytes.

    Args:
        size_string: String like "4M", "520K", "0x100000", or "1024"

    Returns:
        int: Size in bytes
    """
    if not size_string:
        return 0

    size_string = str(size_string)

    # Handle hex format
    if size_string.startswith('0x') or size_string.startswith('0X'):
        return int(size_string, 16)

    # Extract numeric part
    match = re.search(r'^\d+', size_string)
    if not match:
        raise ValueError(f"Invalid size format: {size_string}")

    match_end = match.end()
    int_size = int(size_string[0:match_end])
    extension_part = size_string[match_end:].upper()

    if 'K' in extension_part:
        return int_size * 1024
    if 'M' in extension_part:
        return int_size * 1024 * 1024

    return int_size


class MemoryRegion:
    """Represents a memory region from the MEMORY block in linker script."""

    def __init__(self, name, origin, length):
        self.name = name
        self.origin = origin
        self.length = length
        self.end = origin + length

    def contains(self, address):
        """Check if an address falls within this memory region."""
        return self.origin <= address < self.end

    def __repr__(self):
        return f"MemoryRegion({self.name}, 0x{self.origin:08x}-0x{self.end:08x})"


def get_memory_regions(linker_script_path):
    """Parse MEMORY block to get all memory regions.

    Returns:
        list: List of MemoryRegion objects, or empty list if parsing fails
    """
    try:
        with open(linker_script_path, 'r') as f:
            content = f.read()

        # Remove comments before parsing
        content = remove_comments(content)

        # Find MEMORY block
        memory_block_pattern = r'MEMORY\s*\{([^}]+)\}'
        memory_match = re.search(memory_block_pattern,
                                 content, re.DOTALL | re.IGNORECASE)

        if not memory_match:
            return []

        memory_content = memory_match.group(1)

        # Parse all memory regions: name (flags) : ORIGIN = 0x..., LENGTH = ...
        # Example: flash (rx) : ORIGIN = 0x10000000, LENGTH = 4M
        #          ram (rwx)  : ORIGIN = 0x20000000, LENGTH = 520K
        region_pattern = r'(\w+)\s*\([^)]*\)\s*:\s*ORIGIN\s*=\s*([x0-9A-Fa-f]+)\s*,\s*LENGTH\s*=\s*([x0-9A-FKMa-fkm]+)'

        regions = []
        for match in re.finditer(region_pattern, memory_content, re.IGNORECASE):
            name = match.group(1).lower()
            origin_str = match.group(2)
            length_str = match.group(3)

            # Parse origin
            origin = int(origin_str, 16) if origin_str.startswith('0x') else int(origin_str)

            # Parse length
            length = parse_size(length_str)

            regions.append(MemoryRegion(name, origin, length))

        return regions

    except Exception as e:
        print(f"Error parsing memory regions: {e}", file=sys.stderr)
        return []


def find_region_for_address(regions, address):
    """Find which memory region contains the given address.

    Args:
        regions: List of MemoryRegion objects
        address: Address to look up

    Returns:
        MemoryRegion: The region containing the address, or None if not found
    """
    for region in regions:
        if region.contains(address):
            return region
    return None
