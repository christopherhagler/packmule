/*
 * bundle.c — create the final transport artifact.
 *
 * Writes manifest.json and install.sh (plus requirements.txt for pypi) into
 * the output directory, then compresses the whole directory into a single
 * <output_dir>.tar.gz for transport to an air-gapped machine.
 */

#include "bundle.h"
#include "utils.h"
#include "version.h"

#include "bundle_scripts.h"  /* generated: INSTALL_{PYPI,NPM,RPM}_SH byte arrays */

#include <archive.h>
#include <archive_entry.h>
#include <cjson/cJSON.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ── Helpers ──────────────────────────────────────────────────────────────── */

/* Returns 1 if the file at <dir>/<filename> exists on disk. */
static int file_exists(const char *dir, const char *filename)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", dir, filename);
    struct stat st;
    return stat(path, &st) == 0;
}

/*
 * write_text_file — write `len` bytes plus a trailing newline to `path`,
 * checking every step.
 *
 * A short write or a failed close is how a full disk manifests, and both are
 * invisible if only fopen() is checked: the bundle would be announced as
 * ready while carrying a truncated manifest.  fclose() is where buffered data
 * actually reaches the filesystem, so its return value is the one that
 * matters most.  Returns 0 on success, -1 on any failure.
 */
static int write_text_file(const char *path, const char *data, size_t len)
{
    FILE *fp = fopen(path, "w");
    if (!fp) {
        fprintf(stderr, "packmule: cannot write %s: %s\n",
                path, strerror(errno));
        return -1;
    }

    int ok = (len == 0 || fwrite(data, 1, len, fp) == len) &&
             fputc('\n', fp) != EOF;

    if (fclose(fp) != 0)
        ok = 0;

    if (!ok) {
        fprintf(stderr, "packmule: failed writing %s: %s\n",
                path, strerror(errno));
        remove(path);
        return -1;
    }
    return 0;
}

/* ── Manifest ─────────────────────────────────────────────────────────────── */

static int write_manifest(const BundleOptions *opts)
{
    cJSON *root = cJSON_CreateObject();

    cJSON_AddStringToObject(root, "packmule_version", PACKMULE_VERSION);
    cJSON_AddStringToObject(root, "registry", opts->registry_name);

    time_t     now = time(NULL);
    struct tm *utc = gmtime(&now);
    char       ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", utc);
    cJSON_AddStringToObject(root, "created", ts);

    /* Attach the array up front so an early return frees it with the root. */
    cJSON *pkgs = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "packages", pkgs);

    for (size_t i = 0; i < opts->packages->count; i++) {
        const Package *p = opts->packages->items[i];
        if (!p->filename || !file_exists(opts->output_dir, p->filename))
            continue;

        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "name",     p->name);
        cJSON_AddStringToObject(obj, "version",  p->version  ? p->version  : "");
        cJSON_AddStringToObject(obj, "filename", p->filename);

        /* Record the digest as it was published upstream (algorithm and all)
         * for provenance, plus the SHA-256 of the file as it sits in the
         * bundle, which is what SHA256SUMS and `packmule verify` check. */
        char *published = digest_to_string(&p->digest);
        cJSON_AddStringToObject(obj, "published_digest",
                                published ? published : "");
        pm_free(published);

        char path[4096], hex[DIGEST_HEX_MAX];
        snprintf(path, sizeof(path), "%s/%s", opts->output_dir, p->filename);
        if (digest_file_hex(path, DIGEST_SHA256, hex, sizeof(hex)) != 0) {
            fprintf(stderr, "packmule: cannot hash %s for the manifest\n", path);
            cJSON_Delete(obj);
            cJSON_Delete(root);
            return -1;
        }
        cJSON_AddStringToObject(obj, "sha256", hex);
        cJSON_AddItemToArray(pkgs, obj);
    }

    char *json = cJSON_Print(root);
    cJSON_Delete(root);
    if (!json) {
        fprintf(stderr, "packmule: failed to serialise manifest\n");
        return -1;
    }

    char path[4096];
    snprintf(path, sizeof(path), "%s/manifest.json", opts->output_dir);
    int rc = write_text_file(path, json, strlen(json));
    free(json);   /* cJSON_Print allocates with malloc, not pm_malloc */
    return rc;
}

/* ── Requirements file (pypi only) ────────────────────────────────────────── */

/*
 * write_requirements_txt — emit a pip-readable requirements.txt listing every
 * package whose file is present on disk.  install.sh feeds this to
 * `pip install --no-index` so pip handles install ordering itself.
 */
static int write_requirements_txt(const BundleOptions *opts)
{
    char  *body = pm_strdup("");
    for (size_t i = 0; i < opts->packages->count; i++) {
        const Package *p = opts->packages->items[i];
        if (!p->filename || !file_exists(opts->output_dir, p->filename))
            continue;
        char *next = p->version
            ? pm_asprintf("%s%s==%s\n", body, p->name, p->version)
            : pm_asprintf("%s%s\n",     body, p->name);
        pm_free(body);
        body = next;
    }

    char path[4096];
    snprintf(path, sizeof(path), "%s/requirements.txt", opts->output_dir);
    int rc = write_text_file(path, body, strlen(body));
    pm_free(body);
    return rc;
}

/* ── Transport checksums ─────────────────────────────────────────────────── */

/*
 * write_checksums — emit SHA256SUMS in coreutils format over every file the
 * bundle contains.
 *
 * The registry digest was already verified at download time; this covers a
 * different risk, the one a bundle is actually exposed to: the trip to the
 * air-gapped machine on removable media.  Plain `sha256sum -c` format means
 * the target needs nothing but coreutils to check it, and install.sh runs
 * that check before handing anything to pip/npm/dnf.
 */
static int write_checksums(const BundleOptions *opts, const char *const *meta,
                           size_t meta_n)
{
    char *body = pm_strdup("");

    for (size_t i = 0; i < opts->packages->count; i++) {
        const Package *p = opts->packages->items[i];
        if (!p->filename || !file_exists(opts->output_dir, p->filename))
            continue;

        char path[4096], hex[DIGEST_HEX_MAX];
        snprintf(path, sizeof(path), "%s/%s", opts->output_dir, p->filename);
        if (digest_file_hex(path, DIGEST_SHA256, hex, sizeof(hex)) != 0) {
            pm_free(body);
            return -1;
        }
        char *next = pm_asprintf("%s%s  %s\n", body, hex, p->filename);
        pm_free(body);
        body = next;
    }

    /* Generated metadata is covered too — a tampered requirements.txt or
     * package-lock.json redirects the install just as effectively as a
     * tampered tarball.  SHA256SUMS itself obviously cannot cover itself. */
    for (size_t i = 0; i < meta_n; i++) {
        if (!meta[i] || !file_exists(opts->output_dir, meta[i]))
            continue;
        char path[4096], hex[DIGEST_HEX_MAX];
        snprintf(path, sizeof(path), "%s/%s", opts->output_dir, meta[i]);
        if (digest_file_hex(path, DIGEST_SHA256, hex, sizeof(hex)) != 0) {
            pm_free(body);
            return -1;
        }
        char *next = pm_asprintf("%s%s  %s\n", body, hex, meta[i]);
        pm_free(body);
        body = next;
    }

    char path[4096];
    snprintf(path, sizeof(path), "%s/SHA256SUMS", opts->output_dir);
    /* The trailing newline write_text_file adds would be a blank line here,
     * since every entry already ends in one; trim it. */
    size_t len = strlen(body);
    if (len > 0) body[len - 1] = '\0';
    int rc = write_text_file(path, body, len > 0 ? len - 1 : 0);
    pm_free(body);
    return rc;
}

/* ── Aux file (npm: the project's package-lock.json) ─────────────────────── */

/*
 * copy_aux_file — copy opts->aux_file into the output dir as opts->aux_name.
 * A no-op when the source already IS the destination (e.g. -f package-lock.json
 * with -o pointing at the same directory), which a plain copy would truncate.
 */
static int copy_aux_file(const BundleOptions *opts)
{
    char dest[4096];
    snprintf(dest, sizeof(dest), "%s/%s", opts->output_dir, opts->aux_name);

    char rsrc[PATH_MAX], rdst[PATH_MAX];
    if (realpath(opts->aux_file, rsrc) && realpath(dest, rdst) &&
        strcmp(rsrc, rdst) == 0)
        return 0;

    FILE *in = fopen(opts->aux_file, "rb");
    if (!in) {
        fprintf(stderr, "packmule: cannot read %s: %s\n",
                opts->aux_file, strerror(errno));
        return -1;
    }
    FILE *out = fopen(dest, "wb");
    if (!out) {
        fprintf(stderr, "packmule: cannot write %s: %s\n",
                dest, strerror(errno));
        fclose(in);
        return -1;
    }

    char   buf[65536];
    size_t n;
    int    ret = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fprintf(stderr, "packmule: short write to %s\n", dest);
            ret = -1;
            break;
        }
    }
    if (ferror(in)) {
        fprintf(stderr, "packmule: read error on %s\n", opts->aux_file);
        ret = -1;
    }
    fclose(in);
    fclose(out);
    return ret;
}

/* ── Install script ───────────────────────────────────────────────────────── */

/*
 * script_for — return the embedded install script for `registry_name`, or NULL
 * if the backend has none.  The blobs live in the generated bundle_scripts.h
 * and are sourced from scripts/install_<name>.sh at build time.
 */
static const char *script_for(const char *registry_name)
{
    if (strcmp(registry_name, "pypi") == 0) return (const char *)INSTALL_PYPI_SH;
    if (strcmp(registry_name, "npm")  == 0) return (const char *)INSTALL_NPM_SH;
    if (strcmp(registry_name, "rpm")  == 0) return (const char *)INSTALL_RPM_SH;
    return NULL;
}

static int write_install_script(const BundleOptions *opts)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/install.sh", opts->output_dir);

    const char *script = script_for(opts->registry_name);
    char *fallback = NULL;
    if (!script) {
        fallback = pm_asprintf(
            "#!/bin/sh\n"
            "echo 'packmule: no install script for registry: %s' >&2\n"
            "exit 1\n", opts->registry_name);
        script = fallback;
    }

    int rc = write_text_file(path, script, strlen(script));
    pm_free(fallback);
    if (rc != 0)
        return -1;

    if (chmod(path, 0755) != 0) {
        fprintf(stderr, "packmule: cannot make install.sh executable: %s\n",
                strerror(errno));
        return -1;
    }
    return 0;
}

/* ── Tarball ──────────────────────────────────────────────────────────────── */

/*
 * append_file — add one on-disk file to the open archive `a` under the
 * archive-relative name `arcname`, preserving its mode/size/mtime via
 * libarchive's disk reader.  `disk` is a reusable archive_read_disk handle.
 */
static int append_file(struct archive *a, struct archive *disk,
                       const char *disk_path, const char *arcname)
{
    struct archive_entry *entry = archive_entry_new();
    archive_entry_copy_sourcepath(entry, disk_path);

    /* Populate mode/size/mtime/etc. from the file on disk. */
    if (archive_read_disk_entry_from_file(disk, entry, -1, NULL) != ARCHIVE_OK) {
        fprintf(stderr, "packmule: cannot stat '%s': %s\n",
                disk_path, archive_error_string(disk));
        archive_entry_free(entry);
        return -1;
    }

    /* Store it under the bundle-relative name, not the on-disk path. */
    archive_entry_set_pathname(entry, arcname);

    int ret = 0;
    if (archive_write_header(a, entry) != ARCHIVE_OK) {
        fprintf(stderr, "packmule: write header error for '%s': %s\n",
                arcname, archive_error_string(a));
        ret = -1;
        goto done;
    }

    if (archive_entry_filetype(entry) == AE_IFREG &&
        archive_entry_size(entry) > 0) {
        int fd = open(disk_path, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "packmule: cannot open '%s': %s\n",
                    disk_path, strerror(errno));
            ret = -1;
            goto done;
        }
        char    buf[65536];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            if (archive_write_data(a, buf, (size_t)n) < 0) {
                fprintf(stderr, "packmule: write data error: %s\n",
                        archive_error_string(a));
                ret = -1;
                break;
            }
        }
        close(fd);
    }

done:
    archive_entry_free(entry);
    return ret;
}

/*
 * create_tarball — write `archive_path` containing exactly the files packmule
 * produced: the package files present on disk plus the generated manifest,
 * install script, and (pypi) requirements.txt.  Every entry is stored under a
 * top-level `prefix/` directory so extraction yields one clean folder.
 *
 * Archiving an explicit file list — rather than walking output_dir — keeps
 * unrelated files out of the bundle and makes `-o .` safe: the archive lists
 * only the files we wrote, so it can never try to add itself.
 */
static int create_tarball(const BundleOptions *opts, const char *archive_path,
                          const char *prefix, const char *const *meta,
                          size_t meta_n)
{
    struct archive *a = archive_write_new();
    archive_write_add_filter_gzip(a);
    archive_write_set_format_pax_restricted(a);

    if (archive_write_open_filename(a, archive_path) != ARCHIVE_OK) {
        fprintf(stderr, "packmule: cannot create '%s': %s\n",
                archive_path, archive_error_string(a));
        archive_write_free(a);
        return -1;
    }

    struct archive *disk = archive_read_disk_new();
    archive_read_disk_set_standard_lookup(disk);

    int ret = 0;

    /* Metadata files first, then the package payloads. */
    for (size_t i = 0; i < meta_n && ret == 0; i++) {
        if (!meta[i] || !file_exists(opts->output_dir, meta[i]))
            continue;
        char disk_path[4096], arcname[4096];
        snprintf(disk_path, sizeof(disk_path), "%s/%s", opts->output_dir, meta[i]);
        snprintf(arcname,   sizeof(arcname),   "%s/%s", prefix, meta[i]);
        ret = append_file(a, disk, disk_path, arcname);
    }

    for (size_t i = 0; i < opts->packages->count && ret == 0; i++) {
        const Package *p = opts->packages->items[i];
        if (!p->filename || !file_exists(opts->output_dir, p->filename))
            continue;
        char disk_path[4096], arcname[4096];
        snprintf(disk_path, sizeof(disk_path), "%s/%s", opts->output_dir, p->filename);
        snprintf(arcname,   sizeof(arcname),   "%s/%s", prefix, p->filename);
        ret = append_file(a, disk, disk_path, arcname);
    }

    archive_read_free(disk);

    /*
     * archive_write_close() is where the gzip stream is flushed and the tar
     * trailer written; a full disk fails here and nowhere else.  Ignoring it
     * — as this once did — reports "bundle ready" for a truncated archive.
     */
    if (archive_write_close(a) != ARCHIVE_OK) {
        fprintf(stderr, "packmule: failed to finalise '%s': %s\n",
                archive_path, archive_error_string(a));
        ret = -1;
    }
    archive_write_free(a);

    if (ret != 0)
        remove(archive_path);   /* never leave a half-written bundle behind */
    return ret;
}

/* ── Public entry point ───────────────────────────────────────────────────── */

int bundle_create(const BundleOptions *opts)
{
    printf("packmule: writing manifest.json ...\n");
    if (write_manifest(opts) != 0)
        return -1;

    if (strcmp(opts->registry_name, "pypi") == 0) {
        printf("packmule: writing requirements.txt ...\n");
        if (write_requirements_txt(opts) != 0)
            return -1;
    }

    if (opts->aux_file && opts->aux_name) {
        printf("packmule: copying %s ...\n", opts->aux_name);
        if (copy_aux_file(opts) != 0)
            return -1;
    }

    printf("packmule: writing install.sh ...\n");
    if (write_install_script(opts) != 0)
        return -1;

    /*
     * Everything the bundle carries, in the order it is archived.  install.sh
     * and SHA256SUMS come last for a reason: SHA256SUMS covers the metadata
     * written before it, so it has to be generated once those files are
     * final.  (It cannot cover itself, and install.sh is what runs the check,
     * so neither is listed in `meta`.)
     */
    const char *meta[] = { "manifest.json", "requirements.txt",
                           opts->aux_name };
    const size_t meta_n = sizeof(meta) / sizeof(meta[0]);

    printf("packmule: writing SHA256SUMS ...\n");
    if (write_checksums(opts, meta, meta_n) != 0)
        return -1;

    const char *archived[] = { "manifest.json", "requirements.txt",
                               opts->aux_name, "SHA256SUMS", "install.sh" };

    /* Archive path: strip any trailing slashes from output_dir, append .tar.gz */
    const char *dir  = opts->output_dir;
    size_t      dlen = strlen(dir);
    while (dlen > 1 && dir[dlen - 1] == '/')
        dlen--;

    char *archive_path;
    if (dlen == 1 && dir[0] == '.') {
        /* "." would produce a malformed "..tar.gz"; name the archive after the
         * resolved directory's basename (e.g. the current working dir). */
        char real[PATH_MAX];
        if (realpath(dir, real)) {
            const char *base = strrchr(real, '/');
            archive_path = pm_asprintf("%s.tar.gz", base ? base + 1 : real);
        } else {
            archive_path = pm_strdup("bundle.tar.gz");
        }
    } else {
        archive_path = pm_malloc(dlen + 8);   /* ".tar.gz\0" = 8 bytes */
        memcpy(archive_path, dir, dlen);
        memcpy(archive_path + dlen, ".tar.gz", 8);
    }

    /* Top-level directory the archive entries live under, e.g.
     * "out/manifest.json".  Derived from the resolved output directory's
     * basename so an `-o .` bundle is named after the working directory. */
    char        realdir[PATH_MAX];
    const char *prefix = "bundle";
    if (realpath(opts->output_dir, realdir)) {
        const char *slash = strrchr(realdir, '/');
        if (slash && slash[1])
            prefix = slash + 1;
    }

    printf("packmule: creating %s ...\n", archive_path);
    int ret = create_tarball(opts, archive_path, prefix, archived,
                             sizeof(archived) / sizeof(archived[0]));
    if (ret == 0)
        printf("packmule: bundle ready: %s\n\n", archive_path);

    pm_free(archive_path);
    return ret;
}
