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
echo "$OUT" | grep -qE "digest: sha(256|512)-[0-9a-f]{64,128}" || {
    echo "$OUT"; echo "FAIL: no digest on the resolved package"; exit 1;
}
echo "$OUT" | grep -q "1/1 package(s) resolved" || {
    echo "$OUT"; echo "FAIL: package not resolved"; exit 1;
}

# ── Transitive resolution ────────────────────────────────────────────────────
#
# docker-ce declares real package dependencies (not just file capabilities),
# so it distinguishes the two --rpm-deps modes.  `none` must bundle exactly the
# one package named in the manifest; `resolve` must bundle strictly more.
printf 'docker-ce\n' > packages.txt

count_resolved() {
    # "N/M package(s) resolved" → N
    echo "$1" | sed -n 's/.*[^0-9]\([0-9][0-9]*\)\/[0-9][0-9]* package(s) resolved.*/\1/p' \
        | tail -n 1
}

OUT_NONE="$("$PACKMULE" -f packages.txt -t rpm -a x86_64 -u "$REPO" -n \
            --rpm-deps none 2>&1)" || {
    echo "$OUT_NONE"; echo "FAIL: --rpm-deps none failed"; exit 1
}
N_NONE="$(count_resolved "$OUT_NONE")"

OUT_DEPS="$("$PACKMULE" -f packages.txt -t rpm -a x86_64 -u "$REPO" -n \
            --rpm-deps resolve 2>&1)" || {
    echo "$OUT_DEPS"; echo "FAIL: --rpm-deps resolve failed"; exit 1
}
N_DEPS="$(count_resolved "$OUT_DEPS")"

[ "$N_NONE" = "1" ] || {
    echo "$OUT_NONE"
    echo "FAIL: --rpm-deps none resolved $N_NONE packages, expected 1"
    exit 1
}
[ "$N_DEPS" -gt "$N_NONE" ] || {
    echo "$OUT_DEPS"
    echo "FAIL: --rpm-deps resolve found $N_DEPS packages, no more than none ($N_NONE)"
    exit 1
}

echo "e2e_rpm OK (deps: none=$N_NONE resolve=$N_DEPS)"
