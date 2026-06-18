/*
 * registry.h — vtable interface for package registry backends.
 *
 * Adding a new registry (e.g. Cargo, Maven, Gem) requires:
 *   1. Implement parse_manifest + resolve in a new src/registry_<name>.c.
 *   2. Define a static `const Registry <name>_registry = { … };`
 *   3. Add an `extern` declaration and a pointer entry in src/registry.c.
 *
 * No other file needs to change.
 */

#ifndef PACKMULE_REGISTRY_H
#define PACKMULE_REGISTRY_H

#include "package.h"

typedef struct Registry Registry;

struct Registry {
    /* Short identifier passed to --type: "pypi", "npm", "rpm", … */
    const char *name;

    /* Default manifest filename shown in --help, e.g. "requirements.txt". */
    const char *manifest_name;

    /*
     * parse_manifest — read the registry-specific manifest file and return
     * a PackageList with name/version populated for each entry.
     *
     * Returns a heap-allocated PackageList (possibly empty) on success.
     * Returns NULL on I/O error; the error is printed to stderr.
     * Caller owns the returned list and must call package_list_destroy().
     */
    PackageList *(*parse_manifest)(const Registry *self, const char *path);

    /*
     * detect — return 1 if `basename` (the manifest filename, no directory
     * component) looks like a manifest this backend understands, else 0.
     *
     * Used by registry_detect() to infer --type when the user does not pass
     * one.  May be NULL for backends that cannot be auto-detected.
     */
    int (*detect)(const char *basename);

    /*
     * resolve — query the upstream registry to fill in pkg->url, pkg->sha256,
     * pkg->filename, and (if previously NULL) pkg->version.
     *
     * On success, any previous values in those fields are freed and replaced
     * with newly heap-allocated strings.
     * Returns 0 on success, -1 on failure (error printed to stderr).
     */
    int (*resolve)(const Registry *self, Package *pkg);

    /*
     * get_deps — enqueue transitive dependencies discovered after resolve().
     *
     * For each dependency implied by `pkg` that is not already present in
     * `seen`, appends a new Package to `out`.  The registry is responsible
     * for all format-specific filtering (e.g. extras-only entries for PyPI).
     * `seen` and `out` may point to the same list.
     *
     * Returns the number of packages added, or -1 on internal error.
     * May be NULL for registries that do not support transitive resolution.
     */
    int (*get_deps)(const Registry *self, const Package *pkg,
                    const PackageList *seen, PackageList *out);

    /*
     * ctx — optional opaque per-instance configuration injected by the caller.
     * main.c uses this to pass the target CPU architecture string to backends
     * that need it (e.g. pypi).  NULL for backends that don't use it.
     * The backend must NOT free this pointer.
     */
    void *ctx;

    /*
     * repo_url — base URL of the upstream repository for backends that fetch
     * from a configurable endpoint (e.g. RPM repos).  Set by main.c from the
     * -u / --repo-url flag.  NULL for registries with a fixed public endpoint
     * (PyPI, npm).  Points to argv memory; must NOT be freed by the backend.
     */
    const char *repo_url;
};

/*
 * registry_find — look up a backend by its name string.
 *
 * Returns a pointer to a statically-allocated Registry, or NULL if the name
 * is not recognised.  The returned pointer must NOT be freed.
 */
const Registry *registry_find(const char *name);

/*
 * registry_detect — infer a backend from a manifest file path by asking each
 * backend's detect() hook about the path's basename.
 *
 * Returns a pointer to a statically-allocated Registry, or NULL if no backend
 * recognises the filename.  The returned pointer must NOT be freed.
 */
const Registry *registry_detect(const char *path);

/*
 * registry_names — return a NULL-terminated array of all registered names.
 * Both the array and the strings are statically allocated; do not free.
 */
const char *const *registry_names(void);

#endif /* PACKMULE_REGISTRY_H */
