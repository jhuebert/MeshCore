import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class ReleaseTargetsTest(unittest.TestCase):
    def test_default_listing_preserves_existing_repeater_variants(self):
        script = Path(__file__).resolve().parents[1] / "local-release-build.sh"
        with tempfile.TemporaryDirectory(prefix="meshcore-release-") as directory:
            Path(directory, "build.sh").write_text(
                "#!/bin/bash\n"
                "printf '%s\\n' Board_repeater Board_repeater_bridge_espnow "
                "Board_repeater_ Board_repeater_ethernet Board_repeater_observer "
                "Board_companion_radio_usb Board_room_server\n"
            )
            result = subprocess.run(
                ["bash", str(script), "__list"],
                env={**os.environ, "WORKER_DIR": directory},
                check=True, capture_output=True, text=True,
            )
            self.assertEqual(result.stdout.splitlines(), [
                "Board_repeater", "Board_repeater_bridge_espnow",
                "Board_repeater_", "Board_repeater_ethernet",
            ])


if __name__ == "__main__":
    unittest.main()
