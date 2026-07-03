#!/bin/sh
# e2e_pypi.sh — end-to-end test of the pypi pipeline against the real index:
#   manifest (range constraint + transitive deps) → resolve → download →
#   bundle → extract the tarball → offline install into a fresh venv
#   (install.sh uses --no-index, so this is a faithful air-gap simulation)
#   → import check.
#
# Requires network and python3.  Exits 77 (ctest SKIP_RETURN_CODE) when a
# prerequisite is missing.  The packmule binary is taken from $PACKMULE.
set -eu

PACKMULE="${PACKMULE:-$(cd "$(dirname "$0")/../../build" && pwd)/packmule}"
[ -x "$PACKMULE" ] || { echo "SKIP: packmule binary not found: $PACKMULE"; exit 77; }
command -v python3 >/dev/null 2>&1 || { echo "SKIP: no python3"; exit 77; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/packmule_e2e_pypi.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

# A range with an upper bound plus a package with transitive deps: the two
# resolution behaviours that decide whether the bundle installs offline.
printf 'requests>=2.28,<3\n' > requirements.txt

"$PACKMULE" -f requirements.txt -o vendor -b

[ -f vendor.tar.gz ]        || { echo "FAIL: no tarball";        exit 1; }
[ -f vendor/manifest.json ] || { echo "FAIL: no manifest.json";  exit 1; }
[ -x vendor/install.sh ]    || { echo "FAIL: no install.sh";     exit 1; }

mkdir extracted
tar -xzf vendor.tar.gz -C extracted

python3 -m venv venv
. venv/bin/activate

sh extracted/vendor/install.sh

python3 - <<'EOF'
import requests, urllib3, certifi, idna
assert requests.__version__.startswith("2."), requests.__version__
print("e2e_pypi OK: requests", requests.__version__)
EOF
