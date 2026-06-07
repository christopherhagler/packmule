/*
 * package.h — Core data structures for a resolved Python package.
 *
 * Ownership: all string fields inside Package are heap-allocated and owned
 * by the Package itself.  Always use package_destroy() to free a Package —
 * never free individual fields separately.
 */

#ifndef PACKMULE_PACKAGE_H
#define PACKMULE_PACKAGE_H

#include <stddef.h>

/*
 * Package — a single Python distribution to be downloaded.
 *
 * Fields are populated in stages:
 *   1. name + version are set at parse time (from requirements.txt).
 *   2. url, sha256, filename are filled in after querying the PyPI JSON API.
 *   3. requires_dist is populated from info.requires_dist for transitive resolution.
 */
typedef struct {
    char *name;            /* Normalised distribution name, e.g. "requests" */
    char *version;         /* Resolved version string, e.g. "2.31.0".  May be NULL
                              until dependency resolution completes. */
    char *url;             /* Download URL for the chosen wheel or sdist. */
    char *sha256;          /* Expected SHA-256 hex digest (64 chars + NUL). */
    char *filename;        /* Basename of the downloaded file. */
    char **requires_dist;  /* NULL-terminated array of PEP 508 dep strings, or NULL. */
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

/* Allocate a Package with the given name (and optional version).
 * Caller must eventually call package_destroy().
 * `version` may be NULL.
 */
Package *package_create(const char *name, const char *version);

/* Free a Package and all its owned string fields.  Safe with NULL. */
void package_destroy(Package *pkg);

/* Allocate an empty PackageList.  Caller must call package_list_destroy(). */
PackageList *package_list_create(void);

/*
 * Append `pkg` to `list`, transferring ownership of `pkg` to the list.
 * Returns 0 on success.
 */
int package_list_add(PackageList *list, Package *pkg);

/* Returns 1 if a package with the same name+version already exists. */
int package_list_contains(const PackageList *list,
                           const char *name,
                           const char *version);

/* Returns 1 if any package with the given name exists (case-insensitive). */
int package_list_contains_name(const PackageList *list, const char *name);

/* Free all packages in `list` and the list itself.  Safe with NULL. */
void package_list_destroy(PackageList *list);

#endif /* PACKMULE_PACKAGE_H */
