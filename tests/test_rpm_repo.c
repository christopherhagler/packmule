/*
 * test_rpm_repo.c — unit tests for the indexed primary.xml view.
 *
 * No network: a small hand-written repodata document exercises capability
 * indexing, file provides, version-flag satisfaction, and the filtering of
 * capabilities that are never separate downloads.
 */
#include "rpm_repo.h"
#include "registry_internal.h"
#include "utils.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *PRIMARY =
    "<?xml version=\"1.0\"?><metadata>"

    "<package type=\"rpm\">"
    "  <name>bash</name><arch>x86_64</arch>"
    "  <version epoch=\"0\" ver=\"5.2.26\" rel=\"1.fc40\"/>"
    "  <checksum type=\"sha256\" pkgid=\"YES\">aaa111</checksum>"
    "  <location href=\"Packages/b/bash-5.2.26-1.fc40.x86_64.rpm\"/>"
    "  <format>"
    "    <rpm:provides>"
    "      <rpm:entry name=\"bash\" flags=\"EQ\" epoch=\"0\" ver=\"5.2.26\" rel=\"1.fc40\"/>"
    "      <rpm:entry name=\"/bin/sh\"/>"
    "    </rpm:provides>"
    "    <rpm:requires>"
    "      <rpm:entry name=\"libtinfo.so.6()(64bit)\"/>"
    "      <rpm:entry name=\"glibc\" flags=\"GE\" epoch=\"0\" ver=\"2.34\"/>"
    "      <rpm:entry name=\"rpmlib(FileDigests)\" flags=\"LE\" ver=\"4.6.0\"/>"
    "      <rpm:entry name=\"config(bash)\" flags=\"EQ\" ver=\"5.2.26\"/>"
    "      <rpm:entry name=\"(python3 or python2)\"/>"
    "    </rpm:requires>"
    "    <file>/usr/bin/bash</file>"
    "  </format>"
    "</package>"

    "<package type=\"rpm\">"
    "  <name>glibc</name><arch>x86_64</arch>"
    "  <version epoch=\"0\" ver=\"2.39\" rel=\"1.fc40\"/>"
    "  <checksum type=\"sha512\" pkgid=\"YES\">bbb222</checksum>"
    "  <location href=\"Packages/g/glibc-2.39-1.fc40.x86_64.rpm\"/>"
    "  <format>"
    "    <rpm:provides>"
    "      <rpm:entry name=\"glibc\" flags=\"EQ\" epoch=\"0\" ver=\"2.39\" rel=\"1.fc40\"/>"
    "    </rpm:provides>"
    "  </format>"
    "</package>"

    "<package type=\"rpm\">"
    "  <name>glibc</name><arch>x86_64</arch>"
    "  <version epoch=\"0\" ver=\"2.28\" rel=\"1.fc40\"/>"
    "  <checksum type=\"sha256\" pkgid=\"YES\">ccc333</checksum>"
    "  <location href=\"Packages/g/glibc-2.28-1.fc40.x86_64.rpm\"/>"
    "  <format>"
    "    <rpm:provides>"
    "      <rpm:entry name=\"glibc\" flags=\"EQ\" epoch=\"0\" ver=\"2.28\" rel=\"1.fc40\"/>"
    "    </rpm:provides>"
    "  </format>"
    "</package>"

    "<package type=\"rpm\">"
    "  <name>ncurses-libs</name><arch>x86_64</arch>"
    "  <version epoch=\"0\" ver=\"6.4\" rel=\"1.fc40\"/>"
    "  <checksum type=\"sha256\" pkgid=\"YES\">ddd444</checksum>"
    "  <location href=\"Packages/n/ncurses-libs-6.4-1.fc40.x86_64.rpm\"/>"
    "  <format>"
    "    <rpm:provides>"
    "      <rpm:entry name=\"libtinfo.so.6()(64bit)\"/>"
    "    </rpm:provides>"
    "  </format>"
    "</package>"

    "</metadata>";

static void test_indexes_every_package(void)
{
    RpmRepo *r = rpm_repo_index(PRIMARY);
    assert(r != NULL);
    assert(rpm_repo_count(r) == 4);
    rpm_repo_free(r);
}

static void test_find_by_name_returns_all_versions(void)
{
    RpmRepo *r = rpm_repo_index(PRIMARY);
    size_t   out[8];

    assert(rpm_repo_find_by_name(r, "bash", out, 8)  == 1);
    assert(rpm_repo_find_by_name(r, "glibc", out, 8) == 2);   /* 2.39 and 2.28 */
    assert(rpm_repo_find_by_name(r, "nope", out, 8)  == 0);

    rpm_repo_free(r);
}

static void test_soname_and_file_provides_are_indexed(void)
{
    RpmRepo *r = rpm_repo_index(PRIMARY);
    size_t   out[8];

    /* Soname capability. */
    assert(rpm_repo_find_providers(r, "libtinfo.so.6()(64bit)", out, 8) == 1);
    char *nm = rpm_block_name(rpm_repo_block(r, out[0]));
    assert(strcmp(nm, "ncurses-libs") == 0);
    pm_free(nm);

    /* A path listed under <rpm:provides>. */
    assert(rpm_repo_find_providers(r, "/bin/sh", out, 8) == 1);

    /* A path listed only as a <file> element.  RPM dependencies name paths
     * routinely, and without indexing these they look unsatisfiable. */
    assert(rpm_repo_find_providers(r, "/usr/bin/bash", out, 8) == 1);

    rpm_repo_free(r);
}

static void test_requires_filters_pseudo_capabilities(void)
{
    RpmRepo *r = rpm_repo_index(PRIMARY);
    size_t   out[8];
    assert(rpm_repo_find_by_name(r, "bash", out, 8) == 1);

    size_t  n = 0, rich = 0;
    RpmCap *caps = rpm_block_requires(rpm_repo_block(r, out[0]), &n, &rich);

    /* Of five entries only libtinfo and glibc are real downloads: rpmlib()
     * is satisfied by rpm itself, config() by the package's own files, and
     * the boolean expression needs a solver we do not have. */
    assert(n == 2);
    assert(rich == 1);
    assert(strncmp(caps[0].name, "libtinfo.so.6", 13) == 0);
    assert(strncmp(caps[1].name, "glibc", 5) == 0);
    assert(strcmp(caps[1].flags, "GE") == 0);
    assert(strcmp(caps[1].ver, "2.34") == 0);

    rpm_caps_free(caps, n);
    rpm_repo_free(r);
}

static void test_version_flags_are_honoured(void)
{
    RpmCap req  = { "glibc", 5, "GE", 0, NULL, NULL };
    RpmCap prov = { "glibc", 5, "EQ", 0, NULL, NULL };

    req.ver  = pm_strdup("2.34");

    prov.ver = pm_strdup("2.39");
    assert(rpm_cap_satisfied_by(&req, &prov) == 1);
    pm_free(prov.ver);

    prov.ver = pm_strdup("2.28");
    assert(rpm_cap_satisfied_by(&req, &prov) == 0);
    pm_free(prov.ver);

    /* An unversioned provider satisfies anything: that is how rpm resolves
     * soname and virtual capabilities. */
    prov.ver = NULL;
    assert(rpm_cap_satisfied_by(&req, &prov) == 1);

    /* An unversioned requirement is satisfied by mere existence. */
    pm_free(req.ver);
    req.ver     = NULL;
    req.flags[0] = '\0';
    assert(rpm_cap_satisfied_by(&req, &prov) == 1);
}

static void test_epoch_dominates_version(void)
{
    RpmCap req  = { "x", 1, "GE", 1, pm_strdup("1.0"), NULL };
    RpmCap prov = { "x", 1, "EQ", 0, pm_strdup("99.0"), NULL };

    /* epoch 0 loses to epoch 1 regardless of how large the version is. */
    assert(rpm_cap_satisfied_by(&req, &prov) == 0);

    prov.epoch = 2;
    assert(rpm_cap_satisfied_by(&req, &prov) == 1);

    rpm_cap_clear(&req);
    rpm_cap_clear(&prov);
}

static void test_block_accessors(void)
{
    RpmRepo *r = rpm_repo_index(PRIMARY);
    size_t   out[8];
    assert(rpm_repo_find_by_name(r, "bash", out, 8) == 1);
    const RpmBlock *b = rpm_repo_block(r, out[0]);

    char *name = rpm_block_name(b);
    char *arch = rpm_block_arch(b);
    char *href = rpm_block_href(b);
    assert(strcmp(name, "bash")   == 0);
    assert(strcmp(arch, "x86_64") == 0);
    assert(strstr(href, "bash-5.2.26") != NULL);
    pm_free(name); pm_free(arch); pm_free(href);

    int   epoch = -1;
    char *ver = NULL, *rel = NULL;
    assert(rpm_block_evr(b, &epoch, &ver, &rel) == 0);
    assert(epoch == 0);
    assert(strcmp(ver, "5.2.26") == 0);
    assert(strcmp(rel, "1.fc40") == 0);
    pm_free(ver); pm_free(rel);

    Digest d = {0};
    assert(rpm_block_digest(b, &d) == 0);
    assert(d.algo == DIGEST_SHA256);
    assert(strcmp(d.value, "aaa111") == 0);
    digest_clear(&d);

    rpm_repo_free(r);
}

static void test_non_sha256_checksum_is_accepted(void)
{
    /*
     * createrepo_c emits whichever algorithm it was configured with.
     * Requiring sha256 made a sha512 repository report "package not found",
     * which sent people looking in entirely the wrong place.
     */
    RpmRepo *r = rpm_repo_index(PRIMARY);
    size_t   out[8];
    assert(rpm_repo_find_by_name(r, "glibc", out, 8) == 2);

    int found_sha512 = 0;
    for (int i = 0; i < 2; i++) {
        Digest d = {0};
        assert(rpm_block_digest(rpm_repo_block(r, out[i]), &d) == 0);
        if (d.algo == DIGEST_SHA512 && strcmp(d.value, "bbb222") == 0)
            found_sha512 = 1;
        digest_clear(&d);
    }
    assert(found_sha512);

    rpm_repo_free(r);
}

static void test_empty_document(void)
{
    RpmRepo *r = rpm_repo_index("<metadata></metadata>");
    assert(r != NULL);
    assert(rpm_repo_count(r) == 0);
    size_t out[4];
    assert(rpm_repo_find_providers(r, "anything", out, 4) == 0);
    rpm_repo_free(r);

    assert(rpm_repo_index(NULL) == NULL);
    rpm_repo_free(NULL);   /* must not crash */
}

int main(void)
{
    test_indexes_every_package();
    test_find_by_name_returns_all_versions();
    test_soname_and_file_provides_are_indexed();
    test_requires_filters_pseudo_capabilities();
    test_version_flags_are_honoured();
    test_epoch_dominates_version();
    test_block_accessors();
    test_non_sha256_checksum_is_accepted();
    test_empty_document();
    printf("test_rpm_repo: all tests passed\n");
    return 0;
}
