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

const Registry *registry_detect(const char *path)
{
    /* Reduce the path to its basename: the part after the final '/'. */
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;

    for (int i = 0; REGISTRY_TABLE[i] != NULL; i++) {
        const Registry *reg = REGISTRY_TABLE[i];
        if (reg->detect && reg->detect(base))
            return reg;
    }
    return NULL;
}

const char *const *registry_names(void)
{
    /* Derive the name list from REGISTRY_TABLE on first use so the two can
     * never drift apart when a backend is added or removed. */
    static const char *names[sizeof(REGISTRY_TABLE) / sizeof(REGISTRY_TABLE[0])];
    static int built = 0;

    if (!built) {
        size_t i;
        for (i = 0; REGISTRY_TABLE[i] != NULL; i++)
            names[i] = REGISTRY_TABLE[i]->name;
        names[i] = NULL;
        built = 1;
    }
    return names;
}
