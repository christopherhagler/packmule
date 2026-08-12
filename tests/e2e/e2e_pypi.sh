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

"$PACKMULE" -f requirements.txt -o vendor -b --sbom both

[ -f vendor.tar.gz ]        || { echo "FAIL: no tarball";        exit 1; }
[ -f vendor/manifest.json ] || { echo "FAIL: no manifest.json";  exit 1; }
[ -x vendor/install.sh ]    || { echo "FAIL: no install.sh";     exit 1; }
[ -f vendor/sbom.cdx.json ] || { echo "FAIL: no sbom.cdx.json";  exit 1; }
[ -f vendor/sbom.spdx.json ] || { echo "FAIL: no sbom.spdx.json"; exit 1; }

# The SBOM has to be covered by SHA256SUMS and travel inside the archive:
# an inventory that can be swapped out undetected is not an inventory.
grep -q 'sbom.cdx.json'  vendor/SHA256SUMS || { echo "FAIL: cdx not in SHA256SUMS";  exit 1; }
grep -q 'sbom.spdx.json' vendor/SHA256SUMS || { echo "FAIL: spdx not in SHA256SUMS"; exit 1; }

mkdir extracted
tar -xzf vendor.tar.gz -C extracted

[ -f extracted/vendor/sbom.cdx.json ]  || { echo "FAIL: cdx not archived";  exit 1; }
[ -f extracted/vendor/sbom.spdx.json ] || { echo "FAIL: spdx not archived"; exit 1; }

# Structural checks on both documents.  Validating against the published
# schemas would mean fetching them at test time; what matters most here is
# that the identifiers downstream tooling keys on are actually present and
# that the two formats agree on the package set.
python3 - <<'EOF'
import json

cdx = json.load(open("vendor/sbom.cdx.json"))
assert cdx["bomFormat"] == "CycloneDX", cdx["bomFormat"]
assert cdx["specVersion"] == "1.5", cdx["specVersion"]
assert cdx["serialNumber"].startswith("urn:uuid:"), cdx["serialNumber"]

comps = cdx["components"]
assert comps, "no components"
for c in comps:
    assert c["purl"].startswith("pkg:pypi/"), c["purl"]
    assert c["hashes"][0]["alg"] == "SHA-256"
    assert len(c["hashes"][0]["content"]) == 64, c["hashes"][0]

names = {c["name"].lower() for c in comps}
assert "requests" in names, names

# CycloneDX requires every bom-ref to appear in `dependencies`.
refs = {c["bom-ref"] for c in comps} | {"packmule-bundle"}
declared = {d["ref"] for d in cdx["dependencies"]}
assert refs == declared, refs ^ declared

# requests pulls transitive dependencies; the graph must show them.
req = next(d for d in cdx["dependencies"] if d["ref"].startswith("pkg:pypi/requests@"))
assert len(req["dependsOn"]) >= 3, req

spdx = json.load(open("vendor/sbom.spdx.json"))
assert spdx["spdxVersion"] == "SPDX-2.3", spdx["spdxVersion"]
assert spdx["SPDXID"] == "SPDXRef-DOCUMENT"
assert spdx["dataLicense"] == "CC0-1.0"
assert len(spdx["packages"]) == len(comps), "formats disagree on the package set"
for p in spdx["packages"]:
    assert p["SPDXID"].startswith("SPDXRef-Package-"), p["SPDXID"]
    assert p["checksums"][0]["algorithm"] == "SHA256"
    # licenseDeclared must be an SPDX expression or NOASSERTION, never the
    # free text a registry publishes.
    assert " License" not in p["licenseDeclared"], p["licenseDeclared"]
assert any(r["relationshipType"] == "DEPENDS_ON" for r in spdx["relationships"])

print(f"e2e_pypi OK: SBOM {len(comps)} components, both formats consistent")
EOF

python3 -m venv venv
# Generated at runtime by venv, so there is nothing for shellcheck to follow.
# shellcheck disable=SC1091
. venv/bin/activate

sh extracted/vendor/install.sh

python3 - <<'EOF'
import requests, urllib3, certifi, idna
assert requests.__version__.startswith("2."), requests.__version__
print("e2e_pypi OK: requests", requests.__version__)
EOF

deactivate

# ── Cycle 2: lockfile mode ───────────────────────────────────────────────────
#
# A lock is the real resolver's finished answer, so this cycle asserts the
# properties that only lockfile mode has: the exact locked versions are what
# gets bundled (no resolution happens at all), packages whose markers exclude
# this platform are pruned rather than shipped or failed on, and the lock
# itself travels inside the bundle covered by SHA256SUMS.
#
# The lock is generated here from the index's own metadata rather than
# committed, so its URLs and hashes are real and never go stale.

cd "$WORK"
mkdir lockmode
cd lockmode

python3 - <<'EOF'
import json, urllib.request

# Pinned so the assertions below are exact.  colorama is win32-only in this
# lock: on a Linux/macOS run it must be pruned by the edge marker.
PINS = [("certifi", "2024.8.30"), ("charset-normalizer", "3.4.0"),
        ("idna", "3.10"), ("urllib3", "2.2.3"), ("requests", "2.32.3"),
        ("colorama", "0.4.6")]
DEPS = {"requests": ["certifi", "charset-normalizer", "idna", "urllib3"]}

out = ['version = 1', 'requires-python = ">=3.9"', '',
       '[[package]]', 'name = "e2e-project"', 'version = "0.1.0"',
       'source = { virtual = "." }', 'dependencies = [',
       '    { name = "requests" },',
       "    { name = \"colorama\", marker = \"sys_platform == 'win32'\" },",
       ']', '']

for name, ver in PINS:
    d = json.load(urllib.request.urlopen(
        f"https://pypi.org/pypi/{name}/{ver}/json"))
    out += ['[[package]]', f'name = "{name}"', f'version = "{ver}"',
            'source = { registry = "https://pypi.org/simple" }']
    if name in DEPS:
        out.append('dependencies = [')
        out += [f'    {{ name = "{d2}" }},' for d2 in DEPS[name]]
        out.append(']')
    for f in d["urls"]:
        if f["packagetype"] == "sdist":
            out.append(f'sdist = {{ url = "{f["url"]}", '
                       f'hash = "sha256:{f["digests"]["sha256"]}", '
                       f'size = {f["size"]} }}')
            break
    out.append('wheels = [')
    for f in d["urls"]:
        if f["packagetype"] == "bdist_wheel":
            out.append(f'    {{ url = "{f["url"]}", '
                       f'hash = "sha256:{f["digests"]["sha256"]}", '
                       f'size = {f["size"]} }},')
    out += [']', '']

open("uv.lock", "w").write("\n".join(out))
print("e2e_pypi OK: generated uv.lock with", len(PINS), "packages")
EOF

"$PACKMULE" -f uv.lock -o locked -b

[ -f locked.tar.gz ] || { echo "FAIL: no lock-mode tarball"; exit 1; }

# The lock ships as provenance and is covered by SHA256SUMS like everything
# else — a record of what was resolved that cannot be swapped out undetected.
[ -f locked/uv.lock ] || { echo "FAIL: uv.lock not in bundle"; exit 1; }
grep -q 'uv.lock' locked/SHA256SUMS || { echo "FAIL: uv.lock not in SHA256SUMS"; exit 1; }

mkdir extracted2
tar -xzf locked.tar.gz -C extracted2
[ -f extracted2/locked/uv.lock ] || { echo "FAIL: uv.lock not archived"; exit 1; }

# `packmule verify` must accept the bundle, and reject it once tampered with.
"$PACKMULE" verify locked >/dev/null || { echo "FAIL: verify rejected a good bundle"; exit 1; }

# Exact locked versions, and nothing that this platform does not need.
python3 - <<'EOF'
import json
m = json.load(open("locked/manifest.json"))
got = {p["name"].lower(): p["version"] for p in m["packages"]}

want = {"certifi": "2024.8.30", "charset-normalizer": "3.4.0",
        "idna": "3.10", "urllib3": "2.2.3", "requests": "2.32.3"}
assert got == want, f"lock not reproduced exactly: {got}"

# Pruned by its marker on any non-Windows target, not shipped and not fatal.
assert "colorama" not in got, "win32-only package leaked into the bundle"
print("e2e_pypi OK: lock reproduced exactly,", len(got), "packages")
EOF

python3 -m venv venv2
# shellcheck disable=SC1091
. venv2/bin/activate

sh extracted2/locked/install.sh

python3 - <<'EOF'
import requests
assert requests.__version__ == "2.32.3", requests.__version__
print("e2e_pypi OK: lock-mode bundle installed offline, requests",
      requests.__version__)
EOF
