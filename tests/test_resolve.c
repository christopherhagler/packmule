/*
 * test_resolve.c — unit tests for the fixpoint resolution loop.
 *
 * A stub Registry stands in for a real backend so the loop's behaviour can be
 * exercised without a network: the interesting property is not which version
 * gets picked but that a constraint discovered late still takes effect, which
 * is exactly what the previous single-pass loop could not do.
 */
#include "resolve.h"
#include "registry.h"
#include "package.h"
#include "utils.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ── Stub backend ────────────────────────────────────────────────────────── */

static int g_resolve_calls;

/*
 * stub_resolve — "resolve" by recording the constraint that was in force.
 * A package with constraint "<2" resolves to version "1"; anything else to
 * "9".  That is enough to tell whether a late constraint was honoured.
 */
static int stub_resolve(const Registry *self, Package *pkg)
{
    (void)self;
    g_resolve_calls++;

    if (pkg->user_pinned)
        return 0;

    pm_free(pkg->version);
    pkg->version = pm_strdup(
        (pkg->constraint && strcmp(pkg->constraint, "<2") == 0) ? "1" : "9");
    return 0;
}

static int stub_resolve_fails(const Registry *self, Package *pkg)
{
    (void)self;
    (void)pkg;
    g_resolve_calls++;
    return -1;
}

/*
 * stub_get_deps — "a" depends on "b" and additionally constrains it to "<2".
 * The constraint therefore only becomes known once "a" has been resolved,
 * which in a list where "b" comes first means after "b" already was.
 */
static int stub_get_deps(const Registry *self, const Package *pkg,
                         const PackageList *seen, PackageList *out)
{
    (void)self;
    if (strcmp(pkg->name, "a") != 0)
        return 0;

    Package *b = package_list_find_name(seen, "b", package_name_equal_exact);
    if (b) {
        package_set_constraint(b, pm_strdup("<2"));
        return 0;
    }
    Package *nb = package_create("b", NULL);
    nb->constraint = pm_strdup("<2");
    package_list_add(out, nb);
    return 1;
}

static int stub_get_deps_fatal(const Registry *self, const Package *pkg,
                               const PackageList *seen, PackageList *out)
{
    (void)self; (void)pkg; (void)seen; (void)out;
    return -1;
}

static Registry make_stub(void)
{
    Registry r;
    memset(&r, 0, sizeof(r));
    r.name       = "stub";
    r.name_equal = package_name_equal_exact;
    r.resolve    = stub_resolve;
    r.get_deps   = stub_get_deps;
    return r;
}

/* ── Tests ───────────────────────────────────────────────────────────────── */

static void test_late_constraint_is_honoured(void)
{
    /*
     * The regression this whole loop exists for.  "b" is listed first, so a
     * single pass resolves it before "a" is even looked at, and "a"'s
     * constraint on it arrives too late to matter.
     */
    Registry     reg  = make_stub();
    PackageList *list = package_list_create();
    package_list_add(list, package_create("b", NULL));
    package_list_add(list, package_create("a", NULL));

    ResolveStats st;
    assert(resolve_all(&reg, list, NULL, &st) == 0);

    Package *b = package_list_find_name(list, "b", package_name_equal_exact);
    assert(strcmp(b->version, "1") == 0);   /* would be "9" with one pass */
    assert(st.rounds >= 2);                 /* it took a revisit */
    assert(st.failed == 0);
    assert(st.resolved == 2);

    package_list_destroy(list);
}

static void test_result_is_order_independent(void)
{
    /* The same inputs in the other order must produce the same answer. */
    Registry reg = make_stub();

    PackageList *l1 = package_list_create();
    package_list_add(l1, package_create("a", NULL));
    package_list_add(l1, package_create("b", NULL));
    assert(resolve_all(&reg, l1, NULL, NULL) == 0);

    PackageList *l2 = package_list_create();
    package_list_add(l2, package_create("b", NULL));
    package_list_add(l2, package_create("a", NULL));
    assert(resolve_all(&reg, l2, NULL, NULL) == 0);

    const char *v1 = package_list_find_name(l1, "b",
                                            package_name_equal_exact)->version;
    const char *v2 = package_list_find_name(l2, "b",
                                            package_name_equal_exact)->version;
    assert(strcmp(v1, v2) == 0);

    package_list_destroy(l1);
    package_list_destroy(l2);
}

static void test_reaches_a_fixed_point(void)
{
    /* Once nothing is queued or dirty the loop must stop, not keep spinning
     * until the round cap. */
    Registry     reg  = make_stub();
    PackageList *list = package_list_create();
    package_list_add(list, package_create("a", NULL));

    ResolveStats st;
    g_resolve_calls = 0;
    assert(resolve_all(&reg, list, NULL, &st) == 0);

    assert(st.hit_round_cap == 0);
    assert(st.rounds < RESOLVE_MAX_ROUNDS);
    /* a + b resolved once each, plus b's one revisit at most. */
    assert(g_resolve_calls <= 3);

    package_list_destroy(list);
}

static void test_pinned_package_is_never_revised(void)
{
    Registry     reg  = make_stub();
    PackageList *list = package_list_create();

    Package *b = package_create("b", "7");
    b->user_pinned = 1;
    package_list_add(list, b);
    package_list_add(list, package_create("a", NULL));

    assert(resolve_all(&reg, list, NULL, NULL) == 0);
    assert(strcmp(b->version, "7") == 0);

    package_list_destroy(list);
}

static void test_failure_is_reported_and_not_retried(void)
{
    Registry reg = make_stub();
    reg.resolve  = stub_resolve_fails;
    reg.get_deps = NULL;

    PackageList *list = package_list_create();
    package_list_add(list, package_create("a", NULL));

    ResolveStats st;
    g_resolve_calls = 0;
    assert(resolve_all(&reg, list, NULL, &st) == -1);

    assert(st.failed == 1);
    assert(st.resolved == 0);
    assert(list->items[0]->state == PKG_FAILED);
    /* A failed package must not be retried every round. */
    assert(g_resolve_calls == 1);

    package_list_destroy(list);
}

static void test_unbundleable_dep_stops_the_run(void)
{
    Registry reg = make_stub();
    reg.get_deps = stub_get_deps_fatal;

    PackageList *list = package_list_create();
    package_list_add(list, package_create("a", NULL));

    ResolveStats st;
    assert(resolve_all(&reg, list, NULL, &st) == -1);
    assert(st.fatal == 1);

    package_list_destroy(list);
}

static void test_empty_list(void)
{
    Registry     reg  = make_stub();
    PackageList *list = package_list_create();

    ResolveStats st;
    assert(resolve_all(&reg, list, NULL, &st) == 0);
    assert(st.resolved == 0 && st.failed == 0);

    package_list_destroy(list);
}

int main(void)
{
    test_late_constraint_is_honoured();
    test_result_is_order_independent();
    test_reaches_a_fixed_point();
    test_pinned_package_is_never_revised();
    test_failure_is_reported_and_not_retried();
    test_unbundleable_dep_stops_the_run();
    test_empty_list();
    printf("test_resolve: all tests passed\n");
    return 0;
}
