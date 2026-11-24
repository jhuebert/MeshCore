"""Offline tests for the release title rename helper."""

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "rename-ota-release-titles.py"


class TitleRenameTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="ota-title-rename-")
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)
        self.bin = self.dir / "bin"
        self.bin.mkdir()
        releases = [
            {"id": 1, "tag_name": "v1.18.0-dev.11", "name": "Filter Firmware v1.18.0-dev.11"},
            {"id": 2, "tag_name": "v1.18.0-r4", "name": "Filter Firmware v1.18.0-r4 (stable)"},
            {"id": 3, "tag_name": "v1.18.0", "name": "Firmware v1.18.0 (stable)"},
        ]
        (self.dir / "releases.json").write_text(json.dumps(releases), encoding="utf-8")
        fake = self.bin / "gh"
        fake.write_text(
            "#!/usr/bin/env python3\n"
            "import os,sys\n"
            "args=sys.argv[1:]\n"
            "root=os.environ['TEST_DIR']\n"
            "if '-X' in args:\n"
            "    open(root+'/patches.log','a').write(' '.join(args)+'\\n')\n"
            "else:\n"
            "    print(open(root+'/releases.json').read())\n",
            encoding="utf-8",
        )
        os.chmod(fake, 0o755)

    def run_script(self, *args):
        env = {**os.environ, "PATH": str(self.bin) + os.pathsep + os.environ["PATH"],
               "TEST_DIR": str(self.dir)}
        return subprocess.run([sys.executable, str(SCRIPT), *args], env=env,
                              check=False, capture_output=True, text=True)

    def test_dry_run_preview(self):
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.dir / "patches.log").exists())
        self.assertIn("DRY RUN v1.18.0-dev.11: Filter Firmware v1.18.0-dev.11 -> "
                      "Firmware v1.18.0-dev.11", result.stdout)
        self.assertIn("-> Firmware v1.18.0-r4 (stable)", result.stdout)
        self.assertIn("renamed 0, already current 1, failed 0", result.stdout)

    def test_apply_keeps_stable_suffix(self):
        result = self.run_script("--apply")
        self.assertEqual(result.returncode, 0, result.stderr)
        patches = (self.dir / "patches.log").read_text(encoding="utf-8")
        self.assertEqual(len(patches.splitlines()), 2)
        self.assertIn("name=Firmware v1.18.0-dev.11", patches)
        self.assertIn("name=Firmware v1.18.0-r4 (stable)", patches)

    def test_limit(self):
        result = self.run_script("--apply", "--limit", "1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len((self.dir / "patches.log").read_text().splitlines()), 1)


if __name__ == "__main__":
    unittest.main()
