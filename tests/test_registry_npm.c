/*
 * test_registry_npm.c — unit tests for the npm registry backend.
 *
 * parse_manifest tests: no network calls required.
 * get_deps tests: exercise dep enqueuing and dedup logic.
 */
#include "registry.h"
#include "package.h"

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
    assert(strcmp(list->items[0]->version, "^4.18.2") == 0);
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
    assert(strcmp(list->items[0]->version, "~1.0.0")     == 0);

    package_list_destroy(list);
    cleanup();
}

static void test_dev_and_peer_deps_ignored(void)
{
    /* Only "dependencies" should be collected; dev/peer/optional are ignored. */
    write_file(TMP,
               "{"
               "  \"dependencies\":         { \"express\": \"4.18.2\" },"
               "  \"devDependencies\":       { \"jest\":    \"29.0.0\" },"
               "  \"peerDependencies\":      { \"react\":   \">=17\"   },"
               "  \"optionalDependencies\":  { \"fsevents\":\"2.3.3\"  }"
               "}");

    const Registry *npm  = registry_find("npm");
    PackageList    *list = npm->parse_manifest(npm, TMP);

    assert(list != NULL);
    assert(list->count == 1);
    assert(strcmp(list->items[0]->name, "express") == 0);

    package_list_destroy(list);
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
     * Enterprise-dashboard fixture: 10 production dependencies.
     * devDependencies (6), peerDependencies (1), and optionalDependencies (1)
     * are all ignored — only "dependencies" is collected.
     * Versions stored verbatim (semver range strings are passed through as-is).
     */
    const Registry *npm  = registry_find("npm");
    PackageList    *list = npm->parse_manifest(npm, "fixtures/package.json");

    assert(list != NULL);
    assert(list->count == 10);

    /* JSON key order is preserved by cJSON. */
    assert(strcmp(list->items[0]->name,    "react")                  == 0);
    assert(strcmp(list->items[0]->version, "^18.2.0")                == 0);

    assert(strcmp(list->items[1]->name,    "react-dom")              == 0);
    assert(strcmp(list->items[1]->version, "^18.2.0")                == 0);

    assert(strcmp(list->items[2]->name,    "react-router-dom")       == 0);
    assert(strcmp(list->items[2]->version, "^6.22.3")                == 0);

    assert(strcmp(list->items[4]->name,    "lodash")                 == 0);
    assert(strcmp(list->items[4]->version, "^4.17.21")               == 0);

    assert(strcmp(list->items[5]->name,    "@tanstack/react-query")  == 0);
    assert(strcmp(list->items[5]->version, "^5.28.6")                == 0);

    assert(strcmp(list->items[9]->name,    "@radix-ui/react-dialog") == 0);
    assert(strcmp(list->items[9]->version, "^1.0.5")                 == 0);

    package_list_destroy(list);
}

/* ── get_deps ────────────────────────────────────────────────────────────── */

/* Build a Package with dep_specs set to plain names (as npm_resolve produces). */
static Package *make_npm_resolved(const char *name, const char **dep_names, int n)
{
    Package *pkg = package_create(name, "1.0.0");
    if (n > 0) {
        pkg->dep_specs = malloc(((size_t)n + 1) * sizeof(char *));
        for (int i = 0; i < n; i++)
            pkg->dep_specs[i] = strdup(dep_names[i]);
        pkg->dep_specs[n] = NULL;
    }
    return pkg;
}

static void test_npm_get_deps_enqueues_new(void)
{
    const char *deps[] = { "body-parser", "path-to-regexp" };
    Package     *pkg   = make_npm_resolved("express", deps, 2);
    const Registry *npm  = registry_find("npm");
    PackageList *seen = package_list_create();
    PackageList *out  = package_list_create();

    int added = npm->get_deps(npm, pkg, seen, out);

    assert(added == 2);
    assert(out->count == 2);
    assert(strcmp(out->items[0]->name, "body-parser")    == 0);
    assert(strcmp(out->items[1]->name, "path-to-regexp") == 0);

    package_destroy(pkg);
    package_list_destroy(seen);
    package_list_destroy(out);
}

static void test_npm_get_deps_dedup(void)
{
    const char *deps[] = { "body-parser", "path-to-regexp" };
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

int main(void)
{
    test_basic_dependencies();
    test_scoped_package();
    test_dev_and_peer_deps_ignored();
    test_no_dependencies_key();
    test_empty_dependencies();
    test_invalid_json();
    test_missing_file();
    test_fixture_file();
    test_npm_get_deps_enqueues_new();
    test_npm_get_deps_dedup();
    return 0;
}
