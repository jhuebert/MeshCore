#!/usr/bin/env python3
"""Repoint filter-v* releases to v* tags.

Dry-run by default. Pass --apply only after reviewing the report. Old filter-v
tag refs are deliberately kept as aliases; this script never deletes tags.
Legacy date-scheme tags cannot be expressed as v<version> and are skipped.
"""

import argparse
import json
import re
import subprocess
import sys

TAG_RE = re.compile(r"^filter-v(?P<ver>\d+\.\d+\.\d+(?:-dev\.\d+|-r\d+)?)$")


def gh(*args):
    result = subprocess.run(["gh", *args], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or "gh failed")
    return result.stdout


def rename(repo, apply, limit):
    releases = json.loads(gh("api", f"repos/{repo}/releases?per_page=100", "--paginate"))
    tags = {t["name"] for t in json.loads(gh("api", f"repos/{repo}/tags?per_page=100", "--paginate"))}
    changed, skipped, failed = [], [], []
    for release in releases:
        old = release.get("tag_name", "")
        match = TAG_RE.match(old)
        if not match:
            skipped.append((old, "legacy or non-canonical tag"))
            continue
        new = "v" + match.group("ver")
        if new in tags:
            skipped.append((old, f"collides with existing tag {new}"))
            continue
        changed.append((old, new, release["id"]))

    applied = []
    if apply:
        for old, new, release_id in changed[:limit] if limit else changed:
            try:
                gh("api", "-X", "PATCH", f"repos/{repo}/releases/{release_id}",
                   "-f", f"tag_name={new}")
                applied.append(new)
                print(f"OK {old} -> {new}")
            except RuntimeError as exc:
                failed.append((old, str(exc)))
                print(f"FAILED {old}: {exc}", file=sys.stderr)

    print("\n=== tag rename report ===")
    for old, new, _ in changed:
        print(f"{'APPLY' if new in applied else 'DRY RUN'} {old} -> {new}")
    for old, why in skipped:
        print(f"SKIP {old}: {why}")
    for old, why in failed:
        print(f"FAILED {old}: {why}")
    print(f"renameable {len(changed)}, skipped {len(skipped)}, failed {len(failed)}")
    return 1 if failed else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default="jhuebert/MeshCore")
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--limit", type=int, default=0)
    args = parser.parse_args()
    return rename(args.repo, args.apply, args.limit)


if __name__ == "__main__":
    sys.exit(main())
