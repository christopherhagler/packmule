/*
 * resolve.h — the dependency resolution loop.
 *
 * Resolution is a fixpoint, not a single pass.  Walking the queue once and
 * resolving each entry as it is reached makes the result depend on the order
 * packages happen to appear: a constraint discovered by a later dependent
 * arrives after the package it constrains has already been resolved, and the
 * only options left are to ignore it or to warn.  Listing "urllib3" above
 * "requests==2.20.0" instead of below it produced a bundle pip refuses to
 * install, and packmule reported success.
 *
 * So instead: resolve, let get_deps record every requirement it discovers,
 * and go round again for any package whose requirements changed.  Constraints
 * only ever accumulate and the per-registry merges are idempotent, so the
 * requirement set reaches a fixed point and the loop terminates — bounded
 * anyway by RESOLVE_MAX_ROUNDS as a backstop against a misbehaving backend.
 */

#ifndef PACKMULE_RESOLVE_H
#define PACKMULE_RESOLVE_H

#include "package.h"
#include "registry.h"

/* Safety net; a real tree settles in two or three rounds. */
#define RESOLVE_MAX_ROUNDS 16

typedef struct {
    size_t resolved;      /* packages in PKG_RESOLVED */
    size_t failed;        /* packages in PKG_FAILED */
    int    rounds;        /* rounds actually executed */
    int    hit_round_cap; /* 1 if the loop stopped at RESOLVE_MAX_ROUNDS */
    int    fatal;         /* 1 if a backend reported an unbundleable dep */
} ResolveStats;

/*
 * resolve_all — drive `list` to a fixed point against `reg`.
 *
 * `list` grows as transitive dependencies are discovered.  Progress is
 * reported through `on_progress` (may be NULL), called with the package about
 * to be resolved and the current queue length.
 *
 * Returns 0 when every package resolved, -1 when any failed or a dependency
 * could not be bundled at all.  Per-package errors are printed by the
 * backends; the summary is written to `stats` (may be NULL).
 */
int resolve_all(const Registry *reg, PackageList *list,
                void (*on_progress)(const Package *pkg, size_t index,
                                    size_t total, int round),
                ResolveStats *stats);

#endif /* PACKMULE_RESOLVE_H */
