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
     * resolve URL becomes malformed.  Unlike pep508_spec_name this keeps the
     * manifest's casing (PyPI URLs accept either; tests pin the behaviour). */
    size_t name_len = 0;
    while (trimmed[name_len] && !strchr("[(;<>=!~@ \t", trimmed[name_len]))
        ++name_len;

    if (name_len == 0) {
        fprintf(stderr, "packmule: unrecognised requirements line: %s\n", line);
        *fatal = 1;
        pm_free(work);
        return NULL;
    }

    char *name = pm_strndup(trimmed, name_len);

    /* Reject the two shapes pep508_spec_* silently returns NULL for, because
     * here dropping the requirement would ship an incomplete bundle: an
     * unterminated extras bracket, and a direct URL ("pkg @ https://…") whose
     * artifact we cannot verify or reproduce from PyPI. */
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
        cursor = close + 1;
        while (isspace((unsigned char)*cursor)) ++cursor;
    }
    if (*cursor == '@') {
        fprintf(stderr,
                "packmule: direct-URL requirement not supported: %s\n"
                "          Pin a PyPI version instead, or copy the file into "
                "the output directory manually.\n", trimmed);
        *fatal = 1;
        pm_free(name);
        pm_free(work);
        return NULL;
    }

    /* Extras are recorded lowercased and space-free so that extras-gated
     * dependencies are followed during transitive resolution.  A lone exact
     * pin ("==2.31.0") stays a pin; every other specifier (">=2,<3", "~=2.4",
     * "==1.*") becomes a PEP 440 constraint resolved at fetch time. */
    char *extras      = pep508_spec_extras(trimmed);
    char *version_str = NULL;
    char *constraint  = pep508_spec_constraint(trimmed);
    if (constraint &&
        constraint[0] == '=' && constraint[1] == '=' && constraint[2] != '=' &&
        !strchr(constraint, ',') && !strchr(constraint, '*')) {
        version_str = pm_strdup(constraint + 2);
        pm_free(constraint);
        constraint = NULL;
    }

    Package *pkg    = package_create(name, version_str);
    pkg->constraint = constraint;
    pkg->extras     = extras;
    /* An exact "==" pin in the manifest is the user's decision; resolution
     * must never move off it. */
    pkg->user_pinned = (version_str != NULL);
    pm_free(version_str);
    pm_free(name);
    pm_free(work);
    return pkg;
}

/*
 * merge_constraint — AND another PEP 440 range into pkg->constraint (comma is
 * conjunction), so the eventual resolution honours every requester.  Returns
 * 1 when this actually narrowed the requirement.
 *
 * Clauses are deduplicated: the fixpoint resolver re-runs get_deps on every
 * round, and without this the constraint string would grow without bound and
 * never reach a stable value.
 */
static int merge_constraint(Package *pkg, const char *constraint)
{
    if (!constraint || !*constraint)
        return 0;

    if (!pkg->constraint)
        return package_set_constraint(pkg, pm_strdup(constraint));

    /* Append only the clauses we do not already carry. */
    char *merged = pm_strdup(pkg->constraint);
    for (const char *p = constraint; *p; ) {
        const char *comma  = strchr(p, ',');
        size_t      len    = comma ? (size_t)(comma - p) : strlen(p);

        if (len > 0) {
            int have = 0;
            for (const char *q = merged; *q && !have; ) {
                const char *qc = strchr(q, ',');
                size_t      qn = qc ? (size_t)(qc - q) : strlen(q);
                have = (qn == len && strncmp(q, p, len) == 0);
                if (!qc) break;
                q = qc + 1;
            }
            if (!have) {
                char *next = pm_asprintf("%s,%.*s", merged, (int)len, p);
                pm_free(merged);
                merged = next;
            }
        }
        if (!comma)
            break;
        p = comma + 1;
    }
    return package_set_constraint(pkg, merged);
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
        Package *existing = package_list_find_name(list, pkg->name,
                                                   package_name_equal_pep503);
        if (existing) {
            if (pkg->version && !existing->version) {
                existing->version     = pm_strdup(pkg->version);
                existing->user_pinned = 1;
            } else if (pkg->version && existing->version &&
                       strcmp(pkg->version, existing->version) != 0) {
                /* Two different exact pins in one manifest is a contradiction
                 * the user has to resolve; guessing would ship a bundle that
                 * does not match what they asked for. */
                fprintf(stderr,
                        "packmule: %s is pinned to both %s and %s in the "
                        "manifest.\n          Remove one of the two lines.\n",
                        existing->name, existing->version, pkg->version);
                package_destroy(pkg);
                fclose(fp);
                return -1;
            }
            if (!existing->version && pkg->constraint)
                merge_constraint(existing, pkg->constraint);
            package_add_extras(existing, pkg->extras);
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

    int ret          = -1;
    int deps_unknown = 0;

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
            char **specs = pm_malloc(((size_t)n + 1) * sizeof(char *));
            int k = 0;
            cJSON *item = NULL;
            cJSON_ArrayForEach(item, req_dist) {
                if (cJSON_IsString(item))
                    specs[k++] = pm_strdup(item->valuestring);
            }
            specs[k] = NULL;
            package_set_dep_specs(pkg, specs);
        } else {
            /* requires_dist is absent or null.  For a wheel that genuinely
             * means "no dependencies"; for an sdist it usually means PyPI
             * could not read the metadata without building it, so the
             * dependency walk stops here and the bundle may be incomplete. */
            package_set_dep_specs(pkg, NULL);
            deps_unknown = !cJSON_IsArray(req_dist);
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
    int         saw_file        = 0; /* any file at all, before filtering */
    int         all_yanked      = 1;

    cJSON *dist = NULL;
    cJSON_ArrayForEach(dist, urls) {
        cJSON *fn_item = cJSON_GetObjectItemCaseSensitive(dist, "filename");
        if (!cJSON_IsString(fn_item))
            continue;
        const char *fn = fn_item->valuestring;
        saw_file = 1;

        /* Never select a yanked file: pip refuses them for unpinned installs. */
        cJSON *yanked = cJSON_GetObjectItemCaseSensitive(dist, "yanked");
        if (cJSON_IsTrue(yanked))
            continue;
        all_yanked = 0;

        WheelTags tags;
        if (dist_is_wheel(fn) && wheel_parse_tags(fn, &tags) == 0) {
            int universal = strcmp(tags.platform, "any") == 0;

            if (universal) {
                /* Pure-Python, but still only installable on interpreters its
                 * tag covers: "py39-none-any" is not valid on 3.8. */
                if (wheel_python_matches(fn, py_minor) && chosen_priority < 2) {
                    chosen          = dist;
                    chosen_filename = fn;
                    chosen_priority = 2;
                }
            } else if (arch &&
                       wheel_platform_matches(tags.platform, target_os, arch) &&
                       wheel_python_matches(fn, py_minor)) {
                /* Among matching arch wheels prefer the LOWEST manylinux
                 * glibc floor: a manylinux_2_17 wheel installs everywhere a
                 * manylinux_2_39 one does, but not vice versa — picking the
                 * highest could produce a wheel the target's older glibc
                 * refuses. */
                int gmaj = 0, gmin = 0;
                int gl = wheel_manylinux_glibc(tags.platform, &gmaj, &gmin)
                       ? gmaj * 1000 + gmin : 0;
                if (chosen_priority < 3 ||
                    (chosen_priority == 3 && gl < chosen_glibc)) {
                    chosen          = dist;
                    chosen_filename = fn;
                    chosen_priority = 3;
                    chosen_glibc    = gl;
                }
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
        /* Distinguish "this release was withdrawn" from "nothing matches your
         * target": the fixes are completely different and the previous single
         * message sent people looking at the wrong one. */
        if (saw_file && all_yanked) {
            fprintf(stderr,
                    "packmule: %s==%s has been yanked from PyPI; every file "
                    "for that release is withdrawn.\n"
                    "          Pin a different version.\n",
                    pkg->name, pkg->version ? pkg->version : "(latest)");
            goto done;
        }

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

    cJSON *digests     = cJSON_GetObjectItemCaseSensitive(chosen, "digests");
    cJSON *sha256_item = cJSON_GetObjectItemCaseSensitive(digests, "sha256");
    if (!cJSON_IsString(sha256_item)) {
        fprintf(stderr, "packmule: missing SHA-256 digest for %s\n", chosen_filename);
        goto done;
    }

    pm_free(pkg->url);
    pm_free(pkg->filename);
    pkg->url = pm_strdup(url_item->valuestring);
    digest_set(&pkg->digest, DIGEST_SHA256, DIGEST_ENC_HEX,
               sha256_item->valuestring);
    /* The filename came off the wire: keep only its basename so a hostile or
     * broken index serving "a/../../etc/x.whl" cannot escape the output dir. */
    pkg->filename = pm_strdup(pm_basename(chosen_filename));

    /* An sdist whose metadata PyPI could not read means we are flying blind
     * on its dependencies.  Say so loudly: a bundle missing a transitive dep
     * fails on the air-gapped machine, where it cannot be fixed. */
    if (deps_unknown && dist_is_sdist(pkg->filename))
        fprintf(stderr,
                "packmule: warning: PyPI publishes no dependency metadata for "
                "%s==%s.\n"
                "          Its dependencies were NOT followed and may be "
                "missing from the bundle.\n"
                "          Add them to your manifest explicitly if the "
                "offline install fails.\n",
                pkg->name, pkg->version ? pkg->version : "(latest)");
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

    /*
     * A version we chose ourselves on an earlier round is a hypothesis, not a
     * fact: if a later dependent narrowed the constraint, that choice has to
     * be made again from scratch.  Only a user pin is fixed.
     */
    if (pkg->version && !pkg->user_pinned && pkg->constraint) {
        pm_free(pkg->version);
        pkg->version = NULL;
    }

    char *url = pkg->version
        ? pm_asprintf("%s/%s/%s/json", base_trimmed, pkg->name, pkg->version)
        : pm_asprintf("%s/%s/json", base_trimmed, pkg->name);

    char *json = fetch_json(url);
    pm_free(url);
    if (!json) {
        pm_free(base_trimmed);
        return -1;
    }

    /* A user pin that contradicts what its dependents need is a real conflict
     * the bundle cannot paper over. */
    if (pkg->version && pkg->user_pinned && pkg->constraint &&
        pep440_satisfies(pkg->version, pkg->constraint) == 0) {
        fprintf(stderr,
                "packmule: %s is pinned to %s but other packages require "
                "'%s'.\n"
                "          These cannot both hold; adjust the pin in your "
                "manifest.\n",
                pkg->name, pkg->version, pkg->constraint);
        pm_free(json);
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
        static const char *const BUILD_DEPS[] = { "setuptools", "wheel" };
        for (size_t i = 0; i < sizeof(BUILD_DEPS) / sizeof(BUILD_DEPS[0]); i++) {
            if (!package_list_find_name(seen, BUILD_DEPS[i],
                                        package_name_equal_pep503)) {
                package_list_add(out, package_create(BUILD_DEPS[i], NULL));
                added++;
            }
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

        char *version    = pep508_spec_exact_version(*rd);
        char *constraint = pep508_spec_constraint(*rd);
        char *dep_extras = pep508_spec_extras(*rd);

        Package *existing = package_list_find_name(seen, name,
                                                   package_name_equal_pep503);
        if (existing) {
            /*
             * Merge this dependent's requirement into the existing entry.
             * Every merge that widens what the entry has to satisfy marks it
             * dirty, and the resolver comes back for it — including when it
             * has already been resolved.  Recording the requirement and
             * letting resolution re-run is what makes the outcome independent
             * of the order packages happen to appear in.
             */
            if (constraint)
                merge_constraint(existing, constraint);

            /* An exact "==" from a dependent is a constraint like any other,
             * not a pin: only the manifest can pin. */
            if (version && !existing->user_pinned &&
                (!existing->version ||
                 strcmp(version, existing->version) != 0)) {
                char *as_spec = pm_asprintf("==%s", version);
                merge_constraint(existing, as_spec);
                pm_free(as_spec);
            }

            /* The bug this replaces: extras requested by a dependent used to
             * be dropped whenever the package was already queued, silently
             * omitting every dependency they gate. */
            package_add_extras(existing, dep_extras);

            pm_free(version);
            pm_free(constraint);
            pm_free(dep_extras);
            continue;
        }

        Package *dep = package_create(name, NULL);
        if (version) {
            /* Carry an exact requirement as a constraint so a second
             * dependent asking for a different version is detected as the
             * conflict it is, rather than silently losing. */
            dep->constraint = pm_asprintf("==%s", version);
            if (constraint)
                merge_constraint(dep, constraint);
        } else {
            dep->constraint = constraint;
            constraint = NULL;
        }
        dep->extras = dep_extras;
        package_list_add(out, dep);
        pm_free(version);
        pm_free(constraint);
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
    .name_equal    = package_name_equal_pep503,
    .detect        = pypi_detect,
    .parse_manifest = pypi_parse_manifest,
    .resolve       = pypi_resolve,
    .get_deps      = pypi_get_deps,
    .ctx           = NULL, /* arch string, injected by main.c */
    .repo_url      = NULL, /* optional; defaults to https://pypi.org/pypi */
};
