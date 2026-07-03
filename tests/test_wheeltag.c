/*
 * test_wheeltag.c — unit tests for wheel/sdist filename classification and
 * platform-tag matching (pure string processing, no network).
 */
#include "wheeltag.h"

#include <assert.h>
#include <stdio.h>

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

static void test_platform_matching(void)
{
    const char *manylinux =
        wheel_platform_tag("numpy-1.26.4-cp312-cp312-manylinux_2_17_x86_64.whl");
    const char *macos =
        wheel_platform_tag("numpy-1.26.4-cp312-cp312-macosx_11_0_arm64.whl");
    const char *musl =
        wheel_platform_tag("numpy-1.26.4-cp312-cp312-musllinux_1_2_x86_64.whl");
    const char *win =
        wheel_platform_tag("numpy-1.26.4-cp312-cp312-win_amd64.whl");
    const char *universal2 =
        wheel_platform_tag("x-1.0-cp312-cp312-macosx_10_9_universal2.whl");

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

int main(void)
{
    test_classification();
    test_platform_matching();
    test_python_matching();
    test_manylinux_glibc();
    printf("test_wheeltag: all tests passed\n");
    return 0;
}
