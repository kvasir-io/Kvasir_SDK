#!/usr/bin/env python3

import io
import sys
import re
from pathlib import Path


def extract_meaningful_name(long_name):
    """Extract the most meaningful part of a C++ template name."""

    if '::apply<' in long_name and 'Kvasir::Register' in long_name:
        periph_match = re.search(r'Kvasir::Peripheral::(\w+)::', long_name)
        if periph_match:
            peripheral = periph_match.group(1)
            reg_match = re.search(r':' + peripheral +
                                  r'[^:]*::(\w+)Val', long_name)
            if reg_match:
                return f"[Kvasir::Register::apply<{peripheral}::{reg_match.group(1)}>]"
            return f"[Kvasir::Register::apply<{peripheral}>]"
        return "[Kvasir::Register::apply<...>]"

    if 'enable_if<' in long_name:
        match = re.search(r'::type\s+(\w+::\w+::\w+)<', long_name)
        if match:
            return f"[{match.group(1)}<...>]"
        match = re.search(r'::type\s+(\w+::\w+)<', long_name)
        if match:
            return f"[{match.group(1)}<...>]"
        return "[enable_if<...>::type]"

    if 'rtt::detail::BufferControlBlock' in long_name:
        match = re.search(
            r'(rtt::detail::BufferControlBlock)<[^>]*>::(\w+)<', long_name)
        if match:
            return f"[{match.group(1)}<...>::{match.group(2)}<...>]"
        return "[rtt::detail::BufferControlBlock<...>]"

    if 'remote_fmt::Printer' in long_name:
        match = re.search(r'(remote_fmt::Printer)<[^>]*>::(\w+)<', long_name)
        if match:
            return f"[{match.group(1)}<...>::{match.group(2)}<...>]"
        return "[remote_fmt::Printer<...>]"

    if 'uc_log::' in long_name and 'ComBackend' in long_name:
        match = re.search(r'(uc_log::\w*ComBackend)<', long_name)
        if match:
            return f"[{match.group(1)}<...>]"

    if 'std::__2::span<' in long_name:
        match = re.search(r'std::__2::span<([^,<>]+)', long_name)
        if match:
            elem_type = match.group(1).strip()
            return f"[std::span<{elem_type}, ...>]"
        return "[std::span<...>]"

    if 'Kvasir::Fault::HandlerFor<' in long_name:
        match = re.search(
            r'(Kvasir::Fault::HandlerFor)<[^>]*>::(\w+)', long_name)
        if match:
            return f"[{match.group(1)}<...>::{match.group(2)}]"
        return "[Kvasir::Fault::HandlerFor<...>]"

    if 'CMakeGitVersion::' in long_name:
        match = re.search(r'(CMakeGitVersion::\w+::\w+)<', long_name)
        if match:
            return f"[{match.group(1)}<...>]"
        match = re.search(r'(CMakeGitVersion::\w+)', long_name)
        if match:
            return f"[{match.group(1)}...]"

    # needs a '::' to match; a name GNU objdump left mangled (thousands of
    # \w, no '::') made it a quadratic scan: minutes for a gcc listing
    match = None
    if '::' in long_name:
        match = re.search(r'((?:\w+::)+\w+)<', long_name)
    if match:
        full_name = match.group(1)
        parts = full_name.split('::')
        if len(parts) > 3:
            return f"[{parts[0]}::...::{'::'.join(parts[-2:])}<...>]"
        return f"[{full_name}<...>]"

    if len(long_name) > 80:
        return long_name[:77] + '...'

    return long_name


def shorten_line(line):
    """Shorten any long C++ names in a line."""
    if len(line) > 120:
        match = re.match(r'^([0-9a-f]+\s+<)(.+)(>:)$', line)
        if match:
            prefix = match.group(1)
            name = match.group(2)
            suffix = match.group(3)
            short_name = extract_meaningful_name(name)
            return prefix + short_name + suffix + '\n'

        match = re.match(
            r'^(\s*[0-9a-f]+:?\s+(?:[0-9a-f]+\s+)*(?:\w+\s+)*(?:F|O)?\s+\S+\s+\S+\s+)(.+)$', line)
        if match:
            prefix = match.group(1)
            name = match.group(2).strip()
            short_name = extract_meaningful_name(name)
            return prefix + short_name + '\n'

        match = re.search(r'<([^>]{200,})>', line)
        if match:
            long_name = match.group(1)
            short_name = extract_meaningful_name(long_name)
            return line.replace('<' + long_name + '>', '<' + short_name + '>')

    return line


def beautify(lines, output):
    """Write each line of `lines` (any iterable of text lines) shortened to the text file `output`, one at a time: the
    memory is the longest line, not the listing (a raw listing of a big image is tens of GB)."""
    for line in lines:
        output.write(shorten_line(line))


def process_lst_file(input_path, output_path):
    """Process and beautify the LST file; `-` reads standard input."""
    with open(output_path, 'w', encoding='utf-8') as output:
        if str(input_path) == '-':
            beautify(io.TextIOWrapper(sys.stdin.buffer,
                     encoding='utf-8', errors='replace'), output)
        else:
            with open(input_path, 'r', encoding='utf-8', errors='replace') as lines:
                beautify(lines, output)


def main():
    """Main entry point."""
    if len(sys.argv) != 3:
        print("Usage: beautify_lst.py <input.lst | -> <output.lst>", file=sys.stderr)
        sys.exit(1)

    input_path = Path(sys.argv[1])
    output_path = Path(sys.argv[2])

    if str(input_path) != '-' and not input_path.is_file():
        print(f"Error: Input file not found: {input_path}", file=sys.stderr)
        sys.exit(1)

    output_path.parent.mkdir(parents=True, exist_ok=True)

    try:
        process_lst_file(input_path, output_path)
        sys.exit(0)
    except Exception as e:
        print(f"Error processing file: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
