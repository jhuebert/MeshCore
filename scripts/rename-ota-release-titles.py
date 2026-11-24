#!/usr/bin/env python3
"""Rename existing release titles from "Filter Firmware ..." to "Firmware ...".

Dry-run by default. Pass --apply only after reviewing the report. The tag,
assets and release body are untouched.
"""

import argparse
import json
import subprocess
import sys

OLD_PREFIX = "Filter Firmware "
NEW_PREFIX = "Firmware "


def gh(*args):
    result = subprocess.run(["gh", *args], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or "gh failed")
    return result.stdout


def rename(repo, apply, limit):
    releases = json.loads(gh("api", f"repos/{repo}/releases?per_page=100", "--paginate"))
    changes = []
    for release in releases:
        old = release.get("name") or ""
        if not old.startswith(OLD_PREFIX):
            continue
        new = NEW_PREFIX + old[len(OLD_PREFIX):]
        changes.append((release["tag_name"], old, new, release["id"]))

    applied, failed = [], []
    if apply:
        for tag, _, new, release_id in changes[:limit] if limit else changes:
            try:
                gh("api", "-X", "PATCH", f"repos/{repo}/releases/{release_id}",
                   "-f", f"name={new}")
                applied.append(tag)
            except RuntimeError as exc:
                failed.append((tag, str(exc)))
                print(f"FAILED {tag}: {exc}", file=sys.stderr)

    for tag, old, new, _ in changes:
        state = "APPLY" if tag in applied else "DRY RUN"
        print(f"{state} {tag}: {old} -> {new}")
    for tag, why in failed:
        print(f"FAILED {tag}: {why}", file=sys.stderr)
    print(f"renamed {len(applied)}, already current "
          f"{len(releases) - len(changes)}, failed {len(failed)}")
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
