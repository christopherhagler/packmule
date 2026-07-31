#include "pypi_metadata.h"
#include "utils.h"

#include <archive.h>
#include <archive_entry.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ── Header-block parsing ─────────────────────────────────────────────────── */

/*
 * A METADATA document is an RFC 822 header block followed by a blank line and
 * the long description.  Only the headers matter here, and stopping at the
 * blank line matters too: a README can easily contain a line that begins
 * "Requires-Dist:" inside a fenced code block.
 */

/* Length of the line starting at `p`, excluding the newline. */
static size_t line_len(const char *p)
{
    const char *e = strchr(p, '\n');
    size_t      n = e ? (size_t)(e - p) : strlen(p);
    if (n > 0 && p[n - 1] == '\r')
        n--;                       /* indexes serve CRLF often enough */
    return n;
}

static const char *next_line(const char *p)
{
    const char *e = strchr(p, '\n');
    return e ? e + 1 : p + strlen(p);
}

/* Case-insensitive "does this line start with `header`:" test. */
static const char *header_value(const char *line, size_t len, const char *header)
{
    size_t hl = strlen(header);
    if (len <= hl || strncasecmp(line, header, hl) != 0 || line[hl] != ':')
        return NULL;

    const char *v = line + hl + 1;
    while (*v == ' ' || *v == '\t')
        v++;
    return v;
}

int pypi_metadata_has_requires_header(const char *text)
{
    if (!text)
        return 0;

    for (const char *p = text; *p; p = next_line(p)) {
        size_t len = line_len(p);
        if (len == 0)
            break;                 /* end of the header block */
        if (header_value(p, len, "Requires-Dist"))
            return 1;
    }
    return 0;
}

char *pypi_metadata_license(const char *text)
{
    if (!text)
        return NULL;

    char *expr      = NULL;
    char *classifier = NULL;
    char *freetext  = NULL;

    for (const char *p = text; *p; p = next_line(p)) {
        size_t len = line_len(p);
        if (len == 0)
            break;                 /* headers end here */

        const char *v;
        if (!expr && (v = header_value(p, len, "License-Expression")) != NULL) {
            expr = pm_strndup(v, len - (size_t)(v - p));
        } else if (!freetext && (v = header_value(p, len, "License")) != NULL) {
            size_t vlen = len - (size_t)(v - p);
            if (vlen > 0 && vlen <= 64)
                freetext = pm_strndup(v, vlen);
        } else if ((v = header_value(p, len, "Classifier")) != NULL) {
            size_t vlen = len - (size_t)(v - p);
            if (vlen > 10 && strncmp(v, "License ::", 10) == 0) {
                char       *full = pm_strndup(v, vlen);
                const char *last = strrchr(full, ':');
                if (last && last[1]) {
                    const char *val = last + 1;
                    while (*val == ' ')
                        val++;
                    if (*val && strcmp(val, "OSI Approved") != 0) {
                        pm_free(classifier);
                        classifier = pm_strdup(val);
                    }
                }
                pm_free(full);
            }
        }
    }

    char *out = expr ? expr : (classifier ? classifier : freetext);
    if (out != expr)       pm_free(expr);
    if (out != classifier) pm_free(classifier);
    if (out != freetext)   pm_free(freetext);
    return out;
}

char **pypi_metadata_requires(const char *text)
{
    if (!text)
        return NULL;

    char **specs = NULL;
    size_t n = 0, cap = 0;

    for (const char *p = text; *p; p = next_line(p)) {
        size_t len = line_len(p);
        if (len == 0)
            break;                 /* blank line: the description starts here */

        const char *val = header_value(p, len, "Requires-Dist");
        if (!val)
            continue;

        size_t vlen = len - (size_t)(val - p);
        char  *spec = pm_strndup(val, vlen);

        /*
         * Fold continuation lines.  RFC 822 allows a header to run onto the
         * next line when that line starts with whitespace; setuptools emits
         * this for long marker expressions.
         */
        const char *q = next_line(p);
        while (*q == ' ' || *q == '\t') {
            size_t qlen = line_len(q);
            const char *cont = q;
            while (*cont == ' ' || *cont == '\t') {
                cont++;
                qlen--;
            }
            char *joined = pm_asprintf("%s %.*s", spec, (int)qlen, cont);
            pm_free(spec);
            spec = joined;
            p    = q;
            q    = next_line(q);
        }

        char *trimmed = pm_strtrim(spec);
        if (*trimmed) {
            if (n == cap) {
                cap   = cap ? cap * 2 : 8;
                specs = pm_realloc(specs, (cap + 1) * sizeof(char *));
            }
            specs[n++] = pm_strdup(trimmed);
        }
        pm_free(spec);
    }

    if (!specs)
        specs = pm_malloc(sizeof(char *));
    specs[n] = NULL;
    return specs;
}

void pypi_metadata_free_specs(char **specs)
{
    if (!specs)
        return;
    for (char **s = specs; *s; s++)
        pm_free(*s);
    pm_free(specs);
}

/* ── Archive extraction ───────────────────────────────────────────────────── */

/* Number of '/' in a path, used to prefer the shallowest PKG-INFO. */
static int path_depth(const char *path)
{
    int d = 0;
    for (const char *p = path; *p; p++)
        if (*p == '/')
            d++;
    return d;
}

/*
 * entry_rank — how good a metadata candidate this archive member is.
 *
 * Higher wins; 0 means "not metadata".  A wheel's METADATA is authoritative.
 * An sdist's PKG-INFO is a fallback, and the one at the root of the tree is
 * the real one — a vendored subdirectory can carry others.
 */
static int entry_rank(const char *path)
{
    const char *base = pm_basename(path);

    if (strcmp(base, "METADATA") == 0) {
        /* Only when the containing directory is the .dist-info one: a file
         * called METADATA sitting in the payload is not the wheel's. */
        const char *slash = strrchr(path, '/');
        if (slash && slash - path >= 10 &&
            strncmp(slash - 10, ".dist-info", 10) == 0)
            return 100;
        return 0;
    }

    if (strcmp(base, "PKG-INFO") == 0) {
        /* Shallower is better: 50 at depth 1 (the normal sdist layout). */
        int depth = path_depth(path);
        return depth <= 8 ? 50 - depth : 40;
    }

    return 0;
}

/* Largest metadata document we will read out of an archive (4 MB). */
#define METADATA_MAX_BYTES ((size_t)4 * 1024 * 1024)

char *pypi_metadata_from_archive(const char *path)
{
    struct archive *a = archive_read_new();
    if (!a)
        return NULL;

    archive_read_support_format_zip(a);
    archive_read_support_format_tar(a);
    archive_read_support_filter_gzip(a);
    archive_read_support_filter_bzip2(a);
    archive_read_support_filter_xz(a);

    if (archive_read_open_filename(a, path, (size_t)64 * 1024) != ARCHIVE_OK) {
        fprintf(stderr, "packmule: cannot open %s: %s\n",
                path, archive_error_string(a));
        archive_read_free(a);
        return NULL;
    }

    char *best      = NULL;
    int   best_rank = 0;

    struct archive_entry *entry;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        const char *name = archive_entry_pathname(entry);
        if (!name)
            continue;

        int rank = entry_rank(name);
        if (rank <= best_rank) {
            archive_read_data_skip(a);
            continue;
        }

        la_int64_t size = archive_entry_size(entry);
        if (size < 0 || (size_t)size > METADATA_MAX_BYTES) {
            archive_read_data_skip(a);
            continue;
        }

        char   *buf   = pm_malloc((size_t)size + 1);
        ssize_t got   = archive_read_data(a, buf, (size_t)size);
        if (got < 0) {
            pm_free(buf);
            continue;
        }
        buf[got] = '\0';

        pm_free(best);
        best      = buf;
        best_rank = rank;

        /* A wheel's METADATA is the best there is; stop looking. */
        if (rank == 100)
            break;
    }

    archive_read_close(a);
    archive_read_free(a);
    return best;
}
