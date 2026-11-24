"""Offline tests for the release tag rename helper."""

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "rename-ota-tags.py"


class TagRenameTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="ota-tag-rename-")
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)
        self.bin = self.dir / "bin"
        self.bin.mkdir()
        releases = [
            {"id": 1, "tag_name": "filter-v1.18.0-dev.11"},
            {"id": 2, "tag_name": "filter-v1.18.0-r4"},
            {"id": 3, "tag_name": "filter-v1.17.1-112-20260910-1628-e6dcf291"},
            {"id": 4, "tag_name": "v1.18.0"},
        ]
        tags = [{"name": "filter-v1.18.0-dev.11"}, {"name": "v1.18.0"},
                {"name": "room-server-v1.17.1"}]
        (self.dir / "releases.json").write_text(json.dumps(releases), encoding="utf-8")
        (self.dir / "tags.json").write_text(json.dumps(tags), encoding="utf-8")
        fake = self.bin / "gh"
        fake.write_text(
            "#!/usr/bin/env python3\n"
            "import os,sys\n"
            "args=sys.argv[1:]\n"
            "root=os.environ['TEST_DIR']\n"
            "if '-X' in args:\n"
            "    open(root+'/patches.log','a').write(' '.join(args)+'\\n')\n"
            "elif 'releases' in args[1]:\n"
            "    print(open(root+'/releases.json').read())\n"
            "else:\n"
            "    print(open(root+'/tags.json').read())\n",
            encoding="utf-8",
        )
        os.chmod(fake, 0o755)

    def run_script(self, *args):
        env = {**os.environ, "PATH": str(self.bin) + os.pathsep + os.environ["PATH"],
               "TEST_DIR": str(self.dir)}
        return subprocess.run([sys.executable, str(SCRIPT), *args], env=env,
                              check=False, capture_output=True, text=True)

    def test_dry_run_is_the_default_and_skips_legacy_and_collisions(self):
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.dir / "patches.log").exists())
        self.assertIn("DRY RUN filter-v1.18.0-dev.11 -> v1.18.0-dev.11", result.stdout)
        self.assertIn("DRY RUN filter-v1.18.0-r4 -> v1.18.0-r4", result.stdout)
        self.assertIn("SKIP filter-v1.17.1-112-20260910-1628-e6dcf291", result.stdout)
        self.assertIn("SKIP v1.18.0", result.stdout)
        self.assertIn("renameable 2, skipped 2, failed 0", result.stdout)

    def test_apply_renames_and_keeps_old_tag_refs(self):
        result = self.run_script("--apply")
        self.assertEqual(result.returncode, 0, result.stderr)
        patches = (self.dir / "patches.log").read_text(encoding="utf-8")
        self.assertEqual(len(patches.splitlines()), 2)
        self.assertIn("tag_name=v1.18.0-dev.11", patches)
        self.assertIn("tag_name=v1.18.0-r4", patches)
        self.assertNotIn("DELETE", patches)

    def test_limit(self):
        result = self.run_script("--apply", "--limit", "1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len((self.dir / "patches.log").read_text().splitlines()), 1)


if __name__ == "__main__":
    unittest.main()
