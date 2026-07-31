/*
 * registry_npm.c — npm registry backend.
 *
 * Manifest format: package.json ("dependencies", "optionalDependencies",
 *                  and non-optional "peerDependencies"; devDependencies are
 *                  omitted and install.sh runs npm with --omit=dev).
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
 * Transitive dependencies are read from the resolved version document's
 * dependency objects (see NPM_DEP_KEYS) and stored in pkg->dep_specs as
 * "name@range" for npm_get_deps() to enqueue breadth-first with their
 * ranges intact.  npm 7+ installs peerDependencies by default, so peers
 * must be part of the bundled closure or the offline install fails.
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

/* Largest package.json / package-lock.json we will read into memory. */
#define NPM_MAX_MANIFEST_BYTES ((size_t)256 * 1024 * 1024)

/* ── Dependency-spec helpers (shared by manifest and transitive paths) ───── */

/*
 * apply_alias — rewrite an npm alias in place: "dep": "npm:real-name@range"
 * installs `real-name`, so the alias name must be replaced before querying
 * the registry (the alias itself does not exist as a package).  `*name` and
 * `*range` are owned heap strings (range may be NULL) and are replaced when
 * the range carries an alias.
 */
static void apply_alias(char **name, char **range)
{
    if (!*range || strncmp(*range, "npm:", 4) != 0)
        return;

    /* Split "real-name@range" at the LAST '@' so scoped names survive. */
    const char *real = *range + 4;
    const char *at   = strrchr(real, '@');
    pm_free(*name);
    if (at && at != real) {
        *name = pm_strndup(real, (size_t)(at - real));
        char *nrange = (at[1] != '\0') ? pm_strdup(at + 1) : NULL;
        pm_free(*range);
        *range = nrange;
    } else {
        *name = pm_strdup(real);
        pm_free(*range);
        *range = NULL;
    }
}

/*
 * spec_is_unbundleable — specs with no registry tarball behind them: git
 * repositories, direct URLs, local paths, and workspace references.  A '/'
 * catches the bare GitHub shorthand ("user/repo"); no semver range contains
 * one.  Call after apply_alias (a scoped alias target has a '/').
 */
static int spec_is_unbundleable(const char *spec)
{
    return strncmp(spec, "file:", 5)       == 0 ||
           strncmp(spec, "link:", 5)       == 0 ||
           strncmp(spec, "workspace:", 10) == 0 ||
           strncmp(spec, "git", 3)         == 0 ||   /* git:, git+ssh:, … */
           strncmp(spec, "github:", 7)     == 0 ||
           strncmp(spec, "http://", 7)     == 0 ||
           strncmp(spec, "https://", 8)    == 0 ||
           strchr(spec, '/')               != NULL;
}

/*
 * npm_local_filename — the bundled tarball's local name, derived from the
 * last path component of its registry URL.  For scoped packages the scope is
 * prefixed: "@babel/core" and "@vue/core" both publish "core-<ver>.tgz",
 * which would collide in one output dir.  install.sh's lockfile rewriter
 * mirrors this rule — keep the two in sync.  Caller owns the result.
 */
static char *npm_local_filename(const char *name, const char *tarball_url)
{
    const char *slash = strrchr(tarball_url, '/');
    const char *base  = slash ? slash + 1 : tarball_url;
    if (name[0] == '@') {
        const char *scope_end = strchr(name, '/');
        if (scope_end)
            return pm_asprintf("%.*s-%s", (int)(scope_end - (name + 1)),
                               name + 1, base);
    }
    return pm_strdup(base);
}

/*
 * peer_is_optional — is `name` marked { "optional": true } in the document's
 * peerDependenciesMeta?  Optional peers are the consumer's choice and must
 * not be forced into the bundle.
 */
static int peer_is_optional(const cJSON *doc, const char *name)
{
    const cJSON *meta = cJSON_GetObjectItemCaseSensitive(doc,
                                                         "peerDependenciesMeta");
    const cJSON *m    = cJSON_GetObjectItemCaseSensitive(meta, name);
    const cJSON *opt  = cJSON_GetObjectItemCaseSensitive(m, "optional");
    return cJSON_IsTrue(opt);
}

/*
 * The dependency objects that must reach the air-gapped machine.  npm 7+
 * installs peerDependencies by default, and arborist resolves
 * optionalDependencies before deciding platform compatibility — offline,
 * anything missing from this closure is a hard install failure.
 * devDependencies stay out: install.sh runs npm with --omit=dev.
 */
static const char *const NPM_DEP_KEYS[] = {
    "dependencies", "optionalDependencies", "peerDependencies",
};
enum { NPM_DEP_NKEYS = 3 };

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

/* Specs that mean "whatever is newest" — served directly by /<name>/latest. */
static int is_any_version(const char *ver)
{
    return !ver || ver[0] == '\0' ||
           strcmp(ver, "*")      == 0 ||
           strcmp(ver, "x")      == 0 ||
           strcmp(ver, "latest") == 0;
}

/*
 * npm_package_from_spec — build a Package from a "name": "spec" pair.
 *
 * An exact triple is a pin the resolver must not move off; anything else is a
 * range, and ranges belong in `constraint`.  Storing a range in `version` —
 * which this backend used to do — leaves no way to distinguish what was asked
 * for from what was chosen.
 */
static Package *npm_package_from_spec(const char *name, const char *spec)
{
    if (is_exact_version(spec)) {
        Package *p = package_create(name, spec);
        p->user_pinned = 1;
        return p;
    }
    Package *p = package_create(name, NULL);
    if (spec && !is_any_version(spec))
        p->constraint = pm_strdup(spec);
    return p;
}

/* ── Manifest parser ─────────────────────────────────────────────────────── */

/* read_json_file — parse the JSON document at `path`.  Returns NULL (with a
 * message when `quiet` is 0) on I/O or parse failure.  Caller cJSON_Deletes. */
static cJSON *read_json_file(const char *path, int quiet)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        if (!quiet)
            fprintf(stderr, "packmule: cannot open %s\n", path);
        return NULL;
    }

    long fsize = -1;
    if (fseek(fp, 0, SEEK_END) == 0)
        fsize = ftell(fp);
    /* fseek back rather than rewind(): rewind has no way to report failure,
     * and a silent failure here would read the file from the wrong offset. */
    if (fsize < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        if (!quiet)
            fprintf(stderr, "packmule: cannot determine size of %s\n", path);
        fclose(fp);
        return NULL;
    }

    /* Bound the read.  A lockfile for a very large monorepo runs to a few tens
     * of megabytes; past that the input is not a manifest, and allocating
     * whatever size the filesystem reports would turn a bad path into an
     * out-of-memory abort. */
    if ((size_t)fsize > NPM_MAX_MANIFEST_BYTES) {
        if (!quiet)
            fprintf(stderr,
                    "packmule: %s is larger than %zu MB; refusing to parse it\n",
                    path, NPM_MAX_MANIFEST_BYTES / ((size_t)1024 * 1024));
        fclose(fp);
        return NULL;
    }

    size_t want   = (size_t)fsize;
    char  *buf    = pm_malloc(want + 1);
    size_t nread  = fread(buf, 1, want, fp);
    if (nread > want)          /* unreachable; keeps the bound explicit */
        nread = want;
    /* buf holds want+1 bytes and nread <= want, so this index is in range.
     * The analyzer cannot see the size pm_malloc was called with (it lives in
     * another translation unit and returns void *), so it treats the file
     * size as an unbounded tainted value. */
    buf[nread] = '\0';         /* NOLINT(clang-analyzer-security.ArrayBound) */
    fclose(fp);

    cJSON *root = cJSON_Parse(buf);
    pm_free(buf);
    if (!root && !quiet)
        fprintf(stderr, "packmule: failed to parse JSON in %s\n", path);
    return root;
}

/*
 * npm_parse_lockfile — build the download list from a package-lock.json /
 * npm-shrinkwrap.json "packages" object (lockfileVersion >= 2).
 *
 * The lock is npm's own resolution of the FULL tree — including multiple
 * versions of one package at different nesting positions, which the range
 * walker cannot represent.  Every non-dev entry becomes one download (same
 * name+version at several positions dedupes to one tarball); install.sh
 * replays the tree with `npm ci --offline` against the bundled files.
 */
static PackageList *npm_parse_lockfile(const cJSON *root, const char *path)
{
    const cJSON *lv = cJSON_GetObjectItemCaseSensitive(root, "lockfileVersion");
    if (!cJSON_IsNumber(lv) || lv->valuedouble < 2) {
        fprintf(stderr,
                "packmule: %s uses lockfileVersion %g; version 2 or 3 is "
                "required.\n          Regenerate it with npm 7 or newer "
                "(npm install --package-lock-only).\n",
                path, cJSON_IsNumber(lv) ? lv->valuedouble : 0.0);
        return NULL;
    }

    const cJSON *pkgs = cJSON_GetObjectItemCaseSensitive(root, "packages");
    if (!cJSON_IsObject(pkgs)) {
        fprintf(stderr, "packmule: %s has no \"packages\" object\n", path);
        return NULL;
    }

    PackageList *list  = package_list_create();
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, pkgs) {
        const char *key = entry->string;
        if (!key || !*key)              /* "" is the root project itself */
            continue;

        /* devDependencies are not installed on the target (--omit=dev);
         * bundled deps ship inside their parent's tarball. */
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(entry, "dev")) ||
            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(entry, "inBundle")) ||
            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(entry, "link")))
            continue;

        /* The name is the path segment after the LAST "node_modules/". */
        const char *name = key, *nm;
        while ((nm = strstr(name, "node_modules/")) != NULL)
            name = nm + strlen("node_modules/");

        const cJSON *ver = cJSON_GetObjectItemCaseSensitive(entry, "version");
        const cJSON *res = cJSON_GetObjectItemCaseSensitive(entry, "resolved");
        const cJSON *integ = cJSON_GetObjectItemCaseSensitive(entry, "integrity");

        if (!cJSON_IsString(res) ||
            strncmp(res->valuestring, "http", 4) != 0) {
            fprintf(stderr,
                    "packmule: cannot bundle %s (from %s):\n"
                    "          it does not resolve to a registry tarball "
                    "(git/file/workspace dependency).\n",
                    name, path);
            package_list_destroy(list);
            return NULL;
        }
        if (!cJSON_IsString(integ) ||
            strncmp(integ->valuestring, "sha512-", 7) != 0) {
            fprintf(stderr,
                    "packmule: %s in %s has no SHA-512 integrity; refusing "
                    "to bundle an unverifiable file.\n"
                    "          Regenerate the lock with npm 7+ to refresh "
                    "integrity metadata.\n",
                    name, path);
            package_list_destroy(list);
            return NULL;
        }

        /* One tarball serves every tree position of the same name+version. */
        const char *vstr = cJSON_IsString(ver) ? ver->valuestring : NULL;
        int dup = 0;
        for (size_t i = 0; i < list->count && !dup; i++)
            dup = strcmp(list->items[i]->name, name) == 0 &&
                  list->items[i]->version && vstr &&
                  strcmp(list->items[i]->version, vstr) == 0;
        if (dup)
            continue;

        Package *p  = package_create(name, vstr);
        p->url      = pm_strdup(res->valuestring);
        p->filename = npm_local_filename(name, res->valuestring);
        if (digest_parse_sri(&p->digest, integ->valuestring) != 0) {
            fprintf(stderr,
                    "packmule: %s in %s has an integrity value packmule "
                    "cannot verify (%s).\n",
                    name, path, integ->valuestring);
            package_destroy(p);
            package_list_destroy(list);
            return NULL;
        }
        /* A lock entry is npm's own final answer: fully resolved, and not
         * something the resolver may revise. */
        p->state       = PKG_RESOLVED;
        p->user_pinned = 1;
        package_list_add(list, p);
    }
    return list;
}

/*
 * npm_effective_lockfile — the lockfile a bundle built from `manifest_path`
 * should use: the manifest itself when it IS a lock, else a valid
 * package-lock.json / npm-shrinkwrap.json sitting next to it.  Returns a
 * heap path or NULL (no lock: flat package.json resolution applies).
 * main.c uses this to copy the lock into the bundle.
 */
char *npm_effective_lockfile(const char *manifest_path)
{
    cJSON *root = read_json_file(manifest_path, 1);
    if (root) {
        int is_lock = cJSON_HasObjectItem(root, "lockfileVersion");
        cJSON_Delete(root);
        if (is_lock)
            return pm_strdup(manifest_path);
    }

    static const char *const lock_names[] = { "package-lock.json",
                                              "npm-shrinkwrap.json" };
    const char *slash = strrchr(manifest_path, '/');
    for (size_t i = 0; i < sizeof(lock_names) / sizeof(lock_names[0]); i++) {
        char *cand = slash
            ? pm_asprintf("%.*s/%s", (int)(slash - manifest_path),
                          manifest_path, lock_names[i])
            : pm_strdup(lock_names[i]);
        cJSON *lock = read_json_file(cand, 1);
        if (lock) {
            int is_lock = cJSON_HasObjectItem(lock, "lockfileVersion");
            cJSON_Delete(lock);
            if (is_lock)
                return cand;
        }
        pm_free(cand);
    }
    return NULL;
}

/*
 * npm_parse_manifest — build the download list for an npm project.
 *
 * A lockfile (the manifest itself, or one found next to package.json) wins:
 * it is npm's own exact resolution and the only faithful representation of
 * trees that need multiple versions of a package.  Without one, the
 * production dependency objects (see NPM_DEP_KEYS) of package.json are
 * walked and ranges are resolved against the registry.  npm aliases are
 * rewritten to their real package names; git/URL/path specs are a hard
 * error, because a bundle silently missing them (or shipping "latest" of
 * the wrong thing) would fail on the air-gapped machine.
 */
static PackageList *npm_parse_manifest(const Registry *self, const char *path)
{
    (void)self;

    char *lock = npm_effective_lockfile(path);
    if (lock) {
        if (strcmp(lock, path) != 0)
            printf("packmule: using %s for the exact dependency tree\n", lock);
        cJSON *root = read_json_file(lock, 0);
        PackageList *list = root ? npm_parse_lockfile(root, lock) : NULL;
        cJSON_Delete(root);
        pm_free(lock);
        return list;
    }

    cJSON *root = read_json_file(path, 0);
    if (!root)
        return NULL;

    PackageList *list = package_list_create();

    for (int i = 0; i < NPM_DEP_NKEYS; i++) {
        cJSON *deps = cJSON_GetObjectItemCaseSensitive(root, NPM_DEP_KEYS[i]);
        if (!cJSON_IsObject(deps))
            continue;
        int is_peer = (strcmp(NPM_DEP_KEYS[i], "peerDependencies") == 0);

        cJSON *entry = NULL;
        cJSON_ArrayForEach(entry, deps) {
            if (!entry->string || !cJSON_IsString(entry))
                continue;
            if (is_peer && peer_is_optional(root, entry->string))
                continue;

            char *name  = pm_strdup(entry->string);
            char *range = entry->valuestring[0]
                        ? pm_strdup(entry->valuestring) : NULL;
            apply_alias(&name, &range);

            if (range && spec_is_unbundleable(range)) {
                fprintf(stderr,
                        "packmule: cannot bundle %s@%s (from %s):\n"
                        "          git/URL/path dependencies have no registry "
                        "tarball to download.\n"
                        "          Publish it to a registry, or copy it into "
                        "the bundle manually.\n",
                        name, range, path);
                pm_free(name);
                pm_free(range);
                package_list_destroy(list);
                cJSON_Delete(root);
                return NULL;
            }

            /* The same package may appear in several objects (commonly
             * dependencies + optionalDependencies); the first — most
             * specific — entry wins. */
            if (!package_list_contains_name(list, name,
                                            package_name_equal_casefold))
                package_list_add(list, npm_package_from_spec(name, range));
            pm_free(name);
            pm_free(range);
        }
    }

    cJSON_Delete(root);
    return list;
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

    /*
     * Licence, for the SBOM.  npm's modern field is a string; the legacy
     * "licenses" array (still present on older published versions) holds
     * objects with a "type".
     */
    cJSON *lic = cJSON_GetObjectItemCaseSensitive(doc, "license");
    if (cJSON_IsString(lic) && lic->valuestring[0]) {
        pm_free(pkg->license);
        pkg->license = pm_strdup(lic->valuestring);
    } else {
        cJSON *lics = cJSON_GetObjectItemCaseSensitive(doc, "licenses");
        cJSON *item = NULL;
        if (cJSON_IsArray(lics)) {
            cJSON_ArrayForEach(item, lics) {
                cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
                if (cJSON_IsString(type) && type->valuestring[0]) {
                    pm_free(pkg->license);
                    pkg->license = pm_strdup(type->valuestring);
                    break;
                }
            }
        }
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

    Digest d = {0};
    if (digest_parse_sri(&d, integrity->valuestring) != 0) {
        fprintf(stderr,
                "packmule: npm package %s@%s publishes an integrity value "
                "packmule cannot verify (%s).\n",
                pkg->name, ver_item->valuestring, integrity->valuestring);
        return -1;
    }

    pm_free(pkg->version);
    pm_free(pkg->url);
    pm_free(pkg->filename);
    digest_clear(&pkg->digest);
    pkg->version = pm_strdup(ver_item->valuestring);
    pkg->url     = pm_strdup(tarball->valuestring);
    pkg->digest  = d;                       /* ownership moves into pkg */

    pkg->filename = npm_local_filename(pkg->name, tarball->valuestring);

    /* Extract transitive dependencies as "name@range" from every object in
     * NPM_DEP_KEYS (see there for why peers and optionals are included). */
    int total = 0;
    for (int i = 0; i < NPM_DEP_NKEYS; i++) {
        cJSON *deps = cJSON_GetObjectItemCaseSensitive(doc, NPM_DEP_KEYS[i]);
        if (cJSON_IsObject(deps))
            total += cJSON_GetArraySize(deps);
    }
    package_set_dep_specs(pkg, NULL);
    if (total > 0) {
        char **specs = pm_malloc(((size_t)total + 1) * sizeof(char *));
        int k = 0;
        for (int i = 0; i < NPM_DEP_NKEYS; i++) {
            cJSON *deps = cJSON_GetObjectItemCaseSensitive(doc, NPM_DEP_KEYS[i]);
            if (!cJSON_IsObject(deps))
                continue;
            int is_peer = (strcmp(NPM_DEP_KEYS[i], "peerDependencies") == 0);

            cJSON *entry = NULL;
            cJSON_ArrayForEach(entry, deps) {
                if (!entry->string)
                    continue;
                if (is_peer && peer_is_optional(doc, entry->string))
                    continue;

                /* Skip a name already emitted from an earlier, more specific
                 * object (dependencies + optionalDependencies overlap). */
                size_t nlen = strlen(entry->string);
                int    dup  = 0;
                for (int j = 0; j < k && !dup; j++) {
                    const char *at = strrchr(specs[j], '@');
                    size_t len = (at && at != specs[j])
                               ? (size_t)(at - specs[j])
                               : strlen(specs[j]);
                    dup = (len == nlen &&
                           strncmp(specs[j], entry->string, len) == 0);
                }
                if (dup)
                    continue;

                specs[k++] = pm_asprintf("%s@%s", entry->string,
                                         cJSON_IsString(entry)
                                             ? entry->valuestring : "");
            }
        }
        specs[k] = NULL;
        package_set_dep_specs(pkg, specs);
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
 * highest key of the "versions" object satisfying `pkg->constraint`, mirroring
 * npm's own selection.  A non-semver spec (dist-tag) falls back to the
 * matching dist-tag, then to "latest" with a warning.  Returns 0 on success.
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
        int sat = semver_satisfies(entry->string, pkg->constraint);
        if (sat == -1)
            unparseable = 1;
        else if (sat == 1 &&
                 (!best || semver_cmp(entry->string, best->string) > 0))
            best = entry;
    }

    if (!best && unparseable) {
        /* Not a semver range.  A dist-tag ("next", "beta") resolves through
         * the packument's dist-tags exactly as npm would; anything else falls
         * back to "latest" with a warning (git/URL/path specs never get here —
         * manifest parsing rejects them). */
        cJSON *tags = cJSON_GetObjectItemCaseSensitive(root, "dist-tags");
        cJSON *tag  = cJSON_GetObjectItemCaseSensitive(tags, pkg->constraint);
        if (cJSON_IsString(tag))
            best = cJSON_GetObjectItemCaseSensitive(versions, tag->valuestring);

        if (!best) {
            fprintf(stderr,
                    "packmule: %s: spec '%s' is not a semver range or "
                    "dist-tag; falling back to latest\n",
                    pkg->name, pkg->constraint);
            cJSON *latest = cJSON_GetObjectItemCaseSensitive(tags, "latest");
            if (cJSON_IsString(latest))
                best = cJSON_GetObjectItemCaseSensitive(versions,
                                                        latest->valuestring);
        }
    }

    if (!best) {
        fprintf(stderr,
                "packmule: no published version of %s satisfies '%s'\n",
                pkg->name, pkg->constraint ? pkg->constraint : "(none)");
        /* A space-joined spec is the intersection of several dependents'
         * ranges (see npm_get_deps).  When it is unsatisfiable the tree
         * needs two versions of this package at once — something only a
         * lockfile-driven bundle can represent. */
        if (pkg->constraint && strchr(pkg->constraint, ' '))
            fprintf(stderr,
                    "          The dependency tree needs multiple versions "
                    "of %s.  Bundle your project's\n"
                    "          package-lock.json instead (generate one with "
                    "`npm install --package-lock-only`).\n",
                    pkg->name);
        goto done;
    }

    ret = npm_parse_version_doc(best, pkg);

done:
    cJSON_Delete(root);
    return ret;
}

/* ── Resolver ────────────────────────────────────────────────────────────── */

static int npm_resolve(const Registry *self, Package *pkg)
{
    /* Lockfile entries arrive fully resolved (url/integrity/filename from
     * the lock); there is nothing to ask the registry. */
    if (pkg->user_pinned && pkg->url && digest_is_set(&pkg->digest) &&
        pkg->filename)
        return 0;

    /* A version chosen on an earlier round is provisional: if a dependent has
     * since narrowed the constraint, choose again. */
    if (pkg->version && !pkg->user_pinned && pkg->constraint) {
        pm_free(pkg->version);
        pkg->version = NULL;
    }

    char *url;
    int   want_range = 0;

    if (pkg->version) {
        url = npm_build_url(pkg->name, pkg->version, self->repo_url);
    } else if (is_any_version(pkg->constraint)) {
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

    /* A manifest pin that no longer satisfies what its dependents need is a
     * genuine conflict, not something to install and hope about. */
    if (ret == 0 && pkg->user_pinned && pkg->constraint && pkg->version &&
        semver_satisfies(pkg->version, pkg->constraint) == 0) {
        fprintf(stderr,
                "packmule: %s is pinned to %s but other packages require "
                "'%s'.\n"
                "          Bundle a package-lock.json if the tree genuinely "
                "needs two versions.\n",
                pkg->name, pkg->version, pkg->constraint);
        return -1;
    }
    return ret;
}

/* ── Transitive dependency resolver ─────────────────────────────────────── */

/*
 * npm_merge_range — AND `range` into pkg->constraint, node-semver style
 * (space is conjunction).  Idempotent: the fixpoint resolver calls get_deps
 * once per round, so a constraint that grew a duplicate term every time would
 * never settle.  Returns 1 when the constraint actually changed.
 */
static int npm_merge_range(Package *pkg, const char *range)
{
    if (!pkg->constraint || is_any_version(pkg->constraint))
        return package_set_constraint(pkg, pm_strdup(range));

    if (strcmp(pkg->constraint, range) == 0)
        return 0;

    /* "||" alternation does not distribute over concatenation. */
    if (strstr(pkg->constraint, "||") || strstr(range, "||"))
        return 0;

    /* Already one of the space-separated terms? */
    size_t rlen = strlen(range);
    for (const char *p = pkg->constraint; *p; ) {
        while (*p == ' ') p++;
        size_t n = strcspn(p, " ");
        if (n == rlen && strncmp(p, range, n) == 0)
            return 0;
        p += n;
    }

    return package_set_constraint(pkg,
                                  pm_asprintf("%s %s", pkg->constraint, range));
}

static int npm_get_deps(const Registry *self, const Package *pkg,
                         const PackageList *seen, PackageList *out)
{
    (void)self;
    if (!pkg->dep_specs)
        return 0;
    int added = 0;
    for (char **dep = pkg->dep_specs; *dep; dep++) {
        /* Split "name@range" at the first '@' past index 0: a scoped name's
         * own '@' is only ever at index 0, and the range must keep any '@'
         * of its own ("npm:left-pad@^1.3.0") for apply_alias to split. */
        const char *at = strchr(*dep + 1, '@');
        char *name;
        char *range;
        if (at) {
            name  = pm_strndup(*dep, (size_t)(at - *dep));
            range = (at[1] != '\0') ? pm_strdup(at + 1) : NULL;
        } else {
            name  = pm_strdup(*dep);
            range = NULL;
        }

        apply_alias(&name, &range);

        /* A git/URL dependency deep in the tree cannot be fetched from the
         * registry; shipping the bundle without it guarantees an offline
         * failure, so fail the build while there is still a network. */
        if (range && spec_is_unbundleable(range)) {
            fprintf(stderr,
                    "packmule: cannot bundle %s's dependency %s@%s:\n"
                    "          git/URL/path dependencies have no registry "
                    "tarball to download.\n",
                    pkg->name, name, range);
            pm_free(name);
            pm_free(range);
            return -1;
        }

        Package *existing = package_list_find_name(seen, name,
                                                   package_name_equal_casefold);
        if (existing) {
            /*
             * Record this dependent's range on the entry and let the resolver
             * come back to it.  Space is AND in node-semver, so intersecting
             * is string concatenation — except across "||" alternation, which
             * concatenation cannot express; there we keep the existing spec
             * and rely on the post-bundle install check to catch a genuine
             * mismatch.
             */
            if (range && !is_any_version(range))
                npm_merge_range(existing, range);
        } else {
            package_list_add(out, npm_package_from_spec(name, range));
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
    return strcmp(basename, "package.json")       == 0 ||
           strcmp(basename, "package-lock.json")  == 0 ||
           strcmp(basename, "npm-shrinkwrap.json") == 0;
}

/* ── Registry instance ───────────────────────────────────────────────────── */

const Registry npm_registry = {
    .name           = "npm",
    .manifest_name  = "package.json",
    /* npm names are case-insensitive for lookup, but '.', '-' and '_' are
     * distinct characters: "lodash.merge" and "lodash-merge" are different
     * packages.  PyPI's PEP 503 folding must not be applied here. */
    .name_equal     = package_name_equal_casefold,
    .detect         = npm_detect,
    .parse_manifest = npm_parse_manifest,
    .resolve        = npm_resolve,
    .get_deps       = npm_get_deps,
    .ctx            = NULL, /* arch — npm tarballs are platform-neutral */
    .repo_url       = NULL, /* optional; defaults to https://registry.npmjs.org */
};
