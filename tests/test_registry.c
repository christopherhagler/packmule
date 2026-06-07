/*
 * test_registry.c — unit tests for the registry dispatch table.
 */
#include "registry.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Verify a registry entry is non-NULL, has the right name, and has both
 * function pointers wired up.  Uses unconditional if/abort so the checks
 * survive Release builds where NDEBUG elides assert(). */
static void check_registry(const char *name)
{
    const Registry *r = registry_find(name);
    if (!r) {
        fprintf(stderr, "FAIL: registry_find(\"%s\") returned NULL\n", name);
        abort();
    }
    if (strcmp(r->name, name) != 0) {
        fprintf(stderr, "FAIL: registry[\"%s\"]->name is \"%s\"\n",
                name, r->name);
        abort();
    }
    if (!r->parse_manifest) {
        fprintf(stderr, "FAIL: registry[\"%s\"]->parse_manifest is NULL\n", name);
        abort();
    }
    if (!r->resolve) {
        fprintf(stderr, "FAIL: registry[\"%s\"]->resolve is NULL\n", name);
        abort();
    }
}

static void test_find_known(void)
{
    check_registry("pypi");
    check_registry("npm");
    check_registry("rpm");
}

static void test_find_unknown(void)
{
    assert(registry_find("cargo")  == NULL);
    assert(registry_find("")       == NULL);
    assert(registry_find("PYPI")   == NULL); /* case-sensitive */
}

static void test_names_list(void)
{
    const char *const *names = registry_names();
    assert(names != NULL);
    assert(names[0] != NULL); /* at least one entry */

    /* Every name in the list must be findable and have a matching name field. */
    for (int i = 0; names[i] != NULL; i++) {
        const Registry *r = registry_find(names[i]);
        if (!r || strcmp(r->name, names[i]) != 0) {
            fprintf(stderr, "FAIL: registry_find(\"%s\") mismatch\n", names[i]);
            abort();
        }
    }
}

static void test_manifest_names_set(void)
{
    assert(strcmp(registry_find("pypi")->manifest_name, "requirements.txt") == 0);
    assert(strcmp(registry_find("npm")->manifest_name,  "package.json")      == 0);
    assert(strcmp(registry_find("rpm")->manifest_name,  "packages.txt")      == 0);
}

static void test_get_deps_vtable(void)
{
    /* pypi and npm implement transitive resolution via get_deps. */
    assert(registry_find("pypi")->get_deps != NULL);
    assert(registry_find("npm")->get_deps  != NULL);
    /* rpm does not follow transitive dependencies automatically. */
    assert(registry_find("rpm")->get_deps  == NULL);
}

int main(void)
{
    test_find_known();
    test_find_unknown();
    test_names_list();
    test_manifest_names_set();
    test_get_deps_vtable();
    return 0;
}
