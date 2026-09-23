#!/usr/bin/env python3
"""Exercise the real release-script probe without a GPU, builder, or host cache.

The large-output fixture shrinks only its own pipe and writes well beyond its
capacity. The legacy early-exit consumer must therefore break the pipe, while
the production probe must drain it. This avoids depending on host scheduling
or which NVIDIA libraries happen to be installed (GitHub issue #70 / PR #72).
"""

import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
import unittest


SCRIPT = Path(__file__).resolve().parent / "build_linux_release_in_apptainer.sh"
LEGACY_PROBE = """find_driver_library() {
    local soname="$1"
    ldconfig -p 2>/dev/null | awk -v name="$soname" '$1 == name { print $NF; exit }'
}
"""


def fixture(mode, soname):
    # A shell producer receives SIGPIPE, rather than Python's BrokenPipeError.
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    if mode == "large":
        import fcntl

        fcntl.fcntl(sys.stdout.fileno(), fcntl.F_SETPIPE_SZ, 4096)
    first = f"{soname} (libc6,x86-64) => /mock/first/{soname}\n".encode()
    second = f"{soname} (libc6,x86-64) => /mock/second/{soname}\n".encode()
    noise = b"libother.so.1 (libc6,x86-64) => /mock/libother.so.1\n"
    if mode == "missing":
        sys.stdout.buffer.write(noise)
        return
    sys.stdout.buffer.write(first)
    sys.stdout.buffer.flush()
    if mode == "producer_failure":
        raise SystemExit(23)
    if mode not in ("large", "duplicates"):
        raise ValueError(f"Unknown fixture mode: {mode}")
    if mode == "large":
        for _ in range(32):
            sys.stdout.buffer.write(noise * 1024)
    sys.stdout.buffer.write(second)
    sys.stdout.buffer.flush()


class DriverProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Extract only the actual function: sourcing the script would start
        # builder verification, configuration, compilation, and packaging.
        match = re.search(
            r"^find_driver_library\(\) \{\n.*?^\}",
            SCRIPT.read_text(),
            re.MULTILINE | re.DOTALL,
        )
        if match is None:
            raise AssertionError("Release script driver probe was not found")
        cls.production_probe = match.group(0)

    def probe(self, mode, soname="libcuda.so.1", function=None):
        producer = shlex.join(
            [sys.executable, str(Path(__file__).resolve()), "--fixture", mode, soname]
        )
        harness = (
            "set -Eeuo pipefail\n"
            "ldconfig() {\n"
            '    if [ "$#" -ne 1 ] || [ "$1" != -p ]; then return 97; fi\n'
            f"    {producer}\n"
            "}\n"
            + (self.production_probe if function is None else function)
            + '\ndriver="$(find_driver_library "$1")"\n'
            + 'printf "driver=%s\\n" "$driver"\n'
            + 'printf "after_probe\\n"\n'
        )
        env = os.environ.copy()
        env.pop("BASH_ENV", None)
        return subprocess.run(
            ["bash", "--noprofile", "--norc", "-c", harness, "driver-probe-test", soname],
            capture_output=True,
            text=True,
            timeout=5,
            env=env,
        )

    def test_large_output_is_drained_for_all_driver_libraries(self):
        for soname in ("libcuda.so.1", "libnvcuvid.so.1", "libnvidia-ml.so.1"):
            with self.subTest(soname=soname):
                result = self.probe("large", soname)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, f"driver=/mock/first/{soname}\nafter_probe\n")

    def test_fixture_reproduces_legacy_sigpipe(self):
        result = self.probe("large", function=LEGACY_PROBE)
        self.assertEqual(result.returncode, 128 + signal.SIGPIPE, result.stderr)
        self.assertEqual(result.stdout, "")  # errexit stops at the assignment.

    def test_duplicate_matches_preserve_first_path(self):
        result = self.probe("duplicates")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "driver=/mock/first/libcuda.so.1\nafter_probe\n")

    def test_missing_match_remains_empty_for_callers_readability_check(self):
        result = self.probe("missing")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "driver=\nafter_probe\n")

    def test_genuine_producer_failure_still_aborts_assignment(self):
        result = self.probe("producer_failure")
        self.assertEqual(result.returncode, 23, result.stderr)
        self.assertEqual(result.stdout, "")

    def test_release_script_syntax(self):
        result = subprocess.run(
            ["bash", "-n", str(SCRIPT)], capture_output=True, text=True, timeout=5
        )
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "--fixture":
        fixture(sys.argv[2], sys.argv[3])
    else:
        unittest.main()
