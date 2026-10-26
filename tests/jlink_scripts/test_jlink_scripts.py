#!/usr/bin/env python3
"""cmake/jlink.cmake's generated J-Link Commander scripts: configures tests/jlink_scripts/project (two images through
target_add_flash_jlink) for each probe form (USB serial, USB nickname with a space, bare USB, IP), with and without a
chip's CONNECT_COMMANDS, and as RAM_ONLY images on an Armv6-M and an Armv8-M core, and compares <t>_flash.jlink,
<t>_flash.jlink.in, reset.jlink and connect.jlink with the expected text:

  - a download and a reset first run the chip's connect commands, then reset and halt (`r`, `h`): the flash, RAM-only
    and reset scripts are pinned byte for byte;
  - connect.jlink (`just connect`) only attaches: the probe line and `connect`, no chip command, no reset, no halt.

Configure only: -Djlinkexe=<this python> stands in for JLinkExe, which CI does not have. Needs cmake.

    python3 tests/jlink_scripts/test_jlink_scripts.py -v
"""

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SDK = HERE.parent.parent
RP_LIKE = ["w4 0x40012004 0x00010000", "w4 0x4005801c 0x00000000"]


def nvic_clear(words):
    return "".join(f"w4 {0xE000E180 + 4 * n:#x} 0xFFFFFFFF\nw4 {0xE000E280 + 4 * n:#x} 0xFFFFFFFF\n"
                   for n in range(words))


@unittest.skipUnless(shutil.which("cmake"), "needs cmake")
class JlinkScripts(unittest.TestCase):
    def configure(self, **cache):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        build = Path(tmp.name)
        args = ["cmake", "-S", str(HERE / "project"), "-B", str(build), f"-DKVASIR_SDK_DIR={SDK}",
                f"-Djlinkexe={sys.executable}", "-DTEST_MPU=TEST_DEVICE"]
        if shutil.which("ninja"):
            args += ["-G", "Ninja"]
        args += [f"-D{k}={v}" for (k, v) in cache.items()]
        result = subprocess.run(args, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("CMake Warning", result.stderr)
        return build

    @staticmethod
    def read(build, name):
        return (build / name).read_text()

    def check(self, build, open_, extra=()):
        prepare = open_ + "".join(f"{c}\n" for c in extra) + "r\nh\n"
        for image in ("img_a", "img_b"):
            self.assertEqual(self.read(build, f"{image}_flash.jlink"),
                             f"{prepare}loadfile {image}_flash.hex\nr\nh\nr\ng\nq\n")
        self.assertEqual(self.read(build, "reset.jlink"),
                         f"{prepare}r\ng\nq\n")
        # attach only: nothing on the target changes
        self.assertEqual(self.read(build, "connect.jlink"), open_)
        commands = self.read(build, "build.ninja") if (
            build / "build.ninja").exists() else ""
        if commands:
            self.assertIn("-CommandFile connect.jlink", commands)
            self.assertIn("-CommandFile reset.jlink", commands)

    def test_usb_serial_number(self):
        self.check(self.configure(TEST_JLINK_PROBE="123456789"),
                   "USB 123456789\nconnect\n")

    def test_usb_nickname_with_a_space(self):
        self.check(self.configure(TEST_JLINK_PROBE="My Probe"),
                   'USB "My Probe"\nconnect\n')

    def test_bare_usb(self):
        self.check(self.configure(), "USB\nconnect\n")

    def test_ip(self):
        self.check(self.configure(TEST_JLINK_IP="192.0.2.10"),
                   "IP 192.0.2.10\nconnect\n")

    def test_connect_commands_before_a_reset_only(self):
        self.check(self.configure(TEST_JLINK_PROBE="123456789", TEST_CONNECT_COMMANDS=";".join(RP_LIKE)),
                   "USB 123456789\nconnect\n", RP_LIKE)

    def check_ram_only(self, cpu, words):
        build = self.configure(TEST_JLINK_PROBE="123456789", TEST_CONNECT_COMMANDS=";".join(RP_LIKE),
                               TEST_RAM_ONLY="ON", TARGET_CPU=cpu)
        prepare = "USB 123456789\nconnect\n" + \
            "".join(f"{c}\n" for c in RP_LIKE) + "r\nh\n"
        for image in ("img_a", "img_b"):
            self.assertEqual(self.read(build, f"{image}_flash.jlink.in"),
                             f"{prepare}loadfile {image}_flash.hex\n{nvic_clear(words)}w4 0xE000ED08 {{vector_table}}\n"
                             "wreg MSP {initial_sp}\nwreg XPSR 0x01000000\nSetPC {reset_pc}\ng\nq\n")
        self.assertEqual(self.read(build, "reset.jlink"),
                         f"{prepare}r\ng\nq\n")
        self.assertEqual(self.read(build, "connect.jlink"),
                         "USB 123456789\nconnect\n")

    def test_ram_only_armv6m(self):
        self.check_ram_only("cortex-m0plus", 1)

    def test_ram_only_armv8m(self):
        self.check_ram_only("cortex-m33", 16)


if __name__ == "__main__":
    unittest.main()
