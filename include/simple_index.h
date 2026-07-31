/*
 * simple_index.h — PEP 503 "simple repository API" parsing.
 *
 * The JSON API (/pypi/<name>/json) is a pypi.org extension.  Every private
 * index — JFrog Artifactory, Sonatype Nexus, devpi, GitLab, a plain directory
 * behind nginx — instead speaks the simple API: one HTML page per project
 * whose anchors are the downloadable files.
 *
 *   <a href="../../packages/de/ad/foo-1.2-py3-none-any.whl#sha256=ab…"
 *      data-requires-python="&gt;=3.8"
 *      data-core-metadata="sha256=cd…">foo-1.2-py3-none-any.whl</a>
 *
 * What that page does NOT carry is dependency metadata, which is why the pypi
 * backend needs PEP 658 (data-core-metadata) or a wheel to open before it can
 * follow a package's requirements.  See pypi_metadata.h.
 *
 * The parser is deliberately lenient about the surrounding HTML — indexes emit
 * everything from hand-written pages to Jinja output — and strict about what
 * it takes from it: an anchor without a usable href is skipped, never guessed
 * at.
 */

#ifndef PACKMULE_SIMPLE_INDEX_H
#define PACKMULE_SIMPLE_INDEX_H

#include "hash.h"

#include <stddef.h>

typedef struct {
    char  *filename;         /* basename of the distribution file */
    char  *url;              /* absolute, fragment stripped */
    Digest digest;           /* from the URL fragment; unset if absent */
    char  *requires_python;  /* data-requires-python, or NULL */
    char  *metadata_url;     /* "<url>.metadata" when PEP 658/714 advertises
                              * it, else NULL */
    Digest metadata_digest;  /* digest of that .metadata file, when given */
    int    yanked;           /* PEP 592 data-yanked present */
} SimpleFile;

typedef struct {
    SimpleFile *items;
    size_t      count;
} SimpleFileList;

/*
 * simple_index_parse — extract every distribution anchor from `html`.
 *
 * `page_url` is the absolute URL the page was fetched from; relative hrefs are
 * resolved against it.
 *
 * Returns a list (possibly empty) that the caller frees with
 * simple_index_free().  Returns NULL only when `html` or `page_url` is NULL.
 */
SimpleFileList *simple_index_parse(const char *html, const char *page_url);

void simple_index_free(SimpleFileList *list);

/*
 * simple_normalize_name — PEP 503 name normalisation: lowercase, and every run
 * of '-', '_' or '.' collapsed to a single '-'.
 * Returns a heap string; caller frees with pm_free().
 */
char *simple_normalize_name(const char *name);

/*
 * simple_file_version — the version encoded in a distribution filename, given
 * the project it belongs to.
 *
 * Wheels are unambiguous (PEP 427 fixes the field order).  Source
 * distributions are not — "zope.interface-5.4.0.tar.gz" and
 * "foo-bar-1.0.tar.gz" both contain '-' in places that matter — so the project
 * name is normalised away from the front first and whatever remains is the
 * version.
 *
 * Returns a heap string, or NULL when the filename does not belong to
 * `project` or has no recognised distribution extension.
 */
char *simple_file_version(const char *filename, const char *project);

/*
 * simple_url_join — resolve `ref` against absolute `base` (RFC 3986 §5.2,
 * restricted to the http/https forms an index can produce).
 *
 * Returns a heap string; caller frees with pm_free().  Returns NULL when
 * `base` is not absolute.
 */
char *simple_url_join(const char *base, const char *ref);

#endif /* PACKMULE_SIMPLE_INDEX_H */
