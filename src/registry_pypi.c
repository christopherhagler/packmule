#include "registry.h"
#include "network.h"
#include "package.h"
#include "utils.h"

#include <cjson/cJSON.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
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

/*
 * platform_matches — return 1 if the wheel platform tag is installable on the
 * target OS *and* CPU arch.  `os` is "linux", "macos", "windows", or NULL (no
 * OS preference, arch-only — the legacy behaviour).
 *
 * The OS gate is what keeps a manylinux wheel off a macOS target and vice
 * versa: the bare CPU arch is ambiguous across systems (macOS "arm64" vs Linux
 * "aarch64"), so matching arch alone would happily pick a wheel pip refuses.
 * macOS "universal2" fat wheels carry both arches and match either.
 */
static int platform_matches(const char *platform, const char *os,
                            const char *arch)
{
    if (os) {
        if (strcmp(os, "macos") == 0) {
            if (!strstr(platform, "macosx"))       return 0;
            if (strstr(platform, "universal2"))    return 1;
        } else if (strcmp(os, "windows") == 0) {
            if (!strstr(platform, "win"))          return 0;
        } else if (strcmp(os, "linux") == 0) {
            /* manylinux / musllinux / plain linux all contain "linux". */
            if (!strstr(platform, "linux"))        return 0;
        }
    }
    return arch_matches_platform(platform, arch);
}

/*
 * tag_at — return 1 if `tag` appears in `fn` as a whole '-'/'.'-delimited
 * field (the way interpreter/abi tags are joined in a wheel filename), so
 * "cp31" never spuriously matches inside "cp312".
 */
static int tag_at(const char *fn, const char *tag)
{
    size_t      tlen = strlen(tag);
    const char *p    = fn;
    while ((p = strstr(p, tag)) != NULL) {
        char before = (p == fn) ? '-' : p[-1];
        char after  = p[tlen];
        if ((before == '-' || before == '.') &&
            (after  == '-' || after  == '.'))
            return 1;
        p += tlen;
    }
    return 0;
}

/*
 * wheel_python_matches — return 1 if a non-universal wheel `fn` is installable
 * on the target CPython 3.<py_minor>.  py_minor <= 0 disables the check.
 *
 * A wheel qualifies when it carries the exact interpreter tag (e.g. "cp312"),
 * or when it is a stable-ABI ("abi3") wheel whose CPython floor is <= the
 * target (those install on any newer CPython 3.x, e.g. cp37-abi3 on 3.12).
 * Wheels for a different CPython minor, or for other interpreters (PyPy's
 * "pp310", etc.), are rejected so we never bundle a wheel pip will refuse.
 */
static int wheel_python_matches(const char *fn, int py_minor)
{
    if (py_minor <= 0)
        return 1;

    char want[8];
    snprintf(want, sizeof(want), "cp3%d", py_minor); /* e.g. "cp312" */
    if (tag_at(fn, want))
        return 1;

    /* Stable ABI: accept cp3<floor>-abi3 when floor <= target. */
    if (tag_at(fn, "abi3")) {
        const char *p = fn;
        while ((p = strstr(p, "cp3")) != NULL) {
            const char *d = p + 3;
            if (isdigit((unsigned char)*d)) {
                int floor = atoi(d);
                if (floor <= py_minor)
                    return 1;
            }
            p += 3;
        }
    }
    return 0;
}

/* ── PEP 508 environment markers ─────────────────────────────────────────── */

/*
 * Tri-state marker evaluation.  A clause or expression is TRUE, FALSE, or
 * UNKNOWN (a variable/operator we do not model).  UNKNOWN never excludes a
 * package: when in doubt we keep it rather than risk dropping a real dep.
 */
enum { MARK_FALSE = 0, MARK_TRUE = 1, MARK_UNKNOWN = -1 };

/* version_cmp_3x — compare the target CPython "3.<minor>" against a marker
 * value like "3.9", "3", or "3.9.1".  Returns <0/0/>0 (target vs value). */
static int version_cmp_3x(int target_minor, const char *value)
{
    int vmaj = 0, vmin = 0;
    sscanf(value, "%d.%d", &vmaj, &vmin);
    if (vmaj != 3) return 3 - vmaj;          /* target major is always 3 */
    return target_minor - vmin;
}

/* eval_clause — evaluate one "VAR OP 'VALUE'" marker clause to a tri-state. */
static int eval_clause(const char *clause, const char *target_os, int py_minor)
{
    while (*clause == ' ' || *clause == '(')
        clause++;

    char   var[32];
    size_t i = 0;
    while (clause[i] && (isalnum((unsigned char)clause[i]) || clause[i] == '_') &&
           i < sizeof(var) - 1)
        var[i] = clause[i], i++;
    var[i] = '\0';

    const char *p = clause + i;
    while (*p == ' ') p++;

    char   op[4] = {0};
    size_t oi    = 0;
    while (oi < 3 && (*p == '=' || *p == '!' || *p == '<' || *p == '>' || *p == '~'))
        op[oi++] = *p++;
    if (oi == 0) return MARK_UNKNOWN;        /* "in" / "not in" etc. */
    while (*p == ' ') p++;

    char quote = *p;
    if (quote != '\'' && quote != '"') return MARK_UNKNOWN;
    p++;
    char   val[64];
    size_t vi = 0;
    while (*p && *p != quote && vi < sizeof(val) - 1)
        val[vi++] = *p++;
    val[vi] = '\0';

    /* String-valued OS markers. */
    const char *tval = NULL;
    if (strcmp(var, "sys_platform") == 0) {
        if (!target_os) return MARK_UNKNOWN;
        tval = strcmp(target_os, "windows") == 0 ? "win32"
             : strcmp(target_os, "macos")   == 0 ? "darwin" : "linux";
    } else if (strcmp(var, "platform_system") == 0) {
        if (!target_os) return MARK_UNKNOWN;
        tval = strcmp(target_os, "windows") == 0 ? "Windows"
             : strcmp(target_os, "macos")   == 0 ? "Darwin" : "Linux";
    } else if (strcmp(var, "os_name") == 0) {
        if (!target_os) return MARK_UNKNOWN;
        tval = strcmp(target_os, "windows") == 0 ? "nt" : "posix";
    }
    if (tval) {
        int eq = (strcmp(tval, val) == 0);
        if (strcmp(op, "==") == 0) return eq ? MARK_TRUE  : MARK_FALSE;
        if (strcmp(op, "!=") == 0) return eq ? MARK_FALSE : MARK_TRUE;
        return MARK_UNKNOWN;
    }

    /* Version-valued markers (CPython 3.x). */
    if (strcmp(var, "python_version") == 0 ||
        strcmp(var, "python_full_version") == 0) {
        if (py_minor <= 0) return MARK_UNKNOWN;
        int c = version_cmp_3x(py_minor, val);
        if (strcmp(op, "<")  == 0) return c <  0 ? MARK_TRUE : MARK_FALSE;
        if (strcmp(op, "<=") == 0) return c <= 0 ? MARK_TRUE : MARK_FALSE;
        if (strcmp(op, ">")  == 0) return c >  0 ? MARK_TRUE : MARK_FALSE;
        if (strcmp(op, ">=") == 0) return c >= 0 ? MARK_TRUE : MARK_FALSE;
        if (strcmp(op, "==") == 0) return c == 0 ? MARK_TRUE : MARK_FALSE;
        if (strcmp(op, "!=") == 0) return c != 0 ? MARK_TRUE : MARK_FALSE;
        return MARK_UNKNOWN;
    }

    return MARK_UNKNOWN;                      /* extra, platform_machine, … */
}

/* eval_and_chain — evaluate an "a and b and …" segment (no top-level "or"). */
static int eval_and_chain(const char *s, const char *target_os, int py_minor)
{
    int result = MARK_TRUE;
    while (s && *s) {
        const char *and_pos = strstr(s, " and ");
        size_t      len     = and_pos ? (size_t)(and_pos - s) : strlen(s);
        char        clause[256];
        if (len >= sizeof(clause)) len = sizeof(clause) - 1;
        memcpy(clause, s, len);
        clause[len] = '\0';

        int c = eval_clause(clause, target_os, py_minor);
        if (c == MARK_FALSE)   return MARK_FALSE;       /* short-circuit */
        if (c == MARK_UNKNOWN) result = MARK_UNKNOWN;   /* keep looking for FALSE */
        s = and_pos ? and_pos + 5 : NULL;
    }
    return result;
}

/*
 * marker_eval — tri-state evaluate a PEP 508 marker expression with "and"/"or"
 * ("and" binds tighter).  Parenthesised sub-groups are not modelled; such
 * markers fall back to UNKNOWN (keep), never to a false exclusion.
 */
static int marker_eval(const char *marker, const char *target_os, int py_minor)
{
    int         result = MARK_FALSE;
    const char *s      = marker;
    while (s && *s) {
        const char *or_pos = strstr(s, " or ");
        size_t      len    = or_pos ? (size_t)(or_pos - s) : strlen(s);
        char        term[480];
        if (len >= sizeof(term)) len = sizeof(term) - 1;
        memcpy(term, s, len);
        term[len] = '\0';

        int t = eval_and_chain(term, target_os, py_minor);
        if (t == MARK_TRUE)    return MARK_TRUE;        /* short-circuit */
        if (t == MARK_UNKNOWN) result = MARK_UNKNOWN;
        s = or_pos ? or_pos + 4 : NULL;
    }
    return result;
}

/*
 * marker_excludes — 1 if `marker` (the text at/after ';') positively rules the
 * package OUT for the target environment, so it must be skipped.  Returns 0 to
 * keep it, including when the marker is absent or cannot be fully evaluated.
 */
static int marker_excludes(const char *marker, const char *target_os, int py_minor)
{
    if (!marker) return 0;
    while (*marker == ' ' || *marker == ';') marker++;
    if (!*marker) return 0;
    return marker_eval(marker, target_os, py_minor) == MARK_FALSE;
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

    /* Scan name: stop at any PEP 440 specifier character, extras, marker, or
     * whitespace.  Stopping at whitespace matters for unpinned lines with a
     * marker, e.g. "colorama ; sys_platform != 'win32'": without it the space
     * before ';' is captured into the name ("colorama ") and the resolve URL
     * becomes malformed. */
    size_t name_len = 0;
    while (trimmed[name_len] &&
           !isspace((unsigned char)trimmed[name_len]) &&
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
    while (isspace((unsigned char)*cursor)) ++cursor;
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
    while (isspace((unsigned char)*cursor)) ++cursor;

    /*
     * Accept only '==' for pinned versions; ignore other specifiers.
     * Strip any environment marker (';' and everything after) so that lines
     * like "pywin32==306 ; sys_platform == 'win32'" yield version "306".
     */
    char *version_str = NULL;
    if (cursor[0] == '=' && cursor[1] == '=') {
        const char *ver_start = cursor + 2;
        while (isspace((unsigned char)*ver_start)) ++ver_start;
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

        /* Drop lines whose environment marker excludes the target (e.g.
         * "pywin32==306 ; sys_platform == 'win32'" on a non-Windows target). */
        const char *marker = strchr(trimmed, ';');
        if (marker && marker_excludes(marker, self->target_os, self->py_minor)) {
            fprintf(stderr,
                    "packmule: skipping %s (environment marker excludes target)\n",
                    trimmed);
            continue;
        }

        Package *pkg = parse_line(trimmed);
        if (!pkg)
            continue;

        /* Merge duplicate requirements for the same package (e.g. a bare
         * "requests" plus "requests[socks]==2.31.0"): keep one entry, letting a
         * pinned version win over an unpinned one so we don't bundle two
         * conflicting wheels.  pip unifies these the same way. */
        Package *existing = package_list_find_name(list, pkg->name);
        if (existing) {
            if (pkg->version && !existing->version) {
                existing->version = pm_strdup(pkg->version);
            } else if (pkg->version && existing->version &&
                       strcmp(pkg->version, existing->version) != 0) {
                fprintf(stderr,
                        "packmule: %s requested at both %s and %s; keeping %s\n",
                        existing->name, existing->version, pkg->version,
                        existing->version);
            }
            package_destroy(pkg);
            continue;
        }
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
 *   3 — arch-specific wheel matching `arch` (and target CPython, when set)
 *   2 — universal pure-Python wheel (py3-none-any / py2.py3-none-any)
 *   1 — sdist (.tar.gz or .zip)
 *
 * Also sets pkg->version from info.version when it was previously NULL
 * (i.e., the package was requested unpinned).
 */
static int pypi_parse_response(const char *json, Package *pkg, const char *arch,
                               const char *target_os, int py_minor)
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
                platform_matches(wheel_platform_tag(fn), target_os, arch) &&
                wheel_python_matches(fn, py_minor)) {
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
        char py_buf[16];
        if (py_minor > 0)
            snprintf(py_buf, sizeof(py_buf), "3.%d", py_minor);
        else
            snprintf(py_buf, sizeof(py_buf), "any");
        fprintf(stderr,
                "packmule: no suitable package found for %s==%s"
                " (os: %s, arch: %s, python: %s)\n",
                pkg->name,
                pkg->version ? pkg->version : "(latest)",
                target_os ? target_os : "any",
                arch ? arch : "any",
                py_buf);
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
    const char *target_os = self->target_os;
    const int   py_minor = self->py_minor;
    const char *base = self->repo_url ? self->repo_url : "https://pypi.org/pypi";

    /* Strip trailing slash for consistent URL construction. */
    size_t blen = strlen(base);
    char  *base_trimmed = pm_strndup(base, blen);
    while (blen > 0 && base_trimmed[blen - 1] == '/')
        base_trimmed[--blen] = '\0';

    char *url = pkg->version
        ? pm_asprintf("%s/%s/%s/json", base_trimmed, pkg->name, pkg->version)
        : pm_asprintf("%s/%s/json", base_trimmed, pkg->name);
    pm_free(base_trimmed);

    char *json = fetch_json(url);
    pm_free(url);
    if (!json)
        return -1;

    int ret = pypi_parse_response(json, pkg, arch, target_os, py_minor);
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

/*
 * parse_dep_version — extract an exact ("==") version pin from a PEP 508
 * specifier, e.g. "pydantic-core==2.18.2" → "2.18.2", or "requests (==2.0)" →
 * "2.0".  Returns a heap string the caller owns, or NULL when the spec has no
 * exact pin (unpinned, or a range like ">=4.0" we resolve to latest).
 */
static char *parse_dep_version(const char *spec)
{
    const char *marker = strchr(spec, ';');
    const char *eq     = strstr(spec, "==");
    if (!eq || (marker && eq > marker))
        return NULL;

    const char *v = eq + 2;
    while (*v == ' ') v++;
    size_t len = 0;
    while (v[len] && v[len] != ' ' && v[len] != ',' && v[len] != ')' &&
           v[len] != ';') {
        /* "==1.*" is a wildcard range, not an exact pin — let it resolve to
         * latest rather than fetching the literal version "1.*". */
        if (v[len] == '*')
            return NULL;
        len++;
    }
    return len > 0 ? pm_strndup(v, len) : NULL;
}

static int pypi_get_deps(const Registry *self, const Package *pkg,
                          const PackageList *seen, PackageList *out)
{
    if (!pkg->dep_specs)
        return 0;
    int added = 0;
    for (char **rd = pkg->dep_specs; *rd; rd++) {
        if (dep_is_extras_only(*rd))
            continue;
        /* Skip deps whose environment marker excludes the target environment
         * (e.g. "backports.zoneinfo; python_version < '3.9'" on Python 3.12). */
        const char *marker = strchr(*rd, ';');
        if (marker && marker_excludes(marker, self->target_os, self->py_minor))
            continue;
        char name[256];
        parse_dep_name(*rd, name, sizeof(name));
        if (name[0] && !package_list_contains_name(seen, name)) {
            char *version = parse_dep_version(*rd);
            package_list_add(out, package_create(name, version));
            pm_free(version);
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
};
