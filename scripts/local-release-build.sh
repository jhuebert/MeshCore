#!/usr/bin/env bash
#
# local-release-build.sh — build a filter release's firmware locally and upload
# the assets to its GitHub release, without burning GitHub-hosted runner time.
#
# Usage:
#   scripts/local-release-build.sh [options] <release-url-or-tag>
#
# A release URL works from any clone; a bare tag additionally assumes the
# clone has a remote named "fork" pointing at the release's repo (falling
# back to the URL works everywhere). Requires gh (authenticated), git,
# python3, and bash 4+.
#
#     -j, --jobs N        parallel build workers (default: 4)
#     --targets LIST      comma/space/newline-separated target list
#                         (default: every repeater target in the release tree)
#     --filter REGEX      build only targets matching REGEX (subset of default)
#     --no-upload         build and collect artifacts only
#     --no-warmup         skip the serial first-build-per-platform phase (faster
#                         start, but concurrent first-time toolchain installs
#                         into the PlatformIO home can race)
#     --build-dir DIR     persistent work dir
#                         (default: ~/.cache/meshcore-release-build/<tag>)
#     --dry-run           resolve the release, list targets, exit
#     -h, --help          this help
#
# What it does:
#   1. Resolves the release (URL or tag) via gh, and the exact source commit it
#      names (target_commitish) so file versions match the release body.
#   2. Checks that commit out into a persistent git worktree — reused between
#      runs of the same tag, so .pio/build makes reruns incremental.
#   3. Lists the release tree's repeater targets with build.sh (155 today).
#   4. Builds them N at a time, mirroring build.sh's per-target logic (it cannot
#      be called directly in parallel: it wipes out/ on every invocation).
#   5. Uploads each target's artifacts as soon as it finishes (a small capped
#      pool of gh release upload --clobber runs beside the build pool).
#
# Environment parity with the releases CI cuts: FIRMWARE_VERSION = <tag minus
# the "filter-" prefix> + "-filter" (exactly what filter-release.yml's version
# job derived), PLATFORMIO_BUILD_FLAGS starts with -fstack-usage, and build.sh's
# "-<short-sha>" suffix comes from the release's pinned commit. Artifacts land
# in <build-dir>/<tag>/out/ as well as on the release, so a failed upload never
# loses a build.
#
# Build environment isolation: PlatformIO runs from a dedicated venv (not your
# system Python packages), and every compiler/toolchain comes from the
# PlatformIO home — pinned, self-contained downloads, the same ones CI installs.
# Nothing else on the machine participates in the build. The bytes that vary
# with where/when a build runs are only the embedded build date, the IDF
# compile time-of-day, the PlatformIO home path prefix that the Arduino
# framework embeds via __FILE__, and the SHA256 trailers derived from those —
# all benign, and all shared by every asset in one release (CI's own reruns
# vary the same way; to match CI's exact bytes, export
# PLATFORMIO_HOME_DIR=/home/runner/.platformio after creating that path).
#
set -euo pipefail

die() { echo "local-release-build: $*" >&2; exit 1; }
info() { echo "local-release-build: $*"; }
trap 'exit 130' INT

# Ctrl-C must take the whole tree down: the build workers and the uploader run
# as asynchronous commands, which bash makes ignore SIGINT when job control is
# off, so the terminal's INT would otherwise leave them building and uploading
# after the main script exits (and the uploader's poll loop has no other exit).
kill_tree() {  # kill_tree <pid> — SIGTERM a process and all its descendants
  local pid=$1 kid
  for kid in $(ps -o pid= --ppid "$pid" 2>/dev/null); do
    kill_tree "$kid"
  done
  kill -TERM "$pid" 2>/dev/null || true
}
interrupted=""
cleanup() {
  trap - INT TERM EXIT   # no re-entry via exit
  local pid
  for pid in ${build_pids[@]+"${build_pids[@]}"}; do
    kill_tree "$pid"
  done
  [[ -n ${uploader_pid:-} ]] && kill_tree "$uploader_pid"
  if [[ -n $interrupted ]]; then
    info "interrupted — builds and uploads stopped; a rerun is incremental"
    exit 130
  fi
}
trap 'interrupted=1; cleanup' INT
trap 'interrupted=1; cleanup' TERM
trap cleanup EXIT

usage() { sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d' | sed 's/^# \{0,1\}//'; }

# ---------------------------------------------------------------- modes ----
# Internal modes run inside the release checkout (WORKER_DIR) with the build
# environment already prepared by the orchestrator (venv PATH). Target listing,
# platform mapping and building share one code path.

# repeater targets of the checked-out tree (the selection filter-release.yml
# used to make before firmware builds moved local)
if [[ ${1:-} == __list ]]; then
  cd "$WORKER_DIR"
  bash build.sh list | grep -i repeater
  exit 0
fi

# "target platform" lines for the targets given as arguments (platform family
# detection identical to build.sh's get_platform_for_env)
if [[ ${1:-} == __platforms ]]; then
  shift
  cd "$WORKER_DIR"
  PIO_CONFIG_JSON=$(pio project config --json-output)
  for target in "$@"; do
    platform=$(printf '%s' "$PIO_CONFIG_JSON" | python3 -c "
import sys, json, re
data = json.load(sys.stdin)
for section, options in data:
    if section == 'env:$target':
        for key, value in options:
            if key == 'build_flags':
                for flag in value:
                    match = re.search(r'(ESP32_PLATFORM|NRF52_PLATFORM|STM32_PLATFORM|RP2040_PLATFORM)', flag)
                    if match:
                        print(match.group(1))
                        sys.exit(0)
")
    echo "$target ${platform:-none}"
  done
  exit 0
fi

# build one target: compile + collect artifacts, mirroring build.sh
# build_firmware() (build.sh cannot be called in parallel: its top level does
# rm -rf out on every invocation). Artifact paths are printed on stdout; all
# chatter goes to stderr so the orchestrator can capture the file list.
if [[ ${1:-} == __worker ]]; then
  target=$2
  cd "$WORKER_DIR"

  short_sha=${RELEASE_SHA:0:7}            # build.sh uses `git rev-parse --short HEAD`
  firmware_version_string="${FIRMWARE_VERSION}-${short_sha}"
  firmware_build_date=$(LC_ALL=C date '+%d-%b-%Y')   # %b is locale-dependent; CI runs C.UTF-8

  # flags release firmware has always been built with: stack usage metadata
  # (codegen unchanged, consumed by the stack gate below) + version string
  export PLATFORMIO_BUILD_FLAGS="-fstack-usage -DFIRMWARE_BUILD_DATE='\"${firmware_build_date}\"' -DFIRMWARE_VERSION='\"${firmware_version_string}\"'"

  file_base="${target}-${firmware_version_string}"
  mkdir -p out

  platform=${WORKER_PLATFORM:-}   # passed by the orchestrator's precomputed map

  pio run -e "$target" 1>&2

  # stack usage gate, moved here from filter-release.yml's per-target CI job:
  # one big stack scratch array in boot- or loop-path code brick-reboots every
  # repeater flashed with the release (2026-10: an 8.5 KB scratch in
  # FilterRules::load overflowed the 4 KB nRF52 / 8 KB ESP32 loop task stacks
  # at boot). A failing gate fails the target, exactly as CI did.
  python3 - "$target" <<'EOF' 1>&2
import os, sys

limit = 2048
su_dir = os.path.join('.pio', 'build', sys.argv[1])

# repo code only: src/ and examples/. Vendored crypto (lib/ed25519) and
# libdeps libraries carry frames above the limit that ship today and are
# not what this repo edits; gating them would train everyone to ignore
# the check. The frames this repo owns are the ones it can break.
su_files = []
for sub in ('src', 'examples'):
    base = os.path.join(su_dir, sub)
    if not os.path.isdir(base):
        continue
    for root, _dirs, files in os.walk(base):
        su_files += [os.path.join(root, f) for f in files if f.endswith('.su')]

if not su_files:
    sys.exit(f'ERROR: no .su files under {su_dir} — the build did not run with -fstack-usage, check is blind')

# .su lines are "file:line:col:function\tbytes\ttype"
offenders = []
for path in su_files:
    for line in open(path):
        if not line.strip():
            continue
        loc, size = line.split('\t')[:2]
        if int(size) >= limit:
            offenders.append(f'{size} B  {loc}')

if offenders:
    sys.exit(f'ERROR: stack frames >= {limit} B (loop task is 4 KB on nRF52, 8 KB on ESP32):\n' + '\n'.join(offenders))
print(f'OK: {len(su_files)} translation units, no stack frame >= {limit} B')
EOF

  # artifact collection per platform family, identical to build.sh
  # ("|| true" included: build.sh tolerates a missing merged/zip companion)
  case $platform in
    ESP32_PLATFORM)
      pio run -t mergebin -e "$target" 1>&2
      cp ".pio/build/$target/firmware.bin" "out/${file_base}.bin" 2>/dev/null || true
      cp ".pio/build/$target/firmware-merged.bin" "out/${file_base}-merged.bin" 2>/dev/null || true
      ;;
    NRF52_PLATFORM)
      python3 bin/uf2conv/uf2conv.py ".pio/build/$target/firmware.hex" -c -o ".pio/build/$target/firmware.uf2" -f 0xADA52840 1>&2
      cp ".pio/build/$target/firmware.uf2" "out/${file_base}.uf2" 2>/dev/null || true
      cp ".pio/build/$target/firmware.zip" "out/${file_base}.zip" 2>/dev/null || true
      ;;
    STM32_PLATFORM)
      cp ".pio/build/$target/firmware.bin" "out/${file_base}.bin" 2>/dev/null || true
      cp ".pio/build/$target/firmware.hex" "out/${file_base}.hex" 2>/dev/null || true
      ;;
    RP2040_PLATFORM)
      cp ".pio/build/$target/firmware.bin" "out/${file_base}.bin" 2>/dev/null || true
      cp ".pio/build/$target/firmware.uf2" "out/${file_base}.uf2" 2>/dev/null || true
      ;;
  esac

  # a target that compiled but produced no artifact would otherwise upload
  # nothing and surface only as a confusing gh error later
  shopt -s nullglob
  artifacts=(out/${file_base}.* out/${file_base}-merged.bin)
  shopt -u nullglob
  if ((${#artifacts[@]} == 0)); then
    echo "ERROR: no artifacts produced for $target" >&2
    exit 1
  fi
  printf '%s\n' "${artifacts[@]}"
  exit 0
fi

# ------------------------------------------------------------- options ----
JOBS=4
UPLOAD=1
WARMUP=1
DRY_RUN=0
TARGETS_INPUT=""
FILTER_ARG=""
BUILD_DIR_BASE="${MESH_RELEASE_BUILD_DIR:-$HOME/.cache/meshcore-release-build}"
RELEASE_ARG=""

while [[ $# -gt 0 ]]; do
  case $1 in
    -j|--jobs) JOBS=$2; shift 2;;
    --no-upload) UPLOAD=0; shift;;
    --no-warmup) WARMUP=0; shift;;
    --targets) TARGETS_INPUT=$2; shift 2;;
    --filter) FILTER_ARG=$2; shift 2;;
    --build-dir) BUILD_DIR_BASE=$2; shift 2;;
    --dry-run) DRY_RUN=1; shift;;
    -h|--help) usage; exit 0;;
    -*) die "unknown option: $1 (see --help)";;
    *) RELEASE_ARG=$1; shift;;
  esac
done
[[ -n $RELEASE_ARG ]] || { usage; exit 1; }
command -v gh >/dev/null || die "gh is required"
command -v git >/dev/null || die "git is required"
((BASH_VERSINFO[0] >= 4)) || die "bash 4+ is required (macOS ships 3.2; try 'brew install bash')"

SCRIPT_PATH=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")
REPO_ROOT=$(git -C "$(dirname "$SCRIPT_PATH")" rev-parse --show-toplevel)

# ------------------------------------------------------- release info ----
# accept a release URL or a bare tag; the release lives on the fork
if [[ $RELEASE_ARG == *"/releases/tag/"* ]]; then
  tag=${RELEASE_ARG##*/releases/tag/}
  tag=${tag%%/*}
  release_repo=$(sed -n 's#https://github.com/\([^/]\{1,\}\)/\([^/]\{1,\}\)/.*#\1/\2#p' <<<"$RELEASE_ARG")
else
  tag=$RELEASE_ARG
  release_repo=$(git -C "$REPO_ROOT" remote get-url fork 2>/dev/null | sed -n 's#.*github.com[/:]##p' | sed 's#\.git$##')
fi
[[ -n ${release_repo:-} ]] || die "cannot determine the release's repo — pass the release URL"

tag=$(gh release view "$tag" --repo "$release_repo" --json tagName --jq .tagName) \
  || die "cannot resolve release '$RELEASE_ARG' on $release_repo"
info "release tag: $tag"

# the commit the release names — firmware file names embed its short sha, so
# the local build must check out exactly that revision
release_sha=$(gh release view "$tag" --repo "$release_repo" --json targetCommitish --jq .targetCommitish)
if [[ -z $release_sha || ! $release_sha =~ ^[0-9a-f]{40}$ ]]; then
  release_sha=$(git -C "$REPO_ROOT" rev-parse "${tag}^{commit}")
fi

# fetch the tag from the release's repo, leaving the current checkout untouched
remote=origin
for r in $(git -C "$REPO_ROOT" remote); do
  if git -C "$REPO_ROOT" remote get-url "$r" 2>/dev/null | grep -q "github.com[/:]${release_repo%%/*}/"; then
    remote=$r; break
  fi
done
git -C "$REPO_ROOT" fetch --quiet "$remote" "refs/tags/$tag:refs/tags/$tag" --no-tags --force \
  || die "cannot fetch tag $tag from remote '$remote'"
git -C "$REPO_ROOT" cat-file -e "$release_sha^{commit}" \
  || die "commit $release_sha not present after fetching tag $tag"

# workflow's version job: displayVersion = tag minus "filter-" prefix,
# firmwareVersion = displayVersion + "-filter" (build.sh appends -<sha>)
firmware_version="${tag#filter-}-filter"
[[ $firmware_version == "$tag" ]] && die "tag '$tag' does not start with 'filter-' — not a filter release?"
info "source: $release_sha"
info "FIRMWARE_VERSION: $firmware_version"

# --------------------------------------------------------- work tree ----
build_dir="$BUILD_DIR_BASE/$tag"

# persistent worktree at the release commit: reused when unchanged so
# .pio/build carries over (reruns of the same tag are incremental)
if [[ -e "$build_dir/.git" ]]; then
  current_sha=$(git -C "$build_dir" rev-parse HEAD 2>/dev/null || echo "")
  if [[ $current_sha != "$release_sha" ]]; then
    info "work dir holds $current_sha, wanted $release_sha — recreating"
    git -C "$REPO_ROOT" worktree remove --force "$build_dir"
  fi
fi
if [[ -e "$build_dir" && ! -e "$build_dir/.git" ]]; then
  # stale leftovers (e.g. an interrupted first run): a worktree cannot be
  # added into a non-empty directory
  git -C "$REPO_ROOT" worktree prune
  rm -rf "$build_dir"
fi
if [[ ! -e "$build_dir/.git" ]]; then
  git -C "$REPO_ROOT" worktree add --quiet --detach "$build_dir" "$release_sha"
fi

state_dir="$build_dir/.state"
# fresh run state every time (builds/upload markers must not leak between runs);
# the worktree itself is kept — that is where the .pio build cache lives
rm -rf "$state_dir"
mkdir -p "$state_dir"/{logs,files,done,uploaded,upfail,uploading,failed}
# out/ is never wiped between runs: uploads are driven by the explicit
# per-target file list, and keeping old artifacts means a failed rerun does
# not destroy the previous run's successful builds
mkdir -p "$build_dir/out"

# -------------------------------------------------------- build tool ----
# dedicated venv: PlatformIO is isolated from system packages; the toolchains
# themselves always come from the PlatformIO home (~/.platformio), which is
# also what the CI cache restores
venv_dir="$BUILD_DIR_BASE/venv"
if [[ ! -x "$venv_dir/bin/pio" ]]; then
  info "creating venv with PlatformIO at $venv_dir"
  python3 -m venv "$venv_dir"
  "$venv_dir/bin/pip" install --quiet --upgrade pip
  "$venv_dir/bin/pip" install --quiet platformio
fi
run_tool() {
  (
    cd "$build_dir"
    export FIRMWARE_VERSION="$firmware_version" RELEASE_SHA="$release_sha" WORKER_DIR="$build_dir"
    PATH="$venv_dir/bin:$PATH" bash "$SCRIPT_PATH" "$@"
  )
}
command -v python3 >/dev/null || die "python3 is required"

# ---------------------------------------------------------- targets ----
if [[ -n $TARGETS_INPUT ]]; then
  mapfile -t targets < <(tr ' ,\n' '\n' <<<"$TARGETS_INPUT" | sed '/^$/d')
else
  info "listing repeater targets in the release tree"
  mapfile -t targets < <(run_tool __list)
fi
if [[ -n $FILTER_ARG ]]; then
  mapfile -t targets < <(printf '%s\n' "${targets[@]}" | grep -E "$FILTER_ARG" || true)
fi
[[ ${#targets[@]} -gt 0 ]] || die "no targets selected"

if [[ $DRY_RUN == 1 ]]; then
  info "${#targets[@]} targets:"
  printf '%s\n' "${targets[@]}"
  exit 0
fi

# ------------------------------------------------------- build pool ----
# map targets to platform families, then warm each platform with one serial
# build: first-time toolchain downloads into the shared PlatformIO home are the
# one place concurrent workers can race. Warm-up builds are real builds — their
# artifacts upload like any other.
info "resolving platforms"
mapfile -t platform_lines < <(run_tool __platforms "${targets[@]}")
declare -A target_platform=()
declare -A platform_seen=()
warmup=()
rest=()
for line in "${platform_lines[@]}"; do
  t=${line% *}; p=${line#* }
  target_platform[$t]=$p
  if [[ $WARMUP == 1 && -z ${platform_seen[$p]:-} ]]; then
    platform_seen[$p]=1
    warmup+=("$t")
  else
    rest+=("$t")
  fi
done

run_one() {  # run_one <target> <platform> — build, record artifacts, queue upload
  local t=$1
  local log="$state_dir/logs/$t.log"
  # WORKER_PLATFORM: the caller passes the platform from the map computed once
  # up front; re-deriving it per worker would mean 155 extra pio config runs
  # write to a temp file and rename: the done marker must never be visible
  # while the file list is still empty
  if WORKER_PLATFORM=$2 run_tool __worker "$t" >"$state_dir/files/$t.tmp" 2>"$log"; then
    mv "$state_dir/files/$t.tmp" "$state_dir/files/$t"
    : >"$state_dir/done/$t"
  else
    : >"$state_dir/failed/$t"
    info "FAILED: $t (log: $log)"
  fi
}

upload_one() {  # upload_one <target> — runs in the release's build dir
  local t=$1
  local log="$state_dir/logs/upload-$t.log"
  cd "$build_dir"
  for attempt in 1 2; do
    if gh release upload "$tag" --repo "$release_repo" --clobber $(cat "$state_dir/files/$t") >>"$log" 2>&1; then
      : >"$state_dir/uploaded/$t"
      return 0
    fi
    sleep 5
  done
  : >"$state_dir/upfail/$t"
  info "UPLOAD FAILED: $t (log: $log)"
}

UPLOAD_JOBS=3
# the uploader is itself a background job of this shell: without compensating,
# its slot would silently reduce build parallelism by one
slot_cap=$JOBS
[[ $UPLOAD == 1 ]] && slot_cap=$((JOBS + 1))
builds_done_file="$state_dir/builds_done"

uploader() {  # poll done markers, keep UPLOAD_JOBS uploads in flight
  while :; do
    for f in "$state_dir/done"/*; do
      [[ -f $f ]] || continue
      t=$(basename "$f")
      [[ -e "$state_dir/uploaded/$t" || -e "$state_dir/upfail/$t" || -e "$state_dir/uploading/$t" ]] && continue
      while [[ $(jobs -rp | wc -l) -ge $UPLOAD_JOBS ]]; do wait -n || true; done
      # in-flight marker: the next poll must not spawn a second upload of
      # the same target while this one is still running
      : >"$state_dir/uploading/$t"
      upload_one "$t" &
    done
    if [[ -f $builds_done_file ]]; then
      n_done=$(ls "$state_dir/done" 2>/dev/null | wc -l)
      n_fin=$(( $(ls "$state_dir/uploaded" 2>/dev/null | wc -l) + $(ls "$state_dir/upfail" 2>/dev/null | wc -l) ))
      if [[ $n_done -eq $n_fin && $(jobs -rp | wc -l) -eq 0 ]]; then return; fi
    fi
    sleep 2
  done
}

if [[ $UPLOAD == 1 ]]; then
  info "uploading to release $tag as targets complete"
  uploader &
  uploader_pid=$!
else
  uploader_pid=""
fi

total=${#targets[@]}
queue=("${warmup[@]}" "${rest[@]}")
n_warmup=${#warmup[@]}
build_pids=()
start=$SECONDS
for idx in "${!queue[@]}"; do
  t=${queue[$idx]}
  if [[ $WARMUP == 1 && $idx -lt $n_warmup ]]; then
    info "[$((idx + 1))/$total] warm-up (serial): $t (${target_platform[$t]})"
    run_one "$t" "${target_platform[$t]}"
  else
    while [[ $(jobs -rp | wc -l) -ge $slot_cap ]]; do wait -n || true; done
    run_one "$t" "${target_platform[$t]}" &
    build_pids+=("$!")
    info "[$((idx + 1))/$total] started: $t (${target_platform[$t]})"
  fi
done
# wait for the build jobs only — never bare `wait`, which would also wait for
# the uploader, whose exit condition (builds_done_file) is written just below
((${#build_pids[@]})) && wait "${build_pids[@]}" || true
: >"$builds_done_file"
[[ -n $uploader_pid ]] && wait "$uploader_pid" || true

# ---------------------------------------------------------- summary ----
n_ok=$(ls "$state_dir/done" | wc -l)
n_up=$(ls "$state_dir/uploaded" 2>/dev/null | wc -l)
n_upfail=$(ls "$state_dir/upfail" 2>/dev/null | wc -l)
n_fail=$(ls "$state_dir/failed" 2>/dev/null | wc -l)
mins=$(( (SECONDS - start) / 60 ))
info "built $n_ok/$total in ${mins}m; artifacts: $build_dir/out"
if [[ $UPLOAD == 1 ]]; then
  info "uploaded $n_up to $tag ($n_upfail upload failures)"
  n_assets=$(gh release view "$tag" --repo "$release_repo" --json assets --jq '.assets | length')
  info "release now has $n_assets assets"
fi
if [[ $n_fail -gt 0 ]]; then
  info "build failures:"
  ls "$state_dir/failed" | sed 's/^/  /'
  exit 1
fi
info "all targets built"
