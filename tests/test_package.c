/*
 * test_package.c — unit tests for package.h / package.c
 */
#include "package.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

/* ── package_create / package_destroy ────────────────────────────────────── */

static void test_package_create_with_version(void)
{
    Package *pkg = package_create("requests", "2.31.0");
    assert(pkg != NULL);
    assert(strcmp(pkg->name,    "requests") == 0);
    assert(strcmp(pkg->version, "2.31.0")   == 0);
    assert(pkg->url      == NULL);
    assert(pkg->sha256   == NULL);
    assert(pkg->filename == NULL);
    package_destroy(pkg);
}

static void test_package_create_no_version(void)
{
    Package *pkg = package_create("flask", NULL);
    assert(pkg != NULL);
    assert(strcmp(pkg->name, "flask") == 0);
    assert(pkg->version  == NULL);
    assert(pkg->url      == NULL);
    assert(pkg->sha256   == NULL);
    assert(pkg->filename == NULL);
    package_destroy(pkg);
}

static void test_package_destroy_null(void)
{
    /* Must not crash. */
    package_destroy(NULL);
}

/* ── PackageList basic operations ────────────────────────────────────────── */

static void test_package_list_basic(void)
{
    PackageList *list = package_list_create();
    assert(list != NULL);
    assert(list->count == 0);

    Package *p1 = package_create("requests", "2.31.0");
    Package *p2 = package_create("flask",    "3.0.0");
    package_list_add(list, p1);
    package_list_add(list, p2);

    assert(list->count == 2);
    assert(strcmp(list->items[0]->name, "requests") == 0);
    assert(strcmp(list->items[1]->name, "flask")    == 0);

    package_list_destroy(list);
}

static void test_package_list_grow(void)
{
    /* Force the internal array to grow past the initial capacity (16). */
    PackageList *list = package_list_create();
    for (int i = 0; i < 32; i++) {
        char name[8];
        name[0] = 'p';
        name[1] = (char)('0' + (i / 10));
        name[2] = (char)('0' + (i % 10));
        name[3] = '\0';
        package_list_add(list, package_create(name, NULL));
    }
    assert(list->count == 32);
    package_list_destroy(list);
}

/* ── package_list_contains ───────────────────────────────────────────────── */

static void test_package_list_contains(void)
{
    PackageList *list = package_list_create();
    package_list_add(list, package_create("numpy", "1.26.0"));

    assert(package_list_contains(list, "numpy",  "1.26.0") == 1);
    assert(package_list_contains(list, "numpy",  "1.25.0") == 0);
    assert(package_list_contains(list, "pandas", "1.26.0") == 0);
    /* NULL version matches any version of the package. */
    assert(package_list_contains(list, "numpy",  NULL)     == 1);

    package_list_destroy(list);
}

/* ── package_list_contains_name ──────────────────────────────────────────── */

static void test_contains_name_basic(void)
{
    PackageList *list = package_list_create();
    package_list_add(list, package_create("requests", "2.31.0"));

    assert(package_list_contains_name(list, "requests") == 1);
    assert(package_list_contains_name(list, "flask")    == 0);

    package_list_destroy(list);
}

static void test_contains_name_case_insensitive(void)
{
    PackageList *list = package_list_create();
    package_list_add(list, package_create("Requests", NULL));

    assert(package_list_contains_name(list, "requests") == 1);
    assert(package_list_contains_name(list, "REQUESTS") == 1);
    assert(package_list_contains_name(list, "Requests") == 1);

    package_list_destroy(list);
}

static void test_contains_name_pep503_dash_underscore(void)
{
    /* PEP 503: '-' and '_' are equivalent in package names. */
    PackageList *list = package_list_create();
    package_list_add(list, package_create("more-itertools", NULL));

    assert(package_list_contains_name(list, "more_itertools") == 1);
    assert(package_list_contains_name(list, "more-itertools") == 1);
    assert(package_list_contains_name(list, "more.itertools") == 1);

    package_list_destroy(list);
}

static void test_contains_name_pep503_dot(void)
{
    /* PEP 503: '.' is also equivalent to '-' and '_'. */
    PackageList *list = package_list_create();
    package_list_add(list, package_create("zope.interface", NULL));

    assert(package_list_contains_name(list, "zope-interface") == 1);
    assert(package_list_contains_name(list, "zope_interface") == 1);
    assert(package_list_contains_name(list, "zope.interface") == 1);

    package_list_destroy(list);
}

static void test_contains_name_no_partial_match(void)
{
    /* "requests" must not match "requests-mock" or a prefix thereof. */
    PackageList *list = package_list_create();
    package_list_add(list, package_create("requests", NULL));

    assert(package_list_contains_name(list, "requests-mock") == 0);
    assert(package_list_contains_name(list, "req")           == 0);

    package_list_destroy(list);
}

static void test_contains_name_empty_list(void)
{
    PackageList *list = package_list_create();
    assert(package_list_contains_name(list, "anything") == 0);
    package_list_destroy(list);
}

int main(void)
{
    test_package_create_with_version();
    test_package_create_no_version();
    test_package_destroy_null();
    test_package_list_basic();
    test_package_list_grow();
    test_package_list_contains();
    test_contains_name_basic();
    test_contains_name_case_insensitive();
    test_contains_name_pep503_dash_underscore();
    test_contains_name_pep503_dot();
    test_contains_name_no_partial_match();
    test_contains_name_empty_list();
    return 0;
}
