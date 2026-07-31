/*
 * wheeltag.c — wheel filename parsing and target compatibility.
 * See wheeltag.h for the contract of each function.
 */

#include "wheeltag.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp: POSIX, and not pulled in by string.h */

/* ── Filename classification ─────────────────────────────────────────────── */

static int ends_with(const char *s, const char *suffix)
{
    size_t sl = strlen(s), fl = strlen(suffix);
    return sl > fl && strcmp(s + sl - fl, suffix) == 0;
}

int dist_is_wheel(const char *fn)
{
    return ends_with(fn, ".whl");
}

int dist_is_sdist(const char *fn)
{
    return ends_with(fn, ".tar.gz") || ends_with(fn, ".zip");
}

/* ── Tag parsing ─────────────────────────────────────────────────────────── */

/* copy_field — copy [start,end) into `dst`.  Returns -1 if it will not fit. */
static int copy_field(char *dst, size_t dstsz, const char *start,
                      const char *end)
{
    size_t n = (size_t)(end - start);
    if (n == 0 || n >= dstsz)
        return -1;
    memcpy(dst, start, n);
    dst[n] = '\0';
    return 0;
}

int wheel_parse_tags(const char *fn, WheelTags *out)
{
    if (!dist_is_wheel(fn))
        return -1;

    /*
     * The three tag fields are the LAST three '-'-separated fields before
     * ".whl".  Scanning backwards avoids having to know whether the optional
     * build-number field is present, and avoids tripping over hyphens inside
     * the distribution name.
     */
    const char *end = fn + strlen(fn) - 4;      /* at ".whl" */
    const char *bounds[4];
    bounds[3] = end;

    for (int i = 2; i >= 0; i--) {
        const char *p = bounds[i + 1] - 1;
        while (p > fn && *p != '-')
            p--;
        if (*p != '-')
            return -1;                          /* too few fields */
        bounds[i] = p;
    }

    /* bounds[0..2] point at the '-' preceding python/abi/platform. */
    if (copy_field(out->python,   sizeof(out->python),   bounds[0] + 1, bounds[1]) != 0 ||
        copy_field(out->abi,      sizeof(out->abi),      bounds[1] + 1, bounds[2]) != 0 ||
        copy_field(out->platform, sizeof(out->platform), bounds[2] + 1, bounds[3]) != 0)
        return -1;

    return 0;
}

int dist_is_universal_wheel(const char *fn)
{
    WheelTags t;
    if (wheel_parse_tags(fn, &t) != 0)
        return 0;
    /* "any" is the whole platform field for a universal wheel; it is never a
     * member of a compressed set. */
    return strcmp(t.platform, "any") == 0;
}

/* ── Compressed tag sets ─────────────────────────────────────────────────── */

/*
 * for_each_tag — call `fn` for each '.'-separated member of `set`, stopping at
 * the first one that returns non-zero.  Returns that value, or 0.
 */
static int for_each_tag(const char *set,
                        int (*fn)(const char *tag, void *ctx), void *ctx)
{
    char   member[192];
    size_t total = strlen(set);

    for (size_t off = 0; off < total; ) {
        size_t n = strcspn(set + off, ".");
        if (n == 0) {          /* empty component ("a..b") */
            off++;
            continue;
        }
        if (n >= sizeof(member))
            return 0;          /* malformed tag: match nothing rather than truncate */

        memcpy(member, set + off, n);
        member[n] = '\0';

        int r = fn(member, ctx);
        if (r)
            return r;

        off += n;
        if (off < total && set[off] == '.')
            off++;
    }
    return 0;
}

/* ── Platform tags ───────────────────────────────────────────────────────── */

/*
 * arch_suffix_matches — does the platform tag `plat` end with CPU `arch`?
 *
 * Platform tags put the architecture last ("manylinux_2_17_x86_64",
 * "macosx_11_0_arm64", "win_amd64"), so an anchored suffix test is both
 * correct and far tighter than a substring search — "-a arm" must not match
 * "armv7l" and "aarch64" alike.
 */
static int arch_suffix_matches(const char *plat, const char *arch)
{
    size_t pl = strlen(plat), al = strlen(arch);
    if (al == 0 || pl < al)
        return 0;
    if (strcasecmp(plat + pl - al, arch) != 0)
        return 0;
    /* Either the whole tag, or preceded by the tag separator. */
    return pl == al || plat[pl - al - 1] == '_';
}

/* Spellings of the same CPU across ecosystems. */
static int arch_equivalent(const char *plat, const char *arch)
{
    if (arch_suffix_matches(plat, arch))
        return 1;
    if (strcasecmp(arch, "arm64") == 0)
        return arch_suffix_matches(plat, "aarch64");
    if (strcasecmp(arch, "aarch64") == 0)
        return arch_suffix_matches(plat, "arm64");
    if (strcasecmp(arch, "x86_64") == 0)
        return arch_suffix_matches(plat, "amd64") ||
               arch_suffix_matches(plat, "intel")  ||   /* old macOS fat */
               arch_suffix_matches(plat, "x64");
    if (strcasecmp(arch, "amd64") == 0)
        return arch_suffix_matches(plat, "x86_64");
    return 0;
}

typedef struct {
    const char *os;
    const char *arch;
} PlatCtx;

static int plat_member_matches(const char *tag, void *vctx)
{
    const PlatCtx *c = (const PlatCtx *)vctx;

    if (c->os) {
        if (strcmp(c->os, "macos") == 0) {
            if (strncmp(tag, "macosx", 6) != 0)          return 0;
            /* universal2 fat wheels carry both arches. */
            if (strstr(tag, "universal2"))               return 1;
        } else if (strcmp(c->os, "windows") == 0) {
            if (strncmp(tag, "win", 3) != 0)             return 0;
            /* "win32" is the 32-bit x86 tag with no arch suffix to test. */
            if (strcmp(tag, "win32") == 0)
                return c->arch && (strcasecmp(c->arch, "x86") == 0 ||
                                   strcasecmp(c->arch, "i686") == 0);
        } else if (strcmp(c->os, "linux") == 0) {
            /* manylinux / plain linux only.  musllinux wheels are built
             * against musl libc (Alpine) and pip on a glibc system refuses
             * them, so accepting one here would bundle an uninstallable
             * wheel.  A future --libc flag could opt Alpine targets in. */
            if (!strstr(tag, "linux"))                   return 0;
            if (strncmp(tag, "musllinux", 9) == 0)       return 0;
        }
    } else {
        /* No OS preference: still never pick a musl wheel by accident. */
        if (strncmp(tag, "musllinux", 9) == 0)           return 0;
    }

    return c->arch ? arch_equivalent(tag, c->arch) : 1;
}

int wheel_platform_matches(const char *platform, const char *os,
                           const char *arch)
{
    PlatCtx ctx = { os, arch };
    return for_each_tag(platform, plat_member_matches, &ctx) != 0;
}

/* ── manylinux glibc floors ──────────────────────────────────────────────── */

typedef struct { int found, maj, min; } GlibcCtx;

static int glibc_member(const char *tag, void *vctx)
{
    GlibcCtx *c = (GlibcCtx *)vctx;
    int maj = 0, min = 0;

    if (sscanf(tag, "manylinux_%d_%d", &maj, &min) == 2) {
        /* keep going: we want the lowest floor in the set */
    } else if (strncmp(tag, "manylinux2014", 13) == 0) { maj = 2; min = 17; }
    else if (strncmp(tag, "manylinux2010", 13) == 0)   { maj = 2; min = 12; }
    else if (strncmp(tag, "manylinux1",    10) == 0)   { maj = 2; min = 5;  }
    else return 0;

    if (!c->found || maj * 1000 + min < c->maj * 1000 + c->min) {
        c->found = 1;
        c->maj   = maj;
        c->min   = min;
    }
    return 0;   /* never short-circuit: every member has to be considered */
}

int wheel_manylinux_glibc(const char *platform, int *maj, int *min)
{
    GlibcCtx c = {0, 0, 0};
    for_each_tag(platform, glibc_member, &c);
    if (!c.found)
        return 0;
    *maj = c.maj;
    *min = c.min;
    return 1;
}

/* ── Interpreter and ABI tags ────────────────────────────────────────────── */

/* cp_minor — "cp312" → 12; -1 when `tag` is not a CPython 3.x tag.
 * A trailing 't' ("cp313t") marks the free-threaded build, which is a
 * separate ABI; report it via `*freethreaded`. */
static int cp_minor(const char *tag, int *freethreaded)
{
    if (freethreaded)
        *freethreaded = 0;
    if (strncmp(tag, "cp3", 3) != 0)
        return -1;
    const char *d = tag + 3;
    if (!isdigit((unsigned char)*d))
        return -1;
    int v = atoi(d);
    while (isdigit((unsigned char)*d)) d++;
    if (*d == 't') {
        if (freethreaded)
            *freethreaded = 1;
        d++;
    }
    return *d == '\0' ? v : -1;
}

/* py_minor_tag — "py3" → 0 (any 3.x), "py39" → 9; -1 when not a py3 tag. */
static int py_minor_tag(const char *tag)
{
    if (strncmp(tag, "py3", 3) != 0)
        return -1;
    const char *d = tag + 3;
    if (*d == '\0')
        return 0;
    if (!isdigit((unsigned char)*d))
        return -1;
    int v = atoi(d);
    while (isdigit((unsigned char)*d)) d++;
    return *d == '\0' ? v : -1;
}

typedef struct {
    int target;       /* CPython 3.<target> */
    int want_abi3;    /* the ABI field is "abi3" */
    int abi_none;     /* the ABI field is "none" */
    int abi_cp;       /* cp minor named by the ABI field, or -1 */
    int abi_ft;       /* the ABI field is a free-threaded cp tag */
} PyCtx;

static int py_member_matches(const char *tag, void *vctx)
{
    const PyCtx *c = (const PyCtx *)vctx;

    int ft;
    int cp = cp_minor(tag, &ft);
    if (cp >= 0) {
        if (c->want_abi3)
            return cp <= c->target;         /* stable ABI: floor <= target */
        if (ft || c->abi_ft)
            return 0;                       /* free-threaded: different ABI */
        if (cp != c->target)
            return 0;
        /* ABI must be this interpreter's own, or none. */
        return c->abi_none || c->abi_cp == c->target;
    }

    int py = py_minor_tag(tag);
    if (py >= 0) {
        /* Pure-Python wheels: pip's compatible set for 3.<target> includes
         * py3 and every py3N with N <= target. */
        return (c->abi_none || c->want_abi3) && py <= c->target;
    }

    return 0;   /* pp310, jy27, ip27, … — not this interpreter */
}

int wheel_python_matches(const char *fn, int py_minor)
{
    if (py_minor <= 0)
        return 1;

    WheelTags t;
    if (wheel_parse_tags(fn, &t) != 0)
        return 0;

    int abi_ft = 0;
    PyCtx ctx = {
        .target    = py_minor,
        .want_abi3 = strcmp(t.abi, "abi3") == 0,
        .abi_none  = strcmp(t.abi, "none") == 0,
        .abi_cp    = cp_minor(t.abi, &abi_ft),
        .abi_ft    = 0,
    };
    ctx.abi_ft = abi_ft;

    return for_each_tag(t.python, py_member_matches, &ctx) != 0;
}
