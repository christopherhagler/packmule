/*
 * verify.h — proving a bundle is good before it leaves the network.
 *
 * Two different questions get answered here:
 *
 *   1. "Do the bytes still match?"  — bundle_verify_checksums(), also exposed
 *      as `packmule verify <dir>`.  This is what install.sh runs on the
 *      air-gapped machine, where the bundle has just crossed removable media.
 *
 *   2. "Will the package manager accept this closure offline?" — the pypi and
 *      npm install checks, run right after bundling.  A missing transitive
 *      dependency is invisible to a checksum and fatal on the target, so it
 *      has to be caught here, while there is still a network to fix it with.
 */

#ifndef PACKMULE_VERIFY_H
#define PACKMULE_VERIFY_H

/*
 * bundle_verify_checksums — check every entry of <dir>/SHA256SUMS.
 *
 * Prints one line per failure and a summary.  Returns 0 when every listed
 * file is present and matches, -1 otherwise (including a missing or unreadable
 * SHA256SUMS — a bundle that cannot be checked is not a bundle that passed).
 */
int bundle_verify_checksums(const char *dir);

/*
 * Result of an install check.
 */
typedef enum {
    BUNDLE_CHECK_PASSED = 0,
    BUNDLE_CHECK_FAILED,      /* the tool ran and rejected the bundle */
    BUNDLE_CHECK_SKIPPED,     /* prerequisites absent; nothing was proven */
} BundleCheckResult;

/*
 * bundle_check_pypi — run pip's resolver against only the bundled files
 * (--dry-run --no-index).  Skipped when this machine's os/arch/python differ
 * from the bundle's target (the answer would be about the wrong platform), or
 * when the local pip predates --dry-run (22.2).
 */
BundleCheckResult bundle_check_pypi(const char *output_dir, const char *arch,
                                    const char *host_arch,
                                    const char *target_os, const char *host_os,
                                    int py_minor);

/*
 * bundle_check_npm — run the bundle's own install.sh in a scratch project
 * with the registry pointed at an unroutable address and an empty npm cache,
 * so nothing can be satisfied from this machine's state.  Skipped when npm or
 * node is unavailable.
 */
BundleCheckResult bundle_check_npm(const char *output_dir);

#endif /* PACKMULE_VERIFY_H */
