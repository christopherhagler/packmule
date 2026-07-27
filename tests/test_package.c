/*
 * test_package.c — unit tests for package.h / package.c
 */
#include "hash.h"
#include "package.h"
#include "utils.h"

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
    assert(pkg->url       == NULL);
    assert(!digest_is_set(&pkg->digest));
    assert(pkg->filename  == NULL);
    assert(pkg->dep_specs == NULL);
    package_destroy(pkg);
}

static void test_package_create_no_version(void)
{
    Package *pkg = package_create("flask", NULL);
    assert(pkg != NULL);
    assert(strcmp(pkg->name, "flask") == 0);
    assert(pkg->version   == NULL);
    assert(pkg->url       == NULL);
    assert(!digest_is_set(&pkg->digest));
    assert(pkg->filename  == NULL);
    assert(pkg->dep_specs == NULL);
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

/* ── package_list_contains_name ──────────────────────────────────────────── */

static void test_contains_name_basic(void)
{
    PackageList *list = package_list_create();
    package_list_add(list, package_create("requests", "2.31.0"));

    assert(package_list_contains_name(list, "requests", package_name_equal_pep503) == 1);
    assert(package_list_contains_name(list, "flask", package_name_equal_pep503)    == 0);

    package_list_destroy(list);
}

static void test_contains_name_case_insensitive(void)
{
    PackageList *list = package_list_create();
    package_list_add(list, package_create("Requests", NULL));

    assert(package_list_contains_name(list, "requests", package_name_equal_pep503) == 1);
    assert(package_list_contains_name(list, "REQUESTS", package_name_equal_pep503) == 1);
    assert(package_list_contains_name(list, "Requests", package_name_equal_pep503) == 1);

    package_list_destroy(list);
}

static void test_contains_name_pep503_dash_underscore(void)
{
    /* PEP 503: '-' and '_' are equivalent in package names. */
    PackageList *list = package_list_create();
    package_list_add(list, package_create("more-itertools", NULL));

    assert(package_list_contains_name(list, "more_itertools", package_name_equal_pep503) == 1);
    assert(package_list_contains_name(list, "more-itertools", package_name_equal_pep503) == 1);
    assert(package_list_contains_name(list, "more.itertools", package_name_equal_pep503) == 1);

    package_list_destroy(list);
}

static void test_contains_name_pep503_dot(void)
{
    /* PEP 503: '.' is also equivalent to '-' and '_'. */
    PackageList *list = package_list_create();
    package_list_add(list, package_create("zope.interface", NULL));

    assert(package_list_contains_name(list, "zope-interface", package_name_equal_pep503) == 1);
    assert(package_list_contains_name(list, "zope_interface", package_name_equal_pep503) == 1);
    assert(package_list_contains_name(list, "zope.interface", package_name_equal_pep503) == 1);

    package_list_destroy(list);
}

static void test_contains_name_no_partial_match(void)
{
    /* "requests" must not match "requests-mock" or a prefix thereof. */
    PackageList *list = package_list_create();
    package_list_add(list, package_create("requests", NULL));

    assert(package_list_contains_name(list, "requests-mock", package_name_equal_pep503) == 0);
    assert(package_list_contains_name(list, "req", package_name_equal_pep503)           == 0);

    package_list_destroy(list);
}

static void test_contains_name_empty_list(void)
{
    PackageList *list = package_list_create();
    assert(package_list_contains_name(list, "anything", package_name_equal_pep503) == 0);
    package_list_destroy(list);
}

/* ── Registry-specific name equality ─────────────────────────────────────── */

static void test_name_equality_is_per_registry(void)
{
    /* PEP 503 folding is PyPI's rule and must not leak into the others:
     * npm's "lodash.merge" and "lodash-merge" are different packages, and so
     * are RPM's "python3.11" and "python3-11". */
    assert(package_name_equal_pep503("zope.interface", "zope-interface") == 1);
    assert(package_name_equal_pep503("Requests", "requests")             == 1);

    assert(package_name_equal_casefold("lodash.merge", "lodash-merge")   == 0);
    assert(package_name_equal_casefold("Lodash", "lodash")               == 1);

    assert(package_name_equal_exact("python3.11", "python3-11")          == 0);
    assert(package_name_equal_exact("Bash", "bash")                      == 0);
    assert(package_name_equal_exact("bash", "bash")                      == 1);
}

static void test_find_name_honours_comparator(void)
{
    PackageList *list = package_list_create();
    package_list_add(list, package_create("lodash.merge", NULL));

    assert(package_list_find_name(list, "lodash-merge",
                                  package_name_equal_casefold) == NULL);
    assert(package_list_find_name(list, "lodash.merge",
                                  package_name_equal_casefold) != NULL);
    /* Same list, PyPI rules: now they collide. */
    assert(package_list_find_name(list, "lodash-merge",
                                  package_name_equal_pep503) != NULL);

    package_list_destroy(list);
}

/* ── Constraint and extras merging ───────────────────────────────────────── */

static void test_set_constraint_marks_resolved_package_dirty(void)
{
    /* This is the hinge of the fixpoint resolver: a constraint arriving after
     * a package was resolved must schedule it to be resolved again. */
    Package *p = package_create("urllib3", NULL);
    p->state = PKG_RESOLVED;
    p->dirty = 0;

    assert(package_set_constraint(p, pm_strdup("<1.25")) == 1);
    assert(p->dirty == 1);

    /* Setting the same value again changes nothing and must not re-dirty. */
    p->dirty = 0;
    assert(package_set_constraint(p, pm_strdup("<1.25")) == 0);
    assert(p->dirty == 0);

    package_destroy(p);
}

static void test_add_extras_unions_whole_tokens(void)
{
    Package *p = package_create("uvicorn", NULL);

    assert(package_add_extras(p, "standard") == 1);
    assert(strcmp(p->extras, "standard") == 0);

    /* "sec" must not be considered present just because "security" is: a
     * substring check here silently dropped extras-gated dependencies. */
    assert(package_add_extras(p, "security") == 1);
    assert(package_add_extras(p, "sec")      == 1);
    assert(strcmp(p->extras, "standard,security,sec") == 0);

    /* Re-adding an existing extra is a no-op, so repeated resolver rounds
     * cannot grow the list without bound. */
    assert(package_add_extras(p, "standard") == 0);
    assert(package_add_extras(p, "sec")      == 0);
    assert(strcmp(p->extras, "standard,security,sec") == 0);

    package_destroy(p);
}

static void test_add_extras_marks_resolved_package_dirty(void)
{
    Package *p = package_create("uvicorn", NULL);
    p->state = PKG_RESOLVED;
    p->dirty = 0;

    assert(package_add_extras(p, "standard") == 1);
    assert(p->dirty == 1);

    package_destroy(p);
}

int main(void)
{
    test_name_equality_is_per_registry();
    test_find_name_honours_comparator();
    test_set_constraint_marks_resolved_package_dirty();
    test_add_extras_unions_whole_tokens();
    test_add_extras_marks_resolved_package_dirty();
    test_package_create_with_version();
    test_package_create_no_version();
    test_package_destroy_null();
    test_package_list_basic();
    test_package_list_grow();
    test_contains_name_basic();
    test_contains_name_case_insensitive();
    test_contains_name_pep503_dash_underscore();
    test_contains_name_pep503_dot();
    test_contains_name_no_partial_match();
    test_contains_name_empty_list();
    return 0;
}
