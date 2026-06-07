#include "package.h"
#include "utils.h"

#include <ctype.h>
#include <string.h>

#define INITIAL_CAPACITY 16

/* ── Package ──────────────────────────────────────────────────────────────── */

Package *package_create(const char *name, const char *version)
{
    Package *pkg = pm_calloc(1, sizeof(Package));
    pkg->name    = pm_strdup(name);
    if (version)
        pkg->version = pm_strdup(version);
    /* url, sha256, filename remain NULL until populated by parse_pypi_response */
    return pkg;
}

void package_destroy(Package *pkg)
{
    if (!pkg)
        return;
    pm_free(pkg->name);
    pm_free(pkg->version);
    pm_free(pkg->url);
    pm_free(pkg->sha256);
    pm_free(pkg->filename);
    if (pkg->requires_dist) {
        for (char **p = pkg->requires_dist; *p; p++)
            pm_free(*p);
        pm_free(pkg->requires_dist);
    }
    pm_free(pkg);
}

/* ── PackageList ──────────────────────────────────────────────────────────── */

PackageList *package_list_create(void)
{
    PackageList *list = pm_calloc(1, sizeof(PackageList));
    list->items    = pm_malloc(INITIAL_CAPACITY * sizeof(Package *));
    list->capacity = INITIAL_CAPACITY;
    list->count    = 0;
    return list;
}

int package_list_add(PackageList *list, Package *pkg)
{
    if (list->count >= list->capacity) {
        size_t new_cap = list->capacity * 2;
        list->items    = pm_realloc(list->items, new_cap * sizeof(Package *));
        list->capacity = new_cap;
    }
    list->items[list->count++] = pkg;
    return 0;
}

int package_list_contains(const PackageList *list,
                           const char       *name,
                           const char       *version)
{
    for (size_t i = 0; i < list->count; i++) {
        const Package *p = list->items[i];
        if (strcmp(p->name, name) != 0)
            continue;
        /* If version is NULL we match any version of this package. */
        if (version == NULL || p->version == NULL)
            return 1;
        if (strcmp(p->version, version) == 0)
            return 1;
    }
    return 0;
}

int package_list_contains_name(const PackageList *list, const char *name)
{
    for (size_t i = 0; i < list->count; i++) {
        const char *a = list->items[i]->name;
        const char *b = name;
        for (; *a && *b; a++, b++) {
            /* PyPI normalizes '-', '_', and '.' as equivalent (PEP 503). */
            char ca = (char)tolower((unsigned char)*a);
            char cb = (char)tolower((unsigned char)*b);
            if (ca == '-' || ca == '_' || ca == '.') ca = '-';
            if (cb == '-' || cb == '_' || cb == '.') cb = '-';
            if (ca != cb)
                goto next;
        }
        if (*a == '\0' && *b == '\0')
            return 1;
next:;
    }
    return 0;
}

void package_list_destroy(PackageList *list)
{
    if (!list)
        return;
    for (size_t i = 0; i < list->count; i++)
        package_destroy(list->items[i]);
    pm_free(list->items);
    pm_free(list);
}
