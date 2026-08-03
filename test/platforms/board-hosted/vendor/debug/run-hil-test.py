#!/usr/bin/env python3
"""
f411ce-dynloader HIL (Hardware-In-the-Loop) test runner.

Flashes firmware to WeAct BlackPill STM32F411CE over OpenOCD (CMSIS-DAP),
copies test .so files to /tmp/ for semihosting access, runs the firmware,
captures semihosting trace output, and reports PASS/FAIL.

Usage:
  # Flash + test with OpenOCD (CMSIS-DAP, default):
  python3 debug/run-hil-test.py

  # After building, copy .sos and test:
  make firmware && make libs && python3 debug/run-hil-test.py
"""

import subprocess
import sys
import os
import time
import shutil

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class HilTest:
    """Hardware-In-the-Loop test for f411ce-dynloader."""

    def __init__(self, firmware=None, so_dir=None, so_files=None,
                 openocd_cfg=None, cmdline=None):
        self.firmware = firmware or os.path.join(
            PROJECT_ROOT, "build", "firmware", "f411ce-dynloader.elf")
        self.so_dir = so_dir or os.path.join(PROJECT_ROOT, "build", "libs")
        self.so_files = so_files or ["mylib1.so", "mylib2.so", "libcppsh.so"]
        self.cmdline = cmdline
        self.openocd_cfg = openocd_cfg or os.path.join(
            PROJECT_ROOT, "..", "board", "openocd-weact.cfg")
        self.output = []

    def _check(self):
        errors = []
        if not os.path.exists(self.firmware):
            errors.append(f"Firmware not found: {self.firmware}")
        for so in self.so_files:
            if not os.path.exists(os.path.join(self.so_dir, so)):
                errors.append(f"Library not found: build/libs/{so}")
        if not shutil.which("openocd"):
            errors.append("openocd not found in PATH")
        if errors:
            for e in errors:
                print(f"  MISSING: {e}")
                print(f"  Run: make firmware && make libs")
            return False
        return True

    def _copy_sos_to_tmp(self):
        """Copy .so files to /tmp/ for semihosting access."""
        for so in self.so_files:
            src = os.path.join(self.so_dir, so)
            dst = os.path.join("/tmp", so)
            if os.path.exists(src):
                shutil.copy2(src, dst)
                print(f"  cp {src} -> {dst}")
        print("  .so files ready at /tmp/")

    def _run_openocd(self):
        scripts = os.environ.get("OPENOCD_SCRIPTS",
            "/home/dan/.local/xPacks/@xpack-dev-tools/openocd/0.12.0-7.1/.content/openocd/scripts")

        cmd = [
            "openocd", "-s", scripts, "-f", self.openocd_cfg,
            "-c", "init",
            "-c", "reset halt",
            "-c", f"program {self.firmware}",
            "-c", "arm semihosting enable",
        ]
        if self.cmdline:
            cmd.append(f"-c arm semihosting_cmdline {self.cmdline}")
        cmd += [
            "-c", "reset run",
            "-c", "sleep 30000",
            "-c", "shutdown",
        ]

        print(f"  Running: {' '.join(cmd)}\n")
        proc = subprocess.Popen(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, bufsize=1)
        for line in proc.stdout:
            self.output.append(line.rstrip())
            print(f"  {line}", end="")
        proc.wait(timeout=120)
        return proc.returncode

    def run(self):
        print(f"\n{'='*60}")
        print(f"f411ce-dynloader HIL Test")
        print(f"{'='*60}\n")

        if not self._check():
            return 1

        self._copy_sos_to_tmp()
        print()

        print("  Flashing + running firmware...\n")
        ret = self._run_openocd()

        print(f"\n{'='*60}")
        output_text = "\n".join(self.output)
        if "ALL OK" in output_text:
            print("RESULT: PASS — ALL OK")
            return 0
        elif "FAIL" in output_text:
            print("RESULT: FAIL — Test reported failure")
            return 1
        else:
            print(f"RESULT: UNKNOWN (openocd exit code {ret})")
            print("Check output above or run manually to diagnose.")
            return 2


def main():
    import argparse
    p = argparse.ArgumentParser(description="HIL test for f411ce-dynloader")
    p.add_argument("--firmware", help="Path to firmware .elf")
    p.add_argument("--so-dir", help="Directory with .so files")
    p.add_argument("--so-files", nargs="*",
                   default=["mylib1.so", "mylib2.so", "libcppsh.so"],
                   help=".so files to copy (default: all three)")
    p.add_argument("--openocd-cfg", help="OpenOCD config file path")
    p.add_argument("--cmdline", help="Semihosting command line (argv)")
    args = p.parse_args()

    t = HilTest(firmware=args.firmware, so_dir=args.so_dir,
                so_files=args.so_files,
                openocd_cfg=args.openocd_cfg,
                cmdline=args.cmdline)
    return t.run()


if __name__ == "__main__":
    sys.exit(main())
