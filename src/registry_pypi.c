#include "registry.h"
#include "network.h"
#include "package.h"
#include "utils.h"

#include <cjson/cJSON.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* ── Wheel platform helpers ──────────────────────────────────────────────── */

/*
 * wheel_platform_tag — return a pointer into fn at the start of the platform
 * tag (the last '-'-delimited field before ".whl").
 *
 * Example: "numpy-2.4.6-cp313-cp313-manylinux_2_17_x86_64.manylinux2014_x86_64.whl"
 *          returns ptr to "manylinux_2_17_x86_64.manylinux2014_x86_64.whl"
 */
static const char *wheel_platform_tag(const char *fn)
{
    size_t      len = strlen(fn);
    const char *p   = fn + len - 5; /* char before ".whl" */
    while (p > fn && *p != '-')
        p--;
    return (*p == '-') ? p + 1 : fn;
}

/*
 * arch_matches_platform — return 1 if the platform tag string is compatible
 * with the requested CPU architecture.
 *
 * arm64 (macOS naming) and aarch64 (Linux naming) are treated as identical.
 */
static int arch_matches_platform(const char *platform, const char *arch)
{
    if (strstr(platform, arch))                                         return 1;
    if (strcmp(arch, "arm64")   == 0 && strstr(platform, "aarch64"))   return 1;
    if (strcmp(arch, "aarch64") == 0 && strstr(platform, "arm64"))     return 1;
    return 0;
}

/* ── requirements.txt line parser ────────────────────────────────────────── */

/*
 * parse_line — parse one non-blank, non-comment requirements line.
 *
 * Handles:
 *   requests                    → name="requests",  version=NULL
 *   requests==2.31.0            → name="requests",  version="2.31.0"
 *   requests[security]==2.31.0  → extras stripped, otherwise as above
 *
 * Caller must eventually call package_destroy() on the returned pointer.
 * Returns NULL for blank/comment lines or on syntax error.
 */
static Package *parse_line(const char *line)
{
    /* Strip inline comments. */
    const char *comment = strchr(line, '#');
    char *work    = comment ? pm_strndup(line, (size_t)(comment - line))
                            : pm_strdup(line);
    char *trimmed = pm_strtrim(work);

    if (trimmed[0] == '\0') {
        pm_free(work);
        return NULL;
    }

    /* Scan name: stop at any PEP 440 specifier character, extras, or marker. */
    size_t name_len = 0;
    while (trimmed[name_len] &&
           trimmed[name_len] != '[' &&
           trimmed[name_len] != '=' &&
           trimmed[name_len] != '>' &&
           trimmed[name_len] != '<' &&
           trimmed[name_len] != '!' &&
           trimmed[name_len] != '~' &&
           trimmed[name_len] != ';')
        ++name_len;

    if (name_len == 0) {
        fprintf(stderr, "packmule: unrecognised requirements line: %s\n", line);
        pm_free(work);
        return NULL;
    }

    char *name = pm_strndup(trimmed, name_len);

    /* Skip optional extras marker [extra,…]. */
    const char *cursor = trimmed + name_len;
    if (*cursor == '[') {
        cursor = strchr(cursor, ']');
        if (!cursor) {
            fprintf(stderr, "packmule: unterminated extras in: %s\n", line);
            pm_free(name);
            pm_free(work);
            return NULL;
        }
        ++cursor; /* step past ']' */
    }

    /*
     * Accept only '==' for pinned versions; ignore other specifiers.
     * Strip any environment marker (';' and everything after) so that lines
     * like "pywin32==306 ; sys_platform == 'win32'" yield version "306".
     */
    char *version_str = NULL;
    if (cursor[0] == '=' && cursor[1] == '=') {
        const char *ver_start = cursor + 2;
        const char *marker    = strchr(ver_start, ';');
        size_t      ver_len   = marker ? (size_t)(marker - ver_start)
                                       : strlen(ver_start);
        while (ver_len > 0 && isspace((unsigned char)ver_start[ver_len - 1]))
            ver_len--;
        if (ver_len > 0)
            version_str = pm_strndup(ver_start, ver_len);
    }

    Package *pkg = package_create(name, version_str);
    pm_free(version_str);
    pm_free(name);
    pm_free(work);
    return pkg;
}

static PackageList *pypi_parse_manifest(const Registry *self, const char *path)
{
    (void)self;

    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "packmule: cannot open %s\n", path);
        return NULL;
    }

    PackageList *list = package_list_create();
    char         line[4096];

    while (fgets(line, (int)sizeof(line), fp)) {
        char *trimmed = pm_strtrim(line);

        if (trimmed[0] == '\0' || trimmed[0] == '#')
            continue;

        if (trimmed[0] == '-') {
            fprintf(stderr, "packmule: skipping unsupported option: %s\n", trimmed);
            continue;
        }

        Package *pkg = parse_line(trimmed);
        if (pkg)
            package_list_add(list, pkg);
    }

    fclose(fp);
    return list;
}

/* ── PyPI JSON response decoder ───────────────────────────────────────────── */

static int is_wheel(const char *fn)
{
    size_t len = strlen(fn);
    return len > 4 && strcmp(fn + len - 4, ".whl") == 0;
}

static int is_universal_wheel(const char *fn)
{
    return strstr(fn, "-py3-none-any") != NULL ||
           strstr(fn, "-py2.py3-none-any") != NULL;
}

static int is_sdist(const char *fn)
{
    size_t len = strlen(fn);
    return (len > 7 && strcmp(fn + len - 7, ".tar.gz") == 0) ||
           (len > 4 && strcmp(fn + len - 4, ".zip")    == 0);
}

/*
 * pypi_parse_response — extract url/sha256/filename (and resolved version)
 * from a raw PyPI JSON string into `pkg`.
 *
 * Wheel preference order (highest first):
 *   3 — arch-specific wheel matching `arch` (when arch is non-NULL)
 *   2 — universal pure-Python wheel (py3-none-any / py2.py3-none-any)
 *   1 — sdist (.tar.gz or .zip)
 *
 * Also sets pkg->version from info.version when it was previously NULL
 * (i.e., the package was requested unpinned).
 */
static int pypi_parse_response(const char *json, Package *pkg, const char *arch)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        fprintf(stderr, "packmule: failed to parse PyPI JSON for %s\n", pkg->name);
        return -1;
    }

    int ret = -1;

    cJSON *info = cJSON_GetObjectItemCaseSensitive(root, "info");

    /* If version was unspecified, fill it in from info.version. */
    if (!pkg->version && info) {
        cJSON *ver_item = cJSON_GetObjectItemCaseSensitive(info, "version");
        if (cJSON_IsString(ver_item))
            pkg->version = pm_strdup(ver_item->valuestring);
    }

    /* Extract dep_specs for transitive resolution. */
    if (info) {
        cJSON *req_dist = cJSON_GetObjectItemCaseSensitive(info, "requires_dist");
        if (cJSON_IsArray(req_dist)) {
            int n = cJSON_GetArraySize(req_dist);
            if (pkg->dep_specs) {
                for (char **p = pkg->dep_specs; *p; p++)
                    pm_free(*p);
                pm_free(pkg->dep_specs);
            }
            pkg->dep_specs = pm_malloc(((size_t)n + 1) * sizeof(char *));
            int k = 0;
            cJSON *item = NULL;
            cJSON_ArrayForEach(item, req_dist) {
                if (cJSON_IsString(item))
                    pkg->dep_specs[k++] = pm_strdup(item->valuestring);
            }
            pkg->dep_specs[k] = NULL;
        }
    }

    cJSON *urls = cJSON_GetObjectItemCaseSensitive(root, "urls");
    if (!cJSON_IsArray(urls)) {
        fprintf(stderr, "packmule: PyPI JSON missing 'urls' array for %s\n", pkg->name);
        goto done;
    }

    cJSON      *chosen          = NULL;
    const char *chosen_filename = NULL;
    int         chosen_priority = 0; /* 3=arch wheel, 2=universal wheel, 1=sdist */

    cJSON *dist = NULL;
    cJSON_ArrayForEach(dist, urls) {
        cJSON *fn_item = cJSON_GetObjectItemCaseSensitive(dist, "filename");
        if (!cJSON_IsString(fn_item))
            continue;
        const char *fn = fn_item->valuestring;

        if (is_wheel(fn)) {
            if (arch && !is_universal_wheel(fn) && chosen_priority < 3 &&
                arch_matches_platform(wheel_platform_tag(fn), arch)) {
                chosen          = dist;
                chosen_filename = fn;
                chosen_priority = 3;
            } else if (is_universal_wheel(fn) && chosen_priority < 2) {
                chosen          = dist;
                chosen_filename = fn;
                chosen_priority = 2;
            }
        } else if (is_sdist(fn) && chosen_priority < 1) {
            chosen          = dist;
            chosen_filename = fn;
            chosen_priority = 1;
        }
    }

    if (!chosen) {
        fprintf(stderr,
                "packmule: no suitable package found for %s==%s"
                " (arch: %s)\n",
                pkg->name,
                pkg->version ? pkg->version : "(latest)",
                arch ? arch : "any");
        goto done;
    }

    cJSON *url_item = cJSON_GetObjectItemCaseSensitive(chosen, "url");
    if (!cJSON_IsString(url_item)) {
        fprintf(stderr, "packmule: missing 'url' in distribution entry\n");
        goto done;
    }

    cJSON *digests    = cJSON_GetObjectItemCaseSensitive(chosen, "digests");
    cJSON *sha256_item = cJSON_GetObjectItemCaseSensitive(digests, "sha256");
    if (!cJSON_IsString(sha256_item)) {
        fprintf(stderr, "packmule: missing SHA-256 digest for %s\n", chosen_filename);
        goto done;
    }

    pm_free(pkg->url);
    pm_free(pkg->sha256);
    pm_free(pkg->filename);
    pkg->url      = pm_strdup(url_item->valuestring);
    pkg->sha256   = pm_strdup(sha256_item->valuestring);
    pkg->filename = pm_strdup(chosen_filename);
    ret = 0;

done:
    cJSON_Delete(root);
    return ret;
}

/* ── Resolver ────────────────────────────────────────────────────────────── */

static int pypi_resolve(const Registry *self, Package *pkg)
{
    const char *arch = self->ctx ? (const char *)self->ctx : NULL;
    const char *base = self->repo_url ? self->repo_url : "https://pypi.org/pypi";

    /* Strip trailing slash for consistent URL construction. */
    size_t blen = strlen(base);
    char  *base_trimmed = pm_strndup(base, blen);
    while (blen > 0 && base_trimmed[blen - 1] == '/')
        base_trimmed[--blen] = '\0';

    int n;
    if (pkg->version)
        n = snprintf(NULL, 0, "%s/%s/%s/json", base_trimmed, pkg->name, pkg->version);
    else
        n = snprintf(NULL, 0, "%s/%s/json", base_trimmed, pkg->name);

    if (n < 0) {
        fprintf(stderr, "packmule: internal error: snprintf failed\n");
        pm_free(base_trimmed);
        return -1;
    }

    char *url = pm_malloc((size_t)n + 1);
    if (pkg->version)
        snprintf(url, (size_t)n + 1, "%s/%s/%s/json", base_trimmed, pkg->name, pkg->version);
    else
        snprintf(url, (size_t)n + 1, "%s/%s/json", base_trimmed, pkg->name);
    pm_free(base_trimmed);

    char *json = fetch_json(url);
    pm_free(url);
    if (!json)
        return -1;

    int ret = pypi_parse_response(json, pkg, arch);
    pm_free(json);
    return ret;
}

/* ── Transitive dependency resolver ─────────────────────────────────────── */

/*
 * dep_is_extras_only — return 1 if the PEP 508 specifier is gated by an
 * extras marker ("; extra == 'foo'").  Such deps are optional and must never
 * be pulled in when no extra was explicitly requested.
 */
static int dep_is_extras_only(const char *spec)
{
    const char *marker = strchr(spec, ';');
    if (!marker)
        return 0;
    return strstr(marker, "extra ==") != NULL ||
           strstr(marker, "extra==")  != NULL;
}

/*
 * parse_dep_name — extract the bare package name from a PEP 508 specifier,
 * lowercased so case-insensitive dedup works correctly.
 *
 * Stops at the first '[', '(', ';', '>', '<', '!', '=', '~', or whitespace.
 */
static void parse_dep_name(const char *spec, char *out, size_t out_size)
{
    size_t i = 0;
    while (i < out_size - 1 && spec[i] &&
           spec[i] != '[' && spec[i] != '(' &&
           spec[i] != ';' && spec[i] != '>' &&
           spec[i] != '<' && spec[i] != '!' &&
           spec[i] != '=' && spec[i] != '~' &&
           spec[i] != ' ' && spec[i] != '\t')
    {
        out[i] = (char)tolower((unsigned char)spec[i]);
        i++;
    }
    out[i] = '\0';
}

static int pypi_get_deps(const Registry *self, const Package *pkg,
                          const PackageList *seen, PackageList *out)
{
    (void)self;
    if (!pkg->dep_specs)
        return 0;
    int added = 0;
    for (char **rd = pkg->dep_specs; *rd; rd++) {
        if (dep_is_extras_only(*rd))
            continue;
        char name[256];
        parse_dep_name(*rd, name, sizeof(name));
        if (name[0] && !package_list_contains_name(seen, name)) {
            package_list_add(out, package_create(name, NULL));
            added++;
        }
    }
    return added;
}

/* ── Filename detection ──────────────────────────────────────────────────── */

/*
 * pypi_detect — recognise pip requirements files.
 *
 * Matches any ".txt" basename containing "requirement", so that the common
 * variants are all auto-detected:
 *   requirements.txt, requirements-dev.txt, dev-requirements.txt,
 *   requirements.in is intentionally excluded (".in" is pip-tools input).
 */
static int pypi_detect(const char *basename)
{
    size_t len = strlen(basename);
    if (len < 4 || strcmp(basename + len - 4, ".txt") != 0)
        return 0;
    return strstr(basename, "requirement") != NULL;
}

/* ── Registry instance ───────────────────────────────────────────────────── */

const Registry pypi_registry = {
    .name          = "pypi",
    .manifest_name = "requirements.txt",
    .detect        = pypi_detect,
    .parse_manifest = pypi_parse_manifest,
    .resolve       = pypi_resolve,
    .get_deps      = pypi_get_deps,
    .ctx           = NULL, /* arch string, injected by main.c */
    .repo_url      = NULL, /* optional; defaults to https://pypi.org/pypi */
    .destroy       = NULL,
};
