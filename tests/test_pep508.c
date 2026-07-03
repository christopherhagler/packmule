/*
 * test_pep508.c — unit tests for PEP 508 dependency-specifier parsing and
 * environment-marker evaluation (pure string processing, no network).
 */
#include "pep508.h"
#include "utils.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_marker_excludes(void)
{
    /* OS markers evaluate against the target platform. */
    assert(pep508_marker_excludes("; sys_platform == 'win32'", "linux", 12) == 1);
    assert(pep508_marker_excludes("; sys_platform == 'win32'", "windows", 12) == 0);
    assert(pep508_marker_excludes("; sys_platform != 'win32'", "linux", 12) == 0);
    assert(pep508_marker_excludes("; platform_system == 'Darwin'", "macos", 12) == 0);
    assert(pep508_marker_excludes("; os_name == 'nt'", "linux", 12) == 1);

    /* Python-version markers evaluate against the target minor. */
    assert(pep508_marker_excludes("; python_version < '3.9'", "linux", 12) == 1);
    assert(pep508_marker_excludes("; python_version >= '3.8'", "linux", 12) == 0);

    /* Unknown target or unmodelled variable never excludes. */
    assert(pep508_marker_excludes("; sys_platform == 'win32'", NULL, 12) == 0);
    assert(pep508_marker_excludes("; python_version < '3.9'", "linux", 0) == 0);
    assert(pep508_marker_excludes("; platform_machine == 'x86_64'", "linux", 12) == 0);
    /* Three-part version values cannot be decided from a minor alone. */
    assert(pep508_marker_excludes("; python_full_version < '3.9.7'", "linux", 9) == 0);

    /* and/or combinations; "and" binds tighter. */
    assert(pep508_marker_excludes(
        "; python_version < '3.9' and sys_platform == 'linux'", "linux", 12) == 1);
    assert(pep508_marker_excludes(
        "; python_version < '3.9' or sys_platform == 'linux'", "linux", 12) == 0);

    /* Absent / empty markers keep the dep. */
    assert(pep508_marker_excludes(NULL, "linux", 12) == 0);
    assert(pep508_marker_excludes(";  ", "linux", 12) == 0);
}

static void test_extras_gating(void)
{
    assert(pep508_dep_is_extras_only("cryptography>=1.3.4; extra == 'security'"));
    assert(!pep508_dep_is_extras_only("certifi>=2017.4.17"));

    assert(pep508_marker_matches_extras("; extra == 'security'", "security"));
    assert(pep508_marker_matches_extras("; extra == \"security\"", "security"));
    assert(pep508_marker_matches_extras("; extra == 'security'",
                                        "socks,security"));
    assert(!pep508_marker_matches_extras("; extra == 'security'", "socks"));
    assert(!pep508_marker_matches_extras("; extra == 'security'", NULL));
    assert(!pep508_marker_matches_extras(NULL, "security"));
}

static void test_spec_name(void)
{
    char name[64];

    pep508_spec_name("Django>=3.2", name, sizeof(name));
    assert(strcmp(name, "django") == 0);

    pep508_spec_name("uvicorn[standard]==0.29.0", name, sizeof(name));
    assert(strcmp(name, "uvicorn") == 0);

    pep508_spec_name("colorama ; sys_platform == 'win32'", name, sizeof(name));
    assert(strcmp(name, "colorama") == 0);

    pep508_spec_name("requests (>=2.0)", name, sizeof(name));
    assert(strcmp(name, "requests") == 0);
}

static void test_spec_exact_version(void)
{
    char *v;

    v = pep508_spec_exact_version("pydantic-core==2.18.2");
    assert(v && strcmp(v, "2.18.2") == 0);
    pm_free(v);

    v = pep508_spec_exact_version("requests (==2.0)");
    assert(v && strcmp(v, "2.0") == 0);
    pm_free(v);

    assert(pep508_spec_exact_version("urllib3>=1.21.1") == NULL);
    assert(pep508_spec_exact_version("packaging==1.*")  == NULL); /* wildcard */
    assert(pep508_spec_exact_version("certifi")         == NULL);
    /* An "==" inside the marker is not a pin. */
    assert(pep508_spec_exact_version("colorama ; extra == 'cli'") == NULL);
}

static void test_spec_constraint(void)
{
    char *c;

    c = pep508_spec_constraint("urllib3 (>=1.21.1,<3)");
    assert(c && strcmp(c, ">=1.21.1,<3") == 0);
    pm_free(c);

    c = pep508_spec_constraint("charset-normalizer<4,>=2 ; python_version >= '3'");
    assert(c && strcmp(c, "<4,>=2") == 0);
    pm_free(c);

    c = pep508_spec_constraint("uvicorn[standard]>=0.12.0");
    assert(c && strcmp(c, ">=0.12.0") == 0);
    pm_free(c);

    assert(pep508_spec_constraint("certifi") == NULL);
    assert(pep508_spec_constraint("pkg @ https://example.com/x.whl") == NULL);
}

static void test_spec_extras(void)
{
    char *e;

    e = pep508_spec_extras("uvicorn[standard]==0.29.0");
    assert(e && strcmp(e, "standard") == 0);
    pm_free(e);

    e = pep508_spec_extras("passlib[Bcrypt, argon2]>=1.7");
    assert(e && strcmp(e, "bcrypt,argon2") == 0);
    pm_free(e);

    assert(pep508_spec_extras("requests==2.31.0") == NULL);
}

int main(void)
{
    test_marker_excludes();
    test_extras_gating();
    test_spec_name();
    test_spec_exact_version();
    test_spec_constraint();
    test_spec_extras();
    printf("test_pep508: all tests passed\n");
    return 0;
}
