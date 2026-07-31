/*
 * test_sbom.c — package URL construction and format selection.
 *
 * The purl is the identifier every downstream SBOM consumer matches on, so a
 * wrong one is worse than a missing one: it silently associates a component
 * with the wrong upstream project (and the wrong advisories).
 */
#include "package.h"
#include "sbom.h"
#include "utils.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static Package *pkg_of(const char *name, const char *version,
                       const char *filename)
{
    Package *p = package_create(name, version);
    if (filename)
        p->filename = pm_strdup(filename);
    return p;
}

static void purl_is(const char *registry, const char *name, const char *version,
                    const char *filename, const char *want)
{
    Package *p   = pkg_of(name, version, filename);
    char    *got = sbom_purl(registry, p);

    if (!want) {
        assert(got == NULL);
    } else {
        assert(got != NULL);
        if (strcmp(got, want) != 0) {
            fprintf(stderr, "purl(%s, %s) = %s, want %s\n",
                    registry, name, got, want);
            assert(0);
        }
        pm_free(got);
    }
    package_destroy(p);
}

static void test_pypi_purls(void)
{
    purl_is("pypi", "requests", "2.31.0", NULL, "pkg:pypi/requests@2.31.0");

    /* The purl spec mandates PEP 503 normalisation for the pypi type, so the
     * same project cannot appear under two identifiers. */
    purl_is("pypi", "Django",           "4.2.11", NULL, "pkg:pypi/django@4.2.11");
    purl_is("pypi", "zope.interface",   "5.4.0",  NULL, "pkg:pypi/zope-interface@5.4.0");
    purl_is("pypi", "typing_extensions", "4.12.2", NULL,
            "pkg:pypi/typing-extensions@4.12.2");
    purl_is("pypi", "ruamel.yaml.clib", "0.2.8",  NULL,
            "pkg:pypi/ruamel-yaml-clib@0.2.8");

    /* A local version identifier contains '+', which must be encoded. */
    purl_is("pypi", "torch", "2.1.0+cpu", NULL, "pkg:pypi/torch@2.1.0%2Bcpu");
}

static void test_npm_purls(void)
{
    purl_is("npm", "express", "4.18.2", NULL, "pkg:npm/express@4.18.2");

    /* A scope is a purl namespace, and its '@' must be percent-encoded — the
     * separator between namespace and name stays a literal '/'. */
    purl_is("npm", "@babel/core", "7.24.0", NULL, "pkg:npm/%40babel/core@7.24.0");
    purl_is("npm", "@isaacs/cliui", "8.0.2", NULL,
            "pkg:npm/%40isaacs/cliui@8.0.2");

    /* A dot in an unscoped name is legal and must survive unescaped. */
    purl_is("npm", "lodash.merge", "4.6.2", NULL, "pkg:npm/lodash.merge@4.6.2");
}

static void test_rpm_purls(void)
{
    /* The architecture comes from the filename's penultimate field. */
    purl_is("rpm", "bash", "5.2.15-3.fc39", "bash-5.2.15-3.fc39.x86_64.rpm",
            "pkg:rpm/bash@5.2.15-3.fc39?arch=x86_64");

    /*
     * packmule renders an epoch inline as "epoch:ver-rel"; purl carries it as
     * a qualifier instead, so it has to be split back out or the version field
     * would contain a colon that no consumer expects.
     */
    purl_is("rpm", "docker-ce", "3:29.6.1-1.el9",
            "docker-ce-29.6.1-1.el9.x86_64.rpm",
            "pkg:rpm/docker-ce@29.6.1-1.el9?arch=x86_64&epoch=3");

    purl_is("rpm", "tzdata", "2024a-1.fc39", "tzdata-2024a-1.fc39.noarch.rpm",
            "pkg:rpm/tzdata@2024a-1.fc39?arch=noarch");

    /* No filename means no architecture to report; the rest still stands. */
    purl_is("rpm", "bash", "5.2.15-3.fc39", NULL, "pkg:rpm/bash@5.2.15-3.fc39");
    purl_is("rpm", "bash", "1:5.2.15", NULL, "pkg:rpm/bash@5.2.15?epoch=1");
}

static void test_unresolved_has_no_purl(void)
{
    /* A package that never resolved has no version, so it cannot be
     * identified — emitting "pkg:pypi/foo@" would be worse than nothing. */
    purl_is("pypi", "foo", NULL, NULL, NULL);
    purl_is("pypi", "foo", "",   NULL, NULL);
}

static void test_unknown_registry(void)
{
    /* A backend added later still gets a well-formed identifier. */
    purl_is("cargo", "serde", "1.0.197", NULL, "pkg:cargo/serde@1.0.197");
}

static void test_format_parsing(void)
{
    assert(sbom_parse_format("cyclonedx") == SBOM_CYCLONEDX);
    assert(sbom_parse_format("spdx")      == SBOM_SPDX);
    assert(sbom_parse_format("both")      == (SBOM_CYCLONEDX | SBOM_SPDX));

    assert(sbom_parse_format("CycloneDX") == SBOM_NONE);   /* case-sensitive */
    assert(sbom_parse_format("json")      == SBOM_NONE);
    assert(sbom_parse_format("")          == SBOM_NONE);
    assert(sbom_parse_format(NULL)        == SBOM_NONE);
}

int main(void)
{
    test_pypi_purls();
    test_npm_purls();
    test_rpm_purls();
    test_unresolved_has_no_purl();
    test_unknown_registry();
    test_format_parsing();

    printf("test_sbom: all tests passed\n");
    return 0;
}
