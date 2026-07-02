/*
 * semver.h — SemVer 2.0 comparison and node-semver range matching.
 *
 * Used by the npm backend to resolve dependency ranges ("^4.18.0", "~1.2.3",
 * ">=2 <3", "1.x", "^16 || ^17 || ^18") against the version list in a
 * registry packument, the way npm itself does.
 *
 * Supported range syntax: exact versions, "^", "~", comparators
 * (">=", "<=", ">", "<", "="), x-ranges ("1.x", "1.2.*", "*"), bare partial
 * versions ("1", "1.2"), hyphen ranges ("1.2.3 - 2.0.0"), space-separated
 * AND, and "||" OR.
 *
 * Prerelease policy: a version carrying a prerelease tag (e.g. 2.0.0-rc.1)
 * only satisfies a range when some comparator in the matching clause names a
 * prerelease of the same major.minor.patch — mirroring node-semver, so a
 * range like "^1.0.0" never silently pulls in a prerelease.
 */

#ifndef PACKMULE_SEMVER_H
#define PACKMULE_SEMVER_H

/*
 * semver_cmp — compare two version strings by SemVer 2.0 precedence
 * (numeric major.minor.patch, then prerelease identifiers; a prerelease
 * sorts BELOW its release).  Tolerates a leading 'v' and partial versions
 * ("1.2" == "1.2.0").  Falls back to strcmp() ordering when either string
 * is not a version at all.
 *
 * Returns <0, 0, >0 for a < b, a == b, a > b.
 */
int semver_cmp(const char *a, const char *b);

/*
 * semver_satisfies — does `version` satisfy the node-semver `range`?
 *
 * Returns 1 (satisfies), 0 (does not satisfy), or -1 when the range is not
 * parseable as semver at all (git URL, "latest", "file:…") — callers may
 * treat -1 as "resolve to latest" with a warning.
 */
int semver_satisfies(const char *version, const char *range);

#endif /* PACKMULE_SEMVER_H */
