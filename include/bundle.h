#ifndef PACKMULE_BUNDLE_H
#define PACKMULE_BUNDLE_H

#include "package.h"
#include "verify.h"

typedef struct Registry Registry;

typedef struct {
    const char  *output_dir;
    const char  *registry_name;
    PackageList *packages;

    /*
     * Optional extra file copied into the bundle (and archived) as
     * `aux_name`.  Used by the npm backend to ship the project's
     * package-lock.json so install.sh can replay the exact tree offline.
     * Both NULL when unused.
     */
    const char  *aux_file;
    const char  *aux_name;

    /*
     * SbomFormat flags (see sbom.h).  Generated before SHA256SUMS so the
     * documents are covered by it and travel inside the archive: an inventory
     * that can be swapped out without detection is not much of an inventory.
     */
    int          sbom_formats;

    /*
     * The backend that resolved these packages.  The SBOM needs its name (to
     * choose a purl type) and its name-equality rule (to match a dependency
     * specifier back to the component satisfying it).
     */
    const Registry *registry;

    /*
     * Optional offline install check.
     *
     * Called once the files it needs exist (requirements.txt for pypi,
     * install.sh for npm) but before manifest.json is written, so its verdict
     * can be recorded there and covered by SHA256SUMS — a bundle that says it
     * was checked is then no more forgeable than the packages themselves.
     *
     * The callback fills `rep`; anything else it wants to report back (the
     * result, for the process exit code) it can stash in `ctx`.  NULL disables
     * the check, which is what --no-verify does.
     */
    void (*install_check)(const char *output_dir, void *ctx,
                          BundleCheckReport *rep);
    void  *install_check_ctx;
} BundleOptions;

/*
 * bundle_create — write manifest.json and install.sh into opts->output_dir,
 * then compress the whole directory to <output_dir>.tar.gz.
 * Returns 0 on success, -1 on any failure.
 *
 * A failed install check is not a bundle_create failure: the bundle is still
 * written (the caller may want to inspect it) and the caller decides what the
 * verdict means for the exit code.
 */
int bundle_create(const BundleOptions *opts);

#endif /* PACKMULE_BUNDLE_H */
