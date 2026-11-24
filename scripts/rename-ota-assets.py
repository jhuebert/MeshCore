#!/usr/bin/env python3
"""Strip the release's short commit sha from existing asset names.

Changes <env>-v<version>-filter-<sha7>.bin into <env>-v<version>.bin, so
firmware can build the download URL from a version token and its own env.
Renames are in place via the GitHub API: bytes, asset ID and digest are kept.
Use --dry-run first. Re-running is a no-op once every name is updated.
"""

import argparse
import json
import re
import subprocess
import sys

# Some older releases carry the sha twice (filter-<8 hex>-<7 hex>); strip the
# whole suffix so the remaining name is just <env>-v<version> plus its extension.
ASSET_RE = re.compile(r"-filter-[0-9a-f]{7,}(?:-[0-9a-f]{7,})?")


def gh(*args):
    result = subprocess.run(["gh", *args], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or "gh failed")
    return result.stdout


def rename(repo, dry_run, limit):
    releases = json.loads(gh("api", f"repos/{repo}/releases?per_page=100", "--paginate"))
    renamed, failed = 0, 0
    for release in releases:
        tag = release.get("tag_name", "")
        for asset in release.get("assets", []):
            old = asset.get("name", "")
            new = ASSET_RE.sub("", old, count=1)
            if new == old:
                continue
            if dry_run:
                print(f"DRY RUN {tag}: {old} -> {new}")
                renamed += 1
                continue
            try:
                gh("api", "-X", "PATCH",
                   f"repos/{repo}/releases/assets/{asset['id']}",
                   "-f", f"name={new}")
                print(f"OK {tag}: {old} -> {new}")
                renamed += 1
            except RuntimeError as exc:
                print(f"FAILED {tag}: {old}: {exc}", file=sys.stderr)
                failed += 1
            if limit and renamed >= limit:
                break
        if limit and renamed >= limit:
            break
    print(f"renamed {renamed}, failed {failed}")
    return 1 if failed else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default="jhuebert/MeshCore")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--limit", type=int, default=0)
    args = parser.parse_args()
    return rename(args.repo, args.dry_run, args.limit)


if __name__ == "__main__":
    sys.exit(main())
