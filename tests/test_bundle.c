/*
 * test_bundle.c — unit tests for bundle_create().
 *
 * Each test creates a real temporary directory, populates it with zero-byte
 * placeholder files (simulating downloaded packages), calls bundle_create(),
 * and verifies the generated manifest.json, install.sh, requirements.txt,
 * and .tar.gz archive.  No network access is required.
 */
#include "bundle.h"
#include "sbom.h"
#include "package.h"
#include "utils.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── Helpers ──────────────────────────────────────────────────────────────── */

/* Create an empty file at <dir>/<filename>. */
static void touch(const char *dir, const char *filename)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", dir, filename);
    FILE *fp = fopen(path, "w");
    assert(fp != NULL);
    fclose(fp);
}

/* Return 1 if the file at `path` exists. */
static int path_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* Return the permission bits of `path`, or 0 on error. */
static unsigned file_mode(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return 0;
    return (unsigned)(st.st_mode & 0777);
}

/* Return 1 if the file at `path` begins with `prefix`. */
static int file_starts_with(const char *path, const char *prefix)
{
    FILE *fp = fopen(path, "r");
    if (!fp)
        return 0;
    char buf[64] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    (void)n;
    return strncmp(buf, prefix, strlen(prefix)) == 0;
}

/* Return 1 if the file at `path` contains `needle` anywhere. */
static int file_contains(const char *path, const char *needle)
{
    FILE *fp = fopen(path, "r");
    if (!fp)
        return 0;
    char buf[65536] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    (void)n;
    return strstr(buf, needle) != NULL;
}

/* Remove the temp directory and the adjacent .tar.gz file. */
static void cleanup(const char *dir)
{
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    /* Cleanup is best-effort -- the assertions have already run, so a failure
     * here cannot invalidate the test.  The result still has to be consumed:
     * glibc declares system() warn_unused_result under _FORTIFY_SOURCE (which
     * Ubuntu enables when optimising), and a (void) cast does not satisfy
     * that attribute.  Report it rather than hiding it in a dummy variable. */
    int rc = system(cmd);
    if (rc != 0)
        fprintf(stderr, "test_bundle: could not remove %s (rc=%d)\n", dir, rc);

    char tarball[4096];
    snprintf(tarball, sizeof(tarball), "%s.tar.gz", dir);
    remove(tarball);
}

/* Make a Package with all fields populated (simulating a resolved+downloaded pkg). */
static Package *make_pkg(const char *name, const char *version,
                          const char *filename, const char *sha256)
{
    Package *pkg    = package_create(name, version);
    pkg->filename   = pm_strdup(filename);
    digest_set(&pkg->digest, DIGEST_SHA256, DIGEST_ENC_HEX, sha256);
    pkg->url        = pm_strdup("https://example.com/placeholder");
    return pkg;
}

/* ── Tests ───────────────────────────────────────────────────────────────── */

static void test_pypi_bundle(void)
{
    char tmpdir[] = "/tmp/pm_test_pypi_XXXXXX";
    char *dir = mkdtemp(tmpdir);
    assert(dir != NULL);

    /* Place fake downloaded wheels in the output directory. */
    touch(dir, "requests-2.31.0-py3-none-any.whl");
    touch(dir, "certifi-2024.2.2-py3-none-any.whl");

    PackageList *list = package_list_create();
    package_list_add(list, make_pkg("requests", "2.31.0",
        "requests-2.31.0-py3-none-any.whl",
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
    package_list_add(list, make_pkg("certifi", "2024.2.2",
        "certifi-2024.2.2-py3-none-any.whl",
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"));

    BundleOptions opts = { dir, "pypi", list, NULL, NULL, SBOM_NONE, NULL };
    assert(bundle_create(&opts) == 0);

    /* manifest.json — cJSON uses tab separators between key and value. */
    char mpath[4096];
    snprintf(mpath, sizeof(mpath), "%s/manifest.json", dir);
    assert(path_exists(mpath));
    assert(file_contains(mpath, "\"registry\""));
    assert(file_contains(mpath, "\"pypi\""));
    assert(file_contains(mpath, "\"requests\""));
    assert(file_contains(mpath, "\"2.31.0\""));
    assert(file_contains(mpath, "\"certifi\""));

    /* install.sh */
    char ipath[4096];
    snprintf(ipath, sizeof(ipath), "%s/install.sh", dir);
    assert(path_exists(ipath));
    assert(file_starts_with(ipath, "#!/bin/sh"));
    assert(file_mode(ipath) == 0755);
    assert(file_contains(ipath, "pip install"));
    assert(file_contains(ipath, "--no-index"));
    assert(file_contains(ipath, "requirements.txt"));

    /* requirements.txt */
    char rpath[4096];
    snprintf(rpath, sizeof(rpath), "%s/requirements.txt", dir);
    assert(path_exists(rpath));
    assert(file_contains(rpath, "requests==2.31.0"));
    assert(file_contains(rpath, "certifi==2024.2.2"));

    /* tarball */
    char tarball[4096];
    snprintf(tarball, sizeof(tarball), "%s.tar.gz", dir);
    assert(path_exists(tarball));

    package_list_destroy(list);
    cleanup(dir);
}

static void test_npm_bundle(void)
{
    char tmpdir[] = "/tmp/pm_test_npm_XXXXXX";
    char *dir = mkdtemp(tmpdir);
    assert(dir != NULL);

    touch(dir, "lodash-4.17.21.tgz");
    touch(dir, "axios-1.6.8.tgz");

    PackageList *list = package_list_create();
    package_list_add(list, make_pkg("lodash", "4.17.21",
        "lodash-4.17.21.tgz",
        "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"));
    package_list_add(list, make_pkg("axios", "1.6.8",
        "axios-1.6.8.tgz",
        "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"));

    BundleOptions opts = { dir, "npm", list, NULL, NULL, SBOM_NONE, NULL };
    assert(bundle_create(&opts) == 0);

    char mpath[4096];
    snprintf(mpath, sizeof(mpath), "%s/manifest.json", dir);
    assert(path_exists(mpath));
    assert(file_contains(mpath, "\"npm\""));
    assert(file_contains(mpath, "\"lodash\""));
    assert(file_contains(mpath, "\"axios\""));

    char ipath[4096];
    snprintf(ipath, sizeof(ipath), "%s/install.sh", dir);
    assert(path_exists(ipath));
    assert(file_starts_with(ipath, "#!/bin/sh"));
    assert(file_mode(ipath) == 0755);
    assert(file_contains(ipath, "npm install"));

    /* npm bundles do NOT write requirements.txt */
    char rpath[4096];
    snprintf(rpath, sizeof(rpath), "%s/requirements.txt", dir);
    assert(!path_exists(rpath));

    char tarball[4096];
    snprintf(tarball, sizeof(tarball), "%s.tar.gz", dir);
    assert(path_exists(tarball));

    package_list_destroy(list);
    cleanup(dir);
}

static void test_rpm_bundle(void)
{
    char tmpdir[] = "/tmp/pm_test_rpm_XXXXXX";
    char *dir = mkdtemp(tmpdir);
    assert(dir != NULL);

    touch(dir, "bash-5.2.15-1.fc40.x86_64.rpm");
    touch(dir, "curl-8.6.0-1.fc40.x86_64.rpm");

    PackageList *list = package_list_create();
    package_list_add(list, make_pkg("bash", "5.2.15",
        "bash-5.2.15-1.fc40.x86_64.rpm",
        "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"));
    package_list_add(list, make_pkg("curl", "8.6.0",
        "curl-8.6.0-1.fc40.x86_64.rpm",
        "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"));

    BundleOptions opts = { dir, "rpm", list, NULL, NULL, SBOM_NONE, NULL };
    assert(bundle_create(&opts) == 0);

    char mpath[4096];
    snprintf(mpath, sizeof(mpath), "%s/manifest.json", dir);
    assert(path_exists(mpath));
    assert(file_contains(mpath, "\"rpm\""));
    assert(file_contains(mpath, "\"bash\""));
    assert(file_contains(mpath, "\"curl\""));

    char ipath[4096];
    snprintf(ipath, sizeof(ipath), "%s/install.sh", dir);
    assert(path_exists(ipath));
    assert(file_starts_with(ipath, "#!/bin/sh"));
    assert(file_mode(ipath) == 0755);
    assert(file_contains(ipath, "dnf install") || file_contains(ipath, "rpm -Uvh"));

    char tarball[4096];
    snprintf(tarball, sizeof(tarball), "%s.tar.gz", dir);
    assert(path_exists(tarball));

    package_list_destroy(list);
    cleanup(dir);
}

static void test_bundle_skips_missing_files(void)
{
    /*
     * Three packages in the list: only one has a file on disk.
     * The other two (no filename set, or file missing) must be excluded from
     * the manifest and requirements.txt.
     */
    char tmpdir[] = "/tmp/pm_test_skip_XXXXXX";
    char *dir = mkdtemp(tmpdir);
    assert(dir != NULL);

    /* Only this file is actually on disk. */
    touch(dir, "flask-3.0.3-py3-none-any.whl");

    PackageList *list = package_list_create();

    /* Package with a file present. */
    package_list_add(list, make_pkg("flask", "3.0.3",
        "flask-3.0.3-py3-none-any.whl",
        "1111111111111111111111111111111111111111111111111111111111111111111"));

    /* Package whose file was never downloaded (filename set but missing). */
    package_list_add(list, make_pkg("werkzeug", "3.0.2",
        "werkzeug-3.0.2-py3-none-any.whl",  /* not on disk */
        "2222222222222222222222222222222222222222222222222222222222222222222"));

    /* Unresolved package (no filename at all). */
    Package *unresolved = package_create("itsdangerous", NULL);
    package_list_add(list, unresolved);

    BundleOptions opts = { dir, "pypi", list, NULL, NULL, SBOM_NONE, NULL };
    assert(bundle_create(&opts) == 0);

    /* manifest.json must include flask but not werkzeug or itsdangerous. */
    char mpath[4096];
    snprintf(mpath, sizeof(mpath), "%s/manifest.json", dir);
    assert(path_exists(mpath));
    assert( file_contains(mpath, "\"flask\""));
    assert(!file_contains(mpath, "werkzeug"));
    assert(!file_contains(mpath, "itsdangerous"));

    /* requirements.txt must only have flask */
    char rpath[4096];
    snprintf(rpath, sizeof(rpath), "%s/requirements.txt", dir);
    assert(path_exists(rpath));
    assert( file_contains(rpath, "flask==3.0.3"));
    assert(!file_contains(rpath, "werkzeug"));
    assert(!file_contains(rpath, "itsdangerous"));

    package_list_destroy(list);
    cleanup(dir);
}

int main(void)
{
    test_pypi_bundle();
    test_npm_bundle();
    test_rpm_bundle();
    test_bundle_skips_missing_files();
    return 0;
}
