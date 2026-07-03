/*
 * wheeltag.c — wheel/sdist filename classification and platform-tag matching.
 *
 * See wheeltag.h for the contract of each function.  Kept free of allocation
 * and I/O so it is cheap to call per distribution file in a response and
 * trivial to unit-test.
 */

#include "wheeltag.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Filename classification ─────────────────────────────────────────────── */

int dist_is_wheel(const char *fn)
{
    size_t len = strlen(fn);
    return len > 4 && strcmp(fn + len - 4, ".whl") == 0;
}

int dist_is_universal_wheel(const char *fn)
{
    return strstr(fn, "-py3-none-any") != NULL ||
           strstr(fn, "-py2.py3-none-any") != NULL;
}

int dist_is_sdist(const char *fn)
{
    size_t len = strlen(fn);
    return (len > 7 && strcmp(fn + len - 7, ".tar.gz") == 0) ||
           (len > 4 && strcmp(fn + len - 4, ".zip")    == 0);
}

/* ── Platform tags ───────────────────────────────────────────────────────── */

/*
 * Example: "numpy-2.4.6-cp313-cp313-manylinux_2_17_x86_64.manylinux2014_x86_64.whl"
 *          returns ptr to "manylinux_2_17_x86_64.manylinux2014_x86_64.whl"
 */
const char *wheel_platform_tag(const char *fn)
{
    size_t      len = strlen(fn);
    const char *p   = fn + len - 5; /* char before ".whl" */
    while (p > fn && *p != '-')
        p--;
    return (*p == '-') ? p + 1 : fn;
}

/*
 * arch_matches_platform — 1 if the platform tag is compatible with the
 * requested CPU architecture.  arm64 (macOS naming) and aarch64 (Linux
 * naming) are treated as identical; Windows spells x86_64 as "amd64"
 * (win_amd64) and old macOS fat wheels as "intel" — without those
 * equivalences `-s windows -a x86_64` would reject every compiled wheel.
 */
static int arch_matches_platform(const char *platform, const char *arch)
{
    if (strstr(platform, arch))                                         return 1;
    if (strcmp(arch, "arm64")   == 0 && strstr(platform, "aarch64"))   return 1;
    if (strcmp(arch, "aarch64") == 0 && strstr(platform, "arm64"))     return 1;
    if (strcmp(arch, "x86_64")  == 0 && (strstr(platform, "amd64") ||
                                         strstr(platform, "intel"))) return 1;
    if (strcmp(arch, "amd64")   == 0 && strstr(platform, "x86_64"))    return 1;
    return 0;
}

int wheel_manylinux_glibc(const char *platform, int *maj, int *min)
{
    const char *p = strstr(platform, "manylinux_");
    if (p && sscanf(p, "manylinux_%d_%d", maj, min) == 2)
        return 1;
    if (strstr(platform, "manylinux2014")) { *maj = 2; *min = 17; return 1; }
    if (strstr(platform, "manylinux2010")) { *maj = 2; *min = 12; return 1; }
    if (strstr(platform, "manylinux1"))    { *maj = 2; *min = 5;  return 1; }
    return 0;
}

/*
 * The OS gate is what keeps a manylinux wheel off a macOS target and vice
 * versa: the bare CPU arch is ambiguous across systems (macOS "arm64" vs
 * Linux "aarch64"), so matching arch alone would happily pick a wheel pip
 * refuses.  macOS "universal2" fat wheels carry both arches and match either.
 */
int wheel_platform_matches(const char *platform, const char *os,
                           const char *arch)
{
    if (os) {
        if (strcmp(os, "macos") == 0) {
            if (!strstr(platform, "macosx"))       return 0;
            if (strstr(platform, "universal2"))    return 1;
        } else if (strcmp(os, "windows") == 0) {
            if (!strstr(platform, "win"))          return 0;
        } else if (strcmp(os, "linux") == 0) {
            /* manylinux / plain linux only.  musllinux wheels are built
             * against musl libc (Alpine) and pip on a glibc system refuses
             * them, so accepting one here would bundle an uninstallable
             * wheel.  A future --libc flag could opt Alpine targets in. */
            if (!strstr(platform, "linux"))        return 0;
            if (strstr(platform, "musllinux"))     return 0;
        }
    }
    return arch_matches_platform(platform, arch);
}

/* ── Interpreter tags ────────────────────────────────────────────────────── */

/*
 * tag_at — 1 if `tag` appears in `fn` as a whole '-'/'.'-delimited field (the
 * way interpreter/abi tags are joined in a wheel filename), so "cp31" never
 * spuriously matches inside "cp312".
 */
static int tag_at(const char *fn, const char *tag)
{
    size_t      tlen = strlen(tag);
    const char *p    = fn;
    while ((p = strstr(p, tag)) != NULL) {
        char before = (p == fn) ? '-' : p[-1];
        char after  = p[tlen];
        if ((before == '-' || before == '.') &&
            (after  == '-' || after  == '.'))
            return 1;
        p += tlen;
    }
    return 0;
}

/*
 * A wheel qualifies when it carries the exact interpreter tag (e.g. "cp312"),
 * or when it is a stable-ABI ("abi3") wheel whose CPython floor is <= the
 * target (those install on any newer CPython 3.x, e.g. cp37-abi3 on 3.12).
 * Wheels for a different CPython minor, or for other interpreters (PyPy's
 * "pp310", etc.), are rejected so we never bundle a wheel pip will refuse.
 */
int wheel_python_matches(const char *fn, int py_minor)
{
    if (py_minor <= 0)
        return 1;

    char want[16];
    snprintf(want, sizeof(want), "cp3%d", py_minor); /* e.g. "cp312" */
    if (tag_at(fn, want))
        return 1;

    /* Stable ABI: accept cp3<floor>-abi3 when floor <= target. */
    if (tag_at(fn, "abi3")) {
        const char *p = fn;
        while ((p = strstr(p, "cp3")) != NULL) {
            const char *d = p + 3;
            if (isdigit((unsigned char)*d)) {
                int floor = atoi(d);
                if (floor <= py_minor)
                    return 1;
            }
            p += 3;
        }
    }
    return 0;
}
