/*
 * registry_npm.c — npm registry backend.
 *
 * Manifest format: package.json ("dependencies" object only).
 * Registry API:    https://registry.npmjs.org/<name>/<version>
 *                  https://registry.npmjs.org/<name>/latest
 *                  https://registry.npmjs.org/<name>          (packument)
 *
 * Version resolution:
 *   Exact versions (digits, dots, hyphens only) → query /<name>/<version>.
 *   Empty / "*" / "latest"                       → query /<name>/latest.
 *   Range specifiers (^, ~, >=, 1.x, "a || b")  → fetch the packument and
 *     pick the highest version in its "versions" object that satisfies the
 *     range (semver_satisfies), the same choice npm itself would make.
 *     A spec that is not semver at all (git URL, tag) falls back to latest
 *     with a warning.
 *
 * Integrity verification uses dist.integrity (SHA-512 SRI) when present,
 * which is verified by verify_file() in hash.c.  Packages published before
 * the integrity field was introduced (~2017) are rejected rather than falling
 * back to the weaker dist.shasum (SHA-1).
 *
 * Transitive dependencies are read from the "dependencies" object in the
 * resolved version document and stored in pkg->dep_specs as "name@range"
 * for npm_get_deps() to enqueue breadth-first with their ranges intact.
 */

#include "registry.h"
#include "registry_internal.h"
#include "network.h"
#include "package.h"
#include "semver.h"
#include "utils.h"

#include <cjson/cJSON.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* ── Manifest parser ─────────────────────────────────────────────────────── */

/*
 * npm_parse_manifest — read "dependencies" from a package.json file.
 *
 * Only the "dependencies" object is processed; "devDependencies",
 * "peerDependencies", and "optionalDependencies" are intentionally ignored
 * because they are not required for a production offline install.
 *
 * Version strings are stored verbatim (e.g. "^4.18.0", "~1.2.3", "2.31.0").
 * Range resolution happens in npm_resolve.
 */
static PackageList *npm_parse_manifest(const Registry *self, const char *path)
{
    (void)self;

    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "packmule: cannot open %s\n", path);
        return NULL;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fprintf(stderr, "packmule: cannot seek in %s\n", path);
        fclose(fp);
        return NULL;
    }
    long fsize = ftell(fp);
    if (fsize < 0) {
        fprintf(stderr, "packmule: cannot determine size of %s\n", path);
        fclose(fp);
        return NULL;
    }
    rewind(fp);

    char  *buf   = pm_malloc((size_t)fsize + 1);
    size_t nread = fread(buf, 1, (size_t)fsize, fp);
    buf[nread]   = '\0';
    fclose(fp);

    cJSON *root = cJSON_Parse(buf);
    pm_free(buf);

    if (!root) {
        fprintf(stderr, "packmule: failed to parse JSON in %s\n", path);
        return NULL;
    }

    PackageList *list = package_list_create();

    cJSON *deps = cJSON_GetObjectItemCaseSensitive(root, "dependencies");
    if (cJSON_IsObject(deps)) {
        cJSON *entry = NULL;
        cJSON_ArrayForEach(entry, deps) {
            if (!cJSON_IsString(entry))
                continue;
            Package *pkg = package_create(entry->string, entry->valuestring);
            package_list_add(list, pkg);
        }
    }

    cJSON_Delete(root);
    return list;
}

/* ── Version classifier ──────────────────────────────────────────────────── */

/*
 * is_exact_version — return 1 if `ver` is an exact full semver triple.
 *
 * Exact: "major.minor.patch" with all three numeric parts present, then
 * optionally a prerelease/build suffix.
 * Examples:  "4.18.2"  "1.0.0-beta.1"  "2.0.0+build.1"
 * Ranges:    "^4.18.2"  "~1.2.3"  ">=1.0.0"  "*"  ""
 *            "4"  "4.2"   ← bare partials are x-range shorthand in npm
 *                           (>=4.0.0 <5.0.0), NOT versions the registry
 *                           serves at /<name>/<ver> (that 404s).
 */
static int is_exact_version(const char *ver)
{
    if (!ver)
        return 0;

    const char *p = ver;
    for (int part = 0; part < 3; part++) {
        if (!isdigit((unsigned char)*p))
            return 0;
        while (isdigit((unsigned char)*p))
            p++;
        if (part < 2) {
            if (*p != '.')
                return 0;
            p++;
        }
    }

    /* Anything after the triple must be a prerelease/build suffix. */
    if (*p == '\0')
        return 1;
    if (*p != '-' && *p != '+')
        return 0;
    for (; *p; p++) {
        if (!isalnum((unsigned char)*p) &&
            *p != '.' && *p != '-' && *p != '+')
            return 0;
    }
    return 1;
}

/* ── URL builder ─────────────────────────────────────────────────────────── */

/*
 * npm_url_name — package name as it appears in a registry URL path: the '/'
 * in a scoped name ("@scope/pkg") is percent-encoded so the version suffix
 * is unambiguous.  Caller owns the result.
 */
static char *npm_url_name(const char *name)
{
    const char *slash = strchr(name, '/');
    if (!slash)
        return pm_strdup(name);
    return pm_asprintf("%.*s%%2F%s", (int)(slash - name), name, slash + 1);
}

/* npm_build_url — "<base>/<encoded name>[/<suffix>]".  Caller owns result. */
static char *npm_build_url(const char *name, const char *suffix,
                           const char *repo_url)
{
    const char *base = repo_url ? repo_url : "https://registry.npmjs.org/";

    /* Ensure exactly one slash between base and package name. */
    size_t blen = strlen(base);
    int    sep  = (blen > 0 && base[blen - 1] == '/') ? 0 : 1;

    char *enc = npm_url_name(name);
    char *url = suffix
        ? pm_asprintf("%s%s%s/%s", base, sep ? "/" : "", enc, suffix)
        : pm_asprintf("%s%s%s",    base, sep ? "/" : "", enc);
    pm_free(enc);
    return url;
}

/* ── JSON response decoder ───────────────────────────────────────────────── */

/*
 * npm_parse_version_doc — decode one version document (either a whole
 * /<name>/<version> response or one entry of a packument's "versions"
 * object) into `pkg`.  Returns 0 on success, -1 on missing fields.
 */
static int npm_parse_version_doc(const cJSON *doc, Package *pkg)
{
    /* Fill in resolved version. */
    cJSON *ver_item = cJSON_GetObjectItemCaseSensitive(doc, "version");
    if (!cJSON_IsString(ver_item)) {
        fprintf(stderr, "packmule: npm JSON missing 'version' for %s\n", pkg->name);
        return -1;
    }

    /* dist object holds tarball URL and integrity hash. */
    cJSON *dist = cJSON_GetObjectItemCaseSensitive(doc, "dist");
    if (!cJSON_IsObject(dist)) {
        fprintf(stderr, "packmule: npm JSON missing 'dist' for %s\n", pkg->name);
        return -1;
    }

    cJSON *tarball = cJSON_GetObjectItemCaseSensitive(dist, "tarball");
    if (!cJSON_IsString(tarball)) {
        fprintf(stderr, "packmule: npm JSON missing 'dist.tarball' for %s\n", pkg->name);
        return -1;
    }

    /* Prefer SHA-512 SRI integrity; refuse SHA-1-only packages. */
    cJSON *integrity = cJSON_GetObjectItemCaseSensitive(dist, "integrity");
    if (!cJSON_IsString(integrity)) {
        fprintf(stderr,
                "packmule: npm package %s@%s has no 'dist.integrity' field.\n"
                "  Packages published before 2017 lack SHA-512 integrity; "
                "refusing download.\n",
                pkg->name, ver_item->valuestring);
        return -1;
    }

    pm_free(pkg->version);
    pm_free(pkg->url);
    pm_free(pkg->sha256);
    pm_free(pkg->filename);
    pkg->version = pm_strdup(ver_item->valuestring);
    pkg->url     = pm_strdup(tarball->valuestring);
    pkg->sha256  = pm_strdup(integrity->valuestring);  /* "sha512-<base64>" */

    /* Derive filename from the last path component of the tarball URL.  For
     * scoped packages prefix the scope: "@babel/core" and "@vue/core" both
     * publish "core-<ver>.tgz", which would collide in one output dir. */
    const char *slash = strrchr(tarball->valuestring, '/');
    const char *base  = slash ? slash + 1 : tarball->valuestring;
    if (pkg->name[0] == '@') {
        const char *scope_end = strchr(pkg->name, '/');
        if (scope_end)
            pkg->filename = pm_asprintf("%.*s-%s",
                                        (int)(scope_end - (pkg->name + 1)),
                                        pkg->name + 1, base);
        else
            pkg->filename = pm_strdup(base);
    } else {
        pkg->filename = pm_strdup(base);
    }

    /* Extract transitive dependencies as "name@range". */
    cJSON *deps = cJSON_GetObjectItemCaseSensitive(doc, "dependencies");
    if (cJSON_IsObject(deps)) {
        int n = cJSON_GetArraySize(deps);
        if (pkg->dep_specs) {
            for (char **p = pkg->dep_specs; *p; p++)
                pm_free(*p);
            pm_free(pkg->dep_specs);
        }
        pkg->dep_specs = pm_malloc(((size_t)n + 1) * sizeof(char *));
        int    k       = 0;
        cJSON *entry   = NULL;
        cJSON_ArrayForEach(entry, deps) {
            if (!entry->string)
                continue;
            pkg->dep_specs[k++] = pm_asprintf("%s@%s", entry->string,
                                              cJSON_IsString(entry)
                                                  ? entry->valuestring : "");
        }
        pkg->dep_specs[k] = NULL;
    }

    return 0;
}

int npm_parse_response(const char *json, Package *pkg)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        fprintf(stderr, "packmule: failed to parse npm JSON for %s\n", pkg->name);
        return -1;
    }
    int ret = npm_parse_version_doc(root, pkg);
    cJSON_Delete(root);
    return ret;
}

/*
 * npm_resolve_range — resolve a semver range against a packument: pick the
 * highest key of the "versions" object satisfying `pkg->version`, mirroring
 * npm's own selection.  A non-semver spec (git URL, dist-tag) falls back to
 * the "latest" dist-tag with a warning.  Returns 0 on success.
 */
static int npm_resolve_range(const char *json, Package *pkg)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        fprintf(stderr, "packmule: failed to parse npm JSON for %s\n", pkg->name);
        return -1;
    }

    int ret = -1;

    cJSON *versions = cJSON_GetObjectItemCaseSensitive(root, "versions");
    if (!cJSON_IsObject(versions)) {
        fprintf(stderr, "packmule: npm packument missing 'versions' for %s\n",
                pkg->name);
        goto done;
    }

    const cJSON *best        = NULL;
    int          unparseable = 0;
    cJSON       *entry       = NULL;
    cJSON_ArrayForEach(entry, versions) {
        if (!entry->string)
            continue;
        int sat = semver_satisfies(entry->string, pkg->version);
        if (sat == -1)
            unparseable = 1;
        else if (sat == 1 &&
                 (!best || semver_cmp(entry->string, best->string) > 0))
            best = entry;
    }

    if (!best && unparseable) {
        /* Spec isn't semver (git URL, tag like "next"): npm would resolve it
         * out-of-registry; the closest in-registry behaviour is "latest". */
        fprintf(stderr,
                "packmule: %s: spec '%s' is not a semver range; "
                "falling back to latest\n",
                pkg->name, pkg->version);
        cJSON *tags = cJSON_GetObjectItemCaseSensitive(root, "dist-tags");
        cJSON *latest = cJSON_GetObjectItemCaseSensitive(tags, "latest");
        if (cJSON_IsString(latest))
            best = cJSON_GetObjectItemCaseSensitive(versions,
                                                    latest->valuestring);
    }

    if (!best) {
        fprintf(stderr,
                "packmule: no published version of %s satisfies '%s'\n",
                pkg->name, pkg->version ? pkg->version : "(none)");
        goto done;
    }

    ret = npm_parse_version_doc(best, pkg);

done:
    cJSON_Delete(root);
    return ret;
}

/* ── Resolver ────────────────────────────────────────────────────────────── */

/* Specs that mean "whatever is newest" — served directly by /<name>/latest. */
static int is_any_version(const char *ver)
{
    return !ver || ver[0] == '\0' ||
           strcmp(ver, "*")      == 0 ||
           strcmp(ver, "x")      == 0 ||
           strcmp(ver, "latest") == 0;
}

static int npm_resolve(const Registry *self, Package *pkg)
{
    char *url;
    int   want_range = 0;

    if (is_exact_version(pkg->version)) {
        url = npm_build_url(pkg->name, pkg->version, self->repo_url);
    } else if (is_any_version(pkg->version)) {
        url = npm_build_url(pkg->name, "latest", self->repo_url);
    } else {
        /* Semver range: need the full packument to pick from "versions". */
        url        = npm_build_url(pkg->name, NULL, self->repo_url);
        want_range = 1;
    }

    char *json = fetch_json(url);
    pm_free(url);
    if (!json)
        return -1;

    int ret = want_range ? npm_resolve_range(json, pkg)
                         : npm_parse_response(json, pkg);
    pm_free(json);
    return ret;
}

/* ── Transitive dependency resolver ─────────────────────────────────────── */

static int npm_get_deps(const Registry *self, const Package *pkg,
                         const PackageList *seen, PackageList *out)
{
    (void)self;
    if (!pkg->dep_specs)
        return 0;
    int added = 0;
    for (char **dep = pkg->dep_specs; *dep; dep++) {
        /* Split "name@range" at the LAST '@': a scoped name's leading '@' is
         * at index 0 and never matches. */
        const char *at = strrchr(*dep, '@');
        char *name;
        char *range;
        if (at && at != *dep) {
            name  = pm_strndup(*dep, (size_t)(at - *dep));
            range = (at[1] != '\0') ? pm_strdup(at + 1) : NULL;
        } else {
            name  = pm_strdup(*dep);
            range = NULL;
        }

        /* npm alias: "dep": "npm:real-name@range" installs `real-name`.
         * Without this rewrite we would query the registry for the alias
         * name, which does not exist as a package. */
        if (range && strncmp(range, "npm:", 4) == 0) {
            const char *real = range + 4;
            const char *rat  = strrchr(real, '@');
            pm_free(name);
            if (rat && rat != real) {
                name = pm_strndup(real, (size_t)(rat - real));
                char *nrange = (rat[1] != '\0') ? pm_strdup(rat + 1) : NULL;
                pm_free(range);
                range = nrange;
            } else {
                name = pm_strdup(real);
                pm_free(range);
                range = NULL;
            }
        }

        if (!package_list_contains_name(seen, name)) {
            package_list_add(out, package_create(name, range));
            added++;
        }
        pm_free(name);
        pm_free(range);
    }
    return added;
}

/* ── Filename detection ──────────────────────────────────────────────────── */

static int npm_detect(const char *basename)
{
    return strcmp(basename, "package.json") == 0;
}

/* ── Registry instance ───────────────────────────────────────────────────── */

const Registry npm_registry = {
    .name           = "npm",
    .manifest_name  = "package.json",
    .detect         = npm_detect,
    .parse_manifest = npm_parse_manifest,
    .resolve        = npm_resolve,
    .get_deps       = npm_get_deps,
    .ctx            = NULL, /* arch — npm tarballs are platform-neutral */
    .repo_url       = NULL, /* optional; defaults to https://registry.npmjs.org */
};
