/*
 * resolve.c — fixpoint dependency resolution.  See resolve.h for why.
 */

#include "resolve.h"
#include "utils.h"

#include <stdio.h>

/*
 * needs_work — should this package be (re)resolved on this round?
 *
 * A queued package obviously needs resolving.  A resolved one needs it again
 * only when a dependent has since widened what it must satisfy, which the
 * backends signal by setting `dirty` from package_set_constraint() /
 * package_add_extras().  A failed one is left alone: retrying it every round
 * would just repeat the same network error.
 */
static int needs_work(const Package *pkg)
{
    if (pkg->state == PKG_FAILED)
        return 0;
    return pkg->state == PKG_QUEUED || pkg->dirty;
}

int resolve_all(const Registry *reg, PackageList *list,
                void (*on_progress)(const Package *pkg, size_t index,
                                    size_t total, int round),
                ResolveStats *stats)
{
    ResolveStats st = {0};
    int fatal = 0;

    for (st.rounds = 1; st.rounds <= RESOLVE_MAX_ROUNDS; st.rounds++) {
        int worked = 0;

        /*
         * `list->count` is re-read each iteration on purpose: get_deps appends
         * newly discovered dependencies to the same list, and they are picked
         * up within this round rather than waiting for the next one.
         */
        for (size_t i = 0; i < list->count; i++) {
            Package *pkg = list->items[i];
            if (!needs_work(pkg))
                continue;

            /* Clear before resolving: get_deps may legitimately re-dirty this
             * same package (a diamond where a package constrains itself
             * through a cycle), and that must not be lost. */
            pkg->dirty = 0;
            pkg->resolve_count++;
            worked = 1;

            if (on_progress)
                on_progress(pkg, i, list->count, st.rounds);

            if (reg->resolve(reg, pkg) != 0) {
                pkg->state = PKG_FAILED;
                continue;
            }
            pkg->state = PKG_RESOLVED;

            /*
             * A negative return means a dependency that cannot be bundled at
             * all (an npm git or URL dep).  There is no version of the bundle
             * that would work, so stop rather than spend the remaining rounds
             * building something known-broken.
             */
            if (reg->get_deps && reg->get_deps(reg, pkg, list, list) < 0) {
                fatal = 1;
                goto done;
            }
        }

        if (!worked)
            break;      /* fixed point: nothing left queued or dirty */
    }

    if (st.rounds > RESOLVE_MAX_ROUNDS) {
        st.rounds = RESOLVE_MAX_ROUNDS;
        st.hit_round_cap = 1;
        fprintf(stderr,
                "packmule: dependency resolution did not settle after %d "
                "rounds.\n"
                "          This is a packmule bug; please report the manifest "
                "that triggered it.\n", RESOLVE_MAX_ROUNDS);
    }

done:
    st.fatal = fatal;
    for (size_t i = 0; i < list->count; i++) {
        /* Anything not resolved counts against us.  A still-QUEUED entry is
         * only reachable when the fatal path cut the loop short, and an
         * unresolved package is a hole in the bundle either way. */
        if (list->items[i]->state == PKG_RESOLVED)
            st.resolved++;
        else
            st.failed++;
    }

    if (stats)
        *stats = st;

    return (fatal || st.failed > 0 || st.hit_round_cap) ? -1 : 0;
}
