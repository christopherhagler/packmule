/*
 * test_registry_pypi.c — unit tests for the PyPI registry backend.
 *
 * All tests exercise parse_manifest only (no network calls).
 */
#include "registry.h"
#include "package.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TMP = "test_pypi_tmp.txt";

static void write_file(const char *path, const char *contents)
{
    FILE *fp = fopen(path, "w");
    assert(fp != NULL);
    fputs(contents, fp);
    fclose(fp);
}

static void cleanup(void) { remove(TMP); }

/* ── manifest parsing ────────────────────────────────────────────────────── */

static void test_basic_parse(void)
{
    write_file(TMP,
               "requests==2.31.0\n"
               "flask\n"
               "# comment\n"
               "\n"
               "numpy==1.26.0\n");

    const Registry *pypi = registry_find("pypi");
    PackageList    *list = pypi->parse_manifest(pypi, TMP);

    assert(list != NULL);
    assert(list->count == 3);
    assert(strcmp(list->items[0]->name,    "requests") == 0);
    assert(strcmp(list->items[0]->version, "2.31.0")   == 0);
    assert(strcmp(list->items[1]->name,    "flask")    == 0);
    assert(list->items[1]->version == NULL);
    assert(strcmp(list->items[2]->name,    "numpy")    == 0);
    assert(strcmp(list->items[2]->version, "1.26.0")   == 0);

    package_list_destroy(list);
    cleanup();
}

static void test_empty_file(void)
{
    write_file(TMP, "# only a comment\n\n\n");

    const Registry *pypi = registry_find("pypi");
    PackageList    *list = pypi->parse_manifest(pypi, TMP);

    assert(list != NULL);
    assert(list->count == 0);

    package_list_destroy(list);
    cleanup();
}

static void test_extras_stripped(void)
{
    write_file(TMP,
               "requests[security]==2.31.0\n"
               "uvicorn[standard]==0.29.0\n"
               "passlib[bcrypt]==1.7.4\n");

    const Registry *pypi = registry_find("pypi");
    PackageList    *list = pypi->parse_manifest(pypi, TMP);

    assert(list != NULL);
    assert(list->count == 3);
    assert(strcmp(list->items[0]->name,    "requests") == 0);
    assert(strcmp(list->items[0]->version, "2.31.0")   == 0);
    assert(strcmp(list->items[1]->name,    "uvicorn")  == 0);
    assert(strcmp(list->items[1]->version, "0.29.0")   == 0);
    assert(strcmp(list->items[2]->name,    "passlib")  == 0);
    assert(strcmp(list->items[2]->version, "1.7.4")    == 0);

    package_list_destroy(list);
    cleanup();
}

static void test_inline_comment(void)
{
    write_file(TMP, "requests==2.31.0  # the HTTP library\n");

    const Registry *pypi = registry_find("pypi");
    PackageList    *list = pypi->parse_manifest(pypi, TMP);

    assert(list != NULL);
    assert(list->count == 1);
    assert(strcmp(list->items[0]->name,    "requests") == 0);
    assert(strcmp(list->items[0]->version, "2.31.0")   == 0);

    package_list_destroy(list);
    cleanup();
}

static void test_skip_dash_lines(void)
{
    /* Lines starting with '-' (e.g. -r, -c) are skipped with a warning. */
    write_file(TMP,
               "requests==2.31.0\n"
               "-r other-requirements.txt\n"
               "-c constraints.txt\n"
               "flask\n");

    const Registry *pypi = registry_find("pypi");
    PackageList    *list = pypi->parse_manifest(pypi, TMP);

    assert(list != NULL);
    assert(list->count == 2);
    assert(strcmp(list->items[0]->name, "requests") == 0);
    assert(strcmp(list->items[1]->name, "flask")    == 0);

    package_list_destroy(list);
    cleanup();
}

static void test_broader_specifiers_are_unpinned(void)
{
    /* >=, ~=, != etc. yield a package with no version (treated as latest). */
    write_file(TMP,
               "typing-extensions>=4.0\n"
               "packaging>=23.0\n"
               "setuptools~=65.0\n"
               "wheel!=0.40.0\n");

    const Registry *pypi = registry_find("pypi");
    PackageList    *list = pypi->parse_manifest(pypi, TMP);

    assert(list != NULL);
    assert(list->count == 4);
    assert(strcmp(list->items[0]->name, "typing-extensions") == 0);
    assert(strcmp(list->items[1]->name, "packaging")         == 0);
    assert(strcmp(list->items[2]->name, "setuptools")        == 0);
    assert(strcmp(list->items[3]->name, "wheel")             == 0);
    for (size_t i = 0; i < list->count; i++)
        assert(list->items[i]->version == NULL);

    package_list_destroy(list);
    cleanup();
}

static void test_environment_markers_stripped(void)
{
    /* '; marker' after a pinned version must be stripped from the version. */
    write_file(TMP,
               "pywin32==306 ; sys_platform == \"win32\"\n"
               "requests==2.31.0 ; python_version >= \"3.8\"\n");

    const Registry *pypi = registry_find("pypi");
    PackageList    *list = pypi->parse_manifest(pypi, TMP);

    assert(list != NULL);
    assert(list->count == 2);
    assert(strcmp(list->items[0]->name,    "pywin32")  == 0);
    assert(strcmp(list->items[0]->version, "306")      == 0);
    assert(strcmp(list->items[1]->name,    "requests") == 0);
    assert(strcmp(list->items[1]->version, "2.31.0")   == 0);

    package_list_destroy(list);
    cleanup();
}

static void test_missing_file(void)
{
    const Registry *pypi = registry_find("pypi");
    PackageList    *list = pypi->parse_manifest(pypi,
                               "/nonexistent/path/requirements.txt");
    if (list != NULL) {
        fprintf(stderr, "FAIL: expected NULL for missing file\n");
        package_list_destroy(list);
        abort();
    }
}

static void test_fixture_file(void)
{
    /*
     * The fixture is a realistic Django/FastAPI web-app requirements.txt with
     * 24 packages after skipping the -r line:
     *   - extras stripped (uvicorn[standard], passlib[bcrypt])
     *   - broader specifiers (typing-extensions>=4.0, packaging>=23.0) → no version
     *   - environment marker stripped (pywin32==306 ; sys_platform...)
     *   - -r constraints.txt line skipped
     */
    const Registry *pypi = registry_find("pypi");
    PackageList    *list = pypi->parse_manifest(pypi, "fixtures/requirements.txt");

    assert(list != NULL);
    assert(list->count == 24);

    /* First entry. */
    assert(strcmp(list->items[0]->name,    "Django") == 0);
    assert(strcmp(list->items[0]->version, "4.2.11") == 0);

    /* Extras stripped on uvicorn (index 3). */
    assert(strcmp(list->items[3]->name,    "uvicorn") == 0);
    assert(strcmp(list->items[3]->version, "0.29.0")  == 0);

    /* certifi is unpinned (index 9). */
    assert(strcmp(list->items[9]->name, "certifi") == 0);
    assert(list->items[9]->version == NULL);

    /* Extras stripped on passlib (index 14). */
    assert(strcmp(list->items[14]->name,    "passlib") == 0);
    assert(strcmp(list->items[14]->version, "1.7.4")   == 0);

    /* typing-extensions with >= → no version (index 19). */
    assert(strcmp(list->items[19]->name, "typing-extensions") == 0);
    assert(list->items[19]->version == NULL);

    /* pywin32 with == and marker → version "306" (index 21). */
    assert(strcmp(list->items[21]->name,    "pywin32") == 0);
    assert(strcmp(list->items[21]->version, "306")     == 0);

    /* Last entry: pytest-asyncio (index 23). */
    assert(strcmp(list->items[23]->name,    "pytest-asyncio") == 0);
    assert(strcmp(list->items[23]->version, "0.23.5")         == 0);

    package_list_destroy(list);
}

int main(void)
{
    test_basic_parse();
    test_empty_file();
    test_extras_stripped();
    test_inline_comment();
    test_skip_dash_lines();
    test_broader_specifiers_are_unpinned();
    test_environment_markers_stripped();
    test_missing_file();
    test_fixture_file();
    return 0;
}
