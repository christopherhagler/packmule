/*
 * pep440.h — PEP 440 version comparison and specifier matching (pypi backend).
 *
 * Implements the pragmatic subset of PEP 440 needed to resolve version-range
 * requirements against PyPI release lists:
 *
 *   Versions:   [v][EPOCH!]N(.N)*[{a|b|rc|c}N][.postN][.devN][+local]
 *               Separators '.', '-', '_' are accepted between qualifiers, and
 *               "alpha"/"beta"/"pre"/"preview"/"rev"/"r" spellings normalise
 *               to a/b/rc/post per the spec.  Local version labels compare
 *               equal (they are ignored).
 *
 *   Specifiers: comma-separated AND clauses using ==, !=, <=, >=, <, >, ~=,
 *               ===, including trailing-".*" wildcards on == and !=.
 *
 * Anything outside this subset parses as "unknown" and is reported rather
 * than guessed at, so a caller can fall back safely.
 */

#ifndef PACKMULE_PEP440_H
#define PACKMULE_PEP440_H

/* Returns 1 if `v` parses as a PEP 440 version, 0 otherwise. */
int pep440_valid(const char *v);

/*
 * Compare two versions per PEP 440 ordering (epoch, release, dev < a < b < rc
 * < final < post).  Returns <0, 0, >0.  Falls back to strcmp when either side
 * does not parse, so the order is at least stable.
 */
int pep440_cmp(const char *a, const char *b);

/* Returns 1 if `v` is a pre-release or dev release (pip excludes these from
 * range resolution by default), 0 for a final/post release or unparseable. */
int pep440_is_prerelease(const char *v);

/*
 * pep440_satisfies — does `version` satisfy every clause of `spec`
 * (e.g. ">=1.21.1,<3")?  Returns 1 yes, 0 no, -1 when the version or any
 * clause cannot be parsed (caller decides the fallback).
 * An empty/NULL spec is satisfied by anything.
 */
int pep440_satisfies(const char *version, const char *spec);

/* Returns 1 if any clause operand in `spec` is itself a pre-release, which
 * per pip's rules admits pre-release candidates for the whole requirement. */
int pep440_spec_admits_prerelease(const char *spec);

#endif /* PACKMULE_PEP440_H */
