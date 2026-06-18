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

    cJSON *pkgs = cJSON_CreateArray();
    for (size_t i = 0; i < opts->packages->count; i++) {
        const Package *p = opts->packages->items[i];
        if (!p->filename || !file_exists(opts->output_dir, p->filename))
            continue;

        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "name",     p->name);
        cJSON_AddStringToObject(obj, "version",  p->version  ? p->version  : "");
        cJSON_AddStringToObject(obj, "filename", p->filename);
        cJSON_AddStringToObject(obj, "sha256",   p->sha256   ? p->sha256   : "");
        cJSON_AddItemToArray(pkgs, obj);
    }
    cJSON_AddItemToObject(root, "packages", pkgs);

    char *json = cJSON_Print(root);
    cJSON_Delete(root);
    if (!json) {
        fprintf(stderr, "packmule: failed to serialise manifest\n");
        return -1;
    }

    char path[4096];
    snprintf(path, sizeof(path), "%s/manifest.json", opts->output_dir);
    FILE *fp = fopen(path, "w");
    if (!fp) {
        fprintf(stderr, "packmule: cannot write manifest.json: %s\n",
                strerror(errno));
        free(json);
        return -1;
    }
    fputs(json, fp);
    fputc('\n', fp);
    fclose(fp);
    free(json);   /* cJSON_Print allocates with malloc, not pm_malloc */
    return 0;
}

/* ── Requirements file (pypi only) ────────────────────────────────────────── */

/*
 * write_requirements_txt — emit a pip-readable requirements.txt listing every
 * package whose file is present on disk.  install.sh feeds this to
 * `pip install --no-index` so pip handles install ordering itself.
 */
static int write_requirements_txt(const BundleOptions *opts)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/requirements.txt", opts->output_dir);

    FILE *fp = fopen(path, "w");
    if (!fp) {
        fprintf(stderr, "packmule: cannot write requirements.txt: %s\n",
                strerror(errno));
        return -1;
    }

    for (size_t i = 0; i < opts->packages->count; i++) {
        const Package *p = opts->packages->items[i];
        if (!p->filename || !file_exists(opts->output_dir, p->filename))
            continue;
        if (p->version)
            fprintf(fp, "%s==%s\n", p->name, p->version);
        else
            fprintf(fp, "%s\n", p->name);
    }

    fclose(fp);
    return 0;
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

    FILE *fp = fopen(path, "w");
    if (!fp) {
        fprintf(stderr, "packmule: cannot write install.sh: %s\n",
                strerror(errno));
        return -1;
    }

    const char *script = script_for(opts->registry_name);
    if (script) {
        fputs(script, fp);
    } else {
        fprintf(fp,
                "#!/bin/sh\n"
                "echo 'packmule: no install script for registry: %s' >&2\n"
                "exit 1\n",
                opts->registry_name);
    }

    fclose(fp);

    if (chmod(path, 0755) != 0) {
        fprintf(stderr, "packmule: cannot make install.sh executable: %s\n",
                strerror(errno));
        return -1;
    }
    return 0;
}

/* ── Tarball ──────────────────────────────────────────────────────────────── */

static int create_tarball(const char *src_dir, const char *archive_path)
{
    char real_src[PATH_MAX];
    if (!realpath(src_dir, real_src)) {
        fprintf(stderr, "packmule: cannot resolve '%s': %s\n",
                src_dir, strerror(errno));
        return -1;
    }

    /*
     * Compute how many leading characters to strip from each entry's pathname
     * so the archive contains paths relative to the parent directory, e.g.
     * "vendor/file.whl" rather than "/home/user/vendor/file.whl".
     */
    size_t strip = 0;
    for (size_t i = 0; real_src[i]; i++) {
        if (real_src[i] == '/')
            strip = i + 1;
    }

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

    if (archive_read_disk_open(disk, real_src) != ARCHIVE_OK) {
        fprintf(stderr, "packmule: cannot traverse '%s': %s\n",
                real_src, archive_error_string(disk));
        ret = -1;
        goto cleanup;
    }

    struct archive_entry *entry = archive_entry_new();

    for (;;) {
        int r = archive_read_next_header2(disk, entry);
        if (r == ARCHIVE_EOF)
            break;
        if (r != ARCHIVE_OK) {
            fprintf(stderr, "packmule: traverse error: %s\n",
                    archive_error_string(disk));
            ret = -1;
            break;
        }

        if (archive_entry_filetype(entry) == AE_IFDIR)
            archive_read_disk_descend(disk);

        /* Strip the parent prefix so archive paths are relative. */
        const char *orig = archive_entry_pathname(entry);
        if (strlen(orig) >= strip)
            archive_entry_set_pathname(entry, orig + strip);

        if (archive_write_header(a, entry) != ARCHIVE_OK) {
            fprintf(stderr, "packmule: write header error for '%s': %s\n",
                    orig, archive_error_string(a));
            ret = -1;
            break;
        }

        /* Copy data for regular files only. */
        if (archive_entry_filetype(entry) == AE_IFREG) {
            const char *srcpath = archive_entry_sourcepath(entry);
            int fd = open(srcpath, O_RDONLY);
            if (fd < 0) {
                fprintf(stderr, "packmule: cannot open '%s': %s\n",
                        srcpath, strerror(errno));
                ret = -1;
                break;
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
            if (ret != 0)
                break;
        }
    }

    archive_entry_free(entry);

cleanup:
    archive_read_close(disk);
    archive_read_free(disk);
    archive_write_close(a);
    archive_write_free(a);
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

    printf("packmule: writing install.sh ...\n");
    if (write_install_script(opts) != 0)
        return -1;

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

    printf("packmule: creating %s ...\n", archive_path);
    int ret = create_tarball(opts->output_dir, archive_path);
    if (ret == 0)
        printf("packmule: bundle ready: %s\n\n", archive_path);

    pm_free(archive_path);
    return ret;
}
