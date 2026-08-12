/*
 * pylock.h — Python lockfiles as a fully-resolved download list.
 *
 * packmule's own resolver intersects constraints and takes the highest
 * satisfying version; it does not backtrack, so a requirement set that needs
 * backtracking resolves differently from pip.  A lockfile removes that risk
 * entirely: it is the real resolver's finished answer, complete with URLs and
 * hashes, so there is nothing left to decide.  This is the same bargain the
 * npm backend already makes with package-lock.json.
 *
 * Two formats are read:
 *
 *   uv.lock       uv's native lock.  Locks for EVERY platform at once and
 *                 carries a dependency graph whose edges hold PEP 508 markers,
 *                 so the set of packages a given target actually needs is a
 *                 reachability walk with marker evaluation, not the whole file.
 *
 *   pylock.toml   PEP 751, the standardised interchange lock (uv, pip and
 *                 poetry can all emit it).  Already flattened: there is no
 *                 graph, just a per-package marker, so selection is a filter.
 *
 * Both carry the artifact URL and hash inline, so building the download list
 * costs no network at all.  poetry.lock is deliberately not read here: it
 * records filenames and hashes but no URLs, which would require a
 * per-package index lookup to resolve and is a different kind of operation.
 */

#ifndef PACKMULE_PYLOCK_H
#define PACKMULE_PYLOCK_H

#include "package.h"

typedef enum {
    PYLOCK_KIND_NONE = 0,
    PYLOCK_KIND_UV,       /* uv.lock                        */
    PYLOCK_KIND_PEP751,   /* pylock.toml, pylock.<name>.toml */
} PylockKind;

/*
 * pylock_kind_for_name — which lock format `basename` names, by filename
 * alone.  PEP 751 fixes the name (`pylock.toml` or `pylock.<name>.toml`) and
 * uv fixes `uv.lock`, so the name is authoritative for both.
 */
PylockKind pylock_kind_for_name(const char *basename);

/*
 * pylock_effective — the lockfile a bundle built from `manifest_path` should
 * use: the manifest itself when it already is one, else a `uv.lock` or
 * `pylock.toml` sitting next to a pyproject.toml.
 *
 * A sibling is only looked for next to pyproject.toml.  requirements.txt is
 * itself an explicit list, and silently switching to a lock the user did not
 * name would change what gets bundled without saying so.
 *
 * Returns a heap path the caller frees with pm_free(), or NULL when there is
 * no lock to use.
 */
char *pylock_effective(const char *manifest_path);

/*
 * pylock_parse — read `path` into a PackageList in which every entry is
 * already resolved: version, url, digest and filename are populated and the
 * entries are marked PKG_RESOLVED and user_pinned, so the resolver leaves
 * them alone.
 *
 * `arch`, `target_os` and `py_minor` select which artifact of each package to
 * ship (the same preference order as the rest of the pypi backend: an
 * arch-specific wheel with the lowest manylinux floor, then a universal
 * wheel, then an sdist) and which packages are needed at all.
 *
 * Returns NULL on a parse error, on a lock referencing something that cannot
 * cross an air gap (a git, path or URL source), or when a required package
 * has no artifact installable on the target.  All of those are reported to
 * stderr; none of them are ever downgraded to a warning, because each one
 * yields a bundle that fails on the far side of the wire.
 */
PackageList *pylock_parse(const char *path, const char *arch,
                          const char *target_os, int py_minor);

#endif /* PACKMULE_PYLOCK_H */
