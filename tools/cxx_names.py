"""Short forms of demangled C++ names: one algorithm for the Kvasir tools that show or bucket one by name (kvasir_bench's
crash, stack, peek, snapshot and profile; size_report's buckets). Stdlib only, plus remote_fmt's operator table, so
gdb's embedded Python imports it too. Not the post-link listing: cmake/tools/beautify_lst.py shortens whole objdump
lines by its own rules (a name per line shape, the register applies by peripheral), and the listing would change."""
import sys
from pathlib import Path

sys.dont_write_bytecode = True  # no __pycache__ next to the imported tools
_RF = Path(__file__).resolve().parents[1] / "uc_log" / "remote_fmt" / "tools"
if str(_RF) not in sys.path:
    sys.path.insert(0, str(_RF))
from extract_sites import operator_end  # noqa: E402  the llvm-cxxfilt operator spellings live there


def strip_templates(name: str, marker: str = "<>", params: str = "()") -> str:
    """`a::B<c<int>, 3>::f(d<e>, int) const` -> `a::B<>::f() const`: each outermost template argument list becomes
    `marker`, each parameter list `params` ("" drops them). Operators, `(anonymous namespace)`, lambda names
    (`{lambda(int)#1}`, `'lambda'`) and quoted names stay; `[abi:..]` goes; `A<(3 > 2)>` is one list."""
    out, depth, parens, i = [], 0, 0, 0
    while i < len(name):
        c = name[i]
        keep = None
        if name.startswith("[abi:", i) and "]" in name[i:i + 64]:
            i = name.index("]", i) + 1
            continue
        if name.startswith("(anonymous namespace)", i):
            keep = i + len("(anonymous namespace)")
        elif name.startswith(("{lambda", "{unnamed"), i) and "}" in name[i:]:
            keep = name.index("}", i) + 1
        elif c == "'" and "'" in name[i + 1:]:
            keep = name.index("'", i + 1) + 1
        elif (end := operator_end(name, i)) is not None:
            keep = end
        if keep is not None:
            if depth == 0:
                out.append(name[i:keep])
            i = keep
            continue
        # inside a template argument list: `A<(3 > 2)>` has a `>` in parentheses
        if depth:
            parens += c == "("
            parens -= c == ")" and parens > 0
            depth += c == "<" and not parens
            depth -= c == ">" and not parens
        elif c == "<":
            depth = 1
            out.append(marker)
        elif c == "(":   # a parameter list
            level, j = 0, i
            while j < len(name):
                level += name[j] == "("
                level -= name[j] == ")"
                j += 1
                if level == 0:
                    break
            out.append(params)
            i = j
            continue
        else:
            out.append(c)
        i += 1
    return "".join(out)


def display_name(name: str, width: int = 90) -> str:
    """What crash, stack, peek, snapshot (width 90) and profile (width 120) print: `a::B<>::f() const`."""
    return strip_templates(name)[:width]


def plain_name(name: str) -> str:
    """size_report's bucket key: no template arguments, no parameters, no return type.
    `void a::B<c<int>, 3>::f<x>(d<e>, int) const` -> `a::B::f`; `(anonymous namespace)`, operators and lambdas
    stay."""
    name = strip_templates(name, marker="", params="\x00").split(
        "\x00", 1)[0].strip()
    # its space is no return type's
    name = name.replace("(anonymous namespace)", "(anonymous\x01namespace)")
    if " " in name and not name.startswith("operator"):
        # a return type in front (`void f`), but not `operator new` / `operator unsigned int`
        head, _, tail = name.rpartition(" ")
        if not head.endswith("operator"):
            name = tail
    return name.replace("\x01", " ")
