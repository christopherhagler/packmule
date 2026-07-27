/*
 * wheeltag.h — wheel filename parsing and target compatibility.
 *
 * A wheel filename is
 *
 *   {distribution}-{version}(-{build})?-{python}-{abi}-{platform}.whl
 *
 * and each of the last three fields may be a '.'-separated compressed tag set
 * ("py2.py3", "manylinux_2_17_x86_64.manylinux2014_x86_64").  Everything a
 * backend needs to decide whether such a file is installable on a target
 * platform lives here: parsing the three tag fields, OS + CPU gating for
 * platform tags (with the manylinux/musllinux and arm64/aarch64/amd64
 * subtleties), interpreter and ABI matching, and manylinux glibc floors.
 *
 * Pure string processing, no allocation, so it is cheap to call per file in a
 * PyPI response and trivial to unit-test.
 */

#ifndef PACKMULE_WHEELTAG_H
#define PACKMULE_WHEELTAG_H

/* Parsed tag fields of a wheel filename, each without the ".whl" suffix. */
typedef struct {
    char python[64];    /* e.g. "cp312", "py2.py3"          */
    char abi[64];       /* e.g. "cp312", "abi3", "none"     */
    char platform[192]; /* e.g. "manylinux_2_17_x86_64", "any" */
} WheelTags;

/* ── Filename classification ─────────────────────────────────────────────── */

int dist_is_wheel(const char *fn);   /* *.whl              */
int dist_is_sdist(const char *fn);   /* *.tar.gz or *.zip  */

/*
 * wheel_parse_tags — split a wheel filename into its python/abi/platform tag
 * fields.  Returns 0 on success, -1 when `fn` is not a well-formed wheel name
 * (too few '-'-separated fields, or a field longer than its buffer).
 */
int wheel_parse_tags(const char *fn, WheelTags *out);

/*
 * dist_is_universal_wheel — a pure-Python wheel that installs anywhere, i.e.
 * one whose PLATFORM tag is "any".
 *
 * This is deliberately a property of the platform tag rather than a list of
 * known interpreter tags: "py39-none-any" and "py3-none-any" are both
 * universal, and hardcoding the latter caused the former to be rejected
 * outright and silently replaced by a source distribution.
 */
int dist_is_universal_wheel(const char *fn);

/*
 * wheel_platform_matches — 1 if the platform tag field is installable on the
 * target.  `os` is "linux", "macos", "windows", or NULL for arch-only
 * matching.  Compressed tag sets match if any member matches.  musllinux is
 * rejected for linux targets (glibc assumed); arm64/aarch64 and x86_64/amd64
 * spellings are treated as equivalent.
 */
int wheel_platform_matches(const char *platform, const char *os,
                           const char *arch);

/*
 * wheel_python_matches — 1 if the wheel's interpreter and ABI tags are
 * installable on CPython 3.<py_minor>:
 *
 *   - a pure-Python tag ("py3", or "py3N" with N <= py_minor) with ABI "none"
 *   - the exact interpreter tag "cp3<py_minor>" with a matching or "none" ABI
 *   - a stable-ABI wheel ("abi3") whose interpreter floor is <= py_minor
 *
 * Free-threaded builds ("cp313t") are a distinct, incompatible ABI and are
 * rejected for a normal CPython target; so are other interpreters ("pp310").
 * py_minor <= 0 disables the check (returns 1).
 */
int wheel_python_matches(const char *fn, int py_minor);

/*
 * wheel_manylinux_glibc — lowest glibc floor required by a platform tag field
 * (manylinux1 → 2.5, manylinux2010 → 2.12, manylinux2014 → 2.17,
 * manylinux_X_Y → X.Y).  For a compressed set the LOWEST floor across members
 * wins, since satisfying that one is enough.  Returns 1 and sets the outputs
 * when any member is manylinux, 0 otherwise (macOS/Windows/musl/any tags).
 */
int wheel_manylinux_glibc(const char *platform, int *maj, int *min);

#endif /* PACKMULE_WHEELTAG_H */
