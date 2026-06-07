#include "registry.h"

#include <string.h>

/* ── Backend declarations ────────────────────────────────────────────────────
 *
 * Each backend defines one static Registry instance in its own .c file.
 * Declare it here so the dispatch table below can reference it without
 * exposing a per-backend header to the rest of the codebase.
 */
extern const Registry pypi_registry;
extern const Registry npm_registry;
extern const Registry rpm_registry;

/* ── Dispatch table ───────────────────────────────────────────────────────── */

static const Registry * const REGISTRY_TABLE[] = {
    &pypi_registry,
    &npm_registry,
    &rpm_registry,
    NULL /* sentinel */
};

/* ── Public API ───────────────────────────────────────────────────────────── */

const Registry *registry_find(const char *name)
{
    for (int i = 0; REGISTRY_TABLE[i] != NULL; i++) {
        if (strcmp(REGISTRY_TABLE[i]->name, name) == 0)
            return REGISTRY_TABLE[i];
    }
    return NULL;
}

const char *const *registry_names(void)
{
    /* Statically built from the same order as REGISTRY_TABLE. */
    static const char * const names[] = {
        "pypi",
        "npm",
        "rpm",
        NULL
    };
    return names;
}
