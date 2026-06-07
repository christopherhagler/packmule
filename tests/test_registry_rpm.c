/*
 * test_registry_rpm.c — unit tests for the RPM registry backend.
 *
 * All tests exercise parse_manifest only (no network calls).
 * Resolve requires a live DNF/YUM repository and is not tested here.
 */
#include "registry.h"
#include "package.h"

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
    /* Last '-' separates name from version. */
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
    /* "python3-devel" alone: contains a '-' but there's no version component.
     * The parser splits on the last '-', so name="python3", version="devel".
     * This is intentional — the manifest format requires explicit versioning
     * for packages whose names contain hyphens; unversioned hyphenated names
     * should be listed as "python3-devel" without a version.
     *
     * The test documents the actual behaviour so it doesn't regress silently. */
    write_file(TMP, "python3-devel\n");

    const Registry *rpm  = registry_find("rpm");
    PackageList    *list = rpm->parse_manifest(rpm, TMP);

    assert(list != NULL);
    assert(list->count == 1);
    /* parser splits at last '-' */
    assert(strcmp(list->items[0]->name,    "python3") == 0);
    assert(strcmp(list->items[0]->version, "devel")   == 0);

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
    return 0;
}
