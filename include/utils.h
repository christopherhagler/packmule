/*
 * utils.h — Memory allocation wrappers and string helpers.
 *
 * All pm_malloc/pm_calloc/pm_realloc variants abort() on OOM rather than
 * returning NULL.  This is intentional: packmule is a CLI tool, not a
 * library.  Callers do not need to check return values of these wrappers.
 *
 * Ownership convention used project-wide:
 *   - Functions that allocate and return a pointer transfer ownership to the
 *     caller.  The caller is responsible for calling pm_free().
 *   - Functions that return a pointer into an existing buffer do NOT transfer
 *     ownership; the caller must not free the result.
 *   - NULL parameters are accepted only where explicitly documented.
 */

#ifndef PACKMULE_UTILS_H
#define PACKMULE_UTILS_H

#include <stddef.h>

/* Allocate `size` bytes.  Aborts on OOM.  Never returns NULL. */
void *pm_malloc(size_t size);

/* Allocate zeroed array of `nmemb` × `size` bytes.  Aborts on OOM. */
void *pm_calloc(size_t nmemb, size_t size);

/*
 * Resize allocation at `ptr` to `size` bytes.  Aborts on OOM.
 * NOTE: if realloc fails internally, the original `ptr` is lost (not freed)
 * before abort() is called — acceptable because abort() exits the process.
 */
void *pm_realloc(void *ptr, size_t size);

/* Duplicate `s`.  Caller owns the result and must pm_free() it. */
char *pm_strdup(const char *s);

/*
 * Duplicate at most `n` bytes of `s`, always NUL-terminating.
 * Caller owns the result and must pm_free() it.
 */
char *pm_strndup(const char *s, size_t n);

/* Free memory allocated by any pm_* allocator.  Safe to call with NULL. */
void pm_free(void *ptr);

#endif /* PACKMULE_UTILS_H */
