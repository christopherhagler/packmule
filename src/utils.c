#include "utils.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

char *pm_strtrim(char *s)
{
    while (isspace((unsigned char)*s))
        ++s;
    if (*s) {
        char *end = s + strlen(s) - 1;
        while (end > s && isspace((unsigned char)*end))
            *end-- = '\0';
    }
    return s;
}

char *pm_asprintf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int len = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (len < 0) {
        va_end(ap2);
        const char msg[] = "packmule: fatal: string formatting error\n";
        fwrite(msg, 1, sizeof(msg) - 1, stderr);
        abort();
    }

    char *out = pm_malloc((size_t)len + 1);
    vsnprintf(out, (size_t)len + 1, fmt, ap2);
    va_end(ap2);
    return out;
}

void pm_human_size(double bytes, char *buf, size_t bufsz)
{
    static const char *const units[] = { "B", "KB", "MB", "GB", "TB" };
    int u = 0;
    while (bytes >= 1024.0 && u < 4) {
        bytes /= 1024.0;
        u++;
    }
    snprintf(buf, bufsz, u == 0 ? "%.0f %s" : "%.1f %s", bytes, units[u]);
}

const char *pm_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

int pm_mkdir_p(const char *path, unsigned int mode)
{
    if (path[0] == '\0')
        return 0;

    char buf[4096];
    int  n = snprintf(buf, sizeof(buf), "%s", path);
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    /* Create each intermediate component, then the full path. */
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(buf, (mode_t)mode) != 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    if (mkdir(buf, (mode_t)mode) != 0 && errno != EEXIST)
        return -1;
    return 0;
}
