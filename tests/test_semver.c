/*
 * test_semver.c — unit tests for semver_cmp and semver_satisfies.
 *
 * Pure string logic, no network or filesystem access.
 */
#include "semver.h"

#include <assert.h>

/* ── semver_cmp ──────────────────────────────────────────────────────────── */

static void test_cmp_numeric_ordering(void)
{
    assert(semver_cmp("1.0.0",  "1.0.0")  == 0);
    assert(semver_cmp("2.0.0",  "1.9.9")  >  0);
    assert(semver_cmp("1.10.0", "1.9.0")  >  0);   /* numeric, not lexical */
    assert(semver_cmp("1.0.10", "1.0.9")  >  0);
    assert(semver_cmp("0.1.0",  "0.2.0")  <  0);
    assert(semver_cmp("10.0.0", "9.0.0")  >  0);
}

static void test_cmp_partial_and_v_prefix(void)
{
    assert(semver_cmp("1.2",    "1.2.0")  == 0);
    assert(semver_cmp("1",      "1.0.0")  == 0);
    assert(semver_cmp("v1.2.3", "1.2.3")  == 0);
}

static void test_cmp_prerelease(void)
{
    /* A prerelease sorts below its release. */
    assert(semver_cmp("1.0.0-alpha", "1.0.0")        <  0);
    assert(semver_cmp("1.0.0",       "1.0.0-rc.1")   >  0);

    /* SemVer 2.0 precedence examples. */
    assert(semver_cmp("1.0.0-alpha",      "1.0.0-alpha.1")  < 0);
    assert(semver_cmp("1.0.0-alpha.1",    "1.0.0-alpha.beta") < 0); /* num < alpha */
    assert(semver_cmp("1.0.0-alpha.beta", "1.0.0-beta")     < 0);
    assert(semver_cmp("1.0.0-beta.2",     "1.0.0-beta.11")  < 0);   /* numeric */
    assert(semver_cmp("1.0.0-rc.1",       "1.0.0-rc.1")     == 0);

    /* Build metadata is ignored. */
    assert(semver_cmp("1.0.0+build.1", "1.0.0") == 0);
}

/* ── semver_satisfies: caret ─────────────────────────────────────────────── */

static void test_satisfies_caret(void)
{
    assert(semver_satisfies("4.18.2", "^4.18.0") == 1);
    assert(semver_satisfies("4.99.0", "^4.18.0") == 1);
    assert(semver_satisfies("5.0.0",  "^4.18.0") == 0);
    assert(semver_satisfies("4.17.9", "^4.18.0") == 0);

    /* ^0.x pins the minor; ^0.0.x pins the patch. */
    assert(semver_satisfies("0.2.5", "^0.2.3") == 1);
    assert(semver_satisfies("0.3.0", "^0.2.3") == 0);
    assert(semver_satisfies("0.0.3", "^0.0.3") == 1);
    assert(semver_satisfies("0.0.4", "^0.0.3") == 0);

    /* Partial operands. */
    assert(semver_satisfies("1.9.0", "^1.2") == 1);
    assert(semver_satisfies("2.0.0", "^1.2") == 0);
    assert(semver_satisfies("16.8.0", "^16") == 1);
    assert(semver_satisfies("17.0.0", "^16") == 0);
}

/* ── semver_satisfies: tilde ─────────────────────────────────────────────── */

static void test_satisfies_tilde(void)
{
    assert(semver_satisfies("1.2.9", "~1.2.3") == 1);
    assert(semver_satisfies("1.3.0", "~1.2.3") == 0);
    assert(semver_satisfies("1.2.2", "~1.2.3") == 0);

    /* ~1.2 == >=1.2.0 <1.3.0 ; ~1 == >=1.0.0 <2.0.0 */
    assert(semver_satisfies("1.2.5", "~1.2") == 1);
    assert(semver_satisfies("1.3.0", "~1.2") == 0);
    assert(semver_satisfies("1.9.9", "~1")   == 1);
    assert(semver_satisfies("2.0.0", "~1")   == 0);
}

/* ── semver_satisfies: comparators, AND, OR ──────────────────────────────── */

static void test_satisfies_comparators(void)
{
    assert(semver_satisfies("2.0.0", ">=2")       == 1);
    assert(semver_satisfies("1.9.9", ">=2")       == 0);
    assert(semver_satisfies("2.5.0", ">2.0.0")    == 1);
    assert(semver_satisfies("2.0.0", ">2.0.0")    == 0);
    assert(semver_satisfies("1.0.0", "<=1.0.0")   == 1);
    assert(semver_satisfies("1.0.1", "<=1.0.0")   == 0);
    assert(semver_satisfies("0.9.0", "<1.0.0")    == 1);
    assert(semver_satisfies("1.2.3", "=1.2.3")    == 1);
    assert(semver_satisfies("1.2.3", "1.2.3")     == 1);
    assert(semver_satisfies("1.2.4", "1.2.3")     == 0);

    /* Spaces between operator and operand. */
    assert(semver_satisfies("2.5.0", ">= 2.0.0")  == 1);

    /* AND: both comparators must hold. */
    assert(semver_satisfies("2.5.0", ">=2 <3")    == 1);
    assert(semver_satisfies("3.0.0", ">=2 <3")    == 0);

    /* OR. */
    assert(semver_satisfies("16.8.0", "^16 || ^17 || ^18") == 1);
    assert(semver_satisfies("18.2.0", "^16 || ^17 || ^18") == 1);
    assert(semver_satisfies("19.0.0", "^16 || ^17 || ^18") == 0);
}

/* ── semver_satisfies: x-ranges, wildcards, hyphen ranges ────────────────── */

static void test_satisfies_x_ranges(void)
{
    assert(semver_satisfies("1.9.0", "1.x")   == 1);
    assert(semver_satisfies("2.0.0", "1.x")   == 0);
    assert(semver_satisfies("1.2.9", "1.2.x") == 1);
    assert(semver_satisfies("1.3.0", "1.2.x") == 0);
    assert(semver_satisfies("1.2.9", "1.2.*") == 1);
    assert(semver_satisfies("9.9.9", "*")     == 1);
    assert(semver_satisfies("9.9.9", "")      == 1);

    /* Bare partial versions behave like x-ranges. */
    assert(semver_satisfies("1.5.0", "1")     == 1);
    assert(semver_satisfies("2.0.0", "1")     == 0);
    assert(semver_satisfies("1.2.7", "1.2")   == 1);
    assert(semver_satisfies("1.3.0", "1.2")   == 0);
}

static void test_satisfies_hyphen_range(void)
{
    assert(semver_satisfies("1.5.0", "1.2.3 - 2.0.0") == 1);
    assert(semver_satisfies("1.2.3", "1.2.3 - 2.0.0") == 1);
    assert(semver_satisfies("2.0.0", "1.2.3 - 2.0.0") == 1);
    assert(semver_satisfies("2.0.1", "1.2.3 - 2.0.0") == 0);
    assert(semver_satisfies("1.2.2", "1.2.3 - 2.0.0") == 0);

    /* Partial upper bound is inclusive of its whole range: "- 2.3" → <2.4.0 */
    assert(semver_satisfies("2.3.9", "1.0.0 - 2.3") == 1);
    assert(semver_satisfies("2.4.0", "1.0.0 - 2.3") == 0);
}

/* ── semver_satisfies: prerelease admission ──────────────────────────────── */

static void test_satisfies_prerelease_rule(void)
{
    /* A range never matches a prerelease it does not explicitly name. */
    assert(semver_satisfies("2.0.0-rc.1", "^1.0.0")  == 0);
    assert(semver_satisfies("2.0.0-rc.1", ">=1.0.0") == 0);
    assert(semver_satisfies("2.0.0-rc.1", "*")       == 0);

    /* Naming a prerelease of the same tuple admits it. */
    assert(semver_satisfies("2.0.0-rc.2", ">=2.0.0-rc.1") == 1);
    assert(semver_satisfies("2.0.0-rc.1", "2.0.0-rc.1")   == 1);

    /* ...but a different tuple's prerelease stays excluded. */
    assert(semver_satisfies("2.1.0-rc.1", ">=2.0.0-rc.1") == 0);
}

/* ── semver_satisfies: unparseable specs ─────────────────────────────────── */

static void test_satisfies_unparseable(void)
{
    assert(semver_satisfies("1.0.0", "latest")                        == -1);
    assert(semver_satisfies("1.0.0", "git+https://github.com/a/b")    == -1);
    assert(semver_satisfies("1.0.0", "file:../local")                 == -1);

    /* An unparseable OR-arm doesn't block a matching one. */
    assert(semver_satisfies("1.5.0", "weird-tag || ^1.0.0")           == 1);
}

int main(void)
{
    test_cmp_numeric_ordering();
    test_cmp_partial_and_v_prefix();
    test_cmp_prerelease();
    test_satisfies_caret();
    test_satisfies_tilde();
    test_satisfies_comparators();
    test_satisfies_x_ranges();
    test_satisfies_hyphen_range();
    test_satisfies_prerelease_rule();
    test_satisfies_unparseable();
    return 0;
}
