/*
 * test_registry_npm.c — unit tests for the npm registry backend.
 *
 * parse_manifest tests: no network calls required.
 * get_deps tests: exercise dep enqueuing, range splitting, and dedup logic.
 * npm_parse_response tests: decode inline version-document JSON.
 */
#include "registry.h"
#include "registry_internal.h"
#include "package.h"
#include "utils.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TMP = "test_npm_tmp.json";

static void write_file(const char *path, const char *contents)
{
    FILE *fp = fopen(path, "w");
    assert(fp != NULL);
    fputs(contents, fp);
    fclose(fp);
}

static void cleanup(void) { remove(TMP); }

/* ── tests ───────────────────────────────────────────────────────────────── */

static void test_basic_dependencies(void)
{
    write_file(TMP,
               "{"
               "  \"name\": \"my-app\","
               "  \"dependencies\": {"
               "    \"express\": \"^4.18.2\","
               "    \"lodash\": \"4.17.21\""
               "  }"
               "}");

    const Registry *npm  = registry_find("npm");
    PackageList    *list = npm->parse_manifest(npm, TMP);

    assert(list != NULL);
    assert(list->count == 2);
    assert(strcmp(list->items[0]->name,    "express") == 0);
    assert(strcmp(list->items[0]->constraint, "^4.18.2") == 0);
    assert(strcmp(list->items[1]->name,    "lodash")  == 0);
    assert(strcmp(list->items[1]->version, "4.17.21") == 0);

    package_list_destroy(list);
    cleanup();
}

static void test_scoped_package(void)
{
    write_file(TMP,
               "{"
               "  \"dependencies\": {"
               "    \"@scope/pkg\": \"~1.0.0\""
               "  }"
               "}");

    const Registry *npm  = registry_find("npm");
    PackageList    *list = npm->parse_manifest(npm, TMP);

    assert(list != NULL);
    assert(list->count == 1);
    assert(strcmp(list->items[0]->name,    "@scope/pkg") == 0);
    assert(strcmp(list->items[0]->constraint, "~1.0.0")     == 0);

    package_list_destroy(list);
    cleanup();
}

static void test_manifest_dep_objects(void)
{
    /* peer and optional deps are part of the offline closure (npm 7+ installs
     * peers by default); devDependencies stay out (install.sh --omit=dev);
     * a peer marked optional in peerDependenciesMeta is the consumer's choice
     * and must not be forced in. */
    write_file(TMP,
               "{"
               "  \"dependencies\":          { \"express\": \"4.18.2\" },"
               "  \"devDependencies\":       { \"jest\":    \"29.0.0\" },"
               "  \"peerDependencies\":      { \"react\":   \">=17\","
               "                               \"canvas\":  \"^2.0.0\" },"
               "  \"peerDependenciesMeta\":  { \"canvas\":  { \"optional\": true } },"
               "  \"optionalDependencies\":  { \"fsevents\":\"2.3.3\"  }"
               "}");

    const Registry *npm  = registry_find("npm");
    PackageList    *list = npm->parse_manifest(npm, TMP);

    assert(list != NULL);
    assert(list->count == 3);
    assert(strcmp(list->items[0]->name, "express")  == 0);
    assert(strcmp(list->items[1]->name, "fsevents") == 0);
    assert(strcmp(list->items[2]->name, "react")    == 0);
    assert(strcmp(list->items[2]->constraint, ">=17")  == 0);

    package_list_destroy(list);
    cleanup();
}

static void test_manifest_alias(void)
{
    /* "npm:real-name@range" installs real-name; the alias key does not exist
     * as a registry package and must not be queried. */
    write_file(TMP,
               "{"
               "  \"dependencies\": {"
               "    \"my-pad\":   \"npm:left-pad@^1.3.0\","
               "    \"my-scope\": \"npm:@scope/pkg@~2.0.0\","
               "    \"bare\":     \"npm:left-pad\""
               "  }"
               "}");

    const Registry *npm  = registry_find("npm");
    PackageList    *list = npm->parse_manifest(npm, TMP);

    assert(list != NULL);
    assert(list->count == 2);   /* both left-pad aliases collapse to one */
    assert(strcmp(list->items[0]->name,    "left-pad")   == 0);
    assert(strcmp(list->items[0]->constraint, "^1.3.0")     == 0);
    assert(strcmp(list->items[1]->name,    "@scope/pkg") == 0);
    assert(strcmp(list->items[1]->constraint, "~2.0.0")     == 0);

    package_list_destroy(list);
    cleanup();
}

static void test_manifest_unbundleable_specs_fatal(void)
{
    /* git/URL/path deps have no registry tarball; silently resolving the name
     * against the registry instead would bundle the wrong artifact.  Each of
     * these must fail manifest parsing outright. */
    static const char *const specs[] = {
        "git+https://github.com/user/repo.git",
        "github:user/repo",
        "user/repo",
        "file:../local-pkg",
        "link:../local-pkg",
        "workspace:*",
        "https://example.com/pkg.tgz",
    };
    const Registry *npm = registry_find("npm");

    for (size_t i = 0; i < sizeof(specs) / sizeof(specs[0]); i++) {
        char json[256];
        snprintf(json, sizeof(json),
                 "{ \"dependencies\": { \"dep\": \"%s\" } }", specs[i]);
        write_file(TMP, json);
        PackageList *list = npm->parse_manifest(npm, TMP);
        assert(list == NULL);
    }
    cleanup();
}

static void test_no_dependencies_key(void)
{
    /* Valid package.json with no "dependencies" key → empty list, not an error. */
    write_file(TMP, "{ \"name\": \"my-lib\", \"version\": \"1.0.0\" }");

    const Registry *npm  = registry_find("npm");
    PackageList    *list = npm->parse_manifest(npm, TMP);

    assert(list != NULL);
    assert(list->count == 0);

    package_list_destroy(list);
    cleanup();
}

static void test_empty_dependencies(void)
{
    write_file(TMP, "{ \"dependencies\": {} }");

    const Registry *npm  = registry_find("npm");
    PackageList    *list = npm->parse_manifest(npm, TMP);

    assert(list != NULL);
    assert(list->count == 0);

    package_list_destroy(list);
    cleanup();
}

static void test_invalid_json(void)
{
    write_file(TMP, "{ this is not valid JSON }");

    const Registry *npm  = registry_find("npm");
    PackageList    *list = npm->parse_manifest(npm, TMP);

    /* Must return NULL, not crash. */
    assert(list == NULL);
    cleanup();
}

static void test_missing_file(void)
{
    const Registry *npm  = registry_find("npm");
    PackageList    *list = npm->parse_manifest(npm, "/nonexistent/package.json");
    assert(list == NULL);
}

static void test_fixture_file(void)
{
    /*
     * Enterprise-dashboard fixture: 10 production dependencies, plus the
     * optionalDependency (fsevents).  The peerDependency (react) is already
     * in "dependencies" and dedups; the 6 devDependencies are ignored.
     * Versions stored verbatim (semver range strings are passed through as-is).
     */
    const Registry *npm  = registry_find("npm");
    PackageList    *list = npm->parse_manifest(npm, "fixtures/package.json");

    assert(list != NULL);
    assert(list->count == 11);

    /* JSON key order is preserved by cJSON. */
    assert(strcmp(list->items[0]->name,    "react")                  == 0);
    assert(strcmp(list->items[0]->constraint, "^18.2.0")                == 0);

    assert(strcmp(list->items[1]->name,    "react-dom")              == 0);
    assert(strcmp(list->items[1]->constraint, "^18.2.0")                == 0);

    assert(strcmp(list->items[2]->name,    "react-router-dom")       == 0);
    assert(strcmp(list->items[2]->constraint, "^6.22.3")                == 0);

    assert(strcmp(list->items[4]->name,    "lodash")                 == 0);
    assert(strcmp(list->items[4]->constraint, "^4.17.21")               == 0);

    assert(strcmp(list->items[5]->name,    "@tanstack/react-query")  == 0);
    assert(strcmp(list->items[5]->constraint, "^5.28.6")                == 0);

    assert(strcmp(list->items[9]->name,    "@radix-ui/react-dialog") == 0);
    assert(strcmp(list->items[9]->constraint, "^1.0.5")                 == 0);

    assert(strcmp(list->items[10]->name,    "fsevents")              == 0);
    assert(strcmp(list->items[10]->constraint, "^2.3.3")                == 0);

    package_list_destroy(list);
}

/* ── Lockfile manifests ──────────────────────────────────────────────────── */

static void test_lockfile_parse(void)
{
    /* A lockfileVersion-3 manifest is used verbatim: dev entries skipped,
     * duplicate name+version tree positions deduped to one tarball, scoped
     * filenames prefixed, and every entry arrives pre-resolved so resolve()
     * needs no network. */
    write_file(TMP,
        "{"
        "  \"name\": \"app\", \"version\": \"1.0.0\", \"lockfileVersion\": 3,"
        "  \"packages\": {"
        "    \"\": { \"name\": \"app\", \"version\": \"1.0.0\" },"
        "    \"node_modules/debug\": {"
        "      \"version\": \"2.6.9\","
        "      \"resolved\": \"https://registry.npmjs.org/debug/-/debug-2.6.9.tgz\","
        "      \"integrity\": \"sha512-aaa\" },"
        "    \"node_modules/agent-base/node_modules/debug\": {"
        "      \"version\": \"4.4.3\","
        "      \"resolved\": \"https://registry.npmjs.org/debug/-/debug-4.4.3.tgz\","
        "      \"integrity\": \"sha512-bbb\" },"
        "    \"node_modules/https-proxy-agent/node_modules/debug\": {"
        "      \"version\": \"4.4.3\","
        "      \"resolved\": \"https://registry.npmjs.org/debug/-/debug-4.4.3.tgz\","
        "      \"integrity\": \"sha512-bbb\" },"
        "    \"node_modules/@babel/core\": {"
        "      \"version\": \"7.24.0\","
        "      \"resolved\": \"https://registry.npmjs.org/@babel/core/-/core-7.24.0.tgz\","
        "      \"integrity\": \"sha512-ccc\" },"
        "    \"node_modules/typescript\": {"
        "      \"version\": \"5.4.3\","
        "      \"resolved\": \"https://registry.npmjs.org/typescript/-/typescript-5.4.3.tgz\","
        "      \"integrity\": \"sha512-ddd\", \"dev\": true }"
        "  }"
        "}");

    const Registry *npm  = registry_find("npm");
    PackageList    *list = npm->parse_manifest(npm, TMP);

    assert(list != NULL);
    assert(list->count == 3);   /* two debugs (dup deduped) + @babel/core */

    assert(strcmp(list->items[0]->name,     "debug")            == 0);
    assert(strcmp(list->items[0]->version,  "2.6.9")            == 0);
    assert(strcmp(list->items[0]->filename, "debug-2.6.9.tgz")  == 0);
    assert(list->items[0]->digest.algo == DIGEST_SHA512);
    assert(list->items[0]->digest.enc  == DIGEST_ENC_BASE64);
    assert(strcmp(list->items[0]->digest.value, "aaa")          == 0);
    assert(strcmp(list->items[0]->url,
                  "https://registry.npmjs.org/debug/-/debug-2.6.9.tgz") == 0);

    assert(strcmp(list->items[1]->name,     "debug")            == 0);
    assert(strcmp(list->items[1]->version,  "4.4.3")            == 0);
    assert(strcmp(list->items[1]->filename, "debug-4.4.3.tgz")  == 0);

    assert(strcmp(list->items[2]->name,     "@babel/core")            == 0);
    assert(strcmp(list->items[2]->filename, "babel-core-7.24.0.tgz")  == 0);

    /* Pre-resolved entries must not need the registry. */
    assert(npm->resolve(npm, list->items[0]) == 0);

    package_list_destroy(list);
    cleanup();
}

static void test_lockfile_v1_rejected(void)
{
    /* npm 6 locks lack the "packages" object with per-entry integrity. */
    write_file(TMP,
        "{ \"lockfileVersion\": 1,"
        "  \"dependencies\": { \"debug\": { \"version\": \"2.6.9\" } } }");

    const Registry *npm = registry_find("npm");
    assert(npm->parse_manifest(npm, TMP) == NULL);
    cleanup();
}

static void test_lockfile_git_dep_rejected(void)
{
    /* A lock entry resolving to git (or any non-registry source) has no
     * tarball to bundle; the build must fail rather than ship without it. */
    write_file(TMP,
        "{ \"lockfileVersion\": 3,"
        "  \"packages\": {"
        "    \"\": {},"
        "    \"node_modules/weird\": {"
        "      \"version\": \"1.0.0\","
        "      \"resolved\": \"git+ssh://git@github.com/user/repo.git#abc\" }"
        "  } }");

    const Registry *npm = registry_find("npm");
    assert(npm->parse_manifest(npm, TMP) == NULL);
    cleanup();
}

static void test_lockfile_sha1_integrity_rejected(void)
{
    /* Only SHA-512 SRI is accepted, matching the registry-path policy. */
    write_file(TMP,
        "{ \"lockfileVersion\": 2,"
        "  \"packages\": {"
        "    \"\": {},"
        "    \"node_modules/old\": {"
        "      \"version\": \"0.0.1\","
        "      \"resolved\": \"https://registry.npmjs.org/old/-/old-0.0.1.tgz\","
        "      \"integrity\": \"sha1-deadbeef\" }"
        "  } }");

    const Registry *npm = registry_find("npm");
    assert(npm->parse_manifest(npm, TMP) == NULL);
    cleanup();
}

static void test_sibling_lockfile_preferred(void)
{
    /* Given a package.json with a package-lock.json beside it, the lock's
     * exact tree wins over range resolution. */
    write_file(TMP, "{ \"dependencies\": { \"debug\": \"^2.6.9\" } }");
    write_file("package-lock.json",
        "{ \"lockfileVersion\": 3,"
        "  \"packages\": {"
        "    \"\": {},"
        "    \"node_modules/debug\": {"
        "      \"version\": \"2.6.9\","
        "      \"resolved\": \"https://registry.npmjs.org/debug/-/debug-2.6.9.tgz\","
        "      \"integrity\": \"sha512-aaa\" }"
        "  } }");

    const Registry *npm  = registry_find("npm");
    PackageList    *list = npm->parse_manifest(npm, TMP);

    assert(list != NULL);
    assert(list->count == 1);
    assert(strcmp(list->items[0]->version, "2.6.9") == 0);
    assert(list->items[0]->url != NULL);   /* came from the lock */

    package_list_destroy(list);
    remove("package-lock.json");
    cleanup();
}

/* ── get_deps ────────────────────────────────────────────────────────────── */

/* Build a Package with dep_specs set to "name@range" entries, as
 * npm_parse_response produces. */
static Package *make_npm_resolved(const char *name, const char **dep_specs, int n)
{
    Package *pkg = package_create(name, "1.0.0");
    if (n > 0) {
        pkg->dep_specs = malloc(((size_t)n + 1) * sizeof(char *));
        for (int i = 0; i < n; i++)
            pkg->dep_specs[i] = strdup(dep_specs[i]);
        pkg->dep_specs[n] = NULL;
    }
    return pkg;
}

static void test_npm_get_deps_enqueues_new(void)
{
    const char *deps[] = { "body-parser@^1.20.0", "path-to-regexp@0.1.7" };
    Package     *pkg   = make_npm_resolved("express", deps, 2);
    const Registry *npm  = registry_find("npm");
    PackageList *seen = package_list_create();
    PackageList *out  = package_list_create();

    int added = npm->get_deps(npm, pkg, seen, out);

    assert(added == 2);
    assert(out->count == 2);
    /* The semver range must survive into the queued package's version, so
     * npm_resolve can honor it — dropping it meant "always latest". */
    assert(strcmp(out->items[0]->name,    "body-parser")    == 0);
    assert(strcmp(out->items[0]->constraint, "^1.20.0")        == 0);
    assert(strcmp(out->items[1]->name,    "path-to-regexp") == 0);
    assert(strcmp(out->items[1]->version, "0.1.7")          == 0);

    package_destroy(pkg);
    package_list_destroy(seen);
    package_list_destroy(out);
}

static void test_npm_get_deps_scoped_names(void)
{
    /* The split is at the LAST '@': a scoped name's own '@' must survive. */
    const char *deps[] = { "@babel/core@^7.24.0", "@scope/pkg@~1.0.0" };
    Package     *pkg   = make_npm_resolved("my-lib", deps, 2);
    const Registry *npm  = registry_find("npm");
    PackageList *seen = package_list_create();
    PackageList *out  = package_list_create();

    int added = npm->get_deps(npm, pkg, seen, out);

    assert(added == 2);
    assert(strcmp(out->items[0]->name,    "@babel/core") == 0);
    assert(strcmp(out->items[0]->constraint, "^7.24.0")     == 0);
    assert(strcmp(out->items[1]->name,    "@scope/pkg")  == 0);
    assert(strcmp(out->items[1]->constraint, "~1.0.0")      == 0);

    package_destroy(pkg);
    package_list_destroy(seen);
    package_list_destroy(out);
}

static void test_npm_get_deps_bare_name(void)
{
    /* A spec without a range (or with an empty one) queues unpinned. */
    const char *deps[] = { "leftpad", "rimraf@" };
    Package     *pkg   = make_npm_resolved("my-lib", deps, 2);
    const Registry *npm  = registry_find("npm");
    PackageList *seen = package_list_create();
    PackageList *out  = package_list_create();

    int added = npm->get_deps(npm, pkg, seen, out);

    assert(added == 2);
    assert(strcmp(out->items[0]->name, "leftpad") == 0);
    assert(out->items[0]->version == NULL);
    assert(strcmp(out->items[1]->name, "rimraf")  == 0);
    assert(out->items[1]->version == NULL);

    package_destroy(pkg);
    package_list_destroy(seen);
    package_list_destroy(out);
}

static void test_npm_get_deps_dedup(void)
{
    const char *deps[] = { "body-parser@^1.20.0", "path-to-regexp@0.1.7" };
    Package     *pkg   = make_npm_resolved("express", deps, 2);
    const Registry *npm = registry_find("npm");
    PackageList *queue = package_list_create();
    package_list_add(queue, package_create("body-parser", "1.20.2"));

    int added = npm->get_deps(npm, pkg, queue, queue);

    assert(added == 1);
    assert(queue->count == 2); /* 1 pre-existing + 1 new */
    assert(strcmp(queue->items[1]->name, "path-to-regexp") == 0);

    package_destroy(pkg);
    package_list_destroy(queue);
}

static void test_npm_get_deps_alias(void)
{
    /* "npm:real@range" aliases must enqueue the real package. */
    const char *deps[] = { "my-pad@npm:left-pad@^1.3.0" };
    Package     *pkg   = make_npm_resolved("my-lib", deps, 1);
    const Registry *npm = registry_find("npm");
    PackageList *seen = package_list_create();
    PackageList *out  = package_list_create();

    int added = npm->get_deps(npm, pkg, seen, out);

    assert(added == 1);
    assert(strcmp(out->items[0]->name,    "left-pad") == 0);
    assert(strcmp(out->items[0]->constraint, "^1.3.0")   == 0);

    package_destroy(pkg);
    package_list_destroy(seen);
    package_list_destroy(out);
}

static void test_npm_get_deps_range_intersection(void)
{
    /* A duplicate narrows to the intersection of both ranges, so the eventual
     * resolution honours every dependent.  Ranges live in `constraint`;
     * `version` only ever holds a concrete version. */
    const char *deps[] = { "ms@^2.1.3" };
    Package     *pkg   = make_npm_resolved("debug", deps, 1);
    const Registry *npm = registry_find("npm");
    PackageList *queue = package_list_create();
    Package *ms = package_create("ms", NULL);
    ms->constraint = pm_strdup("^2.1.0");
    package_list_add(queue, ms);

    int added = npm->get_deps(npm, pkg, queue, queue);

    assert(added == 0);
    assert(queue->count == 1);
    assert(strcmp(queue->items[0]->constraint, "^2.1.0 ^2.1.3") == 0);
    assert(queue->items[0]->version == NULL);

    /* Idempotent: the fixpoint resolver calls get_deps once per round, and a
     * constraint that grew a duplicate term each time would never settle. */
    npm->get_deps(npm, pkg, queue, queue);
    assert(strcmp(queue->items[0]->constraint, "^2.1.0 ^2.1.3") == 0);

    package_destroy(pkg);
    package_list_destroy(queue);
}

static void test_npm_get_deps_redirties_resolved_package(void)
{
    /*
     * The order-dependence fix: when a dependent's range reaches a package
     * that has ALREADY been resolved, recording it must mark that package
     * dirty so the resolver revisits it.  Previously this path only warned,
     * and the bundle kept whatever version happened to be chosen first.
     */
    const char *deps[] = { "ms@^2.1.3" };
    Package     *pkg   = make_npm_resolved("debug", deps, 1);
    const Registry *npm = registry_find("npm");

    PackageList *queue = package_list_create();
    Package *ms = package_create("ms", "2.0.0");
    ms->state = PKG_RESOLVED;
    ms->dirty = 0;
    package_list_add(queue, ms);

    npm->get_deps(npm, pkg, queue, queue);

    assert(queue->items[0]->dirty == 1);
    assert(strcmp(queue->items[0]->constraint, "^2.1.3") == 0);

    package_destroy(pkg);
    package_list_destroy(queue);
}

static void test_npm_get_deps_unbundleable_fails(void)
{
    /* A git/URL dependency inside the tree cannot be fetched from the
     * registry; get_deps must report failure so the build aborts. */
    const char *deps[] = { "weird@git+https://github.com/user/repo.git" };
    Package     *pkg   = make_npm_resolved("my-lib", deps, 1);
    const Registry *npm = registry_find("npm");
    PackageList *seen = package_list_create();
    PackageList *out  = package_list_create();

    assert(npm->get_deps(npm, pkg, seen, out) == -1);
    assert(out->count == 0);

    package_destroy(pkg);
    package_list_destroy(seen);
    package_list_destroy(out);
}

/* ── npm_parse_response ──────────────────────────────────────────────────── */

static void test_npm_parse_response_basic(void)
{
    const char *json =
        "{"
        "  \"version\": \"4.18.2\","
        "  \"dist\": {"
        "    \"tarball\": \"https://registry.npmjs.org/express/-/express-4.18.2.tgz\","
        "    \"integrity\": \"sha512-abcdef\""
        "  },"
        "  \"dependencies\": { \"accepts\": \"~1.3.8\", \"body-parser\": \"1.20.1\" }"
        "}";

    Package *pkg = package_create("express", "^4.18.0");
    int rc = npm_parse_response(json, pkg);

    assert(rc == 0);
    assert(strcmp(pkg->version,  "4.18.2")             == 0);
    assert(strcmp(pkg->url,
                  "https://registry.npmjs.org/express/-/express-4.18.2.tgz") == 0);
    assert(pkg->digest.algo == DIGEST_SHA512);
    assert(strcmp(pkg->digest.value, "abcdef")         == 0);
    assert(strcmp(pkg->filename, "express-4.18.2.tgz") == 0);

    /* dep_specs carry "name@range". */
    assert(pkg->dep_specs != NULL);
    assert(strcmp(pkg->dep_specs[0], "accepts@~1.3.8")     == 0);
    assert(strcmp(pkg->dep_specs[1], "body-parser@1.20.1") == 0);
    assert(pkg->dep_specs[2] == NULL);

    package_destroy(pkg);
}

static void test_npm_parse_response_missing_integrity(void)
{
    /* Pre-2017 packages without dist.integrity must be refused. */
    const char *json =
        "{"
        "  \"version\": \"0.0.1\","
        "  \"dist\": {"
        "    \"tarball\": \"https://registry.npmjs.org/old/-/old-0.0.1.tgz\","
        "    \"shasum\": \"deadbeef\""
        "  }"
        "}";

    Package *pkg = package_create("old", NULL);
    assert(npm_parse_response(json, pkg) == -1);
    package_destroy(pkg);
}

static void test_npm_parse_response_peer_and_optional_deps(void)
{
    /* dep_specs must cover the whole offline closure: dependencies, then
     * optionalDependencies, then non-optional peerDependencies.  A peer
     * marked optional in peerDependenciesMeta is excluded, and a name
     * listed twice (dependencies + optionalDependencies) appears once. */
    const char *json =
        "{"
        "  \"version\": \"1.0.0\","
        "  \"dist\": {"
        "    \"tarball\": \"https://registry.npmjs.org/x/-/x-1.0.0.tgz\","
        "    \"integrity\": \"sha512-abc\""
        "  },"
        "  \"dependencies\":         { \"accepts\": \"~1.3.8\" },"
        "  \"optionalDependencies\": { \"fsevents\": \"^2.3.3\","
        "                              \"accepts\": \"~1.3.8\" },"
        "  \"peerDependencies\":     { \"react\": \">=17\","
        "                              \"canvas\": \"^2.0.0\" },"
        "  \"peerDependenciesMeta\": { \"canvas\": { \"optional\": true } }"
        "}";

    Package *pkg = package_create("x", NULL);
    assert(npm_parse_response(json, pkg) == 0);

    assert(pkg->dep_specs != NULL);
    assert(strcmp(pkg->dep_specs[0], "accepts@~1.3.8")  == 0);
    assert(strcmp(pkg->dep_specs[1], "fsevents@^2.3.3") == 0);
    assert(strcmp(pkg->dep_specs[2], "react@>=17")      == 0);
    assert(pkg->dep_specs[3] == NULL);

    package_destroy(pkg);
}

static void test_npm_parse_response_scoped_filename(void)
{
    /* "@babel/core" and "@vue/core" both ship "core-<ver>.tgz"; the scope is
     * prefixed into the local filename so they can't collide in one dir. */
    const char *json =
        "{"
        "  \"version\": \"7.24.0\","
        "  \"dist\": {"
        "    \"tarball\": \"https://registry.npmjs.org/@babel/core/-/core-7.24.0.tgz\","
        "    \"integrity\": \"sha512-xyz\""
        "  }"
        "}";

    Package *pkg = package_create("@babel/core", NULL);
    int rc = npm_parse_response(json, pkg);

    assert(rc == 0);
    assert(strcmp(pkg->filename, "babel-core-7.24.0.tgz") == 0);

    package_destroy(pkg);
}

int main(void)
{
    test_basic_dependencies();
    test_scoped_package();
    test_manifest_dep_objects();
    test_manifest_alias();
    test_manifest_unbundleable_specs_fatal();
    test_no_dependencies_key();
    test_empty_dependencies();
    test_invalid_json();
    test_missing_file();
    test_fixture_file();
    test_lockfile_parse();
    test_lockfile_v1_rejected();
    test_lockfile_git_dep_rejected();
    test_lockfile_sha1_integrity_rejected();
    test_sibling_lockfile_preferred();
    test_npm_get_deps_enqueues_new();
    test_npm_get_deps_scoped_names();
    test_npm_get_deps_bare_name();
    test_npm_get_deps_dedup();
    test_npm_get_deps_alias();
    test_npm_get_deps_range_intersection();
    test_npm_get_deps_redirties_resolved_package();
    test_npm_get_deps_unbundleable_fails();
    test_npm_parse_response_basic();
    test_npm_parse_response_missing_integrity();
    test_npm_parse_response_peer_and_optional_deps();
    test_npm_parse_response_scoped_filename();
    return 0;
}
