/*
 * registry_npm.c — npm registry backend.
 *
 * Manifest format: package.json ("dependencies" object only).
 * Registry API:    https://registry.npmjs.org/<name>/<version>
 *                  https://registry.npmjs.org/<name>/latest
 *
 * Version resolution:
 *   Exact versions (digits, dots, hyphens only) → query /<name>/<version>.
 *   All range specifiers (^, ~, >=, *, empty)   → query /<name>/latest.
 *
 * Integrity verification uses dist.integrity (SHA-512 SRI) when present,
 * which is verified by verify_file() in hash.c.  Packages published before
 * the integrity field was introduced (~2017) are rejected rather than falling
 * back to the weaker dist.shasum (SHA-1).
 *
 * Transitive dependencies are read from the "dependencies" object in the
 * resolved version document and stored in pkg->requires_dist so that main.c
 * can enqueue them breadth-first.
 */

#include "registry.h"
#include "network.h"
#include "package.h"
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
 * is_exact_version — return 1 if `ver` is an exact semver (no range prefix).
 *
 * Exact: digits, dots, hyphens, and "+" (build metadata) only.
 * Examples:  "4.18.2"  "1.0.0-beta.1"  "2.0.0+build.1"
 * Ranges:    "^4.18.2"  "~1.2.3"  ">=1.0.0"  "*"  ""
 */
static int is_exact_version(const char *ver)
{
    if (!ver || ver[0] == '\0' || !isdigit((unsigned char)ver[0]))
        return 0;
    for (const char *p = ver + 1; *p; p++) {
        if (!isdigit((unsigned char)*p) && *p != '.' && *p != '-' && *p != '+')
            return 0;
    }
    return 1;
}

/* ── URL builder ─────────────────────────────────────────────────────────── */

static char *npm_build_url(const char *name, const char *version,
                           const char *repo_url)
{
    const char *tag  = is_exact_version(version) ? version : "latest";
    const char *base = repo_url ? repo_url : "https://registry.npmjs.org/";

    /* Ensure exactly one slash between base and package name. */
    size_t blen = strlen(base);
    int    sep  = (blen > 0 && base[blen - 1] == '/') ? 0 : 1;

    int   n   = snprintf(NULL, 0, "%s%s%s/%s", base, sep ? "/" : "", name, tag);
    char *url = pm_malloc((size_t)n + 1);
    snprintf(url, (size_t)n + 1, "%s%s%s/%s", base, sep ? "/" : "", name, tag);
    return url;
}

/* ── JSON response decoder ───────────────────────────────────────────────── */

static int npm_parse_response(const char *json, Package *pkg)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        fprintf(stderr, "packmule: failed to parse npm JSON for %s\n", pkg->name);
        return -1;
    }

    int ret = -1;

    /* Fill in resolved version. */
    cJSON *ver_item = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (!cJSON_IsString(ver_item)) {
        fprintf(stderr, "packmule: npm JSON missing 'version' for %s\n", pkg->name);
        goto done;
    }
    pm_free(pkg->version);
    pkg->version = pm_strdup(ver_item->valuestring);

    /* dist object holds tarball URL and integrity hash. */
    cJSON *dist = cJSON_GetObjectItemCaseSensitive(root, "dist");
    if (!cJSON_IsObject(dist)) {
        fprintf(stderr, "packmule: npm JSON missing 'dist' for %s\n", pkg->name);
        goto done;
    }

    cJSON *tarball = cJSON_GetObjectItemCaseSensitive(dist, "tarball");
    if (!cJSON_IsString(tarball)) {
        fprintf(stderr, "packmule: npm JSON missing 'dist.tarball' for %s\n", pkg->name);
        goto done;
    }

    /* Prefer SHA-512 SRI integrity; refuse SHA-1-only packages. */
    cJSON *integrity = cJSON_GetObjectItemCaseSensitive(dist, "integrity");
    if (!cJSON_IsString(integrity)) {
        fprintf(stderr,
                "packmule: npm package %s@%s has no 'dist.integrity' field.\n"
                "  Packages published before 2017 lack SHA-512 integrity; "
                "refusing download.\n",
                pkg->name, pkg->version);
        goto done;
    }

    pm_free(pkg->url);
    pm_free(pkg->sha256);
    pm_free(pkg->filename);
    pkg->url    = pm_strdup(tarball->valuestring);
    pkg->sha256 = pm_strdup(integrity->valuestring);  /* "sha512-<base64>" */

    /* Derive filename from the last path component of the tarball URL. */
    const char *slash = strrchr(tarball->valuestring, '/');
    pkg->filename = pm_strdup(slash ? slash + 1 : tarball->valuestring);

    /* Extract transitive dependencies. */
    cJSON *deps = cJSON_GetObjectItemCaseSensitive(root, "dependencies");
    if (cJSON_IsObject(deps)) {
        int n = cJSON_GetArraySize(deps);
        if (pkg->requires_dist) {
            for (char **p = pkg->requires_dist; *p; p++)
                pm_free(*p);
            pm_free(pkg->requires_dist);
        }
        pkg->requires_dist    = pm_malloc(((size_t)n + 1) * sizeof(char *));
        int    k              = 0;
        cJSON *entry          = NULL;
        cJSON_ArrayForEach(entry, deps) {
            if (!entry->string)
                continue;
            pkg->requires_dist[k++] = pm_strdup(entry->string);
        }
        pkg->requires_dist[k] = NULL;
    }

    ret = 0;
done:
    cJSON_Delete(root);
    return ret;
}

/* ── Resolver ────────────────────────────────────────────────────────────── */

static int npm_resolve(const Registry *self, Package *pkg)
{
    char *url  = npm_build_url(pkg->name, pkg->version, self->repo_url);
    char *json = fetch_json(url);
    pm_free(url);
    if (!json)
        return -1;
    int ret = npm_parse_response(json, pkg);
    pm_free(json);
    return ret;
}

/* ── Registry instance ───────────────────────────────────────────────────── */

const Registry npm_registry = {
    "npm",
    "package.json",
    npm_parse_manifest,
    npm_resolve,
    NULL, /* ctx      — arch (npm tarballs are platform-neutral) */
    NULL, /* repo_url — optional; defaults to https://registry.npmjs.org */
    NULL  /* destroy */
};
