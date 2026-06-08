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
#include "network.h"
#include "package.h"
#include "utils.h"

#include <archive.h>
#include <archive_entry.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Manifest parser ─────────────────────────────────────────────────────── */

/*
 * rpm_parse_manifest — read a simple line-by-line RPM package list.
 *
 * Format per line:
 *   <name>            — name only; version resolved from the repo
 *   <name>-<version>  — exact version (last '-' separates name from version)
 *   # comment         — ignored
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

        /* Split on the last '-' to separate name from version. */
        char    *last_dash = strrchr(trimmed, '-');
        Package *pkg;

        if (last_dash && last_dash != trimmed) {
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

/* ── primary.xml package scanner ─────────────────────────────────────────── */

/*
 * find_rpm_package — scan primary.xml for a package matching `name` and
 * (optionally) `arch`.  Also accepts "noarch" packages regardless of `arch`.
 *
 * On success, allocates *out_href, *out_sha256, *out_version and returns 0.
 * The caller must pm_free() all three.  Returns -1 if not found.
 */
static int find_rpm_package(const char *primary_xml,
                             const char *name,
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

    const char *p = primary_xml;

    while ((p = strstr(p, "<package ")) != NULL) {
        const char *pkg_end = strstr(p, "</package>");
        if (!pkg_end)
            break;

        /* Fast reject: does this block contain our package name? */
        if (!strstr_bound(p, pkg_end, name_tag)) {
            p = pkg_end + 10;
            continue;
        }

        /* Verify that <name> is the package name element, not a substring
         * inside another tag (e.g. <name>libbash</name> vs <name>bash</name>).
         * The strstr_bound above already guarantees the full tag text matches. */

        /* Arch filter: accept target arch or noarch. */
        if (arch) {
            int has_arch   = strstr_bound(p, pkg_end, arch_tag) != NULL;
            int has_noarch = strstr_bound(p, pkg_end, "<arch>noarch</arch>") != NULL;
            if (!has_arch && !has_noarch) {
                p = pkg_end + 10;
                continue;
            }
        }

        /* Extract location href. */
        const char *loc = strstr_bound(p, pkg_end, "<location ");
        if (!loc) {
            p = pkg_end + 10;
            continue;
        }
        char *href = xml_attr(loc + 10, "href");
        if (!href) {
            p = pkg_end + 10;
            continue;
        }

        /* Extract SHA-256 checksum.
         * The checksum element looks like:
         *   <checksum type="sha256" pkgid="YES">abc123...</checksum>
         * We want the one with type="sha256". */
        char       *sha256    = NULL;
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
                    sha256 = pm_strndup(cs_val, (size_t)(cs_end - cs_val));
                break;
            }
            cs_search++;
        }

        if (!sha256) {
            fprintf(stderr,
                    "packmule: no SHA-256 checksum found for %s in primary.xml\n",
                    name);
            pm_free(href);
            p = pkg_end + 10;
            continue;
        }

        /* Extract version string from <version epoch="0" ver="X" rel="Y"/>. */
        char *version = NULL;
        const char *ver_tag = strstr_bound(p, pkg_end, "<version ");
        if (ver_tag) {
            char *ver  = xml_attr(ver_tag + 9, "ver");
            char *rel  = xml_attr(ver_tag + 9, "rel");
            char *epoch_s = xml_attr(ver_tag + 9, "epoch");
            int   epoch = epoch_s ? atoi(epoch_s) : 0;
            pm_free(epoch_s);

            if (ver && rel) {
                int n;
                if (epoch > 0)
                    n = snprintf(NULL, 0, "%d:%s-%s", epoch, ver, rel);
                else
                    n = snprintf(NULL, 0, "%s-%s", ver, rel);
                version = pm_malloc((size_t)n + 1);
                if (epoch > 0)
                    snprintf(version, (size_t)n + 1, "%d:%s-%s", epoch, ver, rel);
                else
                    snprintf(version, (size_t)n + 1, "%s-%s", ver, rel);
            }
            pm_free(ver);
            pm_free(rel);
        }

        *out_href    = href;
        *out_sha256  = sha256;
        *out_version = version;
        return 0;
    }

    return -1;
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

    /* ── Step 1: fetch repomd.xml ─────────────────────────────────────────── */
    int   n          = snprintf(NULL, 0, "%s/repodata/repomd.xml", repo);
    char *repomd_url = pm_malloc((size_t)n + 1);
    snprintf(repomd_url, (size_t)n + 1, "%s/repodata/repomd.xml", repo);

    char *repomd_xml = fetch_json(repomd_url);
    pm_free(repomd_url);
    if (!repomd_xml) {
        pm_free(repo);
        return -1;
    }

    /* ── Step 2: locate primary database path ─────────────────────────────── */
    char primary_href[1024];
    if (find_primary_href(repomd_xml, primary_href, sizeof(primary_href)) != 0) {
        pm_free(repomd_xml);
        pm_free(repo);
        return -1;
    }
    pm_free(repomd_xml);

    /* ── Step 3: download primary.xml.gz to a temp file ──────────────────── */
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/packmule_primary_%d.gz",
             (int)getpid());

    int  m           = snprintf(NULL, 0, "%s/%s", repo, primary_href);
    char *primary_url = pm_malloc((size_t)m + 1);
    snprintf(primary_url, (size_t)m + 1, "%s/%s", repo, primary_href);

    int dl_rc = download_file(primary_url, tmp_path);
    pm_free(primary_url);
    if (dl_rc != 0) {
        pm_free(repo);
        return -1;
    }

    /* ── Step 4: decompress ───────────────────────────────────────────────── */
    char *primary_xml = decompress_to_string(tmp_path);
    remove(tmp_path);
    if (!primary_xml) {
        pm_free(repo);
        return -1;
    }

    /* ── Step 5: find the package in primary.xml ──────────────────────────── */
    char *href    = NULL;
    char *sha256  = NULL;
    char *version = NULL;

    if (find_rpm_package(primary_xml, pkg->name, arch,
                         &href, &sha256, &version) != 0) {
        fprintf(stderr,
                "packmule: package '%s' not found in repository (arch: %s)\n",
                pkg->name, arch ? arch : "any");
        pm_free(primary_xml);
        pm_free(repo);
        return -1;
    }
    pm_free(primary_xml);

    /* ── Step 6: populate pkg fields ─────────────────────────────────────── */
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
    int   un  = snprintf(NULL, 0, "%s/%s", repo, href);
    pkg->url  = pm_malloc((size_t)un + 1);
    snprintf(pkg->url, (size_t)un + 1, "%s/%s", repo, href);
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
    .destroy        = NULL,
};
