#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Internal helpers ─────────────────────────────────────────────────────── */

static void oom_abort(void)
{
    /* Writing to stderr is async-signal-safe enough for a fatal OOM path. */
    const char msg[] = "packmule: fatal: out of memory\n";
    fwrite(msg, 1, sizeof(msg) - 1, stderr);
    abort();
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void *pm_malloc(size_t size)
{
    void *ptr = malloc(size);
    if (!ptr)
        oom_abort();
    return ptr;
}

void *pm_calloc(size_t nmemb, size_t size)
{
    void *ptr = calloc(nmemb, size);
    if (!ptr)
        oom_abort();
    return ptr;
}

void *pm_realloc(void *old_ptr, size_t size)
{
    void *ptr = realloc(old_ptr, size);
    if (!ptr)
        oom_abort(); /* old_ptr is leaked but we are aborting anyway */
    return ptr;
}

char *pm_strdup(const char *s)
{
    size_t len = strlen(s);
    char  *copy = pm_malloc(len + 1);
    memcpy(copy, s, len + 1);
    return copy;
}

char *pm_strndup(const char *s, size_t n)
{
    /* Compute the actual length without relying on strnlen (POSIX, not C11). */
    size_t len = 0;
    while (len < n && s[len] != '\0')
        ++len;

    char *copy = pm_malloc(len + 1);
    memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

void pm_free(void *ptr)
{
    free(ptr);
}
