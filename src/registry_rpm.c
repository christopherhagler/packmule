/*
 * registry_rpm.c — RPM repository backend (DNF/YUM repodata format).
 *
 * Manifest format: a plain text file listing one package name per line,
 * optionally with an exact version separated by a hyphen, e.g.:
 *
 *   bash
 *   vim-9.0.0
 *   # comments and blank lines are ignored
 *
 * Resolve steps (requires -u <repo_base_url>):
 *   1. GET <repo>/repodata/repomd.xml      — locate primary database path
 *   2. GET <repo>/<primary_href>           — download primary.xml.gz
 *   3. Decompress with libarchive          — stream into memory
 *   4. Scan XML for <package> matching name + arch (or noarch)
 *   5. Extract location href, sha256 checksum, and full version string
 *   6. Populate pkg->url, pkg->sha256, pkg->filename, pkg->version
 *
 * XML is parsed with simple string scanning rather than a full XML library.
 * This handles the predictable DNF/YUM repodata format correctly and avoids
 * an additional dependency (libxml2 / expat).
 */

#include "registry.h"
#include "registry_internal.h"
#include "network.h"
#include "package.h"
#include "rpm_repo.h"
#include "utils.h"

#include <archive.h>
#include <archive_entry.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Ceiling on the decompressed size of primary.xml.  Fedora Everything, the
 * largest repository in common use, is roughly 250 MB decompressed.
 */
#define RPM_MAX_PRIMARY_BYTES ((size_t)1024 * 1024 * 1024)

/* ── Manifest parser ─────────────────────────────────────────────────────── */

/*
 * rpm_parse_manifest — read a simple line-by-line RPM package list.
 *
 * Format per line:
 *   <name>            — name only; latest version resolved from the repo
 *   <name>-<version>  — exact version pin
 *   # comment         — ignored
 *
 * The version split happens at the last '-' ONLY when the character after it
 * is a digit: RPM package names routinely contain hyphens (python3-pip,
 * vim-enhanced) while RPM versions always start with a digit, so a blind
 * last-'-' split would silently turn "python3-pip" into name="python3",
 * version="pip" and download the wrong package.
 */
static PackageList *rpm_parse_manifest(const Registry *self, const char *path)
{
    (void)self;

    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "packmule: cannot open %s\n", path);
        return NULL;
    }

    PackageList *list = package_list_create();
    char         line[4096];

    while (fgets(line, (int)sizeof(line), fp)) {
        char *trimmed = pm_strtrim(line);

        if (trimmed[0] == '\0' || trimmed[0] == '#')
            continue;

        char    *last_dash = strrchr(trimmed, '-');
        Package *pkg;

        if (last_dash && last_dash != trimmed &&
            isdigit((unsigned char)last_dash[1])) {
            *last_dash = '\0';
            pkg = package_create(trimmed, last_dash + 1);
        } else {
            pkg = package_create(trimmed, NULL);
        }

        package_list_add(list, pkg);
    }

    fclose(fp);
    return list;
}

/* ── Minimal XML helpers ─────────────────────────────────────────────────── */

/*
 * strstr_bound — like strstr but only matches if the result lies before `end`.
 * Returns NULL if not found or if the match starts at or past `end`.
 */
static const char *strstr_bound(const char *haystack, const char *end,
                                const char *needle)
{
    const char *p = strstr(haystack, needle);
    if (!p || p >= end)
        return NULL;
    return p;
}

/*
 * xml_attr — extract the value of an XML attribute from a tag fragment.
 *
 * `tag` points to text like `data type="primary"` (opening '<' already consumed).
 * Looks for `attr="value"` or `attr='value'`.
 * Returns a heap-allocated copy of value, or NULL if not found.
 */
static char *xml_attr(const char *tag, const char *attr_name)
{
    /* Build search pattern: attr_name=" */
    char pattern[128];
    int  n = snprintf(pattern, sizeof(pattern), "%s=\"", attr_name);
    if (n < 0 || (size_t)n >= sizeof(pattern))
        return NULL;

    const char *p = strstr(tag, pattern);
    if (!p) {
        /* Try single-quote variant. */
        snprintf(pattern, sizeof(pattern), "%s='", attr_name);
        p = strstr(tag, pattern);
        if (!p)
            return NULL;
    }
    p += strlen(pattern);
    char delim    = *(p - 1);   /* '"' or '\'' */
    const char *e = strchr(p, delim);
    if (!e)
        return NULL;
    return pm_strndup(p, (size_t)(e - p));
}

/* ── repomd.xml parser ───────────────────────────────────────────────────── */

/*
 * find_primary_href — scan repomd.xml for the <data type="primary"> block and
 * extract both the location href and the checksum of the compressed file.
 *
 * The checksum is the point of the exercise.  RPM's repodata is a chain:
 * repomd.xml records a digest for primary.xml, primary.xml records a digest
 * for every package.  Fetching primary.xml and trusting it without checking
 * that first link means every package digest packmule goes on to verify came
 * from an unauthenticated document — the verification looks thorough and
 * establishes nothing.
 *
 * Returns 0 and writes the href into `out` (and the digest into `out_digest`)
 * on success; -1 on failure.
 */
static int find_primary_href(const char *repomd, char *out, size_t out_size,
                             Digest *out_digest)
{
    const char *p = repomd;

    while ((p = strstr(p, "<data ")) != NULL) {
        /* Find the end of the opening tag. */
        const char *tag_end = strchr(p + 6, '>');
        if (!tag_end)
            break;

        char *type_val = xml_attr(p + 6, "type");
        int   is_pri   = type_val && strcmp(type_val, "primary") == 0;
        pm_free(type_val);

        if (!is_pri) {
            p = tag_end + 1;
            continue;
        }

        /* Within this <data> block, find <location href="..."/>. */
        const char *block_end = strstr(p, "</data>");
        if (!block_end)
            break;

        const char *loc = strstr_bound(p, block_end, "<location ");
        if (!loc)
            break;

        char *href = xml_attr(loc + 10, "href");
        if (!href)
            break;

        snprintf(out, out_size, "%s", href);
        pm_free(href);

        /*
         * <checksum> in a repomd <data> block covers the compressed file;
         * <open-checksum> covers the decompressed one.  We verify what we
         * downloaded, so it is the former we want — and it must be the first
         * <checksum> in the block, not an <open-checksum> that strstr for
         * "<checksum " would also match were the tag not anchored.
         */
        const char *cs = p;
        while ((cs = strstr_bound(cs, block_end, "<checksum ")) != NULL) {
            const char *cs_tag_end = strchr(cs + 10, '>');
            if (!cs_tag_end || cs_tag_end >= block_end)
                break;
            char      *ty   = xml_attr(cs + 10, "type");
            DigestAlgo algo = digest_algo_from_name(ty);
            pm_free(ty);
            if (algo != DIGEST_NONE) {
                const char *val = cs_tag_end + 1;
                const char *end = strstr(val, "</checksum>");
                if (end && end < block_end) {
                    char *hex = pm_strndup(val, (size_t)(end - val));
                    digest_set(out_digest, algo, DIGEST_ENC_HEX, hex);
                    pm_free(hex);
                }
                break;
            }
            cs++;
        }
        return 0;
    }

    fprintf(stderr, "packmule: could not find primary database in repomd.xml\n");
    return -1;
}

/* ── primary.xml decompressor ────────────────────────────────────────────── */

/*
 * decompress_to_string — decompress a .gz (or any libarchive-supported) file
 * entirely into a NUL-terminated heap string.  Returns NULL on error.
 * Caller must pm_free() the result.
 */
static char *decompress_to_string(const char *path)
{
    struct archive       *a     = archive_read_new();
    struct archive_entry *entry = NULL;

    archive_read_support_filter_gzip(a);
    archive_read_support_filter_bzip2(a);
    archive_read_support_filter_xz(a);
    /* zstd is the modern createrepo_c default (Fedora/RHEL 9+); surface a
     * clear message now rather than an opaque header error later when this
     * libarchive was built without it. */
    if (archive_read_support_filter_zstd(a) == ARCHIVE_FATAL)
        fprintf(stderr,
                "packmule: warning: libarchive lacks zstd support; "
                "zstd-compressed repodata will fail to decompress\n");
    archive_read_support_format_raw(a);

    if (archive_read_open_filename(a, path, 65536) != ARCHIVE_OK) {
        fprintf(stderr, "packmule: cannot open '%s': %s\n",
                path, archive_error_string(a));
        archive_read_free(a);
        return NULL;
    }

    if (archive_read_next_header(a, &entry) != ARCHIVE_OK) {
        fprintf(stderr, "packmule: cannot read archive header from '%s'\n", path);
        archive_read_free(a);
        return NULL;
    }

    size_t capacity = (size_t)4 * 1024 * 1024;  /* start at 4 MiB */
    size_t size     = 0;
    char  *buf      = pm_malloc(capacity);

    const void *block;
    size_t      block_size;
    la_int64_t  offset;
    int         r;

    while ((r = archive_read_data_block(a, &block, &block_size, &offset))
           == ARCHIVE_OK) {
        /*
         * A compressed stream can expand without bound; without a ceiling a
         * malformed or hostile repository turns into an OOM abort.  Even the
         * largest real primary.xml (all of Fedora Everything) is well under
         * this.
         */
        if (size + block_size > RPM_MAX_PRIMARY_BYTES) {
            fprintf(stderr,
                    "packmule: '%s' expands beyond %zu MB; refusing it\n",
                    path, RPM_MAX_PRIMARY_BYTES / ((size_t)1024 * 1024));
            pm_free(buf);
            archive_read_free(a);
            return NULL;
        }
        if (size + block_size + 1 > capacity) {
            capacity = (size + block_size + 1) * 2;
            if (capacity > RPM_MAX_PRIMARY_BYTES + 1)
                capacity = RPM_MAX_PRIMARY_BYTES + 1;
            buf = pm_realloc(buf, capacity);
        }
        memcpy(buf + size, block, block_size);
        size += block_size;
    }

    if (r != ARCHIVE_EOF) {
        fprintf(stderr, "packmule: decompression error for '%s': %s\n",
                path, archive_error_string(a));
        pm_free(buf);
        archive_read_free(a);
        return NULL;
    }

    buf[size] = '\0';
    archive_read_free(a);
    return buf;
}

/* ── RPM version comparison ──────────────────────────────────────────────── */

/*
 * rpm_vercmp — compare two RPM version/release strings with the standard
 * rpmvercmp algorithm: split into alternating numeric and alphabetic
 * segments, compare numeric segments as numbers ("1.10" > "1.9"), compare
 * alphabetic segments lexically, and rank a numeric segment above an
 * alphabetic one.  A '~' segment (pre-release) sorts before everything,
 * including the empty string ("1.0~rc1" < "1.0").
 *
 * Returns <0, 0, >0 for a < b, a == b, a > b.
 */
int rpm_vercmp(const char *a, const char *b)
{
    if (strcmp(a, b) == 0)
        return 0;

    const char *p1 = a, *p2 = b;
    while (*p1 || *p2) {
        /* Skip separators (anything that is not alnum or '~'). */
        while (*p1 && !isalnum((unsigned char)*p1) && *p1 != '~') p1++;
        while (*p2 && !isalnum((unsigned char)*p2) && *p2 != '~') p2++;

        /* Tilde sorts before everything, even end-of-string. */
        if (*p1 == '~' || *p2 == '~') {
            if (*p1 != '~') return 1;
            if (*p2 != '~') return -1;
            p1++; p2++;
            continue;
        }
        if (!*p1 || !*p2)
            break;

        /* Grab one segment of the same character class from each string. */
        const char *s1 = p1, *s2 = p2;
        int numeric = isdigit((unsigned char)*s1);
        if (numeric) {
            while (isdigit((unsigned char)*p1)) p1++;
            while (isdigit((unsigned char)*p2)) p2++;
        } else {
            while (isalpha((unsigned char)*p1)) p1++;
            while (isalpha((unsigned char)*p2)) p2++;
        }

        /* Segment class mismatch: the numeric side is newer. */
        if (s2 == p2)
            return numeric ? 1 : -1;

        if (numeric) {
            /* Compare as numbers: strip leading zeros, longer run wins. */
            while (*s1 == '0') s1++;
            while (*s2 == '0') s2++;
            size_t l1 = (size_t)(p1 - s1), l2 = (size_t)(p2 - s2);
            if (l1 != l2)
                return l1 > l2 ? 1 : -1;
        }

        size_t l1 = (size_t)(p1 - s1), l2 = (size_t)(p2 - s2);
        int rc = strncmp(s1, s2, l1 < l2 ? l1 : l2);
        if (rc != 0)
            return rc > 0 ? 1 : -1;
        if (l1 != l2)
            return l1 > l2 ? 1 : -1;
    }

    /* One string ran out of segments: the longer one is newer. */
    if (!*p1 && !*p2)
        return 0;
    return *p1 ? 1 : -1;
}

/* ── primary.xml package scanner ─────────────────────────────────────────── */

/* One parsed <package> candidate from primary.xml. */
typedef struct {
    char  *href;
    Digest digest;  /* whatever algorithm the repository publishes */
    char  *ver;     /* <version ver=…>  */
    char  *rel;     /* <version rel=…>  */
    int    epoch;
} RpmCandidate;

static void candidate_free(RpmCandidate *c)
{
    pm_free(c->href);
    digest_clear(&c->digest);
    pm_free(c->ver);
    pm_free(c->rel);
    memset(c, 0, sizeof(*c));
}

/*
 * candidate_matches_pin — does this candidate satisfy the manifest's exact
 * version pin?  Accepted pin forms, most to least specific:
 *   "epoch:ver-rel"   "epoch:ver"   "ver-rel"   "ver"
 */
static int candidate_matches_pin(const RpmCandidate *c, const char *pin)
{
    char *forms[4];
    int   n = 0;
    forms[n++] = pm_asprintf("%s", c->ver);
    if (c->rel)
        forms[n++] = pm_asprintf("%s-%s", c->ver, c->rel);
    forms[n++] = pm_asprintf("%d:%s", c->epoch, c->ver);
    if (c->rel)
        forms[n++] = pm_asprintf("%d:%s-%s", c->epoch, c->ver, c->rel);

    int match = 0;
    for (int i = 0; i < n; i++) {
        if (!match && strcmp(forms[i], pin) == 0)
            match = 1;
        pm_free(forms[i]);
    }
    return match;
}

/* candidate_cmp — order two candidates by (epoch, version, release). */
static int candidate_cmp(const RpmCandidate *x, const RpmCandidate *y)
{
    if (x->epoch != y->epoch)
        return x->epoch > y->epoch ? 1 : -1;
    int rc = rpm_vercmp(x->ver, y->ver);
    if (rc != 0)
        return rc;
    /* A missing rel compares as the lowest. */
    if (!x->rel || !y->rel)
        return (x->rel ? 1 : 0) - (y->rel ? 1 : 0);
    return rpm_vercmp(x->rel, y->rel);
}

/*
 * parse_package_block — extract href/sha256/epoch/ver/rel from one
 * <package>…</package> block.  Returns 0 on success (caller owns the strings
 * in `out`), -1 when a required field is missing.
 */
static int parse_package_block(const char *p, const char *pkg_end,
                               RpmCandidate *out)
{
    memset(out, 0, sizeof(*out));

    const char *loc = strstr_bound(p, pkg_end, "<location ");
    if (!loc)
        return -1;
    out->href = xml_attr(loc + 10, "href");
    if (!out->href)
        return -1;

    /*
     * Checksum:  <checksum type="sha256" pkgid="YES">abc123…</checksum>
     *
     * createrepo_c emits whatever algorithm it was configured with — sha1 on
     * older repositories, sha512 on some hardened ones.  Take whichever the
     * repository publishes rather than demanding sha256: insisting on one
     * algorithm used to surface as a misleading "package not found".
     */
    const char *cs_search = p;
    while ((cs_search = strstr_bound(cs_search, pkg_end, "<checksum ")) != NULL) {
        const char *cs_tag_end = strchr(cs_search + 10, '>');
        if (!cs_tag_end || cs_tag_end >= pkg_end) {
            cs_search++;
            continue;
        }
        char      *cs_type = xml_attr(cs_search + 10, "type");
        DigestAlgo algo    = digest_algo_from_name(cs_type);
        pm_free(cs_type);
        if (algo != DIGEST_NONE) {
            const char *cs_val = cs_tag_end + 1;
            const char *cs_end = strstr(cs_val, "</checksum>");
            if (cs_end && cs_end < pkg_end) {
                char *hex = pm_strndup(cs_val, (size_t)(cs_end - cs_val));
                digest_set(&out->digest, algo, DIGEST_ENC_HEX, hex);
                pm_free(hex);
            }
            break;
        }
        cs_search++;
    }
    if (!digest_is_set(&out->digest)) {
        candidate_free(out);
        return -1;
    }

    /* Version: <version epoch="0" ver="X" rel="Y"/>. */
    const char *ver_tag = strstr_bound(p, pkg_end, "<version ");
    if (ver_tag) {
        out->ver = xml_attr(ver_tag + 9, "ver");
        out->rel = xml_attr(ver_tag + 9, "rel");
        char *epoch_s = xml_attr(ver_tag + 9, "epoch");
        out->epoch = epoch_s ? atoi(epoch_s) : 0;
        pm_free(epoch_s);
    }
    if (!out->ver) {
        candidate_free(out);
        return -1;
    }
    return 0;
}

/*
 * find_rpm_package — scan primary.xml for a package matching `name` and
 * (optionally) `arch`.  Also accepts "noarch" packages regardless of `arch`.
 *
 * When `version` is non-NULL only a candidate whose EVR matches the pin (see
 * candidate_matches_pin) is accepted; when NULL, the HIGHEST epoch:ver-rel
 * among all matches is selected (repositories routinely carry several
 * versions of a package — first-match would return an arbitrary one).
 *
 * On success, allocates *out_href, *out_sha256, *out_version and returns 0.
 * The caller must pm_free() all three.
 * Returns RPM_FIND_NOT_FOUND when no package has that name/arch, and
 * RPM_FIND_VERSION_MISMATCH when the name exists but not at `version`.
 */
int find_rpm_package(const char *primary_xml,
                     const char *name,
                     const char *version,
                     const char *arch,
                     char **out_href,
                     Digest *out_digest,
                     char **out_version)
{
    /* Build search strings once. */
    char name_tag[256];
    snprintf(name_tag, sizeof(name_tag), "<name>%s</name>", name);

    char arch_tag[128]  = "";
    if (arch)
        snprintf(arch_tag, sizeof(arch_tag), "<arch>%s</arch>", arch);

    RpmCandidate best      = {0};
    int          have_best = 0;
    int          name_seen = 0;

    const char *p = primary_xml;
    while ((p = strstr(p, "<package ")) != NULL) {
        const char *pkg_end = strstr(p, "</package>");
        if (!pkg_end)
            break;

        /* Fast reject: does this block contain our package name?  The full
         * <name>…</name> tag text is matched, so <name>libbash</name> never
         * matches a search for "bash". */
        if (!strstr_bound(p, pkg_end, name_tag)) {
            p = pkg_end + 10;
            continue;
        }

        /* Arch filter: accept target arch or noarch. */
        if (arch) {
            int has_arch   = strstr_bound(p, pkg_end, arch_tag) != NULL;
            int has_noarch = strstr_bound(p, pkg_end, "<arch>noarch</arch>") != NULL;
            if (!has_arch && !has_noarch) {
                p = pkg_end + 10;
                continue;
            }
        }

        RpmCandidate cand;
        if (parse_package_block(p, pkg_end, &cand) != 0) {
            p = pkg_end + 10;
            continue;
        }
        name_seen = 1;

        if (version) {
            /* Pinned: first exact EVR match wins. */
            if (candidate_matches_pin(&cand, version)) {
                if (have_best)
                    candidate_free(&best);
                best      = cand;
                have_best = 1;
                break;
            }
            candidate_free(&cand);
        } else {
            /* Unpinned: keep the highest epoch:ver-rel seen so far. */
            if (!have_best || candidate_cmp(&cand, &best) > 0) {
                if (have_best)
                    candidate_free(&best);
                best      = cand;
                have_best = 1;
            } else {
                candidate_free(&cand);
            }
        }

        p = pkg_end + 10;
    }

    if (!have_best)
        return name_seen ? RPM_FIND_VERSION_MISMATCH : RPM_FIND_NOT_FOUND;

    /* Render the canonical version string: [epoch:]ver[-rel]. */
    char *ver_str;
    if (best.epoch > 0)
        ver_str = best.rel
                ? pm_asprintf("%d:%s-%s", best.epoch, best.ver, best.rel)
                : pm_asprintf("%d:%s",    best.epoch, best.ver);
    else
        ver_str = best.rel
                ? pm_asprintf("%s-%s", best.ver, best.rel)
                : pm_asprintf("%s",    best.ver);

    *out_href    = best.href;
    *out_digest  = best.digest;   /* ownership moves to the caller */
    *out_version = ver_str;
    pm_free(best.ver);
    pm_free(best.rel);
    return 0;
}

/* ── Primary database cache ──────────────────────────────────────────────── */

/*
 * fetch_primary_xml — return the decompressed primary.xml for `repo` (already
 * trailing-slash-stripped), fetching repomd.xml + primary.xml.gz on first use
 * and caching the result for the rest of the run.  primary.xml is the same
 * multi-MB blob for every package in the manifest; without this cache it
 * would be downloaded and decompressed once PER PACKAGE.
 *
 * The returned pointer is owned by the cache — the caller must NOT free it.
 * Returns NULL on error.  Single-threaded CLI: freed at process exit.
 */
static char *g_cached_repo;
static char *g_cached_xml;
static RpmRepo *g_index;

/* ── One-shot warnings ───────────────────────────────────────────────────── */

/*
 * The fixpoint resolver calls get_deps again every time a package is marked
 * dirty, so a warning printed there repeats once per round — and an unmet
 * capability like '/bin/sh' is requested by most packages in a repository,
 * which multiplies it again.  Warnings are keyed by their subject (the missing
 * capability, or the package with rich dependencies) and printed once.
 *
 * A linear scan is the right structure here: the set holds tens of entries at
 * most, and it is consulted only on the failure path.
 */
static char **g_warned;
static size_t g_warned_n;
static size_t g_warned_cap;

static int warn_once(const char *key)
{
    for (size_t i = 0; i < g_warned_n; i++)
        if (strcmp(g_warned[i], key) == 0)
            return 0;

    if (g_warned_n == g_warned_cap) {
        g_warned_cap = g_warned_cap ? g_warned_cap * 2 : 16;
        g_warned = pm_realloc(g_warned, g_warned_cap * sizeof *g_warned);
    }
    g_warned[g_warned_n++] = pm_strdup(key);
    return 1;
}

void rpm_backend_cleanup(RpmConfig *cfg)
{
    if (cfg)
        cfg->repo = NULL;
    rpm_repo_free(g_index);
    g_index = NULL;
    pm_free(g_cached_xml);
    g_cached_xml = NULL;
    pm_free(g_cached_repo);
    g_cached_repo = NULL;

    for (size_t i = 0; i < g_warned_n; i++)
        pm_free(g_warned[i]);
    pm_free(g_warned);
    g_warned     = NULL;
    g_warned_n   = 0;
    g_warned_cap = 0;
}

static char *fetch_primary_xml(const char *repo)
{
    char **cached_repo = &g_cached_repo;
    char **cached_xml  = &g_cached_xml;

    if (*cached_repo && strcmp(*cached_repo, repo) == 0)
        return *cached_xml;

    /* ── Step 1: fetch repomd.xml ─────────────────────────────────────────── */
    char *repomd_url = pm_asprintf("%s/repodata/repomd.xml", repo);

    char *repomd_xml = fetch_json(repomd_url);
    pm_free(repomd_url);
    if (!repomd_xml)
        return NULL;

    /* ── Step 2: locate primary database path (and its expected digest) ──── */
    char   primary_href[1024];
    Digest primary_digest = {0};
    if (find_primary_href(repomd_xml, primary_href, sizeof(primary_href),
                          &primary_digest) != 0) {
        pm_free(repomd_xml);
        return NULL;
    }
    pm_free(repomd_xml);

    if (!digest_is_set(&primary_digest)) {
        fprintf(stderr,
                "packmule: repomd.xml for %s publishes no usable checksum for "
                "its primary database.\n"
                "          Refusing to trust package digests read from an "
                "unverifiable file.\n", repo);
        return NULL;
    }

    /* ── Step 3: download primary.xml.gz to a temp file ──────────────────── */
    /* Use mkstemp for an unpredictable name: a fixed path in a world-writable
     * directory invites symlink attacks and collides between concurrent runs.
     * The extension is irrelevant — libarchive detects gzip from content. */
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir || !*tmpdir)
        tmpdir = "/tmp";

    char tmp_path[PATH_MAX];
    snprintf(tmp_path, sizeof(tmp_path), "%s/packmule_primary_XXXXXX", tmpdir);
    int tmp_fd = mkstemp(tmp_path);
    if (tmp_fd < 0) {
        fprintf(stderr, "packmule: cannot create temp file in %s\n", tmpdir);
        return NULL;
    }
    close(tmp_fd); /* download_file reopens by path; we only needed the name */

    char *primary_url = pm_asprintf("%s/%s", repo, primary_href);

    int dl_rc = download_file(primary_url, tmp_path, NULL, 0);
    pm_free(primary_url);
    if (dl_rc != 0) {
        remove(tmp_path);
        digest_clear(&primary_digest);
        return NULL;
    }

    /* ── Step 4: verify against repomd.xml, then decompress and cache ────── */
    if (digest_verify_file(tmp_path, &primary_digest) != 0) {
        fprintf(stderr,
                "packmule: %s does not match the checksum in repomd.xml.\n"
                "          The repository metadata is corrupt or has been "
                "tampered with.\n", primary_href);
        remove(tmp_path);
        digest_clear(&primary_digest);
        return NULL;
    }
    digest_clear(&primary_digest);

    char *primary_xml = decompress_to_string(tmp_path);
    remove(tmp_path);
    if (!primary_xml)
        return NULL;

    rpm_repo_free(g_index);
    g_index = NULL;
    pm_free(*cached_repo);
    pm_free(*cached_xml);
    *cached_repo = pm_strdup(repo);
    *cached_xml  = primary_xml;
    return *cached_xml;
}

/*
 * repo_index — the indexed view of the cached primary.xml, built on first use.
 * Only needed when depsolving is on, so the indexing cost is not paid by runs
 * that just name their packages explicitly.
 */
static RpmRepo *repo_index(const char *primary_xml)
{
    if (!g_index)
        g_index = rpm_repo_index(primary_xml);
    return g_index;
}

/* ── Resolver ────────────────────────────────────────────────────────────── */

static int rpm_resolve(const Registry *self, Package *pkg)
{
    const char      *repo_url = self->repo_url;
    RpmConfig       *cfg      = (RpmConfig *)self->ctx;
    const char      *arch     = cfg ? cfg->arch : NULL;

    if (!repo_url) {
        fprintf(stderr,
                "packmule: rpm backend requires a repository base URL.\n"
                "  Pass it with: -u <repo_base_url>\n"
                "  Example: -u https://dl.fedoraproject.org/pub/fedora/linux/"
                "releases/40/Everything/x86_64/os\n");
        return -1;
    }

    /* Strip trailing slash from repo_url for consistent URL construction. */
    size_t rlen    = strlen(repo_url);
    char  *repo    = pm_strndup(repo_url, rlen);
    while (rlen > 0 && repo[rlen - 1] == '/')
        repo[--rlen] = '\0';

    /* Fetch (or reuse) the repository's primary database.  Cache-owned. */
    const char *primary_xml = fetch_primary_xml(repo);
    if (!primary_xml) {
        pm_free(repo);
        return -1;
    }

    /* Index once, on the first resolve, when depsolving is enabled. */
    if (cfg && cfg->resolve_deps && !cfg->repo)
        cfg->repo = repo_index(primary_xml);

    /* Find the package in primary.xml. */
    char  *href    = NULL;
    char  *version = NULL;
    Digest digest  = {0};

    int frc = find_rpm_package(primary_xml, pkg->name, pkg->version, arch,
                               &href, &digest, &version);
    if (frc != 0) {
        if (frc == RPM_FIND_VERSION_MISMATCH)
            fprintf(stderr,
                    "packmule: package '%s' exists in the repository but not "
                    "at version '%s' (arch: %s)\n",
                    pkg->name, pkg->version, arch ? arch : "any");
        else
            fprintf(stderr,
                    "packmule: package '%s' not found in repository (arch: %s)\n",
                    pkg->name, arch ? arch : "any");
        pm_free(repo);
        return -1;
    }

    /* Populate pkg fields. */
    pm_free(pkg->version);
    pm_free(pkg->url);
    pm_free(pkg->filename);
    digest_clear(&pkg->digest);

    pkg->version  = version;
    pkg->digest   = digest;      /* ownership moves into pkg */

    /* Derive filename from the location href (last path component). */
    const char *slash = strrchr(href, '/');
    pkg->filename = pm_strdup(slash ? slash + 1 : href);

    /* Build full download URL: repo_base + "/" + location_href */
    pkg->url = pm_asprintf("%s/%s", repo, href);
    pm_free(href);
    pm_free(repo);

    return 0;
}

/* ── Transitive dependency resolver ─────────────────────────────────────── */

/*
 * pick_provider — choose which of `n` candidate blocks should satisfy `req`.
 *
 * Preference order:
 *   1. the requirement's version constraint must hold;
 *   2. a package whose own <name> IS the capability beats one that merely
 *      provides it (asking for "bash" should get bash, not something that
 *      happens to Provides: bash);
 *   3. the target architecture beats noarch beats anything else;
 *   4. highest EVR.
 *
 * Returns the winning index, or (size_t)-1 when nothing qualifies.
 */
static size_t pick_provider(const RpmRepo *repo, const size_t *cands, int n,
                            const RpmCap *req, const char *cap_name,
                            const char *arch)
{
    size_t best       = (size_t)-1;
    int    best_score = -1;
    int    best_epoch = 0;
    char  *best_ver = NULL, *best_rel = NULL;

    for (int i = 0; i < n; i++) {
        const RpmBlock *b = rpm_repo_block(repo, cands[i]);
        if (!b)
            continue;

        RpmCap prov;
        int    have_prov = rpm_block_provides_matching(b, cap_name, &prov);
        if (have_prov) {
            int ok = rpm_cap_satisfied_by(req, &prov);
            rpm_cap_clear(&prov);
            if (!ok)
                continue;
        } else if (req->flags[0] && req->ver) {
            /* File-path capability with a version constraint: nothing to
             * compare against, so accept it rather than drop the dep. */
        }

        char *bname = rpm_block_name(b);
        char *barch = rpm_block_arch(b);
        int   score = 0;
        if (bname && strcmp(bname, cap_name) == 0)                score += 4;
        if (barch && arch && strcmp(barch, arch) == 0)            score += 2;
        else if (barch && strcmp(barch, "noarch") == 0)           score += 1;
        else if (barch && arch)                                   score -= 8;
        pm_free(bname);
        pm_free(barch);

        int   epoch = 0;
        char *ver = NULL, *rel = NULL;
        if (rpm_block_evr(b, &epoch, &ver, &rel) != 0) {
            pm_free(ver);
            pm_free(rel);
            continue;
        }

        /* First candidate wins by default; after that, a higher preference
         * score wins outright and an equal score is broken on EVR. */
        int better;
        if (best == (size_t)-1 || score > best_score) {
            better = 1;
        } else if (score < best_score) {
            better = 0;
        } else {
            int c = (epoch != best_epoch) ? (epoch > best_epoch ? 1 : -1)
                                          : rpm_vercmp(ver, best_ver);
            if (c == 0 && rel && best_rel)
                c = rpm_vercmp(rel, best_rel);
            better = c > 0;
        }

        if (better) {
            best       = cands[i];
            best_score = score;
            best_epoch = epoch;
            pm_free(best_ver);
            pm_free(best_rel);
            best_ver = ver;
            best_rel = rel;
        } else {
            pm_free(ver);
            pm_free(rel);
        }
    }

    pm_free(best_ver);
    pm_free(best_rel);
    return best;
}

#define RPM_MAX_PROVIDERS 64

/*
 * rpm_get_deps — walk one package's <rpm:requires> and enqueue a provider for
 * each unsatisfied capability.
 *
 * Scope note, because it surprises people: this resolves the closure *within
 * the repository*, which includes base-system packages the target almost
 * certainly already has (glibc, bash, systemd).  That is the safe default for
 * an air-gapped install — dnf skips what is already present, and a bundle
 * missing a dependency is unrecoverable on the target while a bundle
 * carrying a redundant one merely costs disk.  Pass --rpm-deps none for the
 * previous behaviour of bundling only what the manifest names.
 */
static int rpm_get_deps(const Registry *self, const Package *pkg,
                        const PackageList *seen, PackageList *out)
{
    const RpmConfig *cfg = (const RpmConfig *)self->ctx;
    if (!cfg || !cfg->resolve_deps || !cfg->repo)
        return 0;

    size_t idx[RPM_MAX_PROVIDERS];
    int    n = rpm_repo_find_by_name(cfg->repo, pkg->name, idx,
                                     RPM_MAX_PROVIDERS);
    if (n <= 0)
        return 0;

    /* Locate the block matching the version we actually selected, so we walk
     * the right package's requirements. */
    const RpmBlock *self_block = NULL;
    for (int i = 0; i < n && !self_block; i++) {
        const RpmBlock *b = rpm_repo_block(cfg->repo, idx[i]);
        int   epoch = 0;
        char *ver = NULL, *rel = NULL;
        if (b && rpm_block_evr(b, &epoch, &ver, &rel) == 0) {
            char *evr = epoch > 0
                ? (rel ? pm_asprintf("%d:%s-%s", epoch, ver, rel)
                       : pm_asprintf("%d:%s", epoch, ver))
                : (rel ? pm_asprintf("%s-%s", ver, rel)
                       : pm_asprintf("%s", ver));
            if (pkg->version && strcmp(evr, pkg->version) == 0)
                self_block = b;
            pm_free(evr);
        }
        pm_free(ver);
        pm_free(rel);
    }
    if (!self_block)
        self_block = rpm_repo_block(cfg->repo, idx[0]);

    size_t  n_req = 0, n_rich = 0;
    RpmCap *reqs  = rpm_block_requires(self_block, &n_req, &n_rich);

    if (n_rich > 0) {
        char *key = pm_asprintf("rich:%s", pkg->name);
        if (warn_once(key))
            fprintf(stderr,
                    "packmule: warning: %s has %zu boolean/rich dependency "
                    "expression(s) packmule cannot evaluate.\n"
                    "          Verify the install on the target, or list the "
                    "needed packages explicitly.\n", pkg->name, n_rich);
        pm_free(key);
    }

    int added = 0;
    for (size_t i = 0; i < n_req; i++) {
        char *cap = pm_strndup(reqs[i].name, reqs[i].name_len);

        size_t provs[RPM_MAX_PROVIDERS];
        int    np = rpm_repo_find_providers(cfg->repo, cap, provs,
                                            RPM_MAX_PROVIDERS);
        if (np <= 0) {
            /* Keyed on the capability, not the requester: the operator needs
             * the set of things the target must already provide, and most of
             * these are wanted by nearly every package in the repository. */
            char *key = pm_asprintf("cap:%s", cap);
            if (warn_once(key))
                fprintf(stderr,
                        "packmule: warning: nothing in the repository provides "
                        "'%s' (first needed by %s).\n"
                        "          If the target does not already have it, the "
                        "install will fail.\n", cap, pkg->name);
            pm_free(key);
            pm_free(cap);
            continue;
        }

        size_t win = pick_provider(cfg->repo, provs, np, &reqs[i], cap,
                                   cfg->arch);
        if (win == (size_t)-1) {
            fprintf(stderr,
                    "packmule: warning: no package satisfies '%s' at the "
                    "required version (needed by %s)\n", cap, pkg->name);
            pm_free(cap);
            continue;
        }
        pm_free(cap);

        char *pname = rpm_block_name(rpm_repo_block(cfg->repo, win));
        if (!pname)
            continue;

        if (package_list_find_name(seen, pname, package_name_equal_exact)) {
            pm_free(pname);
            continue;   /* already queued or resolved */
        }

        package_list_add(out, package_create(pname, NULL));
        pm_free(pname);
        added++;
    }

    rpm_caps_free(reqs, n_req);
    return added;
}

/* ── Filename detection ──────────────────────────────────────────────────── */

static int rpm_detect(const char *basename)
{
    return strcmp(basename, "packages.txt") == 0;
}

/* ── Registry instance ───────────────────────────────────────────────────── */

const Registry rpm_registry = {
    .name           = "rpm",
    .manifest_name  = "packages.txt",
    /* RPM names are case-sensitive and '.'/'-' are meaningful
     * (java-1.8.0-openjdk, python3.11): compare exactly. */
    .name_equal     = package_name_equal_exact,
    .detect         = rpm_detect,
    .parse_manifest = rpm_parse_manifest,
    .resolve        = rpm_resolve,
    .get_deps       = rpm_get_deps,
    .ctx            = NULL, /* RpmConfig *, injected by main.c */
    .repo_url       = NULL, /* set by main.c from -u flag */
};
