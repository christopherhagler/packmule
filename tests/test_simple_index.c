/*
 * test_simple_index.c — PEP 503 simple-repository page parsing.
 *
 * The inputs here are shaped after what real indexes actually serve: pypi.org
 * (absolute hrefs on a separate file host), JFrog Artifactory (relative hrefs
 * back into the same repository), and a bare directory listing.
 */
#include "simple_index.h"
#include "utils.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ── URL resolution ──────────────────────────────────────────────────────── */

static void join_is(const char *base, const char *ref, const char *want)
{
    char *got = simple_url_join(base, ref);
    if (!want) {
        assert(got == NULL);
        return;
    }
    assert(got != NULL);
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "join(%s, %s) = %s, want %s\n", base, ref, got, want);
        assert(0);
    }
    pm_free(got);
}

static void test_url_join(void)
{
    const char *page = "https://art.corp/artifactory/api/pypi/pypi/simple/foo/";

    /* The form Artifactory and pypi.org both emit. */
    join_is(page, "../../packages/de/ad/foo-1.0.whl",
            "https://art.corp/artifactory/api/pypi/pypi/packages/de/ad/foo-1.0.whl");

    join_is(page, "foo-1.0.whl",
            "https://art.corp/artifactory/api/pypi/pypi/simple/foo/foo-1.0.whl");
    join_is(page, "./foo-1.0.whl",
            "https://art.corp/artifactory/api/pypi/pypi/simple/foo/foo-1.0.whl");
    join_is(page, "/packages/foo-1.0.whl",
            "https://art.corp/packages/foo-1.0.whl");
    join_is(page, "//cdn.corp/packages/foo-1.0.whl",
            "https://cdn.corp/packages/foo-1.0.whl");
    join_is(page, "https://files.pythonhosted.org/x/foo-1.0.whl",
            "https://files.pythonhosted.org/x/foo-1.0.whl");

    /* A page URL without the conventional trailing slash: the last segment is
     * the document, not a directory. */
    join_is("https://art.corp/simple/foo", "foo-1.0.whl",
            "https://art.corp/simple/foo-1.0.whl");

    /* Climbing past the root cannot escape it. */
    join_is("https://art.corp/a/", "../../../etc/passwd",
            "https://art.corp/etc/passwd");

    /* Query strings survive resolution (pre-signed URLs depend on it). */
    join_is(page, "../../packages/foo-1.0.whl?token=abc",
            "https://art.corp/artifactory/api/pypi/pypi/packages/foo-1.0.whl?token=abc");

    join_is("https://art.corp", "foo-1.0.whl", "https://art.corp/foo-1.0.whl");
    join_is("not-a-url", "foo.whl", NULL);
}

/* ── Name normalisation and version extraction ───────────────────────────── */

static void norm_is(const char *in, const char *want)
{
    char *got = simple_normalize_name(in);
    assert(strcmp(got, want) == 0);
    pm_free(got);
}

static void test_normalize(void)
{
    norm_is("Foo",             "foo");
    norm_is("zope.interface",  "zope-interface");
    norm_is("zope_interface",  "zope-interface");
    norm_is("ruamel.yaml.clib", "ruamel-yaml-clib");
    norm_is("A__B",            "a-b");
    norm_is("typing-extensions", "typing-extensions");
}

static void version_is(const char *filename, const char *project,
                       const char *want)
{
    char *got = simple_file_version(filename, project);
    if (!want) {
        if (got) {
            fprintf(stderr, "version(%s) = %s, want NULL\n", filename, got);
            assert(0);
        }
        return;
    }
    assert(got != NULL);
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "version(%s) = %s, want %s\n", filename, got, want);
        assert(0);
    }
    pm_free(got);
}

static void test_file_version(void)
{
    version_is("requests-2.31.0-py3-none-any.whl", "requests", "2.31.0");
    version_is("numpy-1.26.4-cp312-cp312-manylinux_2_17_x86_64.whl",
               "numpy", "1.26.4");
    /* A build tag adds a field but does not move the version. */
    version_is("foo-1.0-1-py3-none-any.whl", "foo", "1.0");
    /* Wheels normalise '-' in the project name to '_'. */
    version_is("typing_extensions-4.12.2-py3-none-any.whl",
               "typing-extensions", "4.12.2");

    /* sdists: the project name has to be stripped, because it can contain the
     * very separators the version is delimited by. */
    version_is("requests-2.31.0.tar.gz",        "requests",       "2.31.0");
    version_is("zope.interface-5.4.0.tar.gz",   "zope.interface", "5.4.0");
    version_is("zope_interface-5.4.0.tar.gz",   "zope.interface", "5.4.0");
    version_is("ruamel.yaml.clib-0.2.8.tar.gz", "ruamel.yaml.clib", "0.2.8");
    version_is("backports.zoneinfo-0.2.1.tar.gz", "backports.zoneinfo", "0.2.1");
    version_is("legacy-1.0.zip",                "legacy",         "1.0");
    version_is("foo-1.0.tar.bz2",               "foo",            "1.0");

    /* Prerelease and local versions come through whole. */
    version_is("foo-2.0.0rc1.tar.gz",     "foo", "2.0.0rc1");
    version_is("foo-1.0+local.1.tar.gz",  "foo", "1.0+local.1");

    version_is("README.txt",              "foo", NULL);
    version_is("otherpkg-1.0.tar.gz",     "foo", NULL);
}

/* ── Page parsing ────────────────────────────────────────────────────────── */

static const SimpleFile *find_file(const SimpleFileList *l, const char *name)
{
    for (size_t i = 0; i < l->count; i++)
        if (strcmp(l->items[i].filename, name) == 0)
            return &l->items[i];
    return NULL;
}

static void test_parse_artifactory_page(void)
{
    /* Relative hrefs, escaped data-requires-python, PEP 714 metadata
     * attribute, a yanked file, and a navigation link to ignore. */
    static const char HTML[] =
        "<!DOCTYPE html><html><head><title>Links for foo</title></head><body>\n"
        "<h1>Links for foo</h1>\n"
        "<a href=\"/artifactory/api/pypi/pypi/simple/\">Back to index</a><br/>\n"
        "<a href=\"../../packages/de/ad/foo-1.0-py3-none-any.whl#sha256=aaa111\"\n"
        "   data-requires-python=\"&gt;=3.8\"\n"
        "   data-core-metadata=\"sha256=bbb222\">foo-1.0-py3-none-any.whl</a><br/>\n"
        "<a href='../../packages/be/ef/foo-1.1.tar.gz#sha256=ccc333' "
        "data-yanked=\"broken build\">foo-1.1.tar.gz</a><br/>\n"
        "<a href=\"../../packages/00/11/foo-0.9-py3-none-any.whl\">"
        "foo-0.9-py3-none-any.whl</a><br/>\n"
        "</body></html>";

    SimpleFileList *l = simple_index_parse(
        HTML, "https://art.corp/artifactory/api/pypi/pypi/simple/foo/");
    assert(l != NULL);
    assert(l->count == 3);        /* "Back to index" is not a distribution */

    const SimpleFile *whl = find_file(l, "foo-1.0-py3-none-any.whl");
    assert(whl != NULL);
    assert(strcmp(whl->url,
                  "https://art.corp/artifactory/api/pypi/pypi/packages/de/ad/"
                  "foo-1.0-py3-none-any.whl") == 0);
    assert(whl->digest.algo == DIGEST_SHA256);
    assert(strcmp(whl->digest.value, "aaa111") == 0);
    assert(strcmp(whl->requires_python, ">=3.8") == 0);
    assert(!whl->yanked);
    assert(whl->metadata_url != NULL);
    assert(strcmp(whl->metadata_url,
                  "https://art.corp/artifactory/api/pypi/pypi/packages/de/ad/"
                  "foo-1.0-py3-none-any.whl.metadata") == 0);
    assert(whl->metadata_digest.algo == DIGEST_SHA256);
    assert(strcmp(whl->metadata_digest.value, "bbb222") == 0);

    const SimpleFile *sd = find_file(l, "foo-1.1.tar.gz");
    assert(sd != NULL);
    assert(sd->yanked);                    /* single-quoted href, too */
    assert(strcmp(sd->digest.value, "ccc333") == 0);
    assert(sd->metadata_url == NULL);

    const SimpleFile *old = find_file(l, "foo-0.9-py3-none-any.whl");
    assert(old != NULL);
    assert(!digest_is_set(&old->digest)); /* no fragment: caller must cope */
    assert(old->requires_python == NULL);

    simple_index_free(l);
}

static void test_parse_edge_cases(void)
{
    /* Absolute hrefs to a separate file host (pypi.org's own shape), an
     * unquoted attribute, uppercase tags, a bare data-yanked with no value,
     * and the legacy PEP 658 attribute name. */
    static const char HTML[] =
        "<A HREF=\"https://files.pythonhosted.org/packages/x/req-2.0.whl"
        "#sha256=deadbeef\" data-yanked>req-2.0.whl</A>\n"
        "<a href=https://files.pythonhosted.org/packages/y/req-1.0.tar.gz "
        "data-dist-info-metadata=true>req-1.0.tar.gz</a>\n"
        "<a href=\"https://files.pythonhosted.org/z/req-3.0.whl\"></a>\n";

    SimpleFileList *l = simple_index_parse(HTML, "https://pypi.org/simple/req/");
    assert(l != NULL);
    assert(l->count == 3);

    assert(l->items[0].yanked);
    assert(strcmp(l->items[0].digest.value, "deadbeef") == 0);
    /* The fragment is not part of the URL we fetch. */
    assert(strchr(l->items[0].url, '#') == NULL);

    assert(l->items[1].metadata_url != NULL);
    /* "true" advertises the file without pinning its digest. */
    assert(!digest_is_set(&l->items[1].metadata_digest));

    /* Empty anchor text falls back to the href's last path segment. */
    assert(strcmp(l->items[2].filename, "req-3.0.whl") == 0);

    simple_index_free(l);
}

static void test_parse_hostile_input(void)
{
    /* A filename with a directory component must never reach the caller: it
     * becomes the name of a file written into the output directory. */
    static const char HTML[] =
        "<a href=\"https://art.corp/p/evil.whl\">../../../etc/evil.whl</a>\n"
        "<a href=\"https://art.corp/p/../../../etc/passwd.whl\">passwd.whl</a>\n";

    SimpleFileList *l = simple_index_parse(HTML, "https://art.corp/simple/x/");
    assert(l != NULL);
    assert(l->count == 2);
    assert(strcmp(l->items[0].filename, "evil.whl") == 0);
    assert(strchr(l->items[0].filename, '/') == NULL);
    /* Dot segments in the href resolve away rather than being sent literally. */
    assert(strcmp(l->items[1].url, "https://art.corp/etc/passwd.whl") == 0);

    simple_index_free(l);
}

static void test_parse_degenerate(void)
{
    SimpleFileList *l;

    l = simple_index_parse("", "https://art.corp/simple/x/");
    assert(l && l->count == 0);
    simple_index_free(l);

    /* A 404 page served with a 200, which private indexes do. */
    l = simple_index_parse("<html><body>Not found</body></html>",
                           "https://art.corp/simple/x/");
    assert(l && l->count == 0);
    simple_index_free(l);

    /* An unterminated tag must not run off the end of the buffer. */
    l = simple_index_parse("<a href=\"foo-1.0.whl\"", "https://art.corp/s/x/");
    assert(l && l->count == 0);
    simple_index_free(l);

    assert(simple_index_parse(NULL, "https://art.corp/") == NULL);
    assert(simple_index_parse("<a/>", NULL) == NULL);
}

int main(void)
{
    test_url_join();
    test_normalize();
    test_file_version();
    test_parse_artifactory_page();
    test_parse_edge_cases();
    test_parse_hostile_input();
    test_parse_degenerate();

    printf("test_simple_index: all tests passed\n");
    return 0;
}
