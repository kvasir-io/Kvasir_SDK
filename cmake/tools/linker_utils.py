"""
Shared utilities for linker script parsing and memory analysis.
Used by two_stage_link.py and pretty_size.py.
"""

import re
import sys


def get_flash_address_range(linker_script_path):
    """Parse MEMORY block to get flash region's address range.

    Returns:
        tuple: (flash_start, flash_end) or None if no flash region found
    """
    try:
        with open(linker_script_path, 'r') as f:
            content = f.read()

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
