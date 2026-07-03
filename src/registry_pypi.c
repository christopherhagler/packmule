#include "registry.h"
#include "registry_internal.h"
#include "network.h"
#include "package.h"
#include "pep440.h"
#include "pep508.h"
#include "utils.h"
#include "wheeltag.h"

#include <cjson/cJSON.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── requirements.txt line parser ────────────────────────────────────────── */

/*
 * parse_line — parse one non-blank, non-comment requirements line.
 *
 * Handles:
 *   requests                     → name only, resolved to latest
 *   requests==2.31.0             → exact pin
 *   requests>=2.28,<3            → PEP 440 constraint, resolved at fetch time
 *   uvicorn[standard]==0.29.0    → extras recorded; their deps are followed
 *
 * Sets *fatal for a requirement we cannot represent (direct URL, unparseable
 * name): silently dropping it would ship an incomplete bundle, so the caller
 * must abort instead of continuing.
 * Returns NULL for blank/comment lines or on error.
 */
static Package *parse_line(const char *line, int *fatal)
{
    *fatal = 0;

    /* Strip inline comments. */
    const char *comment = strchr(line, '#');
    char *work    = comment ? pm_strndup(line, (size_t)(comment - line))
                            : pm_strdup(line);
    char *trimmed = pm_strtrim(work);

    if (trimmed[0] == '\0') {
        pm_free(work);
        return NULL;
    }

    /* Scan name: stop at any PEP 440 specifier character, extras, marker,
     * '@' (direct URL), or whitespace.  Stopping at whitespace matters for
     * unpinned lines with a marker, e.g. "colorama ; sys_platform != 'win32'":
     * without it the space before ';' is captured into the name and the
     * resolve URL becomes malformed. */
    size_t name_len = 0;
    while (trimmed[name_len] &&
           !isspace((unsigned char)trimmed[name_len]) &&
           trimmed[name_len] != '[' &&
           trimmed[name_len] != '=' &&
           trimmed[name_len] != '>' &&
           trimmed[name_len] != '<' &&
           trimmed[name_len] != '!' &&
           trimmed[name_len] != '~' &&
           trimmed[name_len] != '@' &&
           trimmed[name_len] != ';')
        ++name_len;

    if (name_len == 0) {
        fprintf(stderr, "packmule: unrecognised requirements line: %s\n", line);
        *fatal = 1;
        pm_free(work);
        return NULL;
    }

    char *name = pm_strndup(trimmed, name_len);

    /* Optional extras [a,b]: recorded lowercased and space-free so that
     * extras-gated dependencies are followed during transitive resolution. */
    char *extras = NULL;
    const char *cursor = trimmed + name_len;
    while (isspace((unsigned char)*cursor)) ++cursor;
    if (*cursor == '[') {
        const char *close = strchr(cursor, ']');
        if (!close) {
            fprintf(stderr, "packmule: unterminated extras in: %s\n", line);
            *fatal = 1;
            pm_free(name);
            pm_free(work);
            return NULL;
        }
        char ebuf[128];
        size_t k = 0;
        for (const char *p = cursor + 1; p < close && k < sizeof(ebuf) - 1; p++)
            if (!isspace((unsigned char)*p))
                ebuf[k++] = (char)tolower((unsigned char)*p);
        ebuf[k] = '\0';
        if (k)
            extras = pm_strdup(ebuf);
        cursor = close + 1;
    }
    while (isspace((unsigned char)*cursor)) ++cursor;

    /* Direct URL requirement ("pkg @ https://…"): we cannot verify what that
     * URL serves, and resolving the name against PyPI instead would silently
     * bundle a different artifact than the one the manifest pinned. */
    if (*cursor == '@') {
        fprintf(stderr,
                "packmule: direct-URL requirement not supported: %s\n"
                "          Pin a PyPI version instead, or copy the file into "
                "the output directory manually.\n", trimmed);
        *fatal = 1;
        pm_free(extras);
        pm_free(name);
        pm_free(work);
        return NULL;
    }

    /* Collect the version specifier up to any ';' marker, dropping spaces and
     * parentheses.  A lone exact pin ("==2.31.0") stays a pin; every other
     * specifier (">=2,<3", "~=2.4", "==1.*") becomes a PEP 440 constraint
     * resolved against the package's release list at fetch time. */
    char spec[256];
    size_t k = 0;
    for (; *cursor && *cursor != ';' && k < sizeof(spec) - 1; ++cursor) {
        if (isspace((unsigned char)*cursor) || *cursor == '(' || *cursor == ')')
            continue;
        spec[k++] = *cursor;
    }
    spec[k] = '\0';

    char *version_str = NULL;
    char *constraint  = NULL;
    if (k > 0) {
        if (spec[0] == '=' && spec[1] == '=' && spec[2] != '=' &&
            !strchr(spec, ',') && !strchr(spec, '*'))
            version_str = pm_strdup(spec + 2);
        else
            constraint = pm_strdup(spec);
    }

    Package *pkg    = package_create(name, version_str);
    pkg->constraint = constraint;
    pkg->extras     = extras;
    pm_free(version_str);
    pm_free(name);
    pm_free(work);
    return pkg;
}

#define MAX_INCLUDE_DEPTH 8

static int parse_requirements_file(const Registry *self, const char *path,
                                   PackageList *list, int depth);

/*
 * handle_option_line — process a requirements line starting with '-'.
 * `-r`/`--requirement` includes are followed (relative to the including
 * file); `-e` is a hard error (an editable install cannot be bundled);
 * everything else is skipped with a warning.  Returns 0 to continue parsing,
 * -1 to abort.
 */
static int handle_option_line(const Registry *self, const char *manifest_path,
                              const char *line, PackageList *list, int depth)
{
    const char *file = NULL;
    if (line[1] == 'r' && (line[2] == ' ' || line[2] == '\t'))
        file = line + 3;
    else if (strncmp(line, "--requirement", 13) == 0 &&
             (line[13] == ' ' || line[13] == '='))
        file = line + 14;

    if (file) {
        while (isspace((unsigned char)*file)) file++;
        if (!*file) {
            fprintf(stderr, "packmule: missing filename in: %s\n", line);
            return -1;
        }
        char inc[4096];
        const char *slash = strrchr(manifest_path, '/');
        if (file[0] != '/' && slash)
            snprintf(inc, sizeof(inc), "%.*s/%s",
                     (int)(slash - manifest_path), manifest_path, file);
        else
            snprintf(inc, sizeof(inc), "%s", file);
        return parse_requirements_file(self, inc, list, depth + 1);
    }

    if ((line[1] == 'e' && (line[2] == '\0' || line[2] == ' ')) ||
        strncmp(line, "--editable", 10) == 0) {
        fprintf(stderr,
                "packmule: editable requirement cannot be bundled: %s\n", line);
        return -1;
    }

    if ((line[1] == 'c' && (line[2] == '\0' || line[2] == ' ')) ||
        strncmp(line, "--constraint", 12) == 0)
        fprintf(stderr,
                "packmule: warning: ignoring constraints file (%s); resolved "
                "versions may differ from your environment\n", line);
    else
        fprintf(stderr, "packmule: skipping unsupported option: %s\n", line);
    return 0;
}

/*
 * parse_requirements_file — parse `path` into `list`, following -r includes
 * up to MAX_INCLUDE_DEPTH.  Returns 0 on success, -1 on any error that would
 * otherwise produce an incomplete bundle.
 */
static int parse_requirements_file(const Registry *self, const char *path,
                                   PackageList *list, int depth)
{
    if (depth > MAX_INCLUDE_DEPTH) {
        fprintf(stderr, "packmule: requirements include depth exceeds %d "
                        "(circular -r include?): %s\n", MAX_INCLUDE_DEPTH, path);
        return -1;
    }

    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "packmule: cannot open %s\n", path);
        return -1;
    }

    char line[4096];
    while (fgets(line, (int)sizeof(line), fp)) {
        char *trimmed = pm_strtrim(line);

        if (trimmed[0] == '\0' || trimmed[0] == '#')
            continue;

        if (trimmed[0] == '-') {
            if (handle_option_line(self, path, trimmed, list, depth) != 0) {
                fclose(fp);
                return -1;
            }
            continue;
        }

        /* Drop lines whose environment marker excludes the target (e.g.
         * "pywin32==306 ; sys_platform == 'win32'" on a non-Windows target). */
        const char *marker = strchr(trimmed, ';');
        if (marker && pep508_marker_excludes(marker, self->target_os, self->py_minor)) {
            fprintf(stderr,
                    "packmule: skipping %s (environment marker excludes target)\n",
                    trimmed);
            continue;
        }

        int      fatal = 0;
        Package *pkg   = parse_line(trimmed, &fatal);
        if (!pkg) {
            if (fatal) {
                fclose(fp);
                return -1;
            }
            continue;
        }

        /* Merge duplicate requirements for the same package (e.g. a bare
         * "requests" plus "requests[socks]==2.31.0"): keep one entry, letting
         * a pinned version win over an unpinned one so we don't bundle two
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
            /* Unpinned ranges intersect by concatenation: comma is AND. */
            if (!existing->version && pkg->constraint) {
                if (existing->constraint) {
                    char *merged = pm_asprintf("%s,%s", existing->constraint,
                                               pkg->constraint);
                    pm_free(existing->constraint);
                    existing->constraint = merged;
                } else {
                    existing->constraint = pm_strdup(pkg->constraint);
                }
            }
            /* Union of requested extras. */
            if (pkg->extras) {
                if (!existing->extras) {
                    existing->extras = pm_strdup(pkg->extras);
                } else if (!strstr(existing->extras, pkg->extras)) {
                    char *merged = pm_asprintf("%s,%s", existing->extras,
                                               pkg->extras);
                    pm_free(existing->extras);
                    existing->extras = merged;
                }
            }
            package_destroy(pkg);
            continue;
        }
        package_list_add(list, pkg);
    }

    fclose(fp);
    return 0;
}

static PackageList *pypi_parse_manifest(const Registry *self, const char *path)
{
    PackageList *list = package_list_create();
    if (parse_requirements_file(self, path, list, 0) != 0) {
        package_list_destroy(list);
        return NULL;
    }
    return list;
}

/* ── PyPI JSON response decoder ───────────────────────────────────────────── */

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
int pypi_parse_response(const char *json, Package *pkg, const char *arch,
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
    int         chosen_glibc    = 0; /* maj*1000+min for manylinux, 0 otherwise */

    cJSON *dist = NULL;
    cJSON_ArrayForEach(dist, urls) {
        cJSON *fn_item = cJSON_GetObjectItemCaseSensitive(dist, "filename");
        if (!cJSON_IsString(fn_item))
            continue;
        const char *fn = fn_item->valuestring;

        /* Never select a yanked file: pip refuses them for unpinned installs. */
        cJSON *yanked = cJSON_GetObjectItemCaseSensitive(dist, "yanked");
        if (cJSON_IsTrue(yanked))
            continue;

        if (dist_is_wheel(fn)) {
            if (arch && !dist_is_universal_wheel(fn) &&
                wheel_platform_matches(wheel_platform_tag(fn), target_os, arch) &&
                wheel_python_matches(fn, py_minor)) {
                /* Among matching arch wheels prefer the LOWEST manylinux
                 * glibc floor: a manylinux_2_17 wheel installs everywhere a
                 * manylinux_2_39 one does, but not vice versa — picking the
                 * highest could produce a wheel the target's older glibc
                 * refuses. */
                int gmaj = 0, gmin = 0;
                int gl = wheel_manylinux_glibc(wheel_platform_tag(fn), &gmaj, &gmin)
                       ? gmaj * 1000 + gmin : 0;
                if (chosen_priority < 3 ||
                    (chosen_priority == 3 && gl < chosen_glibc)) {
                    chosen          = dist;
                    chosen_filename = fn;
                    chosen_priority = 3;
                    chosen_glibc    = gl;
                }
            } else if (dist_is_universal_wheel(fn) && chosen_priority < 2) {
                chosen          = dist;
                chosen_filename = fn;
                chosen_priority = 2;
            }
        } else if (dist_is_sdist(fn) && chosen_priority < 1) {
            chosen          = dist;
            chosen_filename = fn;
            chosen_priority = 1;
        }
    }

    /* A source distribution means the air-gapped machine must build the
     * package itself.  pypi_get_deps bundles setuptools/wheel alongside it,
     * but a package with C extensions still needs a compiler on the target —
     * say so now, while there is still a connected machine to fix it on. */
    if (chosen && chosen_priority == 1)
        fprintf(stderr,
                "packmule: warning: no compatible wheel for %s==%s; bundling "
                "source distribution %s\n"
                "          (the target machine must be able to build it: "
                "python3 headers, and a compiler if it has C extensions)\n",
                pkg->name, pkg->version ? pkg->version : "(latest)",
                chosen_filename);

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
    /* The filename came off the wire: keep only its basename so a hostile or
     * broken index serving "a/../../etc/x.whl" cannot escape the output dir. */
    pkg->filename = pm_strdup(pm_basename(chosen_filename));
    ret = 0;

done:
    cJSON_Delete(root);
    return ret;
}

/* ── Resolver ────────────────────────────────────────────────────────────── */

/*
 * json_info_version — extract info.version from a PyPI JSON document.
 * Returns a heap copy or NULL.
 */
static char *json_info_version(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root)
        return NULL;
    char  *out  = NULL;
    cJSON *info = cJSON_GetObjectItemCaseSensitive(root, "info");
    cJSON *ver  = cJSON_GetObjectItemCaseSensitive(info, "version");
    if (cJSON_IsString(ver))
        out = pm_strdup(ver->valuestring);
    cJSON_Delete(root);
    return out;
}

/*
 * pick_constrained_version — scan the "releases" object of a PyPI JSON
 * document for the highest PEP 440 version satisfying `constraint`, skipping
 * pre-releases (unless the constraint itself names one, mirroring pip) and
 * releases whose files are all yanked or absent.  Returns a heap version
 * string, or NULL when nothing satisfies.
 */
static char *pick_constrained_version(const char *json, const char *constraint)
{
    cJSON *root = cJSON_Parse(json);
    if (!root)
        return NULL;

    char  *best      = NULL;
    int    admit_pre = pep440_spec_admits_prerelease(constraint);
    cJSON *releases  = cJSON_GetObjectItemCaseSensitive(root, "releases");
    cJSON *rel       = NULL;

    if (cJSON_IsObject(releases)) {
        cJSON_ArrayForEach(rel, releases) {
            const char *ver = rel->string;
            if (!ver || !pep440_valid(ver))
                continue;
            if (!admit_pre && pep440_is_prerelease(ver))
                continue;
            if (pep440_satisfies(ver, constraint) != 1)
                continue;

            /* The release must have at least one non-yanked file. */
            int    usable = 0;
            cJSON *file   = NULL;
            cJSON_ArrayForEach(file, rel) {
                cJSON *yanked = cJSON_GetObjectItemCaseSensitive(file, "yanked");
                if (!cJSON_IsTrue(yanked)) {
                    usable = 1;
                    break;
                }
            }
            if (!usable)
                continue;

            if (!best || pep440_cmp(ver, best) > 0) {
                pm_free(best);
                best = pm_strdup(ver);
            }
        }
    }

    cJSON_Delete(root);
    return best;
}

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

    char *json = fetch_json(url);
    pm_free(url);
    if (!json) {
        pm_free(base_trimmed);
        return -1;
    }

    /*
     * Range requirement: check the latest release against the constraint and,
     * when it does not satisfy, re-fetch the highest release that does.
     * Without this step a requirement like "numpy>=1.20,<2.0" would bundle
     * the latest numpy 2.x — a wheel pip's offline resolver then rejects,
     * making the whole bundle uninstallable on the air-gapped machine.
     */
    if (!pkg->version && pkg->constraint) {
        char *latest = json_info_version(json);
        int   sat    = latest ? pep440_satisfies(latest, pkg->constraint) : 1;

        if (sat == -1) {
            fprintf(stderr,
                    "packmule: warning: cannot evaluate constraint '%s' for "
                    "%s; using latest (%s)\n",
                    pkg->constraint, pkg->name, latest);
        } else if (sat == 0) {
            char *best = pick_constrained_version(json, pkg->constraint);
            if (!best) {
                fprintf(stderr,
                        "packmule: no release of %s satisfies '%s'\n",
                        pkg->name, pkg->constraint);
                pm_free(latest);
                pm_free(json);
                pm_free(base_trimmed);
                return -1;
            }
            char *vurl = pm_asprintf("%s/%s/%s/json",
                                     base_trimmed, pkg->name, best);
            char *vjson = fetch_json(vurl);
            pm_free(vurl);
            if (!vjson) {
                pm_free(best);
                pm_free(latest);
                pm_free(json);
                pm_free(base_trimmed);
                return -1;
            }
            pm_free(json);
            json = vjson;
            pm_free(best);
        }
        pm_free(latest);
    }
    pm_free(base_trimmed);

    int ret = pypi_parse_response(json, pkg, arch, target_os, py_minor);
    pm_free(json);
    return ret;
}

/* ── Transitive dependency resolver ─────────────────────────────────────── */

static int pypi_get_deps(const Registry *self, const Package *pkg,
                          const PackageList *seen, PackageList *out)
{
    int added = 0;

    /* A source distribution means the target machine must build the package.
     * Offline pip cannot fetch build backends, so bundle setuptools and wheel
     * alongside it (install.sh installs them first, then builds with
     * --no-build-isolation). */
    if (pkg->filename && dist_is_sdist(pkg->filename)) {
        if (!package_list_find_name(seen, "setuptools")) {
            package_list_add(out, package_create("setuptools", NULL));
            added++;
        }
        if (!package_list_find_name(seen, "wheel")) {
            package_list_add(out, package_create("wheel", NULL));
            added++;
        }
    }

    if (!pkg->dep_specs)
        return added;

    for (char **rd = pkg->dep_specs; *rd; rd++) {
        const char *marker = strchr(*rd, ';');

        /* Extras-gated deps are included only when the parent was requested
         * with a matching extra (e.g. uvicorn[standard] pulls in httptools);
         * otherwise they are optional and must never be bundled. */
        if (pep508_dep_is_extras_only(*rd) &&
            !pep508_marker_matches_extras(marker, pkg->extras))
            continue;

        /* Skip deps whose environment marker excludes the target environment
         * (e.g. "backports.zoneinfo; python_version < '3.9'" on Python 3.12).
         * The "extra" variable evaluates as UNKNOWN, so a matched extras
         * clause never causes a false exclusion here. */
        if (marker && pep508_marker_excludes(marker, self->target_os, self->py_minor))
            continue;

        char name[256];
        pep508_spec_name(*rd, name, sizeof(name));
        if (!name[0])
            continue;

        Package *existing = package_list_find_name(seen, name);
        if (existing) {
            char *version    = pep508_spec_exact_version(*rd);
            char *constraint = version ? NULL : pep508_spec_constraint(*rd);

            if (version && existing->version &&
                strcmp(version, existing->version) != 0) {
                /* Two different exact pins: surface the conflict instead of
                 * silently keeping the first. */
                fprintf(stderr,
                        "packmule: %s requires %s==%s but %s is already "
                        "selected; keeping %s\n",
                        pkg->name, name, version, existing->version,
                        existing->version);
            } else if (constraint && existing->version &&
                       pep440_satisfies(existing->version, constraint) == 0) {
                fprintf(stderr,
                        "packmule: warning: %s requires %s%s but %s is "
                        "already selected; the offline install may fail\n",
                        pkg->name, name, constraint, existing->version);
            } else if (constraint && !existing->version) {
                /* Not resolved yet: intersect ranges (comma is AND) so the
                 * eventual resolution honours every dependent. */
                if (existing->constraint) {
                    char *merged = pm_asprintf("%s,%s", existing->constraint,
                                               constraint);
                    pm_free(existing->constraint);
                    existing->constraint = merged;
                } else {
                    existing->constraint = pm_strdup(constraint);
                }
            }
            pm_free(version);
            pm_free(constraint);
            continue;
        }

        char    *version = pep508_spec_exact_version(*rd);
        Package *dep     = package_create(name, version);
        if (!version)
            dep->constraint = pep508_spec_constraint(*rd);
        dep->extras = pep508_spec_extras(*rd);
        package_list_add(out, dep);
        pm_free(version);
        added++;
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
