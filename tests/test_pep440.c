/*
 * test_pep440.c — unit tests for PEP 440 version ordering and specifier
 * matching (pure string processing, no network).
 */
#include "pep440.h"

#include <assert.h>
#include <stdio.h>

static void test_validity(void)
{
    assert(pep440_valid("1.0"));
    assert(pep440_valid("2024.10.8"));
    assert(pep440_valid("1.0.0rc1"));
    assert(pep440_valid("1.0.post1"));
    assert(pep440_valid("1.0.dev3"));
    assert(pep440_valid("v1.2.3"));
    assert(pep440_valid("1!2.0"));
    assert(pep440_valid("1.0+local.tag"));
    assert(pep440_valid("1.0a1"));
    assert(pep440_valid("1.0-beta.2"));

    assert(!pep440_valid("not-a-version"));
    assert(!pep440_valid(""));
    assert(!pep440_valid("2.4.7.dfsg-1"));   /* distro-mangled legacy junk */
    /* "r" is a legal PEP 440 spelling of "post", so legacy strings like
     * "0.1dev-r1716" normalise rather than fail. */
    assert(pep440_valid("0.1dev-r1716"));
}

static void test_ordering(void)
{
    /* Basic numeric ordering; "1.10" > "1.9". */
    assert(pep440_cmp("1.10", "1.9")   > 0);
    assert(pep440_cmp("1.0",  "1.0.0") == 0);   /* zero padding */
    assert(pep440_cmp("2.0",  "10.0")  < 0);

    /* dev < alpha < beta < rc < final < post. */
    assert(pep440_cmp("1.0.dev1", "1.0a1")     < 0);
    assert(pep440_cmp("1.0a1",    "1.0b1")     < 0);
    assert(pep440_cmp("1.0b1",    "1.0rc1")    < 0);
    assert(pep440_cmp("1.0rc1",   "1.0")       < 0);
    assert(pep440_cmp("1.0",      "1.0.post1") < 0);
    assert(pep440_cmp("1.0rc1",   "1.0rc2")    < 0);

    /* Epochs dominate. */
    assert(pep440_cmp("1!1.0", "999.0") > 0);

    /* Local version labels are ignored. */
    assert(pep440_cmp("1.0+cpu", "1.0") == 0);

    /* Alternate qualifier spellings normalise. */
    assert(pep440_cmp("1.0alpha1", "1.0a1") == 0);
    assert(pep440_cmp("1.0-rc.1",  "1.0rc1") == 0);
    assert(pep440_cmp("1.0-1",     "1.0.post1") == 0);
}

static void test_prerelease_detection(void)
{
    assert(pep440_is_prerelease("1.0a1"));
    assert(pep440_is_prerelease("1.0rc2"));
    assert(pep440_is_prerelease("1.0.dev1"));
    assert(!pep440_is_prerelease("1.0"));
    assert(!pep440_is_prerelease("1.0.post1"));
}

static void test_satisfies(void)
{
    /* The production bug class this exists for: upper bounds. */
    assert(pep440_satisfies("1.26.4", ">=1.20,<2.0") == 1);
    assert(pep440_satisfies("2.3.0",  ">=1.20,<2.0") == 0);

    assert(pep440_satisfies("2.31.0", ">=2.28")   == 1);
    assert(pep440_satisfies("2.27.1", ">=2.28")   == 0);
    assert(pep440_satisfies("3.0.0",  "<3")       == 0);
    assert(pep440_satisfies("2.9.9",  "<3")       == 1);
    assert(pep440_satisfies("1.7.4",  "!=1.7.3")  == 1);
    assert(pep440_satisfies("1.7.3",  "!=1.7.3")  == 0);

    /* Exact pins pad with zeros. */
    assert(pep440_satisfies("1.0.0", "==1.0") == 1);
    assert(pep440_satisfies("1.0.1", "==1.0") == 0);

    /* Wildcards. */
    assert(pep440_satisfies("1.4.9", "==1.4.*") == 1);
    assert(pep440_satisfies("1.5.0", "==1.4.*") == 0);
    assert(pep440_satisfies("1.5.0", "!=1.4.*") == 1);

    /* Compatible release. */
    assert(pep440_satisfies("65.7.0", "~=65.0")   == 1);
    assert(pep440_satisfies("66.0.0", "~=65.0")   == 0);
    assert(pep440_satisfies("1.4.7",  "~=1.4.2")  == 1);
    assert(pep440_satisfies("1.5.0",  "~=1.4.2")  == 0);

    /* Spaces and parentheses already stripped by callers, but tolerate
     * embedded spaces after operators. */
    assert(pep440_satisfies("2.0.0", ">= 1.0") == 1);

    /* Arbitrary equality is textual. */
    assert(pep440_satisfies("1.0",   "===1.0")   == 1);
    assert(pep440_satisfies("1.0.0", "===1.0")   == 0);

    /* Empty/NULL specs admit anything; garbage reports unparseable. */
    assert(pep440_satisfies("1.0", "")   == 1);
    assert(pep440_satisfies("1.0", NULL) == 1);
    assert(pep440_satisfies("1.0", "@git+https://x") == -1);
}

static void test_spec_prerelease_admission(void)
{
    assert(pep440_spec_admits_prerelease(">=2.0.0rc1"));
    assert(pep440_spec_admits_prerelease("==1.0a1"));
    assert(!pep440_spec_admits_prerelease(">=1.20,<2.0"));
    assert(!pep440_spec_admits_prerelease(NULL));
}

int main(void)
{
    test_validity();
    test_ordering();
    test_prerelease_detection();
    test_satisfies();
    test_spec_prerelease_admission();
    printf("test_pep440: all tests passed\n");
    return 0;
}
