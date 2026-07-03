#!/bin/sh
# e2e_rpm.sh — end-to-end test of the rpm resolution pipeline against a real
# DNF repository: repomd.xml fetch → primary.xml download + decompress →
# package match → href/sha256/version extraction (dry run, no download).
#
# The final `dnf install` cannot be exercised here: it needs a matching Linux
# host with root.  Resolution is the packmule-owned part of the pipeline;
# installation is dnf's.
#
# Requires network.  Exits 77 (ctest SKIP_RETURN_CODE) when a prerequisite is
# missing.  Override the repo with $PACKMULE_E2E_RPM_REPO if the default is
# unreachable from your network.
set -eu

PACKMULE="${PACKMULE:-$(cd "$(dirname "$0")/../../build" && pwd)/packmule}"
[ -x "$PACKMULE" ] || { echo "SKIP: packmule binary not found: $PACKMULE"; exit 77; }

# Docker's repo is stable, small (primary.xml is a few hundred KB), and
# gzip-compressed, so the test stays fast.
REPO="${PACKMULE_E2E_RPM_REPO:-https://download.docker.com/linux/centos/9/x86_64/stable}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/packmule_e2e_rpm.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

printf 'docker-compose-plugin\n' > packages.txt

OUT="$("$PACKMULE" -f packages.txt -t rpm -a x86_64 -u "$REPO" -n 2>&1)" || {
    echo "$OUT"
    echo "FAIL: rpm dry-run resolution failed"
    exit 1
}
echo "$OUT" | grep -q "sha256:" || { echo "$OUT"; echo "FAIL: no sha256"; exit 1; }
echo "$OUT" | grep -q "1/1 package(s) resolved" || {
    echo "$OUT"; echo "FAIL: package not resolved"; exit 1;
}

echo "e2e_rpm OK"
