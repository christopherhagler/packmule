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
#include "utils.h"

#include <archive.h>
#include <archive_entry.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
 * find_primary_href — scan repomd.xml for the <data type="primary"> block
 * and extract the location href.
 *
 * Returns 0 and writes the href into `out` on success; -1 on failure.
 */
static int find_primary_href(const char *repomd, char *out, size_t out_size)
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

    size_t capacity = 4 * 1024 * 1024;  /* start at 4 MiB */
    size_t size     = 0;
    char  *buf      = pm_malloc(capacity);

    const void *block;
    size_t      block_size;
    la_int64_t  offset;
    int         r;

    while ((r = archive_read_data_block(a, &block, &block_size, &offset))
           == ARCHIVE_OK) {
        if (size + block_size + 1 > capacity) {
            capacity = (size + block_size + 1) * 2;
            buf      = pm_realloc(buf, capacity);
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
    char *href;
    char *sha256;
    char *ver;      /* <version ver=…>  */
    char *rel;      /* <version rel=…>  */
    int   epoch;
} RpmCandidate;

static void candidate_free(RpmCandidate *c)
{
    pm_free(c->href);
    pm_free(c->sha256);
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

    /* Checksum: want the element with type="sha256":
     *   <checksum type="sha256" pkgid="YES">abc123…</checksum> */
    const char *cs_search = p;
    while ((cs_search = strstr_bound(cs_search, pkg_end, "<checksum ")) != NULL) {
        const char *cs_tag_end = strchr(cs_search + 10, '>');
        if (!cs_tag_end || cs_tag_end >= pkg_end) {
            cs_search++;
            continue;
        }
        char *cs_type = xml_attr(cs_search + 10, "type");
        int   is_sha  = cs_type && strcmp(cs_type, "sha256") == 0;
        pm_free(cs_type);
        if (is_sha) {
            const char *cs_val = cs_tag_end + 1;
            const char *cs_end = strstr(cs_val, "</checksum>");
            if (cs_end && cs_end < pkg_end)
                out->sha256 = pm_strndup(cs_val, (size_t)(cs_end - cs_val));
            break;
        }
        cs_search++;
    }
    if (!out->sha256) {
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
                     char **out_sha256,
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
    *out_sha256  = best.sha256;
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
static char *fetch_primary_xml(const char *repo)
{
    static char *cached_repo = NULL;
    static char *cached_xml  = NULL;

    if (cached_repo && strcmp(cached_repo, repo) == 0)
        return cached_xml;

    /* ── Step 1: fetch repomd.xml ─────────────────────────────────────────── */
    char *repomd_url = pm_asprintf("%s/repodata/repomd.xml", repo);

    char *repomd_xml = fetch_json(repomd_url);
    pm_free(repomd_url);
    if (!repomd_xml)
        return NULL;

    /* ── Step 2: locate primary database path ─────────────────────────────── */
    char primary_href[1024];
    if (find_primary_href(repomd_xml, primary_href, sizeof(primary_href)) != 0) {
        pm_free(repomd_xml);
        return NULL;
    }
    pm_free(repomd_xml);

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
        return NULL;
    }

    /* ── Step 4: decompress and cache ─────────────────────────────────────── */
    char *primary_xml = decompress_to_string(tmp_path);
    remove(tmp_path);
    if (!primary_xml)
        return NULL;

    pm_free(cached_repo);
    pm_free(cached_xml);
    cached_repo = pm_strdup(repo);
    cached_xml  = primary_xml;
    return cached_xml;
}

/* ── Resolver ────────────────────────────────────────────────────────────── */

static int rpm_resolve(const Registry *self, Package *pkg)
{
    const char *repo_url = self->repo_url;
    const char *arch     = self->ctx ? (const char *)self->ctx : NULL;

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

    /* Find the package in primary.xml. */
    char *href    = NULL;
    char *sha256  = NULL;
    char *version = NULL;

    int frc = find_rpm_package(primary_xml, pkg->name, pkg->version, arch,
                               &href, &sha256, &version);
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
    pm_free(pkg->sha256);
    pm_free(pkg->filename);

    pkg->version  = version;
    pkg->sha256   = sha256;

    /* Derive filename from the location href (last path component). */
    const char *slash = strrchr(href, '/');
    pkg->filename = pm_strdup(slash ? slash + 1 : href);

    /* Build full download URL: repo_base + "/" + location_href */
    pkg->url = pm_asprintf("%s/%s", repo, href);
    pm_free(href);
    pm_free(repo);

    return 0;
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
    .detect         = rpm_detect,
    .parse_manifest = rpm_parse_manifest,
    .resolve        = rpm_resolve,
    .get_deps       = NULL, /* rpm does not support transitive resolution */
    .ctx            = NULL, /* arch string, injected by main.c */
    .repo_url       = NULL, /* set by main.c from -u flag */
};
