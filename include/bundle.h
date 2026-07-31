#ifndef PACKMULE_BUNDLE_H
#define PACKMULE_BUNDLE_H

#include "package.h"

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
} BundleOptions;

/*
 * bundle_create — write manifest.json and install.sh into opts->output_dir,
 * then compress the whole directory to <output_dir>.tar.gz.
 * Returns 0 on success, -1 on any failure.
 */
int bundle_create(const BundleOptions *opts);

#endif /* PACKMULE_BUNDLE_H */
