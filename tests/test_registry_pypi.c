/*
 * test_registry_pypi.c — unit tests for the PyPI registry backend.
 *
 * parse_manifest tests: no network calls required.
 * get_deps tests: exercise extras filtering, name parsing, and dedup logic.
 * pypi_parse_response tests: wheel selection against inline JSON.
 */
#include "registry.h"
#include "registry_internal.h"
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

static void test_option_lines(void)
{
    /* -c is skipped with a warning; -r includes are followed so a split
     * requirements set still yields a complete bundle. */
    write_file("test_pypi_inc.txt", "flask==3.0.0\n");
    write_file(TMP,
               "requests==2.31.0\n"
               "-r test_pypi_inc.txt\n"
               "-c constraints.txt\n");

    const Registry *pypi = registry_find("pypi");
    PackageList    *list = pypi->parse_manifest(pypi, TMP);

    assert(list != NULL);
    assert(list->count == 2);
    assert(strcmp(list->items[0]->name,    "requests") == 0);
    assert(strcmp(list->items[1]->name,    "flask")    == 0);
    assert(strcmp(list->items[1]->version, "3.0.0")    == 0);

    package_list_destroy(list);
    remove("test_pypi_inc.txt");

    /* A missing -r include must fail the parse: silently dropping it would
     * ship an incomplete bundle. */
    write_file(TMP, "-r no-such-file.txt\n");
    list = pypi->parse_manifest(pypi, TMP);
    assert(list == NULL);
    cleanup();
}

static void test_direct_url_requirement_rejected(void)
{
    /* "pkg @ https://…" cannot be verified or pinned; parsing must fail
     * rather than silently resolve the name to a different PyPI artifact. */
    write_file(TMP, "mylib @ https://example.com/mylib-1.0-py3-none-any.whl\n");

    const Registry *pypi = registry_find("pypi");
    PackageList    *list = pypi->parse_manifest(pypi, TMP);

    assert(list == NULL);
    cleanup();
}

static void test_broader_specifiers_become_constraints(void)
{
    /* >=, ~=, != etc. yield an unpinned package carrying the PEP 440
     * constraint for resolve() to honour. */
    write_file(TMP,
               "typing-extensions>=4.0\n"
               "packaging >= 23.0, < 25\n"
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
    assert(strcmp(list->items[0]->constraint, ">=4.0")      == 0);
    assert(strcmp(list->items[1]->constraint, ">=23.0,<25") == 0);
    assert(strcmp(list->items[2]->constraint, "~=65.0")     == 0);
    assert(strcmp(list->items[3]->constraint, "!=0.40.0")   == 0);

    package_list_destroy(list);
    cleanup();
}

static void test_extras_recorded(void)
{
    /* Extras are recorded (lowercased) so their gated deps are followed. */
    write_file(TMP,
               "uvicorn[standard]==0.29.0\n"
               "passlib[Bcrypt, argon2]==1.7.4\n"
               "requests==2.31.0\n");

    const Registry *pypi = registry_find("pypi");
    PackageList    *list = pypi->parse_manifest(pypi, TMP);

    assert(list != NULL);
    assert(list->count == 3);
    assert(strcmp(list->items[0]->extras, "standard")      == 0);
    assert(strcmp(list->items[1]->extras, "bcrypt,argon2") == 0);
    assert(list->items[2]->extras == NULL);

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
     * 24 packages after skipping the -c line:
     *   - extras recorded (uvicorn[standard], passlib[bcrypt])
     *   - broader specifiers (typing-extensions>=4.0) → constraint, no version
     *   - environment marker stripped (pywin32==306 ; sys_platform...)
     *   - -c constraints.txt line skipped with a warning
     */
    const Registry *pypi = registry_find("pypi");
    PackageList    *list = pypi->parse_manifest(pypi, "fixtures/requirements.txt");

    assert(list != NULL);
    assert(list->count == 24);

    /* First entry. */
    assert(strcmp(list->items[0]->name,    "Django") == 0);
    assert(strcmp(list->items[0]->version, "4.2.11") == 0);

    /* Extras recorded on uvicorn (index 3). */
    assert(strcmp(list->items[3]->name,    "uvicorn")  == 0);
    assert(strcmp(list->items[3]->version, "0.29.0")   == 0);
    assert(strcmp(list->items[3]->extras,  "standard") == 0);

    /* certifi is unpinned (index 9). */
    assert(strcmp(list->items[9]->name, "certifi") == 0);
    assert(list->items[9]->version == NULL);

    /* Extras stripped on passlib (index 14). */
    assert(strcmp(list->items[14]->name,    "passlib") == 0);
    assert(strcmp(list->items[14]->version, "1.7.4")   == 0);

    /* typing-extensions with >= → constraint, no version (index 19). */
    assert(strcmp(list->items[19]->name, "typing-extensions") == 0);
    assert(list->items[19]->version == NULL);
    assert(strcmp(list->items[19]->constraint, ">=4.0") == 0);

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

static void test_get_deps_admits_requested_extras(void)
{
    /* When the parent was requested with an extra, deps gated on that extra
     * are followed; other extras stay excluded. */
    const char *specs[] = {
        "certifi>=2017.4.17",
        "cryptography>=1.3.4; extra == 'security'",
        "pysocks!=1.5.7; extra == 'socks'",
    };
    Package *pkg = make_resolved("requests", specs, 3);
    pkg->extras  = strdup("security");
    const Registry *pypi = registry_find("pypi");
    PackageList *seen = package_list_create();
    PackageList *out  = package_list_create();

    int added = pypi->get_deps(pypi, pkg, seen, out);

    assert(added == 2);
    assert(strcmp(out->items[0]->name, "certifi")      == 0);
    assert(strcmp(out->items[1]->name, "cryptography") == 0);

    package_destroy(pkg);
    package_list_destroy(seen);
    package_list_destroy(out);
}

static void test_get_deps_records_constraints(void)
{
    /* Range specifiers on deps are carried as constraints, and two
     * dependents' ranges on the same unresolved dep are intersected. */
    const char *specs[] = { "urllib3<3,>=1.21.1" };
    Package *pkg = make_resolved("requests", specs, 1);
    const Registry *pypi = registry_find("pypi");
    PackageList *queue = package_list_create();

    pypi->get_deps(pypi, pkg, queue, queue);
    assert(queue->count == 1);
    assert(queue->items[0]->version == NULL);
    assert(strcmp(queue->items[0]->constraint, "<3,>=1.21.1") == 0);

    /* Second dependent narrows the range. */
    const char *specs2[] = { "urllib3>=2.0" };
    Package *pkg2 = make_resolved("other", specs2, 1);
    pypi->get_deps(pypi, pkg2, queue, queue);
    assert(queue->count == 1);
    assert(strcmp(queue->items[0]->constraint, "<3,>=1.21.1,>=2.0") == 0);

    package_destroy(pkg);
    package_destroy(pkg2);
    package_list_destroy(queue);
}

static void test_get_deps_sdist_bundles_build_tools(void)
{
    /* A package resolved to an sdist pulls setuptools + wheel into the
     * bundle so the air-gapped machine can build it. */
    Package *pkg  = make_resolved("legacylib", NULL, 0);
    pkg->filename = strdup("legacylib-1.0.tar.gz");
    const Registry *pypi = registry_find("pypi");
    PackageList *queue = package_list_create();

    int added = pypi->get_deps(pypi, pkg, queue, queue);

    assert(added == 2);
    assert(strcmp(queue->items[0]->name, "setuptools") == 0);
    assert(strcmp(queue->items[1]->name, "wheel")      == 0);

    /* Idempotent: a second sdist doesn't add them again. */
    Package *pkg2  = make_resolved("otherlib", NULL, 0);
    pkg2->filename = strdup("otherlib-2.0.zip");
    assert(pypi->get_deps(pypi, pkg2, queue, queue) == 0);
    assert(queue->count == 2);

    package_destroy(pkg);
    package_destroy(pkg2);
    package_list_destroy(queue);
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

/* ── pypi_parse_response: wheel selection ────────────────────────────────── */

/* Minimal PyPI JSON with a musllinux wheel, a manylinux wheel, and an sdist. */
static const char *PYPI_JSON_ALL =
    "{"
    "  \"info\": { \"version\": \"1.0.0\" },"
    "  \"urls\": ["
    "    { \"filename\": \"demo-1.0.0-cp312-cp312-musllinux_1_2_x86_64.whl\","
    "      \"url\": \"https://files.pythonhosted.org/musl.whl\","
    "      \"digests\": { \"sha256\": \"musl111\" } },"
    "    { \"filename\": \"demo-1.0.0-cp312-cp312-manylinux_2_17_x86_64.whl\","
    "      \"url\": \"https://files.pythonhosted.org/many.whl\","
    "      \"digests\": { \"sha256\": \"many222\" } },"
    "    { \"filename\": \"demo-1.0.0.tar.gz\","
    "      \"url\": \"https://files.pythonhosted.org/demo.tar.gz\","
    "      \"digests\": { \"sha256\": \"sdist333\" } }"
    "  ]"
    "}";

static void test_parse_response_prefers_manylinux(void)
{
    Package *pkg = package_create("demo", "1.0.0");
    int rc = pypi_parse_response(PYPI_JSON_ALL, pkg, "x86_64", "linux", 12);

    assert(rc == 0);
    assert(strcmp(pkg->filename,
                  "demo-1.0.0-cp312-cp312-manylinux_2_17_x86_64.whl") == 0);
    assert(strcmp(pkg->sha256, "many222") == 0);

    package_destroy(pkg);
}

static void test_parse_response_prefers_lowest_glibc(void)
{
    /* Among matching manylinux wheels the LOWEST glibc floor wins: a
     * manylinux_2_17 wheel installs on old and new targets alike, while a
     * manylinux_2_39 one is refused by anything older (e.g. RHEL 8). */
    const char *json =
        "{"
        "  \"info\": { \"version\": \"1.0.0\" },"
        "  \"urls\": ["
        "    { \"filename\": \"demo-1.0.0-cp312-cp312-manylinux_2_39_x86_64.whl\","
        "      \"url\": \"https://files.pythonhosted.org/new.whl\","
        "      \"digests\": { \"sha256\": \"glibc239\" } },"
        "    { \"filename\": \"demo-1.0.0-cp312-cp312-manylinux_2_17_x86_64.whl\","
        "      \"url\": \"https://files.pythonhosted.org/old.whl\","
        "      \"digests\": { \"sha256\": \"glibc217\" } }"
        "  ]"
        "}";

    Package *pkg = package_create("demo", "1.0.0");
    int rc = pypi_parse_response(json, pkg, "x86_64", "linux", 12);

    assert(rc == 0);
    assert(strcmp(pkg->sha256, "glibc217") == 0);

    package_destroy(pkg);
}

static void test_parse_response_windows_amd64(void)
{
    /* Windows spells x86_64 as "amd64"; -s windows -a x86_64 must match. */
    const char *json =
        "{"
        "  \"info\": { \"version\": \"1.0.0\" },"
        "  \"urls\": ["
        "    { \"filename\": \"demo-1.0.0-cp312-cp312-win_amd64.whl\","
        "      \"url\": \"https://files.pythonhosted.org/win.whl\","
        "      \"digests\": { \"sha256\": \"win111\" } },"
        "    { \"filename\": \"demo-1.0.0.tar.gz\","
        "      \"url\": \"https://files.pythonhosted.org/demo.tar.gz\","
        "      \"digests\": { \"sha256\": \"sdist333\" } }"
        "  ]"
        "}";

    Package *pkg = package_create("demo", "1.0.0");
    int rc = pypi_parse_response(json, pkg, "x86_64", "windows", 12);

    assert(rc == 0);
    assert(strcmp(pkg->sha256, "win111") == 0);

    package_destroy(pkg);
}

static void test_parse_response_skips_yanked(void)
{
    /* A yanked file must never be selected, even when it matches best. */
    const char *json =
        "{"
        "  \"info\": { \"version\": \"1.0.0\" },"
        "  \"urls\": ["
        "    { \"filename\": \"demo-1.0.0-py3-none-any.whl\","
        "      \"url\": \"https://files.pythonhosted.org/yanked.whl\","
        "      \"yanked\": true,"
        "      \"digests\": { \"sha256\": \"yank111\" } },"
        "    { \"filename\": \"demo-1.0.0.tar.gz\","
        "      \"url\": \"https://files.pythonhosted.org/demo.tar.gz\","
        "      \"digests\": { \"sha256\": \"sdist333\" } }"
        "  ]"
        "}";

    Package *pkg = package_create("demo", "1.0.0");
    int rc = pypi_parse_response(json, pkg, "x86_64", "linux", 12);

    assert(rc == 0);
    assert(strcmp(pkg->sha256, "sdist333") == 0);

    package_destroy(pkg);
}

static void test_parse_response_rejects_musllinux(void)
{
    /* Only the musllinux wheel and the sdist: a glibc linux target must fall
     * back to the sdist — pip on glibc refuses musllinux wheels. */
    const char *json =
        "{"
        "  \"info\": { \"version\": \"1.0.0\" },"
        "  \"urls\": ["
        "    { \"filename\": \"demo-1.0.0-cp312-cp312-musllinux_1_2_x86_64.whl\","
        "      \"url\": \"https://files.pythonhosted.org/musl.whl\","
        "      \"digests\": { \"sha256\": \"musl111\" } },"
        "    { \"filename\": \"demo-1.0.0.tar.gz\","
        "      \"url\": \"https://files.pythonhosted.org/demo.tar.gz\","
        "      \"digests\": { \"sha256\": \"sdist333\" } }"
        "  ]"
        "}";

    Package *pkg = package_create("demo", "1.0.0");
    int rc = pypi_parse_response(json, pkg, "x86_64", "linux", 12);

    assert(rc == 0);
    assert(strcmp(pkg->filename, "demo-1.0.0.tar.gz") == 0);
    assert(strcmp(pkg->sha256,   "sdist333")          == 0);

    package_destroy(pkg);
}

static void test_parse_response_sanitizes_filename(void)
{
    /* A registry-supplied filename with path components must be reduced to
     * its basename so it cannot escape the output directory. */
    const char *json =
        "{"
        "  \"info\": { \"version\": \"1.0.0\" },"
        "  \"urls\": ["
        "    { \"filename\": \"a/../../evil-1.0.0-py3-none-any.whl\","
        "      \"url\": \"https://files.pythonhosted.org/evil.whl\","
        "      \"digests\": { \"sha256\": \"evil444\" } }"
        "  ]"
        "}";

    Package *pkg = package_create("evil", "1.0.0");
    int rc = pypi_parse_response(json, pkg, "x86_64", "linux", 12);

    assert(rc == 0);
    assert(strcmp(pkg->filename, "evil-1.0.0-py3-none-any.whl") == 0);

    package_destroy(pkg);
}

int main(void)
{
    test_basic_parse();
    test_empty_file();
    test_extras_stripped();
    test_inline_comment();
    test_option_lines();
    test_direct_url_requirement_rejected();
    test_broader_specifiers_become_constraints();
    test_extras_recorded();
    test_environment_markers_stripped();
    test_missing_file();
    test_fixture_file();
    test_get_deps_filters_extras();
    test_get_deps_admits_requested_extras();
    test_get_deps_records_constraints();
    test_get_deps_sdist_bundles_build_tools();
    test_get_deps_name_parsing();
    test_get_deps_dedup();
    test_get_deps_null_dep_specs();
    test_parse_response_prefers_manylinux();
    test_parse_response_prefers_lowest_glibc();
    test_parse_response_windows_amd64();
    test_parse_response_skips_yanked();
    test_parse_response_rejects_musllinux();
    test_parse_response_sanitizes_filename();
    return 0;
}
