/*
 * test_wheeltag.c — unit tests for wheel/sdist filename classification and
 * platform-tag matching (pure string processing, no network).
 */
#include "wheeltag.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_classification(void)
{
    assert(dist_is_wheel("requests-2.31.0-py3-none-any.whl"));
    assert(!dist_is_wheel("requests-2.31.0.tar.gz"));

    assert(dist_is_universal_wheel("requests-2.31.0-py3-none-any.whl"));
    assert(dist_is_universal_wheel("six-1.16.0-py2.py3-none-any.whl"));
    assert(!dist_is_universal_wheel(
        "numpy-1.26.4-cp312-cp312-manylinux_2_17_x86_64.whl"));

    assert(dist_is_sdist("requests-2.31.0.tar.gz"));
    assert(dist_is_sdist("legacy-1.0.zip"));
    assert(!dist_is_sdist("requests-2.31.0-py3-none-any.whl"));
}

/*
 * plat_of — copy the platform tag field of `fn` into `buf`.
 *
 * Takes the caller's buffer rather than returning a pointer to a static one:
 * the tests below hold several platform tags live at once.
 */
static const char *plat_of(char *buf, size_t bufsz, const char *fn)
{
    WheelTags t;
    assert(wheel_parse_tags(fn, &t) == 0);
    snprintf(buf, bufsz, "%s", t.platform);
    return buf;
}

static void test_tag_parsing(void)
{
    WheelTags t;

    assert(wheel_parse_tags(
        "numpy-1.26.4-cp312-cp312-manylinux_2_17_x86_64.whl", &t) == 0);
    assert(strcmp(t.python,   "cp312")                 == 0);
    assert(strcmp(t.abi,      "cp312")                 == 0);
    assert(strcmp(t.platform, "manylinux_2_17_x86_64") == 0);

    /* The optional build-number field must not shift the tag fields. */
    assert(wheel_parse_tags("x-1.0-1-py3-none-any.whl", &t) == 0);
    assert(strcmp(t.python,   "py3")  == 0);
    assert(strcmp(t.abi,      "none") == 0);
    assert(strcmp(t.platform, "any")  == 0);

    /* Hyphens in the distribution name must not either. */
    assert(wheel_parse_tags(
        "typing-extensions-4.9.0-py3-none-any.whl", &t) == 0);
    assert(strcmp(t.platform, "any") == 0);

    assert(wheel_parse_tags("not-a-wheel.tar.gz", &t) != 0);
}

static void test_platform_matching(void)
{
    char b1[192], b2[192], b3[192], b4[192], b5[192];
    const char *manylinux = plat_of(b1, sizeof(b1),
        "numpy-1.26.4-cp312-cp312-manylinux_2_17_x86_64.whl");
    const char *macos = plat_of(b2, sizeof(b2),
        "numpy-1.26.4-cp312-cp312-macosx_11_0_arm64.whl");
    const char *musl = plat_of(b3, sizeof(b3),
        "numpy-1.26.4-cp312-cp312-musllinux_1_2_x86_64.whl");
    const char *win = plat_of(b4, sizeof(b4),
        "numpy-1.26.4-cp312-cp312-win_amd64.whl");
    const char *universal2 = plat_of(b5, sizeof(b5),
        "x-1.0-cp312-cp312-macosx_10_9_universal2.whl");

    /* The OS gate keeps wheels off foreign targets. */
    assert(wheel_platform_matches(manylinux, "linux", "x86_64"));
    assert(!wheel_platform_matches(manylinux, "macos", "x86_64"));
    assert(!wheel_platform_matches(macos, "linux", "arm64"));
    assert(wheel_platform_matches(macos, "macos", "arm64"));

    /* musllinux is rejected for (glibc) linux targets. */
    assert(!wheel_platform_matches(musl, "linux", "x86_64"));

    /* Arch spelling equivalences. */
    assert(wheel_platform_matches(win, "windows", "x86_64"));   /* amd64  */
    assert(wheel_platform_matches(macos, "macos", "aarch64"));  /* arm64  */
    assert(wheel_platform_matches(universal2, "macos", "arm64"));
    assert(wheel_platform_matches(universal2, "macos", "x86_64"));

    /* NULL os → arch-only matching. */
    assert(wheel_platform_matches(manylinux, NULL, "x86_64"));
}

static void test_python_matching(void)
{
    const char *cp312 = "numpy-1.26.4-cp312-cp312-manylinux_2_17_x86_64.whl";
    const char *abi3  = "cryptography-42.0.5-cp39-abi3-manylinux_2_28_x86_64.whl";
    const char *pypy  = "x-1.0-pp310-pypy310_pp73-manylinux_2_17_x86_64.whl";

    assert(wheel_python_matches(cp312, 12));
    assert(!wheel_python_matches(cp312, 11));   /* cp31 must not match cp312 */
    assert(wheel_python_matches(abi3, 12));     /* stable ABI floor 3.9 */
    assert(!wheel_python_matches(abi3, 8));     /* target below the floor */
    assert(!wheel_python_matches(pypy, 10));    /* other interpreters refused */
    assert(wheel_python_matches(cp312, 0));     /* 0 disables the check */
}

static void test_manylinux_glibc(void)
{
    int maj = 0, min = 0;

    assert(wheel_manylinux_glibc("manylinux_2_28_x86_64", &maj, &min));
    assert(maj == 2 && min == 28);

    assert(wheel_manylinux_glibc("manylinux2014_x86_64", &maj, &min));
    assert(maj == 2 && min == 17);

    assert(wheel_manylinux_glibc("manylinux1_x86_64", &maj, &min));
    assert(maj == 2 && min == 5);

    /* Compound tags report the modern spelling's floor. */
    assert(wheel_manylinux_glibc(
        "manylinux_2_17_x86_64.manylinux2014_x86_64", &maj, &min));
    assert(maj == 2 && min == 17);

    assert(!wheel_manylinux_glibc("macosx_11_0_arm64", &maj, &min));
    assert(!wheel_manylinux_glibc("win_amd64", &maj, &min));
}

/* ── Regressions ─────────────────────────────────────────────────────────── */

static void test_versioned_pure_python_wheel_is_universal(void)
{
    /*
     * Poetry and hatch emit "py39-none-any" for a project declaring
     * requires-python >= 3.9.  Universality was once decided by matching the
     * literal strings "-py3-none-any"/"-py2.py3-none-any", so these were
     * classified as arch wheels, failed the platform gate, and were silently
     * replaced by a source distribution.  It is the platform tag that decides.
     */
    assert(dist_is_universal_wheel("x-1.0-py39-none-any.whl"));
    assert(dist_is_universal_wheel("x-1.0-py310-none-any.whl"));
    assert(dist_is_universal_wheel("x-1.0-py3-none-any.whl"));
    assert(dist_is_universal_wheel("x-1.0-py2.py3-none-any.whl"));

    /* ... and it still has to be valid for the target interpreter. */
    assert(wheel_python_matches("x-1.0-py39-none-any.whl", 12));
    assert(!wheel_python_matches("x-1.0-py39-none-any.whl", 8));
}

static void test_freethreaded_abi_is_rejected(void)
{
    /*
     * CPython 3.13+ ships a free-threaded build whose ABI tag is "cp313t".
     * It is not interchangeable with the normal cp313 ABI, and pip refuses
     * the wrong one — matching on the interpreter tag alone accepted it.
     */
    assert(!wheel_python_matches(
        "x-1.0-cp313-cp313t-manylinux_2_17_x86_64.whl", 13));
    assert(wheel_python_matches(
        "x-1.0-cp313-cp313-manylinux_2_17_x86_64.whl", 13));
}

static void test_arch_matching_is_anchored(void)
{
    /* Substring matching made "-a arm" accept both aarch64 and armv7l. */
    assert(!wheel_platform_matches("manylinux_2_17_aarch64", "linux", "arm"));
    assert(wheel_platform_matches("manylinux_2_17_aarch64", "linux", "aarch64"));
    assert(wheel_platform_matches("manylinux_2_17_aarch64", "linux", "arm64"));
    assert(!wheel_platform_matches("manylinux_2_17_i686", "linux", "x86_64"));

    /* musl stays rejected even with no OS preference. */
    assert(!wheel_platform_matches("musllinux_1_2_x86_64", NULL, "x86_64"));
}

int main(void)
{
    test_classification();
    test_tag_parsing();
    test_platform_matching();
    test_python_matching();
    test_manylinux_glibc();
    test_versioned_pure_python_wheel_is_universal();
    test_freethreaded_abi_is_rejected();
    test_arch_matching_is_anchored();
    printf("test_wheeltag: all tests passed\n");
    return 0;
}
