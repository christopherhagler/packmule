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

/*
 * Which flavour of index the pypi backend should talk to.
 *
 * pypi.org serves a JSON API that carries dependency metadata directly.
 * Private indexes (JFrog Artifactory, Sonatype Nexus, devpi, GitLab) serve
 * the PEP 503 simple API instead, which is HTML and carries no dependency
 * information — see registry_pypi.c for how that gap is filled.
 */
typedef enum {
    PYPI_INDEX_AUTO = 0,  /* simple when --repo-url is set, else json */
    PYPI_INDEX_JSON,
    PYPI_INDEX_SIMPLE,
} PypiIndexMode;

struct Registry {
    /* Short identifier passed to --type: "pypi", "npm", "rpm", … */
    const char *name;

    /* Default manifest filename shown in --help, e.g. "requirements.txt". */
    const char *manifest_name;

    /*
     * name_equal — this registry's package-name identity rule.  Required.
     * See package_name_equal_fn in package.h for why this is per-registry.
     */
    package_name_equal_fn name_equal;

    /*
     * parse_manifest — read the registry-specific manifest file and return
     * a PackageList with name and, where the manifest is explicit, version /
     * constraint / extras populated for each entry.
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
     * resolve — select a concrete version for `pkg` and fill in pkg->url,
     * pkg->digest, pkg->filename, pkg->version and pkg->dep_specs.
     *
     * The resolver calls this again whenever pkg->dirty is set — a later
     * dependent narrowed pkg->constraint — so an implementation must be
     * idempotent and must honour pkg->constraint on every call.  It must not
     * change pkg->version when pkg->user_pinned is set.
     *
     * On success, previous values in those fields are freed and replaced.
     * Returns 0 on success, -1 on failure (error printed to stderr).
     */
    int (*resolve)(const Registry *self, Package *pkg);

    /*
     * get_deps — record the transitive dependencies discovered by resolve().
     *
     * For each dependency implied by `pkg`: append a new Package to `out` if
     * it is not already in `seen`, or merge the requirement into the existing
     * entry (narrowing its constraint, unioning its extras) if it is.  When a
     * merge widens what an already-resolved package must satisfy, the entry
     * must be marked dirty so the resolver revisits it.  The registry is
     * responsible for all format-specific filtering (e.g. extras-only entries
     * for PyPI).  `seen` and `out` may point to the same list.
     *
     * Returns the number of packages added, or -1 on an error that makes the
     * bundle unbuildable (e.g. an npm git dependency).
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

    /*
     * py_minor — target CPython 3.x minor version for wheel selection (e.g. 12
     * for Python 3.12).  Set by main.c from --python or auto-detected from the
     * local python3.  0 means "no Python target": fall back to arch-only
     * matching.  Only the pypi backend reads this; wheels are interpreter- and
     * ABI-specific, so a wheel built for a different CPython minor will not
     * install on the target.
     */
    int py_minor;

    /*
     * target_os — operating-system family for wheel selection: "linux",
     * "macos", or "windows".  Set by main.c from --os or auto-detected from the
     * host (uname).  NULL means "no OS preference" (arch-only matching).  Only
     * the pypi backend reads it; a wheel's platform tag (manylinux/macosx/win)
     * must match the install target's OS or pip will refuse it.  Points to
     * static/argv memory; must NOT be freed by the backend.
     */
    const char *target_os;

    /*
     * index_mode — which PyPI index API to speak.  Set by main.c from
     * --index; only the pypi backend reads it.  PYPI_INDEX_AUTO resolves to
     * the simple API whenever repo_url is set, because a custom -u is nearly
     * always a private index and the JSON API is a pypi.org extension that
     * most of them do not implement.
     */
    PypiIndexMode index_mode;
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
