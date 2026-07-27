#include "package.h"
#include "utils.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

#define INITIAL_CAPACITY 16

/* ── Name equality ────────────────────────────────────────────────────────── */

int package_name_equal_pep503(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char ca = (char)tolower((unsigned char)*a);
        char cb = (char)tolower((unsigned char)*b);
        if (ca == '-' || ca == '_' || ca == '.') ca = '-';
        if (cb == '-' || cb == '_' || cb == '.') cb = '-';
        if (ca != cb)
            return 0;
    }
    return *a == '\0' && *b == '\0';
}

int package_name_equal_casefold(const char *a, const char *b)
{
    return strcasecmp(a, b) == 0;
}

int package_name_equal_exact(const char *a, const char *b)
{
    return strcmp(a, b) == 0;
}

/* ── Package ──────────────────────────────────────────────────────────────── */

Package *package_create(const char *name, const char *version)
{
    Package *pkg = pm_calloc(1, sizeof(Package));
    pkg->name    = pm_strdup(name);
    if (version)
        pkg->version = pm_strdup(version);
    pkg->state = PKG_QUEUED;
    return pkg;
}

static void free_dep_specs(char **specs)
{
    if (!specs)
        return;
    for (char **p = specs; *p; p++)
        pm_free(*p);
    pm_free(specs);
}

void package_set_dep_specs(Package *pkg, char **specs)
{
    free_dep_specs(pkg->dep_specs);
    pkg->dep_specs = specs;
}

int package_set_constraint(Package *pkg, char *constraint)
{
    int changed = (constraint == NULL) != (pkg->constraint == NULL) ||
                  (constraint && pkg->constraint &&
                   strcmp(constraint, pkg->constraint) != 0);

    pm_free(pkg->constraint);
    pkg->constraint = constraint;

    if (changed && pkg->state == PKG_RESOLVED)
        pkg->dirty = 1;
    return changed;
}

/* has_extra — is `token` (length `len`) already one of `list`'s comma-
 * separated entries?  Whole-token comparison: "sec" must not match inside
 * "security". */
static int has_extra(const char *list, const char *token, size_t len)
{
    if (!list)
        return 0;
    for (const char *p = list; *p; ) {
        const char *comma = strchr(p, ',');
        size_t      n     = comma ? (size_t)(comma - p) : strlen(p);
        if (n == len && strncasecmp(p, token, len) == 0)
            return 1;
        if (!comma)
            break;
        p = comma + 1;
    }
    return 0;
}

int package_add_extras(Package *pkg, const char *extras)
{
    if (!extras || !*extras)
        return 0;

    int added = 0;
    for (const char *p = extras; *p; ) {
        const char *comma = strchr(p, ',');
        size_t      len   = comma ? (size_t)(comma - p) : strlen(p);

        if (len > 0 && !has_extra(pkg->extras, p, len)) {
            char *merged = pkg->extras
                ? pm_asprintf("%s,%.*s", pkg->extras, (int)len, p)
                : pm_strndup(p, len);
            pm_free(pkg->extras);
            pkg->extras = merged;
            added = 1;
        }

        if (!comma)
            break;
        p = comma + 1;
    }

    /* Extras decide which dependencies get followed, so gaining one after
     * resolution means the dependency walk has to run again. */
    if (added && pkg->state == PKG_RESOLVED)
        pkg->dirty = 1;
    return added;
}

void package_destroy(Package *pkg)
{
    if (!pkg)
        return;
    pm_free(pkg->name);
    pm_free(pkg->version);
    pm_free(pkg->constraint);
    pm_free(pkg->extras);
    pm_free(pkg->url);
    pm_free(pkg->filename);
    digest_clear(&pkg->digest);
    free_dep_specs(pkg->dep_specs);
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

Package *package_list_find_name(const PackageList *list, const char *name,
                                package_name_equal_fn eq)
{
    if (!eq)
        eq = package_name_equal_exact;
    for (size_t i = 0; i < list->count; i++)
        if (eq(list->items[i]->name, name))
            return list->items[i];
    return NULL;
}

int package_list_contains_name(const PackageList *list, const char *name,
                               package_name_equal_fn eq)
{
    return package_list_find_name(list, name, eq) != NULL;
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
