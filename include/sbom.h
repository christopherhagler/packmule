/*
 * sbom.h — Software Bill of Materials output.
 *
 * A packmule bundle is often the last point at which anyone can see what is
 * about to enter an air-gapped network, so it is the natural place to record
 * an inventory.  The resolved package set already holds everything an SBOM
 * needs — names, exact versions, upstream digests, download URLs and the
 * dependency edges discovered during resolution — so emitting one costs a
 * serialisation pass, not another network round trip.
 *
 * Two formats are supported because consumers are split between them:
 * CycloneDX is what vulnerability tooling (Dependency-Track, Grype, Trivy)
 * ingests, and SPDX is what licence-compliance and procurement processes ask
 * for.  Both are generated from the same data, so asking for both costs
 * nothing but the second file.
 *
 * Every component carries a package URL (purl), which is the identifier every
 * downstream scanner actually matches on; a component without one is inert in
 * most tooling.
 */

#ifndef PACKMULE_SBOM_H
#define PACKMULE_SBOM_H

#include "package.h"

typedef struct Registry Registry;

/* Bit flags: the two formats are independent and may both be requested. */
typedef enum {
    SBOM_NONE      = 0,
    SBOM_CYCLONEDX = 1 << 0,
    SBOM_SPDX      = 1 << 1,
} SbomFormat;

/* Filenames written into the output directory. */
#define SBOM_FILE_CYCLONEDX "sbom.cdx.json"
#define SBOM_FILE_SPDX      "sbom.spdx.json"

/*
 * sbom_parse_format — map a --sbom value ("cyclonedx", "spdx", "both") to a
 * flag set.  Returns SBOM_NONE for an unrecognised value.
 */
int sbom_parse_format(const char *s);

/*
 * sbom_write — emit the requested SBOM documents into `output_dir`.
 *
 * `reg` supplies the registry name (which decides the purl type) and its
 * name-equality rule, which is what lets a dependency specifier be matched
 * back to the component that satisfied it.
 *
 * Packages with no file on disk are skipped, exactly as the bundle manifest
 * skips them: an SBOM must describe what was actually produced.
 *
 * Returns 0 on success, -1 on failure (message printed to stderr).
 */
int sbom_write(const char *output_dir, const Registry *reg,
               const PackageList *packages, int formats);

/*
 * sbom_purl — the package URL for `pkg` under registry `registry_name`,
 * e.g. "pkg:pypi/requests@2.31.0", "pkg:npm/%40babel/core@7.24.0",
 * "pkg:rpm/docker-ce@29.6.1-1.el9?arch=x86_64&epoch=3".
 *
 * Returns a heap string the caller frees with pm_free(), or NULL when the
 * package lacks the name/version needed to identify it.
 */
char *sbom_purl(const char *registry_name, const Package *pkg);

#endif /* PACKMULE_SBOM_H */
