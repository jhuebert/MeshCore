import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class ReleasePruneTest(unittest.TestCase):
    def _run_worker(self, prune):
        """Run one __worker build in a fake tree and report what it kept."""
        script = Path(__file__).resolve().parents[1] / "local-release-build.sh"
        with tempfile.TemporaryDirectory(prefix="meshcore-prune-") as directory:
            work = Path(directory)
            # a fake PlatformIO: the worker just needs `pio run` to succeed
            Path(work, "pio").write_text("#!/bin/sh\nexit 0\n")
            os.chmod(Path(work, "pio"), 0o755)
            # a build tree that already compiled: .su for the stack gate, plus
            # the artifacts the worker copies out of it
            build = work / ".pio" / "build" / "Board_repeater"
            Path(build, "src").mkdir(parents=True)
            Path(build, "src", "main.su").write_text("main.cpp:1:1:setup\t16\tstatic\n")
            Path(build, "firmware.bin").write_bytes(b"bin")
            Path(build, "firmware.uf2").write_bytes(b"uf2")
            Path(work, ".pio", "libdeps", "Board_repeater").mkdir(parents=True)

            result = subprocess.run(
                ["bash", str(script), "__worker", "Board_repeater"],
                env={
                    **os.environ,
                    "PATH": directory + os.pathsep + os.environ["PATH"],
                    "WORKER_DIR": directory,
                    "WORKER_PLATFORM": "RP2040_PLATFORM",
                    "WORKER_PRUNE": "1" if prune else "0",
                    "RELEASE_SHA": "0" * 40,
                    "FIRMWARE_VERSION": "v1.0.0-filter",
                },
                check=True, capture_output=True, text=True,
            )
            base = "out/Board_repeater-v1.0.0-filter-0000000"
            self.assertEqual(result.stdout.splitlines(), [f"{base}.bin", f"{base}.uf2"])
            return (build.exists(), (work / ".pio" / "libdeps" / "Board_repeater").exists())

    def test_prune_drops_the_target_trees_once_its_artifacts_are_out(self):
        build, libdeps = self._run_worker(prune=True)
        self.assertFalse(build)
        self.assertFalse(libdeps)

    def test_default_keeps_the_target_trees_for_an_incremental_rerun(self):
        build, libdeps = self._run_worker(prune=False)
        self.assertTrue(build)
        self.assertTrue(libdeps)


if __name__ == "__main__":
    unittest.main()
