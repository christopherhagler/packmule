/*
 * pylock.c — uv.lock and PEP 751 pylock.toml readers.
 *
 * The two formats answer the same question in different shapes.  pylock.toml
 * is already flattened: every package carries its own marker, so choosing the
 * set for a target is a filter.  uv.lock is a graph locked for every platform
 * at once, so the set is whatever is reachable from the workspace root once
 * each edge's marker has been evaluated — bundling the whole file instead
 * would ship Windows-only packages into a Linux bundle and then fail the build
 * because none of them has a Linux artifact.
 *
 * Everything downstream of that difference is shared: pick the best artifact
 * for the target, take the URL and hash the lock already recorded, and hand
 * back packages the resolver will not touch.
 */

#include "pylock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hash.h"
#include "pep440.h"
#include "pep508.h"
#include "toml.h"
#include "utils.h"
#include "wheeltag.h"

/* ── Lock entries ────────────────────────────────────────────────────────── */

/*
 * One [[package]] / [[packages]] table, plus what the walk learns about it.
 *
 * `tv` and the two name pointers are borrowed from the parsed TOML tree, which
 * outlives the walk; only `extras` is owned here.
 */
typedef struct {
    const TomlValue *tv;
    const char      *name;
    const char      *version;
    char            *extras;    /* comma-separated, accumulated by the walk */
    int              is_local;  /* the workspace project itself, not shippable */
    int              excluded;  /* ruled out by its own markers */
    int              reachable;
} Entry;

typedef struct {
    Entry *items;
    size_t count;
    size_t capacity;
} EntryList;

static void entries_push(EntryList *el, const Entry *e)
{
    if (el->count == el->capacity) {
        el->capacity = el->capacity ? el->capacity * 2 : 32;
        el->items = pm_realloc(el->items, el->capacity * sizeof(*el->items));
    }
    el->items[el->count++] = *e;
}

static void entries_free(EntryList *el)
{
    for (size_t i = 0; i < el->count; i++)
        pm_free(el->items[i].extras);
    pm_free(el->items);
}

/*
 * extras_add — union one extra name into a comma-separated list, comparing
 * whole tokens so "sec" is not swallowed by "security".  Returns 1 when the
 * list grew, which is what drives the walk to revisit a package whose optional
 * dependencies have just become relevant.
 */
static int extras_add(char **list, const char *extra)
{
    if (!extra || !*extra)
        return 0;

    if (*list) {
        size_t want = strlen(extra);
        for (const char *p = *list; p && *p;) {
            const char *comma = strchr(p, ',');
            size_t      len   = comma ? (size_t)(comma - p) : strlen(p);
            if (len == want && strncmp(p, extra, want) == 0)
                return 0;
            p = comma ? comma + 1 : NULL;
        }
        char *joined = pm_asprintf("%s,%s", *list, extra);
        pm_free(*list);
        *list = joined;
    } else {
        *list = pm_strdup(extra);
    }
    return 1;
}

/*
 * find_entry — the entry for `name` that applies to this target.
 *
 * Sets *ambiguous (and *other) when several entries for one name all apply,
 * which uv produces when a package is locked at different versions for
 * different environments and the target matches more than one of them.
 */
static Entry *find_entry(EntryList *el, const char *name, int *ambiguous,
                         Entry **other)
{
    Entry *found = NULL;
    *ambiguous   = 0;

    for (size_t i = 0; i < el->count; i++) {
        if (el->items[i].excluded ||
            !package_name_equal_pep503(el->items[i].name, name))
            continue;
        if (found) {
            /* Two entries for one name, both applicable here.  Returning
             * either would silently install a version the lock does not
             * unambiguously call for, so say so instead. */
            *ambiguous = 1;
            *other     = &el->items[i];
            return found;
        }
        found = &el->items[i];
    }
    return found;
}

/* ── Artifacts ───────────────────────────────────────────────────────────── */

typedef struct {
    const char *url;
    char       *filename;   /* owned */
    Digest      digest;     /* owned */
} Artifact;

/*
 * url_filename — the basename a URL's artifact is stored under, with any query
 * string or fragment removed.  PyPI download URLs sometimes carry a
 * "#sha256=…" fragment, which is part of the reference, not of the name.
 */
static char *url_filename(const char *url)
{
    size_t len = strcspn(url, "?#");
    while (len > 0 && url[len - 1] == '/')
        len--;

    const char *start = url;
    for (size_t i = 0; i < len; i++)
        if (url[i] == '/')
            start = url + i + 1;

    size_t n = len - (size_t)(start - url);
    if (n == 0)
        return NULL;
    /* pm_basename is applied again by the download path; doing it here too
     * keeps a hostile lock from proposing a name with a directory in it. */
    char       *raw  = pm_strndup(start, n);
    const char *safe = pm_basename(raw);
    if (safe != raw) {
        char *trimmed = pm_strdup(safe);
        pm_free(raw);
        return trimmed;
    }
    return raw;
}

/*
 * artifact_digest — read a file table's hash.  uv writes `hash = "sha256:…"`;
 * PEP 751 writes `hashes = { sha256 = "…" }` and permits several algorithms,
 * of which the strongest we support wins.  Returns 0 on success.
 */
static int artifact_digest(const TomlValue *file, Digest *out)
{
    const char *flat = toml_table_string(file, "hash");
    if (flat) {
        const char *colon = strchr(flat, ':');
        if (!colon)
            return -1;
        char      *algo_s = pm_strndup(flat, (size_t)(colon - flat));
        DigestAlgo algo   = digest_algo_from_name(algo_s);
        pm_free(algo_s);
        if (algo == DIGEST_NONE || !colon[1])
            return -1;
        digest_set(out, algo, DIGEST_ENC_HEX, colon + 1);
        return 0;
    }

    const TomlValue *hashes = toml_get(file, "hashes");
    if (!hashes)
        return -1;
    static const char *const preferred[] = { "sha512", "sha256" };
    for (size_t i = 0; i < sizeof(preferred) / sizeof(preferred[0]); i++) {
        const char *hex = toml_table_string(hashes, preferred[i]);
        if (hex && *hex) {
            digest_set(out, digest_algo_from_name(preferred[i]),
                       DIGEST_ENC_HEX, hex);
            return 0;
        }
    }
    return -1;
}

/*
 * wheel_priority — how much we want this wheel on the target, using the same
 * ordering as the rest of the pypi backend: an arch-specific wheel (3) beats a
 * universal one (2), and 0 means it will not install here at all.  `*glibc`
 * receives the manylinux floor so the widest-compatibility wheel can win ties.
 */
static int wheel_priority(const char *filename, const char *arch,
                          const char *target_os, int py_minor, int *glibc)
{
    *glibc = 0;

    WheelTags tags;
    if (!dist_is_wheel(filename) || wheel_parse_tags(filename, &tags) != 0)
        return 0;
    if (!wheel_python_matches(filename, py_minor))
        return 0;

    if (strcmp(tags.platform, "any") == 0)
        return 2;
    if (!arch || !wheel_platform_matches(tags.platform, target_os, arch))
        return 0;

    int maj = 0, min = 0;
    if (wheel_manylinux_glibc(tags.platform, &maj, &min))
        *glibc = maj * 1000 + min;
    return 3;
}

/*
 * file_name_of — the filename a lock's file table describes.  PEP 751 records
 * it explicitly; uv records only the URL, whose last segment is the name PyPI
 * serves the file under.  Returns a heap string, or NULL when the table has
 * no usable URL.
 */
static char *file_name_of(const TomlValue *file, const char **out_url)
{
    const char *url = toml_table_string(file, "url");
    *out_url = url;

    const char *declared = toml_table_string(file, "name");
    if (declared && *declared)
        return pm_strdup(pm_basename(declared));
    if (!url)
        return NULL;
    return url_filename(url);
}

/*
 * select_artifact — choose what to ship for one locked package.
 *
 * Returns 0 with `out` filled, or -1 when nothing in the lock installs on this
 * target.  A source distribution is accepted only as a last resort and says so,
 * because it moves a build onto the machine that cannot download anything.
 */
static int select_artifact(const TomlValue *entry, const char *name,
                           const char *version, const char *arch,
                           const char *target_os, int py_minor, Artifact *out)
{
    const TomlValue *wheels = toml_get(entry, "wheels");
    size_t           n      = toml_array_len(wheels);

    const TomlValue *best     = NULL;
    char            *best_fn  = NULL;
    const char      *best_url = NULL;
    int              best_pri = 0;
    int              best_gl  = 0;
    int              saw_wheel = 0;

    for (size_t i = 0; i < n; i++) {
        const TomlValue *w   = toml_array_at(wheels, i);
        const char      *url = NULL;
        char            *fn  = file_name_of(w, &url);
        if (!fn || !url) {
            pm_free(fn);
            continue;
        }
        saw_wheel = 1;

        int gl  = 0;
        int pri = wheel_priority(fn, arch, target_os, py_minor, &gl);
        if (pri > best_pri || (pri == best_pri && pri == 3 && gl < best_gl)) {
            pm_free(best_fn);
            best     = w;
            best_fn  = fn;
            best_url = url;
            best_pri = pri;
            best_gl  = gl;
        } else {
            pm_free(fn);
        }
    }

    if (!best) {
        const TomlValue *sdist = toml_get(entry, "sdist");
        /* NULL-initialised because file_name_of() is not called at all when
         * there is no sdist: the short-circuit below would otherwise be the
         * only thing keeping this from being read uninitialised. */
        const char      *url = NULL;
        char            *fn  = sdist ? file_name_of(sdist, &url) : NULL;
        if (fn && url) {
            fprintf(stderr,
                    "packmule: warning: no compatible wheel for %s==%s in the "
                    "lock; bundling source distribution %s\n"
                    "          (the target machine must be able to build it: "
                    "python3 headers, and a compiler if it has C extensions)\n",
                    name, version, fn);
            best     = sdist;
            best_fn  = fn;
            best_url = url;
        } else {
            pm_free(fn);
            fprintf(stderr,
                    "packmule: %s==%s has no distribution that installs on "
                    "%s/%s (python 3.%d).\n",
                    name, version, target_os ? target_os : "any",
                    arch ? arch : "any", py_minor);
            fprintf(stderr, saw_wheel
                    ? "          The lock has wheels, but none for this "
                      "target.  Re-lock covering it, or adjust "
                      "--os/--arch/--python.\n"
                    : "          The lock records no artifact for it at all.\n");
            return -1;
        }
    }

    Digest digest;
    memset(&digest, 0, sizeof(digest));
    if (artifact_digest(best, &digest) != 0) {
        fprintf(stderr,
                "packmule: %s==%s has no usable hash in the lock; refusing to "
                "bundle an unverifiable file.\n", name, version);
        pm_free(best_fn);
        return -1;
    }

    out->url      = best_url;
    out->filename = best_fn;
    out->digest   = digest;
    return 0;
}

/* ── uv.lock ─────────────────────────────────────────────────────────────── */

/*
 * uv_source_kind — classify a uv `source` table.  Returns the key naming the
 * source ("registry", "virtual", "git", …), or NULL when there is none.
 */
static const char *uv_source_kind(const TomlValue *entry)
{
    const TomlValue *src = toml_get(entry, "source");
    if (!src || src->type != TOML_TABLE || src->u.table.count == 0)
        return NULL;
    return src->u.table.keys[0];
}

/*
 * markers_exclude_all — 1 when every one of a package's `resolution-markers`
 * rules it out.  uv emits these when one name is locked at several versions
 * for different environments; the entry that survives is the one for ours.
 * No markers means the entry applies everywhere.
 */
static int markers_exclude_all(const TomlValue *entry, const char *target_os,
                               int py_minor)
{
    const TomlValue *rm = toml_get(entry, "resolution-markers");
    size_t           n  = toml_array_len(rm);
    if (n == 0)
        return 0;

    for (size_t i = 0; i < n; i++) {
        const char *m = toml_string(toml_array_at(rm, i));
        if (!m || !pep508_marker_excludes(m, target_os, py_minor))
            return 0;
    }
    return 1;
}

/*
 * uv_visit_edges — follow one package's dependency edges, marking targets
 * reachable and propagating requested extras.  Returns 1 when anything
 * changed, so the caller can iterate to a fixpoint: an extra discovered late
 * can pull in dependencies that were not relevant on the first pass.
 */
static int uv_visit_edges(EntryList *el, const TomlValue *deps,
                          const char *target_os, int py_minor, const char *from,
                          int *fatal)
{
    int    changed = 0;
    size_t n       = toml_array_len(deps);

    for (size_t i = 0; i < n; i++) {
        const TomlValue *edge = toml_array_at(deps, i);
        const char      *name = toml_table_string(edge, "name");
        if (!name)
            continue;

        const char *marker = toml_table_string(edge, "marker");
        if (marker && pep508_marker_excludes(marker, target_os, py_minor))
            continue;

        int    ambiguous = 0;
        Entry *other     = NULL;
        Entry *target    = find_entry(el, name, &ambiguous, &other);

        if (ambiguous) {
            fprintf(stderr,
                    "packmule: the lock has %s at both %s and %s for this "
                    "target.\n          pip can install only one; narrow the "
                    "target with --python/--os/--arch.\n",
                    name, target->version ? target->version : "?",
                    other->version ? other->version : "?");
            *fatal = 1;
            return changed;
        }
        if (!target) {
            /* Distinguish "absent" from "present but ruled out": the second
             * means the lock covers this package only for environments other
             * than the target, which is a targeting problem, not a corrupt
             * lock, and the fix is completely different. */
            int shadowed = 0;
            for (size_t j = 0; j < el->count && !shadowed; j++)
                shadowed = el->items[j].excluded &&
                           package_name_equal_pep503(el->items[j].name, name);

            if (shadowed)
                fprintf(stderr,
                        "packmule: %s needs %s, but every version of it in the "
                        "lock is\n          restricted to a different "
                        "environment.  Re-lock covering this target, or "
                        "adjust --python/--os/--arch.\n", from, name);
            else
                fprintf(stderr,
                        "packmule: %s depends on %s, which the lock does not "
                        "contain.\n          The lock is inconsistent; "
                        "regenerate it with `uv lock`.\n", from, name);
            *fatal = 1;
            return changed;
        }

        if (!target->reachable) {
            target->reachable = 1;
            changed           = 1;
        }

        const TomlValue *extras = toml_get(edge, "extra");
        for (size_t j = 0; j < toml_array_len(extras); j++) {
            const char *x = toml_string(toml_array_at(extras, j));
            if (x && extras_add(&target->extras, x))
                changed = 1;
        }
    }
    return changed;
}

/*
 * uv_mark_reachable — reachability from the workspace roots.
 *
 * The roots are the `virtual` and `editable` entries: the project being locked
 * and its workspace members.  They are never downloaded (they are the local
 * source tree) but their dependencies are the entry points.  A lock with no
 * such entry carries no root information, so nothing is pruned — over-shipping
 * is recoverable behind the wire, under-shipping is not.
 */
static int uv_mark_reachable(EntryList *el, const char *target_os, int py_minor)
{
    int roots = 0;
    for (size_t i = 0; i < el->count; i++) {
        if (el->items[i].excluded)
            continue;
        if (el->items[i].is_local) {
            el->items[i].reachable = 1;
            roots++;
        }
    }

    if (roots == 0) {
        for (size_t i = 0; i < el->count; i++)
            if (!el->items[i].excluded)
                el->items[i].reachable = 1;
        return 0;
    }

    /* Iterate to a fixpoint.  The bound is the number of entries: each pass
     * either marks a new package or adds an extra, and both are monotone. */
    for (size_t pass = 0; pass <= el->count; pass++) {
        int changed = 0;
        int fatal   = 0;

        for (size_t i = 0; i < el->count; i++) {
            Entry *e = &el->items[i];
            if (!e->reachable || e->excluded)
                continue;

            changed |= uv_visit_edges(el, toml_get(e->tv, "dependencies"),
                                      target_os, py_minor, e->name, &fatal);
            if (fatal)
                return -1;

            const TomlValue *opt = toml_get(e->tv, "optional-dependencies");
            for (const char *x = e->extras; x && *x;) {
                const char *comma = strchr(x, ',');
                size_t      len   = comma ? (size_t)(comma - x) : strlen(x);
                char       *one   = pm_strndup(x, len);
                changed |= uv_visit_edges(el, toml_get(opt, one), target_os,
                                          py_minor, e->name, &fatal);
                pm_free(one);
                if (fatal)
                    return -1;
                x = comma ? comma + 1 : NULL;
            }
        }
        if (!changed)
            return 0;
    }
    return 0;
}

/*
 * uv_dep_specs — the reachable dependency names of one package, as PEP 508
 * specs.  Only the SBOM writer consumes these, to draw the dependency graph;
 * a bare name is a valid spec and is all the lock gives us.
 */
static char **uv_dep_specs(const EntryList *el, const TomlValue *entry,
                           const char *target_os, int py_minor)
{
    const TomlValue *deps = toml_get(entry, "dependencies");
    size_t           n    = toml_array_len(deps);
    if (n == 0)
        return NULL;

    char **specs = pm_calloc(n + 1, sizeof(*specs));
    size_t k     = 0;

    for (size_t i = 0; i < n; i++) {
        const TomlValue *edge = toml_array_at(deps, i);
        const char      *name = toml_table_string(edge, "name");
        if (!name)
            continue;
        const char *marker = toml_table_string(edge, "marker");
        if (marker && pep508_marker_excludes(marker, target_os, py_minor))
            continue;
        /* Only name edges that survived into the bundle. */
        for (size_t j = 0; j < el->count; j++)
            if (el->items[j].reachable && !el->items[j].is_local &&
                package_name_equal_pep503(el->items[j].name, name)) {
                specs[k++] = pm_strdup(name);
                break;
            }
    }

    if (k == 0) {
        pm_free(specs);
        return NULL;
    }
    return specs;
}

static PackageList *uv_build(const TomlValue *root, const char *path,
                             const char *arch, const char *target_os,
                             int py_minor)
{
    const TomlValue *packages = toml_get(root, "package");
    if (toml_array_len(packages) == 0) {
        fprintf(stderr, "packmule: %s contains no [[package]] entries\n", path);
        return NULL;
    }

    const char *requires_python = toml_table_string(root, "requires-python");
    if (requires_python && py_minor > 0) {
        char *target = pm_asprintf("3.%d", py_minor);
        if (pep440_satisfies(target, requires_python) == 0)
            fprintf(stderr,
                    "packmule: warning: %s was locked for Python %s, but the "
                    "target is 3.%d.\n          The bundled wheels may not "
                    "install there.\n", path, requires_python, py_minor);
        pm_free(target);
    }

    EntryList el;
    memset(&el, 0, sizeof(el));

    for (size_t i = 0; i < toml_array_len(packages); i++) {
        const TomlValue *tv   = toml_array_at(packages, i);
        const char      *name = toml_table_string(tv, "name");
        if (!name) {
            fprintf(stderr, "packmule: %s has a [[package]] with no name\n",
                    path);
            entries_free(&el);
            return NULL;
        }

        const char *kind = uv_source_kind(tv);
        Entry       e;
        memset(&e, 0, sizeof(e));
        e.tv       = tv;
        e.name     = name;
        e.version  = toml_table_string(tv, "version");
        /* `virtual` and `editable` are the project being locked and its
         * workspace members — local source trees, not downloads.  A
         * `directory` source is a plain path dependency and is NOT one of
         * these: it is a package the target genuinely needs and cannot get,
         * so it falls through to the unshippable-source check below. */
        e.is_local = kind && (strcmp(kind, "virtual") == 0 ||
                              strcmp(kind, "editable") == 0);
        e.excluded = markers_exclude_all(tv, target_os, py_minor);
        entries_push(&el, &e);
    }

    if (uv_mark_reachable(&el, target_os, py_minor) != 0) {
        entries_free(&el);
        return NULL;
    }

    /* One name may appear at several versions when resolution markers differ.
     * If more than one survived for this target, the lock cannot tell us which
     * to install, and guessing would silently ship the wrong version. */
    for (size_t i = 0; i < el.count; i++) {
        if (!el.items[i].reachable || el.items[i].is_local)
            continue;
        for (size_t j = i + 1; j < el.count; j++) {
            if (!el.items[j].reachable || el.items[j].is_local)
                continue;
            if (package_name_equal_pep503(el.items[i].name, el.items[j].name)) {
                fprintf(stderr,
                        "packmule: %s locks %s at both %s and %s for this "
                        "target.\n          pip can install only one; narrow "
                        "the target with --python/--os/--arch.\n",
                        path, el.items[i].name,
                        el.items[i].version ? el.items[i].version : "?",
                        el.items[j].version ? el.items[j].version : "?");
                entries_free(&el);
                return NULL;
            }
        }
    }

    PackageList *list = package_list_create();

    for (size_t i = 0; i < el.count; i++) {
        Entry *e = &el.items[i];
        if (!e->reachable || e->is_local)
            continue;

        const char *kind = uv_source_kind(e->tv);
        if (kind && strcmp(kind, "registry") != 0) {
            fprintf(stderr,
                    "packmule: cannot bundle %s (from %s):\n"
                    "          it is locked from a %s source, which has no "
                    "artifact to carry across an air gap.\n",
                    e->name, path, kind);
            package_list_destroy(list);
            entries_free(&el);
            return NULL;
        }
        if (!e->version) {
            fprintf(stderr, "packmule: %s in %s has no version\n", e->name,
                    path);
            package_list_destroy(list);
            entries_free(&el);
            return NULL;
        }

        Artifact art;
        memset(&art, 0, sizeof(art));
        if (select_artifact(e->tv, e->name, e->version, arch, target_os,
                            py_minor, &art) != 0) {
            package_list_destroy(list);
            entries_free(&el);
            return NULL;
        }

        Package *p  = package_create(e->name, e->version);
        p->url      = pm_strdup(art.url);
        p->filename = art.filename;
        digest_set(&p->digest, art.digest.algo, art.digest.enc,
                   art.digest.value);
        digest_clear(&art.digest);
        package_set_dep_specs(p, uv_dep_specs(&el, e->tv, target_os, py_minor));
        /* A lock entry is the real resolver's final answer: already resolved,
         * and not something packmule's resolver may revise. */
        p->state       = PKG_RESOLVED;
        p->user_pinned = 1;
        package_list_add(list, p);
    }

    entries_free(&el);

    if (list->count == 0) {
        fprintf(stderr,
                "packmule: %s resolved to no packages for this target.\n",
                path);
        package_list_destroy(list);
        return NULL;
    }
    return list;
}

/* ── pylock.toml (PEP 751) ───────────────────────────────────────────────── */

static PackageList *pep751_build(const TomlValue *root, const char *path,
                                 const char *arch, const char *target_os,
                                 int py_minor)
{
    const char *lock_version = toml_table_string(root, "lock-version");
    if (lock_version && strncmp(lock_version, "1.", 2) != 0) {
        fprintf(stderr,
                "packmule: %s declares lock-version %s; only 1.x is "
                "supported.\n", path, lock_version);
        return NULL;
    }

    const TomlValue *packages = toml_get(root, "packages");
    if (toml_array_len(packages) == 0) {
        fprintf(stderr, "packmule: %s contains no [[packages]] entries\n",
                path);
        return NULL;
    }

    const char *requires_python = toml_table_string(root, "requires-python");
    if (requires_python && py_minor > 0) {
        char *target = pm_asprintf("3.%d", py_minor);
        if (pep440_satisfies(target, requires_python) == 0)
            fprintf(stderr,
                    "packmule: warning: %s was locked for Python %s, but the "
                    "target is 3.%d.\n          The bundled wheels may not "
                    "install there.\n", path, requires_python, py_minor);
        pm_free(target);
    }

    PackageList *list = package_list_create();

    for (size_t i = 0; i < toml_array_len(packages); i++) {
        const TomlValue *tv   = toml_array_at(packages, i);
        const char      *name = toml_table_string(tv, "name");
        if (!name) {
            fprintf(stderr, "packmule: %s has a [[packages]] entry with no "
                    "name\n", path);
            goto error;
        }

        /* PEP 751 flattens the graph: an entry that does not apply to this
         * environment is simply marked, not omitted. */
        const char *marker = toml_table_string(tv, "marker");
        if (marker && pep508_marker_excludes(marker, target_os, py_minor))
            continue;

        static const char *const unshippable[] = { "vcs", "directory",
                                                   "archive" };
        int bad = 0;
        for (size_t k = 0; k < sizeof(unshippable) / sizeof(unshippable[0]); k++)
            if (toml_get(tv, unshippable[k])) {
                fprintf(stderr,
                        "packmule: cannot bundle %s (from %s):\n"
                        "          it is locked from a %s source, which has no "
                        "artifact to carry across an air gap.\n",
                        name, path, unshippable[k]);
                bad = 1;
            }
        if (bad)
            goto error;

        const char *version = toml_table_string(tv, "version");
        if (!version) {
            fprintf(stderr, "packmule: %s in %s has no version\n", name, path);
            goto error;
        }

        Artifact art;
        memset(&art, 0, sizeof(art));
        if (select_artifact(tv, name, version, arch, target_os, py_minor,
                            &art) != 0)
            goto error;

        Package *p  = package_create(name, version);
        p->url      = pm_strdup(art.url);
        p->filename = art.filename;
        digest_set(&p->digest, art.digest.algo, art.digest.enc,
                   art.digest.value);
        digest_clear(&art.digest);
        p->state       = PKG_RESOLVED;
        p->user_pinned = 1;
        package_list_add(list, p);
    }

    if (list->count == 0) {
        fprintf(stderr,
                "packmule: %s resolved to no packages for this target.\n",
                path);
        goto error;
    }
    return list;

error:
    package_list_destroy(list);
    return NULL;
}

/* ── Entry points ────────────────────────────────────────────────────────── */

PylockKind pylock_kind_for_name(const char *basename)
{
    if (!basename)
        return PYLOCK_KIND_NONE;
    if (strcmp(basename, "uv.lock") == 0)
        return PYLOCK_KIND_UV;
    /* PEP 751: "pylock.toml", or "pylock.<name>.toml" for a named lock. */
    if (strcmp(basename, "pylock.toml") == 0)
        return PYLOCK_KIND_PEP751;

    size_t len = strlen(basename);
    if (len > strlen("pylock.") + strlen(".toml") &&
        strncmp(basename, "pylock.", 7) == 0 &&
        strcmp(basename + len - 5, ".toml") == 0)
        return PYLOCK_KIND_PEP751;

    return PYLOCK_KIND_NONE;
}

char *pylock_effective(const char *manifest_path)
{
    if (!manifest_path)
        return NULL;

    if (pylock_kind_for_name(pm_basename(manifest_path)) != PYLOCK_KIND_NONE)
        return pm_strdup(manifest_path);

    if (strcmp(pm_basename(manifest_path), "pyproject.toml") != 0)
        return NULL;

    /* uv.lock first: it is the richer format, and a project that has both has
     * a pylock.toml exported FROM the uv.lock. */
    static const char *const names[] = { "uv.lock", "pylock.toml" };
    const char *slash = strrchr(manifest_path, '/');

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char *cand = slash
            ? pm_asprintf("%.*s/%s", (int)(slash - manifest_path),
                          manifest_path, names[i])
            : pm_strdup(names[i]);
        FILE *fp = fopen(cand, "rb");
        if (fp) {
            fclose(fp);
            return cand;
        }
        pm_free(cand);
    }
    return NULL;
}

PackageList *pylock_parse(const char *path, const char *arch,
                          const char *target_os, int py_minor)
{
    PylockKind kind = pylock_kind_for_name(pm_basename(path));
    if (kind == PYLOCK_KIND_NONE) {
        fprintf(stderr, "packmule: %s is not a lockfile packmule reads\n", path);
        return NULL;
    }

    TomlValue *root = toml_parse_file(path, 0);
    if (!root)
        return NULL;

    PackageList *list = (kind == PYLOCK_KIND_UV)
        ? uv_build(root, path, arch, target_os, py_minor)
        : pep751_build(root, path, arch, target_os, py_minor);

    toml_free(root);
    return list;
}
