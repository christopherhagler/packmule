/*
 * pep508.h — PEP 508 dependency-specifier parsing and environment markers.
 *
 * A PEP 508 spec is what appears in a package's requires_dist metadata, e.g.
 *
 *   urllib3 (>=1.21.1,<3)
 *   cryptography>=1.3.4 ; extra == 'security'
 *   backports.zoneinfo ; python_version < "3.9"
 *
 * This module extracts the parts (name, extras, exact pin, range constraint)
 * and evaluates the environment marker after ';' against a target platform.
 *
 * Marker evaluation is tri-state: TRUE, FALSE, or UNKNOWN for variables and
 * operators not modelled (platform_machine, "in", parenthesised groups, …).
 * UNKNOWN never excludes a dependency — when in doubt the dep is kept rather
 * than risking a broken bundle.
 */

#ifndef PACKMULE_PEP508_H
#define PACKMULE_PEP508_H

#include <stddef.h>

/* ── Environment markers ─────────────────────────────────────────────────── */

/*
 * pep508_marker_excludes — 1 if `marker` (the text at/after ';') positively
 * rules the package OUT for the target environment.  Returns 0 to keep it,
 * including when the marker is absent or cannot be fully evaluated.
 * `target_os` is "linux", "macos", "windows", or NULL (unknown); `py_minor`
 * is the target CPython 3.x minor, or <= 0 for unknown.
 */
int pep508_marker_excludes(const char *marker, const char *target_os,
                           int py_minor);

/*
 * pep508_dep_is_extras_only — 1 if the spec is gated by an extras marker
 * ("; extra == 'foo'").  Such deps are optional and must only be followed
 * when the parent was requested with a matching extra.
 */
int pep508_dep_is_extras_only(const char *spec);

/*
 * pep508_marker_matches_extras — 1 if the marker's "extra == '<name>'"
 * clause names one of the comma-separated `extras`.  Both quote styles used
 * in PyPI metadata (' and ") are recognised.  NULL-safe on both arguments.
 */
int pep508_marker_matches_extras(const char *marker, const char *extras);

/* ── Specifier parts ─────────────────────────────────────────────────────── */

/*
 * pep508_spec_name — copy the bare package name from `spec` into `out`
 * (lowercased, so case-insensitive dedup works), stopping at extras,
 * version specifiers, markers, or whitespace.
 */
void pep508_spec_name(const char *spec, char *out, size_t out_size);

/*
 * pep508_spec_exact_version — heap copy of an exact "==" pin ("2.18.2"), or
 * NULL when the spec is unpinned, a range, or a wildcard ("==1.*").
 */
char *pep508_spec_exact_version(const char *spec);

/*
 * pep508_spec_constraint — heap copy of the full version specifier as a
 * compact PEP 440 constraint string ("urllib3 (>=1.21.1,<3)" → ">=1.21.1,<3"),
 * or NULL when there is none (or the spec is a direct URL reference).
 */
char *pep508_spec_constraint(const char *spec);

/*
 * pep508_spec_extras — heap copy of the extras list ("foo[Bar, baz]" →
 * "bar,baz", lowercased and space-free), or NULL when there are none.
 */
char *pep508_spec_extras(const char *spec);

#endif /* PACKMULE_PEP508_H */
