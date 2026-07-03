/*
 * pep508.c — PEP 508 dependency-specifier parsing and environment markers.
 *
 * See pep508.h for the grammar handled.  Marker evaluation models the
 * variables that decide bundling for a target platform (sys_platform,
 * platform_system, os_name, python_version/python_full_version) and returns
 * UNKNOWN for everything else, so an unmodelled marker keeps the dependency
 * instead of dropping it.
 */

#include "pep508.h"
#include "utils.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* ── Marker evaluation ───────────────────────────────────────────────────── */

enum { MARK_FALSE = 0, MARK_TRUE = 1, MARK_UNKNOWN = -1 };

/* version_cmp_3x — compare the target CPython "3.<minor>" against a marker
 * value like "3.9" or "3".  Returns <0/0/>0 (target vs value). */
static int version_cmp_3x(int target_minor, const char *value)
{
    int vmaj = 0, vmin = 0;
    sscanf(value, "%d.%d", &vmaj, &vmin);
    if (vmaj != 3) return 3 - vmaj;          /* target major is always 3 */
    return target_minor - vmin;
}

/* eval_clause — evaluate one "VAR OP 'VALUE'" marker clause to a tri-state. */
static int eval_clause(const char *clause, const char *target_os, int py_minor)
{
    while (*clause == ' ' || *clause == '(')
        clause++;

    char   var[32];
    size_t i = 0;
    while (clause[i] && (isalnum((unsigned char)clause[i]) || clause[i] == '_') &&
           i < sizeof(var) - 1)
        var[i] = clause[i], i++;
    var[i] = '\0';

    const char *p = clause + i;
    while (*p == ' ') p++;

    char   op[4] = {0};
    size_t oi    = 0;
    while (oi < 3 && (*p == '=' || *p == '!' || *p == '<' || *p == '>' || *p == '~'))
        op[oi++] = *p++;
    if (oi == 0) return MARK_UNKNOWN;        /* "in" / "not in" etc. */
    while (*p == ' ') p++;

    char quote = *p;
    if (quote != '\'' && quote != '"') return MARK_UNKNOWN;
    p++;
    char   val[64];
    size_t vi = 0;
    while (*p && *p != quote && vi < sizeof(val) - 1)
        val[vi++] = *p++;
    val[vi] = '\0';

    /* String-valued OS markers. */
    const char *tval = NULL;
    if (strcmp(var, "sys_platform") == 0) {
        if (!target_os) return MARK_UNKNOWN;
        tval = strcmp(target_os, "windows") == 0 ? "win32"
             : strcmp(target_os, "macos")   == 0 ? "darwin" : "linux";
    } else if (strcmp(var, "platform_system") == 0) {
        if (!target_os) return MARK_UNKNOWN;
        tval = strcmp(target_os, "windows") == 0 ? "Windows"
             : strcmp(target_os, "macos")   == 0 ? "Darwin" : "Linux";
    } else if (strcmp(var, "os_name") == 0) {
        if (!target_os) return MARK_UNKNOWN;
        tval = strcmp(target_os, "windows") == 0 ? "nt" : "posix";
    }
    if (tval) {
        int eq = (strcmp(tval, val) == 0);
        if (strcmp(op, "==") == 0) return eq ? MARK_TRUE  : MARK_FALSE;
        if (strcmp(op, "!=") == 0) return eq ? MARK_FALSE : MARK_TRUE;
        return MARK_UNKNOWN;
    }

    /* Version-valued markers (CPython 3.x). */
    if (strcmp(var, "python_version") == 0 ||
        strcmp(var, "python_full_version") == 0) {
        if (py_minor <= 0) return MARK_UNKNOWN;
        /* A three-part value ("3.9.7") cannot be decided from a minor alone:
         * target 3.9 vs "< 3.9.7" depends on the patch level we don't know.
         * UNKNOWN keeps the dep rather than risking a wrong exclusion. */
        const char *dot = strchr(val, '.');
        if (dot && strchr(dot + 1, '.'))
            return MARK_UNKNOWN;
        int c = version_cmp_3x(py_minor, val);
        if (strcmp(op, "<")  == 0) return c <  0 ? MARK_TRUE : MARK_FALSE;
        if (strcmp(op, "<=") == 0) return c <= 0 ? MARK_TRUE : MARK_FALSE;
        if (strcmp(op, ">")  == 0) return c >  0 ? MARK_TRUE : MARK_FALSE;
        if (strcmp(op, ">=") == 0) return c >= 0 ? MARK_TRUE : MARK_FALSE;
        if (strcmp(op, "==") == 0) return c == 0 ? MARK_TRUE : MARK_FALSE;
        if (strcmp(op, "!=") == 0) return c != 0 ? MARK_TRUE : MARK_FALSE;
        return MARK_UNKNOWN;
    }

    return MARK_UNKNOWN;                      /* extra, platform_machine, … */
}

/* eval_and_chain — evaluate an "a and b and …" segment (no top-level "or"). */
static int eval_and_chain(const char *s, const char *target_os, int py_minor)
{
    int result = MARK_TRUE;
    while (s && *s) {
        const char *and_pos = strstr(s, " and ");
        size_t      len     = and_pos ? (size_t)(and_pos - s) : strlen(s);
        char        clause[256];
        if (len >= sizeof(clause)) len = sizeof(clause) - 1;
        memcpy(clause, s, len);
        clause[len] = '\0';

        int c = eval_clause(clause, target_os, py_minor);
        if (c == MARK_FALSE)   return MARK_FALSE;       /* short-circuit */
        if (c == MARK_UNKNOWN) result = MARK_UNKNOWN;   /* keep looking for FALSE */
        s = and_pos ? and_pos + 5 : NULL;
    }
    return result;
}

/*
 * marker_eval — tri-state evaluate a marker expression with "and"/"or"
 * ("and" binds tighter).  Parenthesised sub-groups are not modelled; such
 * markers fall back to UNKNOWN (keep), never to a false exclusion.
 */
static int marker_eval(const char *marker, const char *target_os, int py_minor)
{
    int         result = MARK_FALSE;
    const char *s      = marker;
    while (s && *s) {
        const char *or_pos = strstr(s, " or ");
        size_t      len    = or_pos ? (size_t)(or_pos - s) : strlen(s);
        char        term[480];
        if (len >= sizeof(term)) len = sizeof(term) - 1;
        memcpy(term, s, len);
        term[len] = '\0';

        int t = eval_and_chain(term, target_os, py_minor);
        if (t == MARK_TRUE)    return MARK_TRUE;        /* short-circuit */
        if (t == MARK_UNKNOWN) result = MARK_UNKNOWN;
        s = or_pos ? or_pos + 4 : NULL;
    }
    return result;
}

int pep508_marker_excludes(const char *marker, const char *target_os,
                           int py_minor)
{
    if (!marker) return 0;
    while (*marker == ' ' || *marker == ';') marker++;
    if (!*marker) return 0;
    return marker_eval(marker, target_os, py_minor) == MARK_FALSE;
}

int pep508_dep_is_extras_only(const char *spec)
{
    const char *marker = strchr(spec, ';');
    if (!marker)
        return 0;
    return strstr(marker, "extra ==") != NULL ||
           strstr(marker, "extra==")  != NULL;
}

int pep508_marker_matches_extras(const char *marker, const char *extras)
{
    if (!marker || !extras)
        return 0;

    const char *e = extras;
    while (*e) {
        const char *comma = strchr(e, ',');
        size_t      len   = comma ? (size_t)(comma - e) : strlen(e);

        /* PyPI metadata quotes the extra with either ' or ". */
        char pat[96];
        snprintf(pat, sizeof(pat), "extra == '%.*s'", (int)len, e);
        if (strstr(marker, pat))
            return 1;
        snprintf(pat, sizeof(pat), "extra == \"%.*s\"", (int)len, e);
        if (strstr(marker, pat))
            return 1;

        e = comma ? comma + 1 : e + len;
    }
    return 0;
}

/* ── Specifier parts ─────────────────────────────────────────────────────── */

/* Advance past the package name to the first extras/specifier/marker char. */
static const char *skip_name(const char *spec)
{
    while (*spec && !strchr("[(;<>=!~@ \t", *spec))
        spec++;
    return spec;
}

void pep508_spec_name(const char *spec, char *out, size_t out_size)
{
    const char *end = skip_name(spec);
    size_t      i   = 0;
    for (; i < out_size - 1 && spec + i < end; i++)
        out[i] = (char)tolower((unsigned char)spec[i]);
    out[i] = '\0';
}

char *pep508_spec_exact_version(const char *spec)
{
    const char *marker = strchr(spec, ';');
    const char *eq     = strstr(spec, "==");
    if (!eq || (marker && eq > marker))
        return NULL;

    const char *v = eq + 2;
    while (*v == ' ') v++;
    size_t len = 0;
    while (v[len] && v[len] != ' ' && v[len] != ',' && v[len] != ')' &&
           v[len] != ';') {
        /* "==1.*" is a wildcard range, not an exact pin — the caller should
         * resolve it through the constraint path instead. */
        if (v[len] == '*')
            return NULL;
        len++;
    }
    return len > 0 ? pm_strndup(v, len) : NULL;
}

char *pep508_spec_constraint(const char *spec)
{
    const char *p = skip_name(spec);
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '[') {                          /* skip extras */
        const char *close = strchr(p, ']');
        if (!close)
            return NULL;
        p = close + 1;
    }

    char   buf[256];
    size_t k = 0;
    for (; *p && *p != ';' && k < sizeof(buf) - 1; p++) {
        if (*p == '(' || *p == ')' || *p == ' ' || *p == '\t')
            continue;
        buf[k++] = *p;
    }
    buf[k] = '\0';
    if (k == 0 || buf[0] == '@')     /* no specifier, or a direct URL ref */
        return NULL;
    return pm_strdup(buf);
}

char *pep508_spec_extras(const char *spec)
{
    const char *p = skip_name(spec);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '[')
        return NULL;
    const char *close = strchr(p, ']');
    if (!close)
        return NULL;

    char   buf[128];
    size_t k = 0;
    for (p = p + 1; p < close && k < sizeof(buf) - 1; p++)
        if (!isspace((unsigned char)*p))
            buf[k++] = (char)tolower((unsigned char)*p);
    buf[k] = '\0';
    return k ? pm_strdup(buf) : NULL;
}
