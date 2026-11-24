"""Offline tests for the one-time asset rename helper."""

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "rename-ota-assets.py"


class RenameTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="ota-rename-")
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)
        self.bin = self.dir / "bin"
        self.bin.mkdir()
        releases = [{
            "tag_name": "filter-v1.18.0-dev.11",
            "assets": [
                {"id": 1, "name": "Xiao_S3_WIO_repeater-v1.18.0-dev.11-filter-10d4da2.bin"},
                {"id": 2, "name": "Xiao_S3_WIO_repeater-v1.18.0-dev.11-filter-10d4da2-merged.bin"},
                {"id": 3, "name": "Xiao_S3_WIO_repeater-v1.18.0-dev.11.bin"},
                {"id": 4, "name": "wio-e5-v1.18.0-dev.11-filter-10d4da2.uf2"},
                {"id": 5, "name": "SHA256SUMS.txt"},
                {"id": 6, "name": "t1000e_repeater-v1.17.1-filter-e7e9188c-e7e9188.zip"},
            ],
        }]
        (self.dir / "releases.json").write_text(json.dumps(releases), encoding="utf-8")
        fake = self.bin / "gh"
        fake.write_text(
            "#!/usr/bin/env python3\n"
            "import json,os,sys\n"
            "args=sys.argv[1:]\n"
            "root=os.environ['TEST_DIR']\n"
            "if '-X' in args:\n"
            "    open(root+'/patches.log','a').write(' '.join(args)+'\\n')\n"
            "    path=[a for a in args if '/assets/' in a][0]\n"
            "    asset_id=int(path.rsplit('/',1)[1])\n"
            "    new=[a.split('name=',1)[1] for a in args if a.startswith('name=')][0]\n"
            "    rel=json.load(open(root+'/releases.json'))\n"
            "    for release in rel:\n"
            "        for asset in release['assets']:\n"
            "            if asset['id']==asset_id: asset['name']=new\n"
            "    json.dump(rel,open(root+'/releases.json','w'))\n"
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

    def test_dry_run_changes_nothing(self):
        result = self.run_script("--dry-run")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("-filter-10d4da2.bin -> Xiao_S3_WIO_repeater-v1.18.0-dev.11.bin",
                      result.stdout)
        self.assertFalse((self.dir / "patches.log").exists())

    def test_renames_old_names_only_and_is_idempotent(self):
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stderr)
        patches = (self.dir / "patches.log").read_text(encoding="utf-8")
        self.assertEqual(len(patches.splitlines()), 4)
        self.assertIn("name=Xiao_S3_WIO_repeater-v1.18.0-dev.11.bin", patches)
        self.assertIn("name=Xiao_S3_WIO_repeater-v1.18.0-dev.11-merged.bin", patches)
        self.assertIn("name=wio-e5-v1.18.0-dev.11.uf2", patches)
        self.assertIn("name=t1000e_repeater-v1.17.1.zip", patches)
        self.assertNotIn("SHA256SUMS", patches)
        self.run_script()
        self.assertEqual(len((self.dir / "patches.log").read_text().splitlines()), 4)

    def test_limit(self):
        result = self.run_script("--limit", "1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len((self.dir / "patches.log").read_text().splitlines()), 1)


if __name__ == "__main__":
    unittest.main()
