/*
 * package.h — Core data structures for a package moving through the pipeline.
 *
 * Ownership: all heap fields inside Package are owned by the Package itself.
 * Always use package_destroy() to free a Package — never free individual
 * fields separately.
 */

#ifndef PACKMULE_PACKAGE_H
#define PACKMULE_PACKAGE_H

#include <stddef.h>

#include "hash.h"

/*
 * Where a package is in the resolve loop.  The resolver is a fixpoint: it
 * revisits a package whenever a newly discovered dependent widens what that
 * package has to satisfy, so "have we resolved this yet" and "is what we
 * resolved still valid" must both be representable.
 */
typedef enum {
    PKG_QUEUED = 0,   /* known, not yet resolved */
    PKG_RESOLVED,     /* version/url/digest/filename populated */
    PKG_FAILED,       /* resolution attempted and failed; do not retry */
} PackageState;

/*
 * Package — a single distribution to be downloaded.
 *
 * The split between `version` and `constraint` is load-bearing: `version` is
 * only ever a single concrete version (either the user's exact pin or the one
 * resolution selected), while `constraint` holds the range requirement in the
 * registry's own syntax (PEP 440 for pypi, node-semver for npm).  Keeping a
 * range in `version` — as the npm backend once did — makes it impossible to
 * tell a request from a decision.
 */
typedef struct {
    char *name;        /* Package name, e.g. "requests" */
    char *version;     /* Concrete version, or NULL until resolved.
                          Never a range — see `constraint`. */
    char *constraint;  /* Version range requirement (">=1.20,<2.0", "^4.18.0"),
                          or NULL.  Accumulated from every dependent; consumed
                          by resolve() to pick a satisfying version. */
    char *extras;      /* Comma-separated requested extras ("standard"),
                          lowercase, or NULL.  Consumed by get_deps() to admit
                          extras-gated dependencies. */
    char *url;         /* Download URL for the chosen artifact. */
    Digest digest;     /* Expected digest of the artifact at `url`. */
    char *filename;    /* Basename the artifact is stored under. */
    char **dep_specs;  /* NULL-terminated array of registry dep specifiers, or
                          NULL.  Populated by resolve(); read by get_deps(). */
    char *license;     /* Licence as the registry declares it, or NULL when it
                          publishes none.  Free text, not a validated SPDX
                          identifier — registries emit everything from
                          "BSD-3-Clause" to "Apache 2.0".  Consumed only by the
                          SBOM writer, which is why nothing here interprets it. */

    PackageState state;
    int  user_pinned;  /* 1 when `version` came from the manifest rather than
                          from resolution: the resolver must never revise it. */
    int  dirty;        /* 1 when constraint/extras widened after this package
                          was resolved, so it needs resolving again. */
    int  resolve_count;/* Resolution attempts, to bound pathological churn. */
} Package;

/*
 * PackageList — a growable array of Package pointers.
 *
 * Ownership: the list owns each Package * in items.
 * package_list_destroy() frees the packages and the list itself.
 */
typedef struct {
    Package **items;
    size_t    count;
    size_t    capacity;
} PackageList;

/*
 * package_name_equal_fn — registry-supplied name equality.
 *
 * Name identity is a registry rule, not a universal one: PyPI folds case and
 * treats '-', '_' and '.' as equivalent (PEP 503), while on npm and in RPM
 * repositories those characters distinguish genuinely different packages
 * (npm's "lodash.merge" is not "lodash-merge").  Lookups take the comparator
 * so the shared container never imposes one registry's rule on another.
 */
typedef int (*package_name_equal_fn)(const char *a, const char *b);

/* PEP 503 name equality: case-insensitive with '-', '_', '.' equivalent. */
int package_name_equal_pep503(const char *a, const char *b);

/* Case-insensitive exact equality (npm). */
int package_name_equal_casefold(const char *a, const char *b);

/* Byte-exact equality (rpm). */
int package_name_equal_exact(const char *a, const char *b);

/* Allocate a Package with the given name (and optional exact version).
 * Caller must eventually call package_destroy().  `version` may be NULL. */
Package *package_create(const char *name, const char *version);

/* Free a Package and all its owned fields.  Safe with NULL. */
void package_destroy(Package *pkg);

/* Replace pkg->dep_specs with `specs` (a NULL-terminated array the package
 * takes ownership of), freeing any previous contents.  `specs` may be NULL. */
void package_set_dep_specs(Package *pkg, char **specs);

/*
 * package_set_constraint — install `constraint` (a heap string the Package
 * takes ownership of, or NULL) as pkg->constraint.
 *
 * When the value actually changes and the package was already resolved, the
 * package is marked dirty so the resolver revisits it: a constraint that
 * arrives after resolution is exactly the case a single-pass resolver gets
 * wrong.  Returns 1 when the constraint changed, 0 when it was already that.
 */
int package_set_constraint(Package *pkg, char *constraint);

/*
 * package_add_extras — union `extras` (comma-separated) into pkg->extras,
 * comparing whole tokens so "sec" is not swallowed by "security".  Marks the
 * package dirty if it had already been resolved without them, since extras
 * gate which dependencies get followed.  Returns 1 when anything was added.
 */
int package_add_extras(Package *pkg, const char *extras);

/* Allocate an empty PackageList.  Caller must call package_list_destroy(). */
PackageList *package_list_create(void);

/*
 * Append `pkg` to `list`, transferring ownership of `pkg` to the list.
 * Returns 0 on success.
 */
int package_list_add(PackageList *list, Package *pkg);

/*
 * Return the first package whose name matches under `eq`, or NULL.
 * The returned pointer is owned by the list; do not free it.
 */
Package *package_list_find_name(const PackageList *list, const char *name,
                                package_name_equal_fn eq);

/* Returns 1 if any package matches `name` under `eq`. */
int package_list_contains_name(const PackageList *list, const char *name,
                               package_name_equal_fn eq);

/* Free all packages in `list` and the list itself.  Safe with NULL. */
void package_list_destroy(PackageList *list);

#endif /* PACKMULE_PACKAGE_H */
