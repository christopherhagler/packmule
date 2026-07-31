/*
 * test_pypi_metadata.c — reading Requires-Dist out of a METADATA document and
 * out of a distribution archive.
 *
 * This is the path a private index without PEP 658 forces packmule down, so
 * getting it wrong means silently incomplete bundles.
 */
#include "pypi_metadata.h"
#include "utils.h"

#include <archive.h>
#include <archive_entry.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static size_t count_specs(char **specs)
{
    size_t n = 0;
    while (specs[n])
        n++;
    return n;
}

static int has_spec(char **specs, const char *want)
{
    for (char **s = specs; *s; s++)
        if (strcmp(*s, want) == 0)
            return 1;
    return 0;
}

static void test_header_parsing(void)
{
    static const char META[] =
        "Metadata-Version: 2.1\n"
        "Name: requests\n"
        "Version: 2.31.0\n"
        "Requires-Python: >=3.7\n"
        "Requires-Dist: charset-normalizer <4, >=2\n"
        "Requires-Dist: idna <4, >=2.5\n"
        "Provides-Extra: socks\n"
        "Requires-Dist: PySocks !=1.5.7, >=1.5.6 ; extra == 'socks'\n"
        "\n"
        "# Requests\n"
        "\n"
        "Install with pip.  Note that Requires-Dist: not-a-real-dep appears\n"
        "in this description and must not be picked up.\n";

    char **specs = pypi_metadata_requires(META);
    assert(specs != NULL);
    assert(count_specs(specs) == 3);
    assert(has_spec(specs, "charset-normalizer <4, >=2"));
    assert(has_spec(specs, "idna <4, >=2.5"));
    assert(has_spec(specs, "PySocks !=1.5.7, >=1.5.6 ; extra == 'socks'"));
    /* The blank line ends the headers; prose after it is not metadata. */
    assert(!has_spec(specs, "not-a-real-dep"));
    pypi_metadata_free_specs(specs);

    assert(pypi_metadata_has_requires_header(META));
}

static void test_no_dependencies(void)
{
    static const char META[] =
        "Metadata-Version: 2.1\nName: six\nVersion: 1.16.0\n\nDescription.\n";

    char **specs = pypi_metadata_requires(META);
    assert(specs != NULL);
    assert(count_specs(specs) == 0);
    pypi_metadata_free_specs(specs);

    /* No Requires-Dist at all: for a wheel that means "none", for an sdist it
     * means "unknown".  The caller needs to be able to tell. */
    assert(!pypi_metadata_has_requires_header(META));
}

static void test_crlf_and_folding(void)
{
    /* Indexes serve CRLF, and setuptools folds long marker expressions onto a
     * continuation line. */
    static const char META[] =
        "Metadata-Version: 2.1\r\n"
        "Name: pkg\r\n"
        "Requires-Dist: alpha >=1.0\r\n"
        "Requires-Dist: beta ;\r\n"
        "        python_version < \"3.9\"\r\n"
        "Requires-Dist: gamma\r\n"
        "\r\n"
        "Body.\r\n";

    char **specs = pypi_metadata_requires(META);
    assert(count_specs(specs) == 3);
    assert(has_spec(specs, "alpha >=1.0"));
    assert(has_spec(specs, "beta ; python_version < \"3.9\""));
    assert(has_spec(specs, "gamma"));
    pypi_metadata_free_specs(specs);
}

static void test_case_insensitive_header(void)
{
    static const char META[] = "Name: pkg\nrequires-dist: alpha\n\n";
    char **specs = pypi_metadata_requires(META);
    assert(count_specs(specs) == 1);
    assert(has_spec(specs, "alpha"));
    pypi_metadata_free_specs(specs);

    /* A header whose name merely starts with the same letters is not a match. */
    static const char OTHER[] = "Requires-Dist-Extra: alpha\nRequires-Python: >=3\n\n";
    specs = pypi_metadata_requires(OTHER);
    assert(count_specs(specs) == 0);
    pypi_metadata_free_specs(specs);
}

static void test_null_input(void)
{
    assert(pypi_metadata_requires(NULL) == NULL);
    assert(!pypi_metadata_has_requires_header(NULL));
    pypi_metadata_free_specs(NULL);          /* must not crash */
}

/* ── Archive extraction ──────────────────────────────────────────────────── */

/* Write a zip containing `entries` (NULL-terminated name/body pairs). */
static void write_zip(const char *path, const char *const *entries)
{
    struct archive *a = archive_write_new();
    assert(a != NULL);
    archive_write_set_format_zip(a);
    assert(archive_write_open_filename(a, path) == ARCHIVE_OK);

    for (const char *const *e = entries; *e; e += 2) {
        struct archive_entry *ent = archive_entry_new();
        archive_entry_set_pathname(ent, e[0]);
        archive_entry_set_size(ent, (la_int64_t)strlen(e[1]));
        archive_entry_set_filetype(ent, AE_IFREG);
        archive_entry_set_perm(ent, 0644);
        assert(archive_write_header(a, ent) == ARCHIVE_OK);
        archive_write_data(a, e[1], strlen(e[1]));
        archive_entry_free(ent);
    }

    archive_write_close(a);
    archive_write_free(a);
}

static void test_wheel_extraction(void)
{
    char tmpl[] = "/tmp/pm-meta-test-XXXXXX";
    assert(mkdtemp(tmpl) != NULL);

    char *whl = pm_asprintf("%s/widget-1.2.0-py3-none-any.whl", tmpl);

    /* A decoy METADATA outside .dist-info must lose to the real one, and a
     * package payload file must not be mistaken for metadata. */
    static const char *const ENTRIES[] = {
        "widget/METADATA",                "Requires-Dist: decoy\n\n",
        "widget/__init__.py",             "\n",
        "widget-1.2.0.dist-info/WHEEL",   "Wheel-Version: 1.0\n",
        "widget-1.2.0.dist-info/METADATA",
            "Metadata-Version: 2.1\nName: widget\nRequires-Dist: gadget >=0.5\n\nDesc\n",
        NULL, NULL
    };
    write_zip(whl, ENTRIES);

    char *text = pypi_metadata_from_archive(whl);
    assert(text != NULL);

    char **specs = pypi_metadata_requires(text);
    assert(count_specs(specs) == 1);
    assert(has_spec(specs, "gadget >=0.5"));
    assert(!has_spec(specs, "decoy"));
    pypi_metadata_free_specs(specs);
    pm_free(text);

    remove(whl);
    pm_free(whl);
    rmdir(tmpl);
}

static void test_sdist_extraction(void)
{
    char tmpl[] = "/tmp/pm-meta-test-XXXXXX";
    assert(mkdtemp(tmpl) != NULL);

    char *sdist = pm_asprintf("%s/widget-1.2.0.zip", tmpl);

    /* The PKG-INFO at the root of the tree is the real one; a vendored copy
     * deeper in the tree belongs to something else. */
    static const char *const ENTRIES[] = {
        "widget-1.2.0/vendor/other-0.1/PKG-INFO",
            "Name: other\nRequires-Dist: wrong\n\n",
        "widget-1.2.0/PKG-INFO",
            "Metadata-Version: 2.1\nName: widget\nRequires-Dist: gadget >=0.5\n\n",
        "widget-1.2.0/setup.py", "\n",
        NULL, NULL
    };
    write_zip(sdist, ENTRIES);

    char *text = pypi_metadata_from_archive(sdist);
    assert(text != NULL);

    char **specs = pypi_metadata_requires(text);
    assert(count_specs(specs) == 1);
    assert(has_spec(specs, "gadget >=0.5"));
    pypi_metadata_free_specs(specs);
    pm_free(text);

    remove(sdist);
    pm_free(sdist);
    rmdir(tmpl);
}

static void test_archive_without_metadata(void)
{
    char tmpl[] = "/tmp/pm-meta-test-XXXXXX";
    assert(mkdtemp(tmpl) != NULL);

    char *whl = pm_asprintf("%s/empty-1.0-py3-none-any.whl", tmpl);
    static const char *const ENTRIES[] = { "empty/__init__.py", "\n", NULL, NULL };
    write_zip(whl, ENTRIES);

    assert(pypi_metadata_from_archive(whl) == NULL);

    remove(whl);
    pm_free(whl);
    rmdir(tmpl);

    /* A file that is not an archive at all must fail, not crash. */
    assert(pypi_metadata_from_archive("/nonexistent/nope.whl") == NULL);
}

int main(void)
{
    test_header_parsing();
    test_no_dependencies();
    test_crlf_and_folding();
    test_case_insensitive_header();
    test_null_input();
    test_wheel_extraction();
    test_sdist_extraction();
    test_archive_without_metadata();

    printf("test_pypi_metadata: all tests passed\n");
    return 0;
}
