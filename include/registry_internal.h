/*
 * registry_internal.h — backend internals exposed for unit testing.
 *
 * These functions are implementation details of their registry backends and
 * are NOT part of the public interface main.c consumes (that is registry.h).
 * They are non-static solely so the test suite can exercise the pure
 * response-decoding and version-comparison logic without network access.
 */

#ifndef PACKMULE_REGISTRY_INTERNAL_H
#define PACKMULE_REGISTRY_INTERNAL_H

#include "hash.h"
#include "package.h"

/* ── registry_rpm.c ──────────────────────────────────────────────────────── */

typedef struct RpmRepo RpmRepo;

/*
 * RpmConfig — the rpm backend's `Registry.ctx`.
 *
 * The other backends only need the target architecture there, so ctx used to
 * be the arch string itself.  rpm needs more (whether to depsolve, and the
 * indexed repository once it has been fetched), and a struct is the honest
 * way to say so rather than overloading one pointer differently per backend.
 *
 * main.c fills `arch` and `resolve_deps`; the backend fills `repo` after it
 * has downloaded and indexed primary.xml.
 */
typedef struct {
    const char *arch;
    int         resolve_deps;   /* --rpm-deps resolve */
    RpmRepo    *repo;           /* owned by the backend; NULL until indexed */
} RpmConfig;

/* Release anything the rpm backend cached during the run. */
void rpm_backend_cleanup(RpmConfig *cfg);

/* find_rpm_package result codes (0 = success). */
#define RPM_FIND_NOT_FOUND        (-1)
#define RPM_FIND_VERSION_MISMATCH (-2)

/*
 * rpm_vercmp — standard rpmvercmp ordering of two version/release strings.
 * Returns <0, 0, >0 for a < b, a == b, a > b.
 */
int rpm_vercmp(const char *a, const char *b);

/*
 * find_rpm_package — scan a decompressed primary.xml for `name` (+ optional
 * `arch`; noarch always accepted).  `version` NULL selects the highest
 * epoch:ver-rel; non-NULL requires an exact EVR match ("ver", "ver-rel",
 * "epoch:ver", or "epoch:ver-rel").
 *
 * On success returns 0, allocates *out_href and *out_version (caller frees
 * both with pm_free()) and fills *out_digest with whichever checksum the
 * repository publishes (caller clears it with digest_clear()).  Otherwise
 * returns RPM_FIND_NOT_FOUND or RPM_FIND_VERSION_MISMATCH.
 */
int find_rpm_package(const char *primary_xml,
                     const char *name,
                     const char *version,
                     const char *arch,
                     char **out_href,
                     Digest *out_digest,
                     char **out_version);

/* ── registry_npm.c ──────────────────────────────────────────────────────── */

/*
 * npm_parse_response — decode a single npm version document (JSON text) into
 * `pkg`: version, dist.tarball URL, dist.integrity (SHA-512 SRI), filename,
 * and dep_specs entries of the form "name@range".
 * Returns 0 on success, -1 on malformed/insufficient JSON.
 */
int npm_parse_response(const char *json, Package *pkg);

/*
 * npm_effective_lockfile — the lockfile a bundle built from `manifest_path`
 * uses: the manifest itself when it is a package-lock.json/npm-shrinkwrap.json
 * (identified by content, not name), else a valid lock sitting next to it.
 * Returns a heap path (caller frees with pm_free) or NULL when the bundle is
 * built from package.json ranges alone.  main.c copies the returned file into
 * the bundle so install.sh can replay the exact tree with `npm ci --offline`.
 */
char *npm_effective_lockfile(const char *manifest_path);

/* ── registry_pypi.c ─────────────────────────────────────────────────────── */

/*
 * pypi_parse_response — decode a PyPI JSON API response into `pkg`, selecting
 * the best distribution for the given arch / target OS / CPython minor.
 * Returns 0 on success, -1 when no suitable distribution exists.
 */
int pypi_parse_response(const char *json, Package *pkg, const char *arch,
                        const char *target_os, int py_minor);

/*
 * pypi_cached_artifact — path to `filename` inside the run's metadata cache,
 * or NULL when it is not there.
 *
 * On an index without PEP 658 support the backend must download a
 * distribution to read its METADATA.  That is the same file the download phase
 * then wants, so main.c claims it from here rather than fetching it twice.
 * The returned pointer is to static storage, valid until the next call.
 */
const char *pypi_cached_artifact(const char *filename);

/* Remove the metadata cache directory and everything the backend put in it. */
void pypi_backend_cleanup(void);

#endif /* PACKMULE_REGISTRY_INTERNAL_H */
