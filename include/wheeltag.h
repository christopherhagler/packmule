/*
 * wheeltag.h — wheel/sdist filename classification and platform-tag matching.
 *
 * Everything a backend needs to decide whether a distribution filename from
 * an index is installable on a target platform: OS + CPU arch gating for
 * wheel platform tags (with the manylinux/musllinux and arm64/aarch64/amd64
 * subtleties), CPython interpreter/abi3 tag matching, and manylinux glibc
 * floors.  Pure string processing, no allocation.
 */

#ifndef PACKMULE_WHEELTAG_H
#define PACKMULE_WHEELTAG_H

/* Distribution filename classification. */
int dist_is_wheel(const char *fn);            /* *.whl                    */
int dist_is_universal_wheel(const char *fn);  /* py3-none-any and friends */
int dist_is_sdist(const char *fn);            /* *.tar.gz or *.zip        */

/*
 * wheel_platform_tag — pointer into `fn` at the start of the platform tag
 * (the last '-'-delimited field before ".whl").  Call only on wheels.
 */
const char *wheel_platform_tag(const char *fn);

/*
 * wheel_platform_matches — 1 if the platform tag is installable on the
 * target.  `os` is "linux", "macos", "windows", or NULL for arch-only
 * matching.  musllinux is rejected for linux targets (glibc assumed);
 * arm64/aarch64 and x86_64/amd64 spellings are treated as equivalent.
 */
int wheel_platform_matches(const char *platform, const char *os,
                           const char *arch);

/*
 * wheel_python_matches — 1 if a non-universal wheel `fn` is installable on
 * CPython 3.<py_minor>: exact interpreter tag ("cp312") or a stable-ABI
 * ("abi3") wheel with a floor <= the target.  py_minor <= 0 disables the
 * check (returns 1).
 */
int wheel_python_matches(const char *fn, int py_minor);

/*
 * wheel_manylinux_glibc — glibc floor required by a manylinux platform tag
 * (manylinux1 → 2.5, manylinux2010 → 2.12, manylinux2014 → 2.17,
 * manylinux_X_Y → X.Y).  Returns 1 and sets the outputs when the tag is
 * manylinux, 0 otherwise (macOS/Windows/musl tags).
 */
int wheel_manylinux_glibc(const char *platform, int *maj, int *min);

#endif /* PACKMULE_WHEELTAG_H */
