#!/bin/sh
# e2e_npm.sh — end-to-end test of the npm pipeline against the real registry:
#   package.json (range + exact pin) → resolve → download → bundle → extract →
#   install into a fresh project with the registry pointed at an unroutable
#   address, so any attempt to touch the network fails the test — a faithful
#   air-gap simulation → require() check.
#
# Requires network, npm, and node.  Exits 77 (ctest SKIP_RETURN_CODE) when a
# prerequisite is missing.  The packmule binary is taken from $PACKMULE.
set -eu

PACKMULE="${PACKMULE:-$(cd "$(dirname "$0")/../../build" && pwd)/packmule}"
[ -x "$PACKMULE" ] || { echo "SKIP: packmule binary not found: $PACKMULE"; exit 77; }
command -v npm  >/dev/null 2>&1 || { echo "SKIP: no npm";  exit 77; }
command -v node >/dev/null 2>&1 || { echo "SKIP: no node"; exit 77; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/packmule_e2e_npm.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

# Dependency-free packages keep the test fast; ^ exercises range resolution.
printf '{ "dependencies": { "left-pad": "^1.3.0", "ms": "2.1.3" } }\n' \
    > package.json

"$PACKMULE" -f package.json -t npm -o vendor -b

[ -f vendor.tar.gz ] || { echo "FAIL: no tarball"; exit 1; }

mkdir extracted
tar -xzf vendor.tar.gz -C extracted

mkdir app
cd app
printf '{ "name": "e2e-app", "version": "1.0.0" }\n' > package.json

# The unroutable registry turns any network access into a hard failure.
npm_config_registry="http://127.0.0.1:9" \
    sh ../extracted/vendor/install.sh

node -e 'require("left-pad"); require("ms"); console.log("e2e_npm OK")'
