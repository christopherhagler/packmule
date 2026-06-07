#ifndef PACKMULE_BUNDLE_H
#define PACKMULE_BUNDLE_H

#include "package.h"

typedef struct {
    const char  *output_dir;
    const char  *registry_name;
    PackageList *packages;
} BundleOptions;

/*
 * bundle_create — write manifest.json and install.sh into opts->output_dir,
 * then compress the whole directory to <output_dir>.tar.gz.
 * Returns 0 on success, -1 on any failure.
 */
int bundle_create(const BundleOptions *opts);

#endif /* PACKMULE_BUNDLE_H */
