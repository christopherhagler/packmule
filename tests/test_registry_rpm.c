/*
 * test_registry_rpm.c — unit tests for the RPM registry backend.
 *
 * parse_manifest tests: no network calls required.
 * rpm_vercmp / find_rpm_package tests: exercise version ordering and
 * primary.xml scanning against inline XML — no network calls either.
 */
#include "registry.h"
#include "registry_internal.h"
#include "package.h"
#include "utils.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TMP = "test_rpm_tmp.txt";

static void write_file(const char *path, const char *contents)
{
    FILE *fp = fopen(path, "w");
    assert(fp != NULL);
    fputs(contents, fp);
    fclose(fp);
}

static void cleanup(void) { remove(TMP); }

/* ── tests ───────────────────────────────────────────────────────────────── */

static void test_name_only(void)
{
    write_file(TMP, "bash\npython3\n");

    const Registry *rpm  = registry_find("rpm");
    PackageList    *list = rpm->parse_manifest(rpm, TMP);

    assert(list != NULL);
    assert(list->count == 2);
    assert(strcmp(list->items[0]->name, "bash")    == 0);
    assert(list->items[0]->version == NULL);
    assert(strcmp(list->items[1]->name, "python3") == 0);
    assert(list->items[1]->version == NULL);

    package_list_destroy(list);
    cleanup();
}

static void test_name_version(void)
{
    /* Last '-' followed by a digit separates name from version. */
    write_file(TMP, "vim-9.0.0\nopenssl-3.1.4\n");

    const Registry *rpm  = registry_find("rpm");
    PackageList    *list = rpm->parse_manifest(rpm, TMP);

    assert(list != NULL);
    assert(list->count == 2);
    assert(strcmp(list->items[0]->name,    "vim")    == 0);
    assert(strcmp(list->items[0]->version, "9.0.0")  == 0);
    assert(strcmp(list->items[1]->name,    "openssl") == 0);
    assert(strcmp(list->items[1]->version, "3.1.4")   == 0);

    package_list_destroy(list);
    cleanup();
}

static void test_comments_and_blank_lines(void)
{
    write_file(TMP,
               "# a comment\n"
               "\n"
               "bash\n"
               "# another comment\n"
               "\n"
               "vim-9.0.0\n");

    const Registry *rpm  = registry_find("rpm");
    PackageList    *list = rpm->parse_manifest(rpm, TMP);

    assert(list != NULL);
    assert(list->count == 2);
    assert(strcmp(list->items[0]->name, "bash") == 0);
    assert(strcmp(list->items[1]->name, "vim")  == 0);

    package_list_destroy(list);
    cleanup();
}

static void test_empty_file(void)
{
    write_file(TMP, "# only comments\n\n");

    const Registry *rpm  = registry_find("rpm");
    PackageList    *list = rpm->parse_manifest(rpm, TMP);

    assert(list != NULL);
    assert(list->count == 0);

    package_list_destroy(list);
    cleanup();
}

static void test_hyphenated_package_name(void)
{
    /* "python3-devel-3.11.0": last '-' → name="python3-devel", ver="3.11.0" */
    write_file(TMP, "python3-devel-3.11.0\n");

    const Registry *rpm  = registry_find("rpm");
    PackageList    *list = rpm->parse_manifest(rpm, TMP);

    assert(list != NULL);
    assert(list->count == 1);
    assert(strcmp(list->items[0]->name,    "python3-devel") == 0);
    assert(strcmp(list->items[0]->version, "3.11.0")        == 0);

    package_list_destroy(list);
    cleanup();
}

static void test_no_version_hyphenated_name(void)
{
    /* Hyphenated names WITHOUT a version pin must stay whole: the version
     * split only happens when the text after the last '-' starts with a
     * digit (RPM versions always do; RPM names routinely contain hyphens).
     * The old blind last-'-' split turned "python3-pip" into name="python3",
     * version="pip" and silently downloaded the wrong package. */
    write_file(TMP, "python3-devel\npython3-pip\nvim-enhanced\n");

    const Registry *rpm  = registry_find("rpm");
    PackageList    *list = rpm->parse_manifest(rpm, TMP);

    assert(list != NULL);
    assert(list->count == 3);
    assert(strcmp(list->items[0]->name, "python3-devel") == 0);
    assert(list->items[0]->version == NULL);
    assert(strcmp(list->items[1]->name, "python3-pip")   == 0);
    assert(list->items[1]->version == NULL);
    assert(strcmp(list->items[2]->name, "vim-enhanced")  == 0);
    assert(list->items[2]->version == NULL);

    package_list_destroy(list);
    cleanup();
}

static void test_missing_file(void)
{
    const Registry *rpm  = registry_find("rpm");
    PackageList    *list = rpm->parse_manifest(rpm, "/nonexistent/packages.txt");
    assert(list == NULL);
}

static void test_fixture_file(void)
{
    /*
     * Server-deployment fixture: 17 packages covering system utilities, dev
     * tools, Python runtime, databases, web servers, and security libraries.
     * Comments and blank lines are skipped; the last '-' separates name from
     * version (so "python3-devel-3.11.9" → name="python3-devel", ver="3.11.9").
     */
    const Registry *rpm  = registry_find("rpm");
    PackageList    *list = rpm->parse_manifest(rpm, "fixtures/packages.txt");

    assert(list != NULL);
    assert(list->count == 17);

    /* System utilities [0–5] — no version suffix */
    assert(strcmp(list->items[0]->name, "bash")      == 0);
    assert(list->items[0]->version == NULL);
    assert(strcmp(list->items[1]->name, "coreutils") == 0);
    assert(list->items[1]->version == NULL);
    assert(strcmp(list->items[5]->name, "wget")      == 0);
    assert(list->items[5]->version == NULL);

    /* cmake-3.28.1: last '-' splits to name=cmake, ver=3.28.1 [8] */
    assert(strcmp(list->items[8]->name,    "cmake")  == 0);
    assert(strcmp(list->items[8]->version, "3.28.1") == 0);

    /* python3: no '-digit', so version=NULL [10] */
    assert(strcmp(list->items[10]->name, "python3")  == 0);
    assert(list->items[10]->version == NULL);

    /* python3-devel-3.11.9: last '-' → name=python3-devel, ver=3.11.9 [11] */
    assert(strcmp(list->items[11]->name,    "python3-devel") == 0);
    assert(strcmp(list->items[11]->version, "3.11.9")        == 0);

    /* postgresql-16.2 [12] */
    assert(strcmp(list->items[12]->name,    "postgresql") == 0);
    assert(strcmp(list->items[12]->version, "16.2")       == 0);

    /* nginx-1.26.0 [14] */
    assert(strcmp(list->items[14]->name,    "nginx")  == 0);
    assert(strcmp(list->items[14]->version, "1.26.0") == 0);

    /* httpd: no version [15] */
    assert(strcmp(list->items[15]->name, "httpd")    == 0);
    assert(list->items[15]->version == NULL);

    /* openssl-3.2.1 [16] — last entry */
    assert(strcmp(list->items[16]->name,    "openssl") == 0);
    assert(strcmp(list->items[16]->version, "3.2.1")   == 0);

    package_list_destroy(list);
}

/* ── rpm_vercmp ──────────────────────────────────────────────────────────── */

static void test_vercmp(void)
{
    assert(rpm_vercmp("1.0",   "1.0")   == 0);
    assert(rpm_vercmp("1.10",  "1.9")   >  0);   /* numeric, not lexical */
    assert(rpm_vercmp("2.0",   "10.0")  <  0);
    assert(rpm_vercmp("1.0",   "1.0.1") <  0);   /* extra segment is newer */
    assert(rpm_vercmp("1.0a",  "1.0")   >  0);   /* trailing alpha is newer */
    assert(rpm_vercmp("1.0.1", "1.0a")  >  0);   /* numeric beats alpha */
    assert(rpm_vercmp("1.0~rc1", "1.0") <  0);   /* tilde = pre-release */
    assert(rpm_vercmp("1.0~rc1", "1.0~rc2") < 0);
    assert(rpm_vercmp("1.fc40", "1.fc39") > 0);  /* release strings too */
    assert(rpm_vercmp("2",     "1.0-10") >  0);
    assert(rpm_vercmp("1.02",  "1.2")   == 0);   /* leading zeros ignored */
}

/* ── find_rpm_package ────────────────────────────────────────────────────── */

/*
 * Inline primary.xml snippet: two versions of bash (x86_64), one aarch64-only
 * package, one noarch package, one hyphenated name, and one epoch example.
 */
static const char *PRIMARY_XML =
    "<metadata>"
    "<package type=\"rpm\">"
    "  <name>bash</name><arch>x86_64</arch>"
    "  <version epoch=\"0\" ver=\"5.2.15\" rel=\"3.fc39\"/>"
    "  <checksum type=\"sha256\" pkgid=\"YES\">aaa111</checksum>"
    "  <location href=\"Packages/b/bash-5.2.15-3.fc39.x86_64.rpm\"/>"
    "</package>"
    "<package type=\"rpm\">"
    "  <name>bash</name><arch>x86_64</arch>"
    "  <version epoch=\"0\" ver=\"5.2.26\" rel=\"1.fc40\"/>"
    "  <checksum type=\"sha256\" pkgid=\"YES\">bbb222</checksum>"
    "  <location href=\"Packages/b/bash-5.2.26-1.fc40.x86_64.rpm\"/>"
    "</package>"
    "<package type=\"rpm\">"
    "  <name>htop</name><arch>aarch64</arch>"
    "  <version epoch=\"0\" ver=\"3.3.0\" rel=\"1.fc40\"/>"
    "  <checksum type=\"sha256\" pkgid=\"YES\">ccc333</checksum>"
    "  <location href=\"Packages/h/htop-3.3.0-1.fc40.aarch64.rpm\"/>"
    "</package>"
    "<package type=\"rpm\">"
    "  <name>tzdata</name><arch>noarch</arch>"
    "  <version epoch=\"0\" ver=\"2024a\" rel=\"1.fc40\"/>"
    "  <checksum type=\"sha256\" pkgid=\"YES\">ddd444</checksum>"
    "  <location href=\"Packages/t/tzdata-2024a-1.fc40.noarch.rpm\"/>"
    "</package>"
    "<package type=\"rpm\">"
    "  <name>python3-pip</name><arch>noarch</arch>"
    "  <version epoch=\"0\" ver=\"23.3.2\" rel=\"1.fc40\"/>"
    "  <checksum type=\"sha256\" pkgid=\"YES\">eee555</checksum>"
    "  <location href=\"Packages/p/python3-pip-23.3.2-1.fc40.noarch.rpm\"/>"
    "</package>"
    "<package type=\"rpm\">"
    "  <name>dnf</name><arch>noarch</arch>"
    "  <version epoch=\"1\" ver=\"0.5.0\" rel=\"1.fc40\"/>"
    "  <checksum type=\"sha256\" pkgid=\"YES\">fff666</checksum>"
    "  <location href=\"Packages/d/dnf-0.5.0-1.fc40.noarch.rpm\"/>"
    "</package>"
    "<package type=\"rpm\">"
    "  <name>dnf</name><arch>noarch</arch>"
    "  <version epoch=\"0\" ver=\"4.19.0\" rel=\"1.fc40\"/>"
    "  <checksum type=\"sha256\" pkgid=\"YES\">ggg777</checksum>"
    "  <location href=\"Packages/d/dnf-4.19.0-1.fc40.noarch.rpm\"/>"
    "</package>"
    "</metadata>";

static void test_find_unpinned_picks_newest(void)
{
    char *href = NULL, *sha = NULL, *ver = NULL;
    int rc = find_rpm_package(PRIMARY_XML, "bash", NULL, "x86_64",
                              &href, &sha, &ver);
    assert(rc == 0);
    /* 5.2.26 > 5.2.15 — first-match would have returned 5.2.15. */
    assert(strcmp(ver,  "5.2.26-1.fc40") == 0);
    assert(strcmp(sha,  "bbb222")        == 0);
    assert(strstr(href, "bash-5.2.26")   != NULL);
    pm_free(href); pm_free(sha); pm_free(ver);
}

static void test_find_pin_honored(void)
{
    /* Pin the OLDER version: it must be returned, not the newest. */
    char *href = NULL, *sha = NULL, *ver = NULL;
    int rc = find_rpm_package(PRIMARY_XML, "bash", "5.2.15", "x86_64",
                              &href, &sha, &ver);
    assert(rc == 0);
    assert(strcmp(ver, "5.2.15-3.fc39") == 0);
    assert(strcmp(sha, "aaa111")        == 0);
    pm_free(href); pm_free(sha); pm_free(ver);

    /* "ver-rel" pin form works too. */
    href = sha = ver = NULL;
    rc = find_rpm_package(PRIMARY_XML, "bash", "5.2.15-3.fc39", "x86_64",
                          &href, &sha, &ver);
    assert(rc == 0);
    assert(strcmp(sha, "aaa111") == 0);
    pm_free(href); pm_free(sha); pm_free(ver);
}

static void test_find_pin_mismatch(void)
{
    char *href = NULL, *sha = NULL, *ver = NULL;
    int rc = find_rpm_package(PRIMARY_XML, "bash", "9.9.9", "x86_64",
                              &href, &sha, &ver);
    assert(rc == RPM_FIND_VERSION_MISMATCH);
}

static void test_find_not_found(void)
{
    char *href = NULL, *sha = NULL, *ver = NULL;
    int rc = find_rpm_package(PRIMARY_XML, "zsh", NULL, "x86_64",
                              &href, &sha, &ver);
    assert(rc == RPM_FIND_NOT_FOUND);
}

static void test_find_arch_filter(void)
{
    /* htop only exists for aarch64: invisible to an x86_64 target… */
    char *href = NULL, *sha = NULL, *ver = NULL;
    int rc = find_rpm_package(PRIMARY_XML, "htop", NULL, "x86_64",
                              &href, &sha, &ver);
    assert(rc == RPM_FIND_NOT_FOUND);

    /* …but found for aarch64. */
    rc = find_rpm_package(PRIMARY_XML, "htop", NULL, "aarch64",
                          &href, &sha, &ver);
    assert(rc == 0);
    assert(strcmp(sha, "ccc333") == 0);
    pm_free(href); pm_free(sha); pm_free(ver);

    /* noarch packages match any target arch. */
    href = sha = ver = NULL;
    rc = find_rpm_package(PRIMARY_XML, "tzdata", NULL, "x86_64",
                          &href, &sha, &ver);
    assert(rc == 0);
    assert(strcmp(sha, "ddd444") == 0);
    pm_free(href); pm_free(sha); pm_free(ver);
}

static void test_find_hyphenated_name(void)
{
    /* The full hyphenated name must match as a whole. */
    char *href = NULL, *sha = NULL, *ver = NULL;
    int rc = find_rpm_package(PRIMARY_XML, "python3-pip", NULL, "x86_64",
                              &href, &sha, &ver);
    assert(rc == 0);
    assert(strcmp(sha, "eee555")         == 0);
    assert(strcmp(ver, "23.3.2-1.fc40")  == 0);
    pm_free(href); pm_free(sha); pm_free(ver);

    /* And a prefix of it ("python3") is NOT a match in this repo. */
    rc = find_rpm_package(PRIMARY_XML, "python3", NULL, "x86_64",
                          &href, &sha, &ver);
    assert(rc == RPM_FIND_NOT_FOUND);
}

static void test_find_epoch_wins(void)
{
    /* dnf 1:0.5.0 outranks 0:4.19.0 — epoch trumps version. */
    char *href = NULL, *sha = NULL, *ver = NULL;
    int rc = find_rpm_package(PRIMARY_XML, "dnf", NULL, "x86_64",
                              &href, &sha, &ver);
    assert(rc == 0);
    assert(strcmp(ver, "1:0.5.0-1.fc40") == 0);
    assert(strcmp(sha, "fff666")         == 0);
    pm_free(href); pm_free(sha); pm_free(ver);

    /* Epoch-qualified pin form. */
    href = sha = ver = NULL;
    rc = find_rpm_package(PRIMARY_XML, "dnf", "1:0.5.0-1.fc40", "x86_64",
                          &href, &sha, &ver);
    assert(rc == 0);
    assert(strcmp(sha, "fff666") == 0);
    pm_free(href); pm_free(sha); pm_free(ver);
}

int main(void)
{
    test_name_only();
    test_name_version();
    test_comments_and_blank_lines();
    test_empty_file();
    test_hyphenated_package_name();
    test_no_version_hyphenated_name();
    test_missing_file();
    test_fixture_file();
    test_vercmp();
    test_find_unpinned_picks_newest();
    test_find_pin_honored();
    test_find_pin_mismatch();
    test_find_not_found();
    test_find_arch_filter();
    test_find_hyphenated_name();
    test_find_epoch_wins();
    return 0;
}
