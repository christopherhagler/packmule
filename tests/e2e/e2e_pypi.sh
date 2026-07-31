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
