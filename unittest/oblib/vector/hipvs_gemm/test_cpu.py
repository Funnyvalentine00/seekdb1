#!/usr/bin/env python3
"""CPU regression for the GEMM bridge's tiling and failure cleanup.

Run: python3 unittest/oblib/vector/hipvs_gemm/test_cpu.py
Requires a C++17 compiler (CXX, default g++), not ROCm. Compiles the production
translation unit with HIP launches lowered to serial calls and external GPU
libraries replaced by CPU doubles. This does NOT validate HIP compilation,
WMMA, asynchronous stream ordering, GPU floating-point error, or performance.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]


class GemmRegression(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="hipvs-gemm-")
        cls.binary = Path(cls.tmp.name) / "test_gemm"
        source = (ROOT / "tools/hipvs_bridge/seekdb_bf_gemm.cpp").read_text()
        source = re.sub(r"^#include <(?:hip/|rocblas/|raft/)[^>]+>\n", "", source,
                        flags=re.MULTILINE)
        source = re.sub(r"(sk_\w+_kernel)<<<(.*?),\s*(.*?),\s*0,\s*0>>>\(",
                        r"cpu_launch(\1, dim3(\2), dim3(\3), ", source)
        unit = Path(cls.tmp.name) / "test.cpp"
        unit.write_text('#include "cpu_runtime.h"\n' + source +
                        '\n#include "test_cases.inc"\n')
        subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-O1",
                        "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        "-no-pie", "-pthread", "-I", str(HERE), str(unit),
                        "-o", str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def check_case(self, *args):
        run = subprocess.run([str(self.binary), *map(str, args)],
                             capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_single_tile(self):
        self.check_case("shape", 128, 96, 32, 128)

    def test_distinct_norms_across_tiles(self):
        self.check_case("shape", 256, 96, 32, 128)

    def test_short_final_base_tile(self):
        self.check_case("shape", 257, 96, 32, 128)

    def test_tiles_smaller_than_requested_k(self):
        self.check_case("shape", 65, 48, 8, 8)

    def test_partial_allocation_cleanup(self):
        for fail_at in range(1, 7):
            with self.subTest(fail_at=fail_at):
                self.check_case("allocation", fail_at)

    def test_failed_result_copy_cleanup(self):
        self.check_case("copy")

    def test_failed_selection_cleanup(self):
        self.check_case("selection")


class BuildRegression(unittest.TestCase):
    def test_headless_offload_target_with_and_without_wmma_headers(self):
        for wmma in (False, True):
            with self.subTest(wmma=wmma), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                bridge = root / "bridge"
                rocm = root / "rocm"
                hipvs = root / "hipvs"
                # Only the build script runs. Fake compiler records its arguments;
                # this checks target selection, not HIP compilation or linking.
                for path in (bridge / "raft/include/raft/core/device_mdspan.hpp",
                             hipvs / "include/cuvs/core/c_api.h",
                             hipvs / "lib/libcuvs_c.so"):
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.touch()
                if wmma:
                    header = rocm / "include/rocwmma/rocwmma.hpp"
                    header.parent.mkdir(parents=True)
                    header.touch()
                compiler = root / "hipcc"
                compiler.write_text('#!/bin/sh\nprintf "%s\\n" "$@" >> "$CAPTURE"\n')
                compiler.chmod(0o755)
                (bridge / "build.sh").write_text((ROOT / "tools/hipvs_bridge/build.sh").read_text())
                capture = root / "args"
                env = dict(os.environ, HIPCC=str(compiler), ROCM_ROOT=str(rocm),
                           HIPVS_ROOT=str(hipvs), CAPTURE=str(capture))
                run = subprocess.run(["bash", str(bridge / "build.sh")], env=env,
                                     capture_output=True, text=True)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertIn("--offload-arch=gfx1100", capture.read_text().splitlines())


if __name__ == "__main__":
    unittest.main(verbosity=2)
