"""A peripheral's registers from the tree's chip.svd (for the chip plugins)."""
import re
from pathlib import Path

from . import die
from .trees import Tree


def svd_registers(tree: Tree, peripheral: str) -> tuple[int, dict[str, tuple[int, dict[str, tuple[int, int]]]]]:
    """A peripheral's base address and registers from chip.svd: name -> (offset, field -> (lsb, width))."""
    import xml.etree.ElementTree as ET
    chip_root = tree.chip_root
    svd = Path(chip_root, "chip.svd") if chip_root else None
    if svd is None or not svd.is_file():
        die(
            f"no chip.svd under the tree's CHIP_ROOT ({chip_root or 'not set'})")
    for p in ET.parse(svd).getroot().iter("peripheral"):
        if p.findtext("name") != peripheral:
            continue
        regs = {}
        for r in p.iter("register"):
            fields = {}
            for f in r.iter("field"):
                m = re.fullmatch(r"\[(\d+):(\d+)\]",
                                 f.findtext("bitRange") or "")
                if m:
                    fields[f.findtext("name")] = (
                        int(m.group(2)), int(m.group(1)) - int(m.group(2)) + 1)
                elif f.findtext("bitOffset") is not None:
                    fields[f.findtext("name")] = (int(f.findtext("bitOffset"), 0),
                                                  int(f.findtext("bitWidth"), 0))
            regs[r.findtext("name")] = (
                int(r.findtext("addressOffset"), 0), fields)
        return int(p.findtext("baseAddress"), 0), regs
    die(f"{svd} has no peripheral {peripheral}")
