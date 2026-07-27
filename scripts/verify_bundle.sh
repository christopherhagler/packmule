# shellcheck shell=sh
# ── Bundle integrity check ──────────────────────────────────────────────────
# Sourced by every generated install.sh before anything is installed.
#
# The registry digest was checked when the file was downloaded, on a machine
# that had a network.  This checks something different: that the bytes are
# still the bytes after the bundle crossed whatever removable media brought it
# here.  That transfer is the only part of the chain nobody else validates,
# and it is the part an air-gapped workflow leans on hardest.
#
# SHA256SUMS is in coreutils format, so the target needs nothing beyond a
# stock sha256sum/shasum.  If neither exists we say so and stop rather than
# quietly installing unverified files.
packmule_verify() {
    dir="$1"

    if [ ! -f "$dir/SHA256SUMS" ]; then
        echo "install.sh: SHA256SUMS is missing from the bundle -- refusing" >&2
        echo "            to install files that cannot be verified." >&2
        return 1
    fi

    if command -v sha256sum >/dev/null 2>&1; then
        _pm_check() { sha256sum -c --quiet SHA256SUMS; }
    elif command -v shasum >/dev/null 2>&1; then
        _pm_check() { shasum -a 256 -c SHA256SUMS >/dev/null; }
    else
        echo "install.sh: no sha256sum or shasum on this machine -- cannot" >&2
        echo "            verify the bundle.  Install coreutils and retry," >&2
        echo "            or set PACKMULE_SKIP_VERIFY=1 to accept the risk." >&2
        [ "${PACKMULE_SKIP_VERIFY:-0}" = "1" ] && return 0
        return 1
    fi

    echo "install.sh: verifying bundle integrity ..."
    # Run from the bundle directory: SHA256SUMS names files without paths.
    if ( cd "$dir" && _pm_check ); then
        echo "install.sh: bundle verified"
        return 0
    fi

    echo "install.sh: BUNDLE VERIFICATION FAILED -- the files do not match" >&2
    echo "            SHA256SUMS.  Do not install this bundle; transfer it" >&2
    echo "            again from the machine that built it." >&2
    return 1
}
