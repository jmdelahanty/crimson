#!/usr/bin/env python3
"""Test actual builder preflight blocks and release lock gates using fixtures.

No image is built or modified. Container preflights execute against a temporary
file tree; hash-gate tests use a tiny fake SIF and an explicit Apptainer stub.
"""

import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
RELEASE = ROOT / "tools/build_linux_release_in_apptainer.sh"
BUILDER = ROOT / "tools/build_linux_apptainer_builder.sh"
DEFINITION = ROOT / "packaging/linux/ubuntu22-cuda12.4-trt10/CrimsonLinuxBuilder.def"
HEADERS = ("NvInfer.h", "NvInferVersion.h")


def preflight_blocks():
    blocks = {}
    for script in (RELEASE, BUILDER):
        match = re.search(
            r"^apptainer exec --cleanenv .* /bin/bash -lc '\n(.*?)^'",
            script.read_text(), re.MULTILINE | re.DOTALL,
        )
        if match is None:
            raise AssertionError(f"Container preflight not found in {script}")
        blocks[script.name] = match.group(1)
    match = re.search(
        r"^%test\s*\n(.*?)(?=^%|\Z)",
        DEFINITION.read_text(), re.MULTILINE | re.DOTALL,
    )
    if match is None:
        raise AssertionError("Builder definition %test not found")
    blocks[DEFINITION.name] = match.group(1)
    return blocks


class BuilderHeaderTests(unittest.TestCase):
    def run_preflight(self, block, header=None, condition=None):
        # No spaces: substitute the recipe's fixed unquoted /opt paths verbatim.
        with tempfile.TemporaryDirectory(prefix="crimson-builder-test-", dir="/tmp") as folder:
            root = Path(folder)
            paths = [
                "cmake-3.30.5-linux-x86_64/bin/cmake",
                "opencv-4.10.0-jammy/lib/cmake/opencv4/OpenCVConfig.cmake",
                "tensorrt-10.0.1.6/lib/libnvinfer.so",
                "ffmpeg-jammy/lib/libavcodec.so",
            ] + [f"tensorrt-10.0.1.6/include/{name}" for name in HEADERS]
            for relative in paths:
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("fixture\n")
                path.chmod(0o700)
            if header is not None:
                path = root / "tensorrt-10.0.1.6/include" / header
                path.unlink()
                if condition == "directory":
                    path.mkdir()
                elif condition == "broken_symlink":
                    path.symlink_to(root / "missing-header")
                elif condition == "valid_symlink":
                    target = root / "valid-header"
                    target.write_text("fixture\n")
                    path.symlink_to(target)
                elif condition == "unreadable":
                    path.write_text("fixture\n")
                    path.chmod(0)
                elif condition != "missing":
                    raise ValueError(condition)
            harness = (
                'getconf() { [ "$1" = GNU_LIBC_VERSION ] || return 97; '
                'printf "glibc 2.35\\n"; }\n'
                + block.replace("/opt/crimson", str(root))
            )
            return subprocess.run(
                ["sh", "-c", harness], capture_output=True, text=True, timeout=5
            )

    def test_complete_headers_pass_all_three_entry_points(self):
        for name, block in preflight_blocks().items():
            with self.subTest(entry_point=name):
                result = self.run_preflight(block)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_missing_headers_fail_with_the_specific_path(self):
        for name, block in preflight_blocks().items():
            for header in HEADERS:
                with self.subTest(entry_point=name, header=header):
                    result = self.run_preflight(block, header, "missing")
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("TensorRT development header missing or unreadable:", result.stderr)
                    self.assertIn(f"/include/{header}", result.stderr)

    def test_directories_and_broken_symlinks_are_not_headers(self):
        for name, block in preflight_blocks().items():
            for header in HEADERS:
                for condition in ("directory", "broken_symlink"):
                    with self.subTest(entry_point=name, header=header, condition=condition):
                        result = self.run_preflight(block, header, condition)
                        self.assertNotEqual(result.returncode, 0)
                        self.assertIn(f"/include/{header}", result.stderr)

    def test_valid_header_symlinks_pass(self):
        for name, block in preflight_blocks().items():
            for header in HEADERS:
                with self.subTest(entry_point=name, header=header):
                    result = self.run_preflight(block, header, "valid_symlink")
                    self.assertEqual(result.returncode, 0, result.stderr)

    @unittest.skipIf(os.geteuid() == 0, "root may read mode-000 fixtures")
    def test_unreadable_headers_fail(self):
        for name, block in preflight_blocks().items():
            for header in HEADERS:
                with self.subTest(entry_point=name, header=header):
                    result = self.run_preflight(block, header, "unreadable")
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(f"/include/{header}", result.stderr)

    def test_script_syntax(self):
        for script in (RELEASE, BUILDER):
            with self.subTest(script=script.name):
                result = subprocess.run(
                    ["bash", "-n", str(script)], capture_output=True, text=True, timeout=5
                )
                self.assertEqual(result.returncode, 0, result.stderr)


class BuilderLockTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory(prefix="crimson-builder-lock-")
        self.addCleanup(self.folder.cleanup)
        self.root = Path(self.folder.name)
        self.image = self.root / "candidate.sif"
        self.image.write_bytes(b"test fixture, not an executable image\n")
        self.digest = hashlib.sha256(self.image.read_bytes()).hexdigest()
        self.lock = self.root / "local.lock.json"
        self.marker = self.root / "apptainer-called"
        self.sidecar = self.root / "candidate.sif.sha256"
        binaries = self.root / "bin"
        binaries.mkdir()
        stub = binaries / "apptainer"
        stub.write_text('#!/bin/sh\nprintf "called\\n" > "$CRIMSON_TEST_APPTAINER_MARKER"\n')
        stub.chmod(0o700)
        self.env = os.environ.copy()
        self.env.pop("BASH_ENV", None)
        self.env["PATH"] = str(binaries) + os.pathsep + self.env["PATH"]
        self.env["CRIMSON_TEST_APPTAINER_MARKER"] = str(self.marker)

    def verify(self, digest):
        self.lock.write_text(json.dumps({"builder_sif_sha256": digest}, indent=2))
        return subprocess.run(
            ["bash", str(RELEASE), "--builder", str(self.image),
             "--builder-lock", str(self.lock), "--verify-builder-only"],
            capture_output=True, text=True, timeout=5, env=self.env,
        )

    def test_explicit_local_lock_still_requires_matching_bytes(self):
        result = self.verify("0" * 64)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not match the selected lock", result.stderr)
        self.assertIn(str(self.lock), result.stderr)
        self.assertFalse(self.marker.exists())

    def test_matching_local_lock_reaches_container_preflight(self):
        result = self.verify(self.digest)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("builder checksum is missing", result.stderr)
        self.assertTrue(self.marker.exists())

    def test_matching_portable_sidecar_and_lock_pass(self):
        self.sidecar.write_text(f"{self.digest}  {self.image.name}\n")
        result = self.verify(self.digest)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(self.marker.exists())

    def test_bad_sidecar_is_not_bypassed_by_matching_lock(self):
        self.sidecar.write_text(f"{'0' * 64}  {self.image.name}\n")
        result = self.verify(self.digest)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.marker.exists())

    def test_matching_sidecar_does_not_bypass_mismatched_lock(self):
        self.sidecar.write_text(f"{self.digest}  {self.image.name}\n")
        result = self.verify("0" * 64)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not match the selected lock", result.stderr)
        self.assertFalse(self.marker.exists())

    def test_missing_lock_digest_fails_before_container_preflight(self):
        result = self.verify("")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Builder SHA-256 is missing", result.stderr)
        self.assertFalse(self.marker.exists())


if __name__ == "__main__":
    unittest.main()
