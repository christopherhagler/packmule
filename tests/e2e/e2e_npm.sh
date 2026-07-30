#!/bin/sh
# e2e_npm.sh — end-to-end test of the npm pipeline against the real registry:
#   package.json → resolve → download → bundle → extract → install into a
#   fresh project with the registry pointed at an unroutable address, so any
#   attempt to touch the network fails the test — a faithful air-gap
#   simulation → require() check.
#
# The manifest exercises the paths that broke real-world bundles:
#   debug ^4.3.0                     → transitive dep (ms@^2.1.3)
#   ms ^2.1.0                        → range intersection with debug's range
#   @isaacs/string-locale-compare    → scoped name (URL encoding + filename)
#   devDependencies                  → must be ignored offline (--omit=dev)
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

# count_glob — how many of the given paths exist.  Called with an unquoted
# glob, so a pattern matching nothing yields 0 rather than counting the
# literal unexpanded pattern the shell leaves behind.
count_glob() {
    n=0
    for f in "$@"; do
        [ -e "$f" ] && n=$((n + 1))
    done
    echo "$n"
}

cat > package.json <<'EOF'
{
  "dependencies": {
    "debug": "^4.3.0",
    "ms": "^2.1.0",
    "@isaacs/string-locale-compare": "^1.1.0"
  },
  "devDependencies": {
    "typescript": "^5.0.0"
  }
}
EOF

# Capture output for the PASSED assertion below (a pipe to tee would mask
# packmule's exit status under `set -e`).
"$PACKMULE" -f package.json -t npm -o vendor -b > bundle.log 2>&1 \
    || { cat bundle.log; echo "FAIL: packmule exited non-zero"; exit 1; }
cat bundle.log

[ -f vendor.tar.gz ] || { echo "FAIL: no tarball"; exit 1; }

# The scoped tarball must carry its scope in the filename, the transitive
# dep (ms) must be present exactly once, and typescript must NOT be bundled.
ls vendor/isaacs-string-locale-compare-*.tgz >/dev/null \
    || { echo "FAIL: scoped tarball missing/misnamed"; ls vendor; exit 1; }
[ "$(count_glob vendor/ms-*.tgz)" -eq 1 ] \
    || { echo "FAIL: expected exactly one ms tarball"; ls vendor; exit 1; }
if ls vendor/typescript-*.tgz >/dev/null 2>&1; then
    echo "FAIL: devDependency was bundled"; exit 1
fi

# The build-time offline check must have run and passed.
grep -q "offline install check PASSED" bundle.log \
    || { echo "FAIL: npm offline install check did not pass"; exit 1; }

mkdir extracted
tar -xzf vendor.tar.gz -C extracted

mkdir app
cd app
# devDependencies here prove --omit=dev: resolving typescript would need the
# registry, which is unroutable below.
cat > package.json <<'EOF'
{
  "name": "e2e-app",
  "version": "1.0.0",
  "devDependencies": { "typescript": "^5.0.0" }
}
EOF

cp package.json package.json.orig

# The unroutable registry turns any network access into a hard failure.
npm_config_registry="http://127.0.0.1:9" \
    sh ../extracted/vendor/install.sh

# install.sh hides devDependencies from npm during the install; the user's
# manifest must come back byte-identical.
cmp -s package.json package.json.orig \
    || { echo "FAIL: install.sh did not restore package.json"; exit 1; }

node -e '
const debug = require("debug");
const ms = require("ms");
const slc = require("@isaacs/string-locale-compare");
if (ms(1000) !== "1s") throw new Error("ms broken");
console.log("e2e_npm flat OK");
'

# ── Lockfile bundle: a tree needing TWO versions of debug at once ──────────
# express's chain needs debug@2.6.9 while https-proxy-agent needs debug@4;
# only the lockfile path can represent this (npm nests the second copy).
mkdir "$WORK/lockproj"
cd "$WORK/lockproj"
cat > package.json <<'EOF'
{
  "name": "lockproj",
  "version": "1.0.0",
  "dependencies": {
    "express": "^4.18.0",
    "https-proxy-agent": "^5.0.0"
  },
  "devDependencies": { "typescript": "^5.0.0" }
}
EOF
npm install --package-lock-only --no-audit --no-fund >/dev/null 2>&1

"$PACKMULE" -f package.json -t npm -o vendor -b > bundle.log 2>&1 \
    || { cat bundle.log; echo "FAIL: lockfile bundle exited non-zero"; exit 1; }
cat bundle.log

grep -q "using package-lock.json" bundle.log \
    || { echo "FAIL: sibling lockfile was not preferred"; exit 1; }
grep -q "offline install check PASSED" bundle.log \
    || { echo "FAIL: lock-mode offline install check did not pass"; exit 1; }
[ "$(count_glob vendor/debug-*.tgz)" -eq 2 ] \
    || { echo "FAIL: expected both debug versions bundled"; ls vendor; exit 1; }

mkdir extracted
tar -xzf vendor.tar.gz -C extracted

mkdir app
cd app
cp ../package.json ../package-lock.json .
cp package-lock.json lock.orig

npm_config_registry="http://127.0.0.1:9" \
    sh ../extracted/vendor/install.sh

cmp -s package-lock.json lock.orig \
    || { echo "FAIL: install.sh did not restore package-lock.json"; exit 1; }
[ -f node_modules/https-proxy-agent/node_modules/debug/package.json ] \
    || { echo "FAIL: nested debug@4 missing"; exit 1; }
node -e '
require("express");
require("https-proxy-agent");
const v = require("debug/package.json").version;
if (!v.startsWith("2.6.")) throw new Error("hoisted debug should be 2.6.x, got " + v);
console.log("e2e_npm lockfile OK");
'
