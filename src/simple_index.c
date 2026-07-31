#include "simple_index.h"
#include "utils.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ── Name normalisation ───────────────────────────────────────────────────── */

char *simple_normalize_name(const char *name)
{
    size_t n   = strlen(name);
    char  *out = pm_malloc(n + 1);
    size_t k   = 0;

    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (c == '-' || c == '_' || c == '.') {
            /* Collapse the run; a leading separator would produce a leading
             * '-', which normalisation also has to swallow. */
            if (k > 0 && out[k - 1] == '-')
                continue;
            out[k++] = '-';
        } else {
            out[k++] = (char)tolower((unsigned char)c);
        }
    }
    while (k > 0 && out[k - 1] == '-')
        k--;
    out[k] = '\0';
    return out;
}

/* ── URL resolution ───────────────────────────────────────────────────────── */

/* Length of "scheme://authority" in an absolute URL, or 0 if not absolute. */
static size_t authority_span(const char *url)
{
    size_t off;
    if      (strncasecmp(url, "https://", 8) == 0) off = 8;
    else if (strncasecmp(url, "http://",  7) == 0) off = 7;
    else return 0;

    const char *p = url + off;
    while (*p && *p != '/' && *p != '?' && *p != '#')
        p++;
    return (size_t)(p - url);
}

/*
 * remove_dot_segments — RFC 3986 §5.2.4, rewriting `path` in place.
 *
 * Index pages overwhelmingly use relative hrefs like "../../packages/…", so
 * without this the resolved URL keeps the literal "../" and every download
 * 404s.  Resolving it as a segment stack rather than by string surgery keeps
 * the ".." cases (which are the ones that actually occur) obviously right.
 *
 * The result is never longer than the input, so writing over `path` is safe.
 */
static void remove_dot_segments(char *path)
{
    char *work = pm_strdup(path);

    /* A query or fragment is not part of the path and must survive intact. */
    char *tail = NULL;
    for (char *p = work; *p; p++) {
        if (*p == '?' || *p == '#') {
            tail = pm_strdup(p);
            *p   = '\0';
            break;
        }
    }

    /*
     * Both of these must be read before strtok(), which writes NULs over the
     * separators it consumes — including the trailing one.
     */
    int    absolute      = (work[0] == '/');
    size_t wlen          = strlen(work);
    int    ends_in_slash = (wlen > 0 && work[wlen - 1] == '/');

    char  **segs = NULL;
    size_t  n = 0, cap = 0;
    int     last_was_dot = 0;

    for (char *tok = strtok(work, "/"); tok; tok = strtok(NULL, "/")) {
        if (strcmp(tok, ".") == 0) {
            last_was_dot = 1;
            continue;
        }
        if (strcmp(tok, "..") == 0) {
            if (n > 0)
                n--;
            last_was_dot = 1;
            continue;
        }
        if (n == cap) {
            cap  = cap ? cap * 2 : 8;
            segs = pm_realloc(segs, cap * sizeof(char *));
        }
        segs[n++]    = tok;
        last_was_dot = 0;
    }

    /*
     * The result names a directory when the input did, and also when it ended
     * on a dot segment: "a/b/.." is the directory "a/".  A real segment after
     * a dot segment cancels that — "a/b/../c" is the file "a/c".
     */
    int trailing = ends_in_slash || last_was_dot;

    char *out = path;
    if (absolute)
        *out++ = '/';
    for (size_t i = 0; i < n; i++) {
        if (i)
            *out++ = '/';
        size_t len = strlen(segs[i]);
        memcpy(out, segs[i], len);
        out += len;
    }
    if (trailing && n > 0)
        *out++ = '/';
    *out = '\0';

    if (tail) {
        strcat(path, tail);
        pm_free(tail);
    }
    pm_free(segs);
    pm_free(work);
}

char *simple_url_join(const char *base, const char *ref)
{
    size_t base_auth = base ? authority_span(base) : 0;
    if (base_auth == 0)
        return NULL;
    if (!ref || !*ref)
        return pm_strdup(base);

    /*
     * Already absolute.  Dot segments still have to go: RFC 3986 §5.2.2 folds
     * them for absolute references too, and leaving them in means the same
     * file has two spellings, which defeats the "already downloaded?" check.
     */
    size_t ref_auth = authority_span(ref);
    if (ref_auth != 0) {
        char *abs = pm_strdup(ref);
        remove_dot_segments(abs + ref_auth);
        return abs;
    }

    /* Protocol-relative: "//other.host/path" inherits only the scheme. */
    if (ref[0] == '/' && ref[1] == '/') {
        const char *colon = strchr(base, ':');
        size_t      slen  = (size_t)(colon - base) + 1;   /* include the ':' */
        char       *joined = pm_asprintf("%.*s%s", (int)slen, base, ref);
        remove_dot_segments(joined + authority_span(joined));
        return joined;
    }

    /* Root-relative: replace the whole path. */
    if (ref[0] == '/') {
        char *joined = pm_asprintf("%.*s%s", (int)base_auth, base, ref);
        remove_dot_segments(joined + base_auth);
        return joined;
    }

    /*
     * Relative: merge against the base's directory.  A simple-index page is
     * conventionally served with a trailing slash ("/simple/foo/"), in which
     * case the whole path is the directory.
     */
    const char *base_path = base + base_auth;
    const char *path_end  = base_path;
    while (*path_end && *path_end != '?' && *path_end != '#')
        path_end++;

    const char *last_slash = NULL;
    for (const char *p = base_path; p < path_end; p++)
        if (*p == '/')
            last_slash = p;

    size_t dir_len = last_slash ? (size_t)(last_slash - base_path) + 1 : 0;

    char *joined = pm_asprintf("%.*s%.*s%s%s",
                               (int)base_auth, base,
                               (int)dir_len,   base_path,
                               dir_len ? "" : "/",
                               ref);
    remove_dot_segments(joined + base_auth);
    return joined;
}

/* ── HTML helpers ─────────────────────────────────────────────────────────── */

/*
 * html_unescape — decode the entities an index page can realistically contain.
 *
 * data-requires-python carries '>' and '<' and therefore always arrives
 * escaped; hrefs with query strings arrive with "&amp;".  Anything else is
 * left as written rather than guessed at.
 */
static char *html_unescape(const char *s, size_t n)
{
    char  *out = pm_malloc(n + 1);
    size_t k   = 0;

    for (size_t i = 0; i < n; i++) {
        if (s[i] != '&') {
            out[k++] = s[i];
            continue;
        }

        size_t      remain = n - i;
        const char *p      = s + i;
        static const struct { const char *ent; size_t len; char ch; } ENTS[] = {
            { "&amp;",  5, '&'  }, { "&lt;",   4, '<'  },
            { "&gt;",   4, '>'  }, { "&quot;", 6, '"'  },
            { "&#39;",  5, '\'' }, { "&#x27;", 6, '\'' },
            { "&apos;", 6, '\'' },
        };

        int matched = 0;
        for (size_t e = 0; e < sizeof(ENTS) / sizeof(ENTS[0]); e++) {
            if (remain >= ENTS[e].len &&
                strncasecmp(p, ENTS[e].ent, ENTS[e].len) == 0) {
                out[k++] = ENTS[e].ch;
                i       += ENTS[e].len - 1;
                matched  = 1;
                break;
            }
        }
        if (!matched)
            out[k++] = '&';
    }

    out[k] = '\0';
    return out;
}

/* Case-insensitive attribute-name comparison over a non-terminated span. */
static int attr_is(const char *name, size_t len, const char *want)
{
    return strlen(want) == len && strncasecmp(name, want, len) == 0;
}

/* ── Anchor scanning ──────────────────────────────────────────────────────── */

typedef struct {
    char *href;
    char *requires_python;
    char *core_metadata;   /* "true" or "<algo>=<hex>"; NULL when absent */
    int   yanked;
} AnchorAttrs;

static void anchor_attrs_free(AnchorAttrs *a)
{
    pm_free(a->href);
    pm_free(a->requires_python);
    pm_free(a->core_metadata);
    memset(a, 0, sizeof(*a));
}

/*
 * parse_attrs — read attributes from `p` up to the tag's '>'.
 *
 * Returns a pointer just past the '>', or NULL if the tag is unterminated.
 */
static const char *parse_attrs(const char *p, AnchorAttrs *out)
{
    while (*p) {
        while (isspace((unsigned char)*p))
            p++;
        if (*p == '>')
            return p + 1;
        if (*p == '/' && p[1] == '>')
            return p + 2;
        if (!*p)
            return NULL;

        const char *name = p;
        while (*p && !isspace((unsigned char)*p) && *p != '=' && *p != '>')
            p++;
        size_t name_len = (size_t)(p - name);
        if (name_len == 0) {           /* stray character; don't spin on it */
            p++;
            continue;
        }

        while (isspace((unsigned char)*p))
            p++;

        const char *val     = NULL;
        size_t      val_len = 0;

        if (*p == '=') {
            p++;
            while (isspace((unsigned char)*p))
                p++;
            if (*p == '"' || *p == '\'') {
                char quote = *p++;
                val = p;
                while (*p && *p != quote)
                    p++;
                val_len = (size_t)(p - val);
                if (*p)
                    p++;
            } else {
                val = p;
                while (*p && !isspace((unsigned char)*p) && *p != '>')
                    p++;
                val_len = (size_t)(p - val);
            }
        }

        if (attr_is(name, name_len, "href") && val && !out->href) {
            out->href = html_unescape(val, val_len);
        } else if (attr_is(name, name_len, "data-requires-python") && val &&
                   !out->requires_python) {
            out->requires_python = html_unescape(val, val_len);
        } else if (attr_is(name, name_len, "data-yanked")) {
            /* PEP 592: presence is the signal; the value is a human-readable
             * reason and may legitimately be empty. */
            out->yanked = 1;
        } else if ((attr_is(name, name_len, "data-core-metadata") ||
                    attr_is(name, name_len, "data-dist-info-metadata")) &&
                   val && !out->core_metadata) {
            /* PEP 714 renamed the attribute; indexes in the field serve one,
             * the other, or both, so accept either. */
            out->core_metadata = html_unescape(val, val_len);
        }
    }
    return NULL;
}

/* Find the next "<a" tag, returning a pointer just past the "a". */
static const char *find_anchor(const char *p)
{
    while ((p = strchr(p, '<')) != NULL) {
        const char *q = p + 1;
        if ((*q == 'a' || *q == 'A') &&
            (isspace((unsigned char)q[1]) || q[1] == '>' || q[1] == '/'))
            return q + 1;
        p++;
    }
    return NULL;
}

/*
 * fragment_digest — read "#sha256=<hex>" (or any algorithm the hash layer
 * knows) off the end of an href, and cut the fragment away from `url`.
 */
static void fragment_digest(char *url, Digest *out)
{
    char *hash = strchr(url, '#');
    if (!hash)
        return;

    *hash = '\0';                 /* the fragment is not part of the URL */

    char *eq = strchr(hash + 1, '=');
    if (!eq)
        return;

    *eq = '\0';
    DigestAlgo algo = digest_algo_from_name(hash + 1);
    if (algo != DIGEST_NONE && eq[1])
        digest_set(out, algo, DIGEST_ENC_HEX, eq + 1);
    *eq = '=';
}

/* Parse a PEP 658 metadata attribute value into `out`; "<algo>=<hex>". */
static void metadata_digest(const char *value, Digest *out)
{
    if (!value || strcasecmp(value, "true") == 0)
        return;                   /* advertised, but no digest given */

    const char *eq = strchr(value, '=');
    if (!eq)
        return;

    char      *name = pm_strndup(value, (size_t)(eq - value));
    DigestAlgo algo = digest_algo_from_name(name);
    if (algo != DIGEST_NONE && eq[1])
        digest_set(out, algo, DIGEST_ENC_HEX, eq + 1);
    pm_free(name);
}

/* ── Distribution filename recognition ────────────────────────────────────── */

static const char *const DIST_EXTS[] = {
    ".whl", ".tar.gz", ".zip", ".tar.bz2", ".tar.xz", ".egg", NULL
};

/* Length of the recognised extension at the end of `name`, or 0. */
static size_t dist_ext_len(const char *name)
{
    size_t n = strlen(name);
    for (const char *const *e = DIST_EXTS; *e; e++) {
        size_t el = strlen(*e);
        if (n > el && strcasecmp(name + n - el, *e) == 0)
            return el;
    }
    return 0;
}

char *simple_file_version(const char *filename, const char *project)
{
    size_t ext = dist_ext_len(filename);
    if (ext == 0)
        return NULL;

    size_t stem_len = strlen(filename) - ext;

    /*
     * A wheel's name is fully specified: distribution-version-{build-}python-
     * abi-platform.whl.  The version is the second '-' separated field, and no
     * field but the first may contain '-', so counting from the front is safe.
     */
    if (ext == 4 && strcasecmp(filename + stem_len, ".whl") == 0) {
        const char *dash = memchr(filename, '-', stem_len);
        if (!dash)
            return NULL;
        const char *vstart = dash + 1;
        const char *vend   = memchr(vstart, '-',
                                    stem_len - (size_t)(vstart - filename));
        if (!vend)
            return NULL;
        return pm_strndup(vstart, (size_t)(vend - vstart));
    }

    /*
     * An sdist gives no such guarantee, so anchor on the project name: strip
     * the normalised prefix and one separator, and the remainder is the
     * version.  Comparing normalised avoids tripping over the many ways an
     * index spells "zope.interface" versus "zope_interface".
     */
    char  *want    = simple_normalize_name(project);
    char  *stem    = pm_strndup(filename, stem_len);
    char  *norm    = simple_normalize_name(stem);
    size_t want_len = strlen(want);
    char  *version  = NULL;

    if (strncmp(norm, want, want_len) == 0 && norm[want_len] == '-') {
        /*
         * Normalisation can change the string's length (runs collapse), so the
         * offset into `norm` is not an offset into `stem`.  Walk the original
         * forward past the same number of name characters instead.
         */
        size_t i = 0, seen = 0;
        while (stem[i] && seen < want_len) {
            if (stem[i] == '-' || stem[i] == '_' || stem[i] == '.') {
                while (stem[i] == '-' || stem[i] == '_' || stem[i] == '.')
                    i++;
                seen++;             /* the whole run collapsed to one '-' */
            } else {
                i++;
                seen++;
            }
        }
        while (stem[i] == '-' || stem[i] == '_' || stem[i] == '.')
            i++;
        if (stem[i])
            version = pm_strdup(stem + i);
    }

    pm_free(want);
    pm_free(stem);
    pm_free(norm);
    return version;
}

/* ── Entry point ──────────────────────────────────────────────────────────── */

SimpleFileList *simple_index_parse(const char *html, const char *page_url)
{
    if (!html || !page_url)
        return NULL;

    SimpleFileList *list = pm_calloc(1, sizeof(SimpleFileList));
    size_t          cap  = 0;

    const char *p = html;
    while ((p = find_anchor(p)) != NULL) {
        AnchorAttrs attrs = {0};
        const char *after = parse_attrs(p, &attrs);
        if (!after) {
            anchor_attrs_free(&attrs);
            break;                      /* unterminated tag: nothing usable left */
        }
        p = after;

        if (!attrs.href || !*attrs.href) {
            anchor_attrs_free(&attrs);
            continue;
        }

        char *url = simple_url_join(page_url, attrs.href);
        if (!url) {
            anchor_attrs_free(&attrs);
            continue;
        }

        Digest digest = {0};
        fragment_digest(url, &digest);

        /*
         * PEP 503 puts the filename in the anchor text, and that is the
         * authoritative spelling.  Some indexes emit an empty or decorated
         * body, so fall back to the last path segment of the href — but never
         * to anything with a directory component, which is how a hostile index
         * would try to write outside the output directory.
         */
        char       *filename = NULL;
        const char *lt       = strchr(p, '<');
        if (lt && lt > p) {
            char *text    = html_unescape(p, (size_t)(lt - p));
            char *trimmed = pm_strtrim(text);
            if (*trimmed && dist_ext_len(trimmed) != 0)
                filename = pm_strdup(pm_basename(trimmed));
            pm_free(text);
        }
        if (!filename) {
            char       *path_only = pm_strdup(url);
            char       *q         = strchr(path_only, '?');
            if (q)
                *q = '\0';
            const char *base      = pm_basename(path_only);
            if (dist_ext_len(base) != 0)
                filename = pm_strdup(base);
            pm_free(path_only);
        }

        /* An anchor that names no distribution file is navigation, not a
         * download: index pages carry "Back to index" links and the like. */
        if (!filename) {
            digest_clear(&digest);
            pm_free(url);
            anchor_attrs_free(&attrs);
            continue;
        }

        if (list->count == cap) {
            cap = cap ? cap * 2 : 16;
            list->items = pm_realloc(list->items, cap * sizeof(SimpleFile));
        }

        SimpleFile *f = &list->items[list->count++];
        memset(f, 0, sizeof(*f));
        f->filename        = filename;
        f->url             = url;
        f->digest          = digest;
        f->yanked          = attrs.yanked;
        f->requires_python = attrs.requires_python;
        attrs.requires_python = NULL;   /* ownership moved into the list */

        if (attrs.core_metadata && strcasecmp(attrs.core_metadata, "false") != 0) {
            f->metadata_url = pm_asprintf("%s.metadata", url);
            metadata_digest(attrs.core_metadata, &f->metadata_digest);
        }

        anchor_attrs_free(&attrs);
    }

    return list;
}

void simple_index_free(SimpleFileList *list)
{
    if (!list)
        return;
    for (size_t i = 0; i < list->count; i++) {
        SimpleFile *f = &list->items[i];
        pm_free(f->filename);
        pm_free(f->url);
        pm_free(f->requires_python);
        pm_free(f->metadata_url);
        digest_clear(&f->digest);
        digest_clear(&f->metadata_digest);
    }
    pm_free(list->items);
    pm_free(list);
}
