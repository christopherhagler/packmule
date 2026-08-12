/*
 * test_pylock.c — unit tests for the uv.lock / pylock.toml readers.
 *
 * These run against fixtures in tests/fixtures and touch no network: a lock
 * already carries every URL and hash, which is the whole reason lockfile mode
 * needs no resolution.  The assertions concentrate on the two things that are
 * easy to get quietly wrong — which packages a target actually needs, and
 * which artifact of each is installable there.
 */
#include "pylock.h"
#include "package.h"
#include "utils.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * The reader dispatches on the filename, so a synthetic lock has to be
 * written as a real "uv.lock" — hence a scratch directory rather than a
 * uniquely-named temp file.  Returns the path, valid until the next call.
 */
#define SCRATCH_DIR "test_pylock_tmp"

static const char *write_uv_lock(const char *text)
{
    static const char *const path = SCRATCH_DIR "/uv.lock";

    mkdir(SCRATCH_DIR, 0755);   /* already existing is fine */
    FILE *fp = fopen(path, "wb");
    assert(fp);
    fputs(text, fp);
    fclose(fp);
    return path;
}

static void drop_uv_lock(void)
{
    remove(SCRATCH_DIR "/uv.lock");
    rmdir(SCRATCH_DIR);
}

static const Package *find(const PackageList *l, const char *name)
{
    return package_list_find_name(l, name, package_name_equal_pep503);
}

static void test_kind_detection(void)
{
    assert(pylock_kind_for_name("uv.lock") == PYLOCK_KIND_UV);
    assert(pylock_kind_for_name("pylock.toml") == PYLOCK_KIND_PEP751);
    /* PEP 751 allows a named lock: pylock.<name>.toml */
    assert(pylock_kind_for_name("pylock.dev.toml") == PYLOCK_KIND_PEP751);

    assert(pylock_kind_for_name("requirements.txt") == PYLOCK_KIND_NONE);
    assert(pylock_kind_for_name("pyproject.toml") == PYLOCK_KIND_NONE);
    assert(pylock_kind_for_name("poetry.lock") == PYLOCK_KIND_NONE);
    assert(pylock_kind_for_name("package-lock.json") == PYLOCK_KIND_NONE);
    /* Near-misses must not be claimed. */
    assert(pylock_kind_for_name("pylock.toml.bak") == PYLOCK_KIND_NONE);
    assert(pylock_kind_for_name("mypylock.toml") == PYLOCK_KIND_NONE);
    assert(pylock_kind_for_name("pylock.") == PYLOCK_KIND_NONE);
    assert(pylock_kind_for_name(NULL) == PYLOCK_KIND_NONE);
}

static void test_effective_lock(void)
{
    /* A lock names itself. */
    char *self = pylock_effective("fixtures/uv.lock");
    assert(self && strcmp(self, "fixtures/uv.lock") == 0);
    pm_free(self);

    /* pyproject.toml resolves to the lock beside it. */
    char *sib = pylock_effective("fixtures/pyproject.toml");
    assert(sib && strcmp(sib, "fixtures/uv.lock") == 0);
    pm_free(sib);

    /* requirements.txt is an explicit list; a sibling lock must NOT silently
     * replace it, or the bundle would stop matching what the user asked for. */
    assert(pylock_effective("fixtures/requirements.txt") == NULL);
    assert(pylock_effective(NULL) == NULL);
}

/*
 * The uv fixture locks for every platform at once.  On Linux the walk must
 * drop the win32-only dependency, keep everything reachable through requests,
 * pull httptools in only because the root asked for uvicorn[standard], and
 * never ship the package nothing depends on.
 */
static void test_uv_reachability(void)
{
    PackageList *l = pylock_parse("fixtures/uv.lock", "x86_64", "linux", 12);
    assert(l);

    assert(find(l, "requests"));
    assert(find(l, "certifi"));
    assert(find(l, "charset-normalizer"));
    assert(find(l, "idna"));
    assert(find(l, "urllib3"));

    /* Reached only via the "standard" extra on the root's uvicorn edge. */
    assert(find(l, "uvicorn"));
    assert(find(l, "httptools"));

    /* Excluded by its edge marker on a Linux target. */
    assert(find(l, "colorama") == NULL);
    /* In the lock, but nothing depends on it. */
    assert(find(l, "unreachable-extra-only") == NULL);
    /* The workspace project itself is a local tree, never a download. */
    assert(find(l, "demo-project") == NULL);

    assert(l->count == 7);

    /* Lock entries are the real resolver's answer; ours must not revise them. */
    const Package *req = find(l, "requests");
    assert(req->state == PKG_RESOLVED);
    assert(req->user_pinned == 1);
    assert(req->dirty == 0);
    assert(strcmp(req->version, "2.32.3") == 0);
    assert(strcmp(req->filename, "requests-2.32.3-py3-none-any.whl") == 0);
    assert(req->digest.algo == DIGEST_SHA256);
    assert(strcmp(req->digest.value,
                  "70761cfe03c773ceb22aa2f671b4757976145175cdfca038c02654d061"
                  "d6dcc6") == 0);

    /* dep_specs feed the SBOM's dependency graph. */
    assert(req->dep_specs && req->dep_specs[0]);
    size_t n = 0;
    while (req->dep_specs[n])
        n++;
    assert(n == 4);

    package_list_destroy(l);
}

/* The win32 dependency becomes reachable when the target IS win32. */
static void test_uv_windows_target(void)
{
    PackageList *l = pylock_parse("fixtures/uv.lock", "x86_64", "windows", 12);
    assert(l);
    assert(find(l, "colorama"));
    assert(l->count == 8);
    package_list_destroy(l);
}

/* One package, three platform wheels: each target must get its own. */
static void test_uv_wheel_selection(void)
{
    static const struct {
        const char *arch, *os, *want;
    } cases[] = {
        { "x86_64", "linux",
          "charset_normalizer-3.4.0-cp312-cp312-manylinux_2_17_x86_64."
          "manylinux2014_x86_64.whl" },
        { "arm64",  "macos",
          "charset_normalizer-3.4.0-cp312-cp312-macosx_11_0_arm64.whl" },
        { "x86_64", "windows",
          "charset_normalizer-3.4.0-cp312-cp312-win_amd64.whl" },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        PackageList *l = pylock_parse("fixtures/uv.lock", cases[i].arch,
                                      cases[i].os, 12);
        assert(l);
        const Package *cn = find(l, "charset-normalizer");
        assert(cn);
        assert(strcmp(cn->filename, cases[i].want) == 0);

        /* A universal wheel serves every target. */
        assert(strcmp(find(l, "idna")->filename,
                      "idna-3.10-py3-none-any.whl") == 0);
        package_list_destroy(l);
    }
}

static void test_pep751(void)
{
    PackageList *l = pylock_parse("fixtures/pylock.toml", "x86_64", "linux", 12);
    assert(l);

    /* PEP 751 is pre-flattened: selection is the per-package marker. */
    assert(find(l, "attrs"));
    assert(find(l, "cffi"));
    assert(find(l, "colorama") == NULL);
    assert(l->count == 2);

    const Package *attrs = find(l, "attrs");
    assert(strcmp(attrs->version, "25.1.0") == 0);
    assert(strcmp(attrs->filename, "attrs-25.1.0-py3-none-any.whl") == 0);
    assert(attrs->state == PKG_RESOLVED && attrs->user_pinned == 1);
    /* PEP 751 records hashes as a table, not a "sha256:" prefixed string. */
    assert(attrs->digest.algo == DIGEST_SHA256);
    assert(strcmp(attrs->digest.value,
                  "c75a69e28a550a7e93789579c22aa26b0f5b83b75dc4e08fe092980051"
                  "e1090a") == 0);

    assert(strcmp(find(l, "cffi")->filename,
                  "cffi-1.17.1-cp312-cp312-manylinux_2_17_x86_64."
                  "manylinux2014_x86_64.whl") == 0);

    package_list_destroy(l);

    /* The marker admits colorama on a Windows target. */
    l = pylock_parse("fixtures/pylock.toml", "x86_64", "windows", 12);
    assert(l);
    assert(find(l, "colorama"));
    assert(strcmp(find(l, "cffi")->filename,
                  "cffi-1.17.1-cp312-cp312-win_amd64.whl") == 0);
    package_list_destroy(l);
}

/*
 * Every one of these yields a bundle that fails on the far side of the wire,
 * so each must fail the build instead of warning.  Written to temp files
 * because the reader dispatches on the filename.
 */
static void test_unbuildable_locks_are_rejected(void)
{
    static const char *const bad[] = {
        /* A git source has no artifact to carry. */
        "version = 1\n"
        "[[package]]\nname = \"demo\"\nversion = \"0.1.0\"\n"
        "source = { virtual = \".\" }\ndependencies = [{ name = \"x\" }]\n"
        "[[package]]\nname = \"x\"\nversion = \"1.0\"\n"
        "source = { git = \"https://example.invalid/x\" }\n",

        /* A local path dependency is equally unshippable. */
        "version = 1\n"
        "[[package]]\nname = \"demo\"\nversion = \"0.1.0\"\n"
        "source = { virtual = \".\" }\ndependencies = [{ name = \"x\" }]\n"
        "[[package]]\nname = \"x\"\nversion = \"1.0\"\n"
        "source = { directory = \"../x\" }\n",

        /* No hash: packmule never keeps a file it cannot verify. */
        "version = 1\n"
        "[[package]]\nname = \"demo\"\nversion = \"0.1.0\"\n"
        "source = { virtual = \".\" }\ndependencies = [{ name = \"x\" }]\n"
        "[[package]]\nname = \"x\"\nversion = \"1.0\"\n"
        "source = { registry = \"https://pypi.org/simple\" }\n"
        "wheels = [{ url = \"https://e.invalid/x-1.0-py3-none-any.whl\" }]\n",

        /* No artifact this target can install, and no sdist to fall back to. */
        "version = 1\n"
        "[[package]]\nname = \"demo\"\nversion = \"0.1.0\"\n"
        "source = { virtual = \".\" }\ndependencies = [{ name = \"x\" }]\n"
        "[[package]]\nname = \"x\"\nversion = \"1.0\"\n"
        "source = { registry = \"https://pypi.org/simple\" }\n"
        "wheels = [{ url = \"https://e.invalid/x-1.0-cp312-cp312-win_amd64.whl\""
        ", hash = \"sha256:aa\" }]\n",

        /* A dependency the lock does not contain: an inconsistent lock. */
        "version = 1\n"
        "[[package]]\nname = \"demo\"\nversion = \"0.1.0\"\n"
        "source = { virtual = \".\" }\ndependencies = [{ name = \"missing\" }]\n",

        /* Two versions of one name, both applicable: pip can install only one,
         * and picking either would silently ship a version not called for. */
        "version = 1\n"
        "[[package]]\nname = \"demo\"\nversion = \"0.1.0\"\n"
        "source = { virtual = \".\" }\ndependencies = [{ name = \"x\" }]\n"
        "[[package]]\nname = \"x\"\nversion = \"1.0\"\n"
        "source = { registry = \"https://pypi.org/simple\" }\n"
        "wheels = [{ url = \"https://e.invalid/x-1.0-py3-none-any.whl\", "
        "hash = \"sha256:aa\" }]\n"
        "[[package]]\nname = \"x\"\nversion = \"2.0\"\n"
        "source = { registry = \"https://pypi.org/simple\" }\n"
        "wheels = [{ url = \"https://e.invalid/x-2.0-py3-none-any.whl\", "
        "hash = \"sha256:bb\" }]\n",
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        PackageList *l = pylock_parse(write_uv_lock(bad[i]), "x86_64",
                                      "linux", 12);
        if (l) {
            fprintf(stdout, "case %zu should have been rejected\n", i);
            assert(l == NULL);
        }
    }
    drop_uv_lock();
}

/*
 * resolution-markers are how uv records one name at several versions for
 * different environments; the entry that survives is the one for our target.
 */
static void test_resolution_markers_disambiguate(void)
{
    const char *doc =
        "version = 1\n"
        "[[package]]\nname = \"demo\"\nversion = \"0.1.0\"\n"
        "source = { virtual = \".\" }\ndependencies = [{ name = \"numpy\" }]\n"
        "[[package]]\nname = \"numpy\"\nversion = \"1.26.4\"\n"
        "source = { registry = \"https://pypi.org/simple\" }\n"
        "resolution-markers = [\"python_full_version < '3.10'\"]\n"
        "wheels = [{ url = \"https://e.invalid/numpy-1.26.4-py3-none-any.whl\", "
        "hash = \"sha256:aa\" }]\n"
        "[[package]]\nname = \"numpy\"\nversion = \"2.1.0\"\n"
        "source = { registry = \"https://pypi.org/simple\" }\n"
        "resolution-markers = [\"python_full_version >= '3.10'\"]\n"
        "wheels = [{ url = \"https://e.invalid/numpy-2.1.0-py3-none-any.whl\", "
        "hash = \"sha256:bb\" }]\n";

    const char *path = write_uv_lock(doc);

    PackageList *old = pylock_parse(path, "x86_64", "linux", 9);
    assert(old && old->count == 1);
    assert(strcmp(find(old, "numpy")->version, "1.26.4") == 0);
    package_list_destroy(old);

    PackageList *recent = pylock_parse(path, "x86_64", "linux", 12);
    assert(recent && recent->count == 1);
    assert(strcmp(find(recent, "numpy")->version, "2.1.0") == 0);
    package_list_destroy(recent);

    drop_uv_lock();
}

int main(void)
{
    printf("Running Python lockfile tests...\n");
    test_kind_detection();
    test_effective_lock();
    test_uv_reachability();
    test_uv_windows_target();
    test_uv_wheel_selection();
    test_pep751();
    test_resolution_markers_disambiguate();
    printf("  (the following lock errors are expected)\n");
    test_unbuildable_locks_are_rejected();
    printf("All Python lockfile tests passed.\n");
    return 0;
}
