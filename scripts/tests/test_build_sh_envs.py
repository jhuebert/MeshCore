import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class BuildShEnvListTest(unittest.TestCase):
    def test_list_excludes_extends_option_lines(self):
        script = Path(__file__).resolve().parents[2] / "build.sh"
        with tempfile.TemporaryDirectory(prefix="meshcore-build-") as directory:
            # `pio project config` prints one option line per setting, so an env
            # that extends another also yields a line holding "env:<base>" — which
            # is not an environment, however much it looks like one.
            Path(directory, "pio").write_text(
                "#!/bin/sh\n"
                "cat <<'EOF'\n"
                "env:Heltec_v3_repeater\n"
                "----------------------\n"
                "build_flags       =  -D ESP32_PLATFORM\n"
                "\n"
                "env:Heltec_v3_repeater_observer\n"
                "-------------------------------\n"
                "extends           =  env:Heltec_v3_repeater\n"
                "build_flags       =  -D ESP32_PLATFORM\n"
                "EOF\n"
            )
            os.chmod(Path(directory, "pio"), 0o755)
            result = subprocess.run(
                ["bash", str(script), "list"],
                env={**os.environ, "PATH": directory + os.pathsep + os.environ["PATH"]},
                check=True, capture_output=True, text=True,
            )
            self.assertEqual(result.stdout.splitlines(),
                             ["Heltec_v3_repeater", "Heltec_v3_repeater_observer"])


if __name__ == "__main__":
    unittest.main()
