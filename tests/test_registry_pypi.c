/*
 * test_registry_pypi.c — unit tests for the PyPI registry backend.
 *
 * parse_manifest tests: no network calls required.
 * get_deps tests: exercise extras filtering, name parsing, and dedup logic.
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

/* ── get_deps ────────────────────────────────────────────────────────────── */

/* Build a Package with dep_specs populated, as if resolve() had been called. */
static Package *make_resolved(const char *name, const char **specs, int n)
{
    Package *pkg = package_create(name, "1.0.0");
    if (n > 0) {
        pkg->dep_specs = malloc(((size_t)n + 1) * sizeof(char *));
        for (int i = 0; i < n; i++)
            pkg->dep_specs[i] = strdup(specs[i]);
        pkg->dep_specs[n] = NULL;
    }
    return pkg;
}

static void test_get_deps_filters_extras(void)
{
    /* Extras-gated entries must be silently skipped. */
    const char *specs[] = {
        "certifi>=2017.4.17",
        "cryptography>=1.3.4; extra == 'security'",
        "idna>=2.0.0; extra == 'security'",
        "charset-normalizer<4,>=2",
    };
    Package     *pkg  = make_resolved("requests", specs, 4);
    const Registry *pypi = registry_find("pypi");
    PackageList *seen = package_list_create();
    PackageList *out  = package_list_create();

    int added = pypi->get_deps(pypi, pkg, seen, out);

    assert(added == 2);
    assert(out->count == 2);
    assert(strcmp(out->items[0]->name, "certifi")            == 0);
    assert(strcmp(out->items[1]->name, "charset-normalizer") == 0);

    package_destroy(pkg);
    package_list_destroy(seen);
    package_list_destroy(out);
}

static void test_get_deps_name_parsing(void)
{
    /* PEP 508: version ranges, extras brackets, env markers all stripped;
     * result is lowercased. */
    const char *specs[] = {
        "Django>=3.2",
        "psycopg2-binary==2.9.9",
        "typing_extensions>=4.0; python_version < '3.11'",
    };
    Package     *pkg  = make_resolved("myapp", specs, 3);
    const Registry *pypi = registry_find("pypi");
    PackageList *seen = package_list_create();
    PackageList *out  = package_list_create();

    pypi->get_deps(pypi, pkg, seen, out);

    assert(out->count == 3);
    assert(strcmp(out->items[0]->name, "django")             == 0);
    assert(strcmp(out->items[1]->name, "psycopg2-binary")    == 0);
    assert(strcmp(out->items[2]->name, "typing_extensions")  == 0);

    package_destroy(pkg);
    package_list_destroy(seen);
    package_list_destroy(out);
}

static void test_get_deps_dedup(void)
{
    /* Deps already present in seen must not be added again. */
    const char *specs[] = {
        "certifi>=2017.4.17",
        "urllib3<3,>=1.21.1",
    };
    Package     *pkg  = make_resolved("requests", specs, 2);
    const Registry *pypi = registry_find("pypi");
    /* seen and out are the same list — the typical caller pattern. */
    PackageList *queue = package_list_create();
    package_list_add(queue, package_create("certifi", "2024.2.2"));

    int added = pypi->get_deps(pypi, pkg, queue, queue);

    assert(added == 1);
    assert(queue->count == 2); /* 1 pre-existing + 1 new */
    assert(strcmp(queue->items[1]->name, "urllib3") == 0);

    package_destroy(pkg);
    package_list_destroy(queue);
}

static void test_get_deps_null_dep_specs(void)
{
    /* Package with no dep_specs set → 0 added, no crash. */
    Package     *pkg  = package_create("flask", "3.0.0");
    const Registry *pypi = registry_find("pypi");
    PackageList *seen = package_list_create();
    PackageList *out  = package_list_create();

    int added = pypi->get_deps(pypi, pkg, seen, out);

    assert(added == 0);
    assert(out->count == 0);

    package_destroy(pkg);
    package_list_destroy(seen);
    package_list_destroy(out);
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
    test_get_deps_filters_extras();
    test_get_deps_name_parsing();
    test_get_deps_dedup();
    test_get_deps_null_dep_specs();
    return 0;
}
