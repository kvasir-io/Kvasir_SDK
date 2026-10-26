"""What a build tree knows: its directory, the target's ELF and printer options, the repo's guard file."""
import functools
import json
import os
import re
import zlib
from pathlib import Path

from . import die, note, refuse


def build_dir(given: str) -> Path:
    """The build directory: as given, or a repo's only build tree (`build/`, `build_<board>/`)."""
    path = Path(given).resolve()
    if (path / "CMakeCache.txt").is_file():
        return path
    trees = sorted(p.parent for p in path.glob("build*/CMakeCache.txt"))
    if len(trees) == 1:
        note(f"using {trees[0]}")
        return trees[0]
    if trees:
        die(f"{path} holds several build trees, name one: " +
            ", ".join(str(t.relative_to(path)) for t in trees))
    die(f"{path} is not a configured build directory (no CMakeCache.txt)")


def targets_in(build: Path) -> list[str]:
    return sorted(e.stem for e in build.glob("*.elf") if not e.stem.endswith("_flash"))


class Tree:
    def __init__(self, build: str, target: str):
        self.build = build_dir(build)
        self.target = target
        cache = self.build / "CMakeCache.txt"
        self.cache = cache.read_text(errors="replace")
        self.elf = self.build / f"{target}.elf"
        if not self.elf.is_file():
            there = targets_in(self.build)
            die(f"no target '{target}' built in {self.build}"
                + (f"; built here: {', '.join(there)}" if there else "; build it first"))
        # The environment wins whenever it sets the variable, empty included: `JLINK_IP=` is how
        # one call reaches a USB probe in a tree whose cache names a network J-Link.
        self.probe = self._setting("JLINK_PROBE")
        self.host = self._setting("JLINK_IP")   # a network J-Link
        self.chip_root = self._cached("CHIP_ROOT")
        # kHz; the probe's maximum is 100000, which the DLL clamps (cmake/jlink.cmake)
        self.swd_speed = (self._arg_values("--speed") or ["100000"])[0]
        self.device = os.environ.get("JLINK_DEVICE") or self._device()
        self.log_dir = self.build / "rtt_log" / target
        # source paths are shown relative to the directory that holds the firmware's checkout,
        # where its sibling libraries usually sit too; a path elsewhere stays whole
        source = self._cached("CMAKE_HOME_DIRECTORY")
        self.path_root = str(Path(source).parent).rstrip(
            "/") + "/" if source else ""

    def short_path(self, path: str) -> str:
        root = self.path_root
        return path[len(root):] if root and path.startswith(root) else path

    def _setting(self, name: str) -> str:
        """JLINK_PROBE / JLINK_IP: the environment, else the target's printer options - which carry
        what the target was configured with, its own J-Link (target_configure_kvasir JLINK_IP /
        JLINK_PROBE) or the cache's."""
        if name in os.environ:
            return os.environ[name]
        values = self._arg_values(
            {"JLINK_PROBE": "--probe", "JLINK_IP": "--host"}[name])
        return values[0] if values else ""

    def _cached(self, name: str) -> str:
        m = re.search(rf"^{name}:[A-Za-z]*=(.*)$", self.cache, re.M)
        return m.group(1).strip() if m else ""

    def _device(self) -> str:
        values = self._arg_values("--device")
        if not values:
            die(f"{self.printer_args_file} names no --device; set JLINK_DEVICE")
        return values[0]

    @property
    def printer_args_file(self) -> Path:
        return self.build / f"{self.target}.printer_args"

    def printer_args(self) -> list[str]:
        """What a printer needs to know about the target - device, speed, its files, the probe,
        the chip package's reset commands and signatures, a RAM image, the log filter: the tree's
        `<target>.printer_args` (uc_log's CMakeLists writes it next to the log_<target> command,
        from the same list), one argument per line. The only source: a tree without the file
        is told to run cmake."""
        if not hasattr(self, "_printer_args"):
            if not self.printer_args_file.is_file():
                die(f"no {self.printer_args_file.name}: run cmake {self.build} once")
            self._printer_args = self.printer_args_file.read_text().split("\n")[
                :-1]
        return self._printer_args

    def _arg_values(self, flag: str) -> list[str]:
        """The values of `flag` in the target's printer options."""
        words = self.printer_args()
        return [words[i + 1] for i, w in enumerate(words[:-1]) if w == flag]

    @property
    def map_file(self) -> Path:
        """The linker map the target's printer decodes with (--map_file): the records' symbols are found there."""
        values = self._arg_values("--map_file")
        if not values:
            die(f"{self.printer_args_file} names no --map_file")
        return self.build / values[0]

    def ram_image(self) -> bool:
        """A RAM_ONLY image: a reset boots flash, never it, so the printer has to load and start
        it by hand (uc_log_printer --ram_image, uc_log's RAM_IMAGE)."""
        return "--ram_image" in self.printer_args()

    def printer_command(self, exe: Path) -> list[str]:
        """The printer for this target, as the bench runs it: the tree's options with the probe
        the environment may override, no UI, the log directory, and no build of its own (the
        bench builds before it flashes)."""
        words, i = [], 0
        args = self.printer_args()
        while i < len(args):
            # the bench's own probe choice (self.probe / self.host)
            if args[i] in ("--probe", "--host"):
                i += 2
                continue
            words.append(args[i])
            i += 1
        if self.host:
            words += ["--host", self.host]
        elif self.probe:
            words += ["--probe", self.probe]
        if "JLINK_DEVICE" in os.environ:
            words = [os.environ["JLINK_DEVICE"] if i > 0 and words[i - 1] == "--device" else w
                     for i, w in enumerate(words)]
        return [str(exe), *words, "--build_command", "true", "--log_dir", str(self.log_dir), "--disable_ui"]

    @functools.cached_property
    def image(self) -> "image.Image":
        """The target's ELF, each tool run at most once for the whole command."""
        from . import image
        return image.Image(self.elf)


def guard_file(tree: Tree) -> Path | None:
    """The repo's .kvasir_bench.json: the build dir, its parent or grandparent."""
    for d in (tree.build, tree.build.parent, tree.build.parent.parent):
        if (d / ".kvasir_bench.json").is_file():
            return d / ".kvasir_bench.json"
    return None


def confirm_needed(tree: Tree, action: str, confirmed: bool) -> None:
    """A repo can say that an action does something physical ({"confirm": {"reset": "why"}}):
    then it runs only with --confirm, and the reason is shown either way."""
    path = guard_file(tree)
    if path is None:
        return
    try:
        why = json.loads(path.read_text()).get("confirm", {}).get(action)
    except (OSError, ValueError) as e:
        die(f"{path}: {e}")
    if why is None:
        return
    if not confirmed:
        refuse(f"{action} on {path.parent.name} needs --confirm ({path}): {why}")
    note(f"{action} confirmed: {why}")


def hex_build_id(tree: Tree) -> str:
    """The CRC-32 the printer names a build by: over the hex file's data bytes, in file order."""
    path = tree.build / f"{tree.target}_flash.hex"
    if not path.is_file():
        return "?"
    crc = 0
    for line in path.read_text().splitlines():
        if line.startswith(":") and line[7:9] == "00":
            crc = zlib.crc32(bytes.fromhex(line[9:-2]), crc)
    return f"{crc:08x}"
