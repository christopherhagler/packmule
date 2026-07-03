/*
 * pep440.c — PEP 440 version ordering and specifier matching.
 *
 * Pure string processing, no allocation: cheap to call in a loop over every
 * release key of a PyPI JSON document.  See pep440.h for the supported
 * grammar.  The ordering key mirrors pypa/packaging's `_cmpkey`:
 *
 *   (epoch, release, pre, post, dev)
 *
 * with dev-only releases sorting below pre-releases, pre-releases below the
 * final release, and post releases above it.
 */

#include "pep440.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define PEP440_MAX_REL 8

typedef struct {
    long epoch;
    long rel[PEP440_MAX_REL];
    int  n;          /* release components present (1..PEP440_MAX_REL) */
    int  pre_rank;   /* -1 dev-only, 1 a, 2 b, 3 rc, 99 no pre-release */
    long pre_num;
    long post;       /* -1 when absent (sorts below any present post) */
    long dev;        /* LONG_MAX when absent (sorts above any present dev) */
} Pep440;

/* ── Parser ──────────────────────────────────────────────────────────────── */

/* Case-insensitive "does `s` start with `word`"; returns the length matched. */
static size_t word_at(const char *s, const char *word)
{
    size_t n = strlen(word);
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)s[i]) != word[i])
            return 0;
    return n;
}

/*
 * pep440_parse — parse `s` into `v`.  Returns 0 on success, -1 when the
 * string is not a PEP 440 version.  A trailing "+local" label is ignored.
 */
static int pep440_parse(const char *s, Pep440 *v)
{
    memset(v, 0, sizeof(*v));
    v->pre_rank = 99;
    v->post     = -1;
    v->dev      = LONG_MAX;

    while (*s == ' ' || *s == '\t') s++;
    if (*s == 'v' || *s == 'V') s++;

    if (!isdigit((unsigned char)*s))
        return -1;

    /* Optional epoch: digits followed by '!'. */
    {
        char *end;
        long  first = strtol(s, &end, 10);
        if (*end == '!') {
            v->epoch = first;
            s = end + 1;
            if (!isdigit((unsigned char)*s))
                return -1;
        }
    }

    /* Release segment: N(.N)* */
    for (v->n = 0; v->n < PEP440_MAX_REL; ) {
        char *end;
        v->rel[v->n++] = strtol(s, &end, 10);
        s = end;
        if (*s == '.' && isdigit((unsigned char)s[1])) {
            s++;
            continue;
        }
        break;
    }
    if (v->n == PEP440_MAX_REL && *s == '.' && isdigit((unsigned char)s[1]))
        return -1;                       /* absurdly long release tuple */

    /* Qualifier segments: pre / post / dev, in any of PEP 440's spellings. */
    int have_pre = 0;
    while (*s && *s != '+') {
        int implicit_post = (*s == '-');           /* "1.0-1" → post 1 */
        while (*s == '.' || *s == '-' || *s == '_') s++;
        if (!*s || *s == '+')
            break;

        int  rank = 0;
        long *slot = NULL;
        size_t w;
        if      ((w = word_at(s, "alpha"))   ) rank = 1;
        else if ((w = word_at(s, "beta"))    ) rank = 2;
        else if ((w = word_at(s, "preview")) ) rank = 3;
        else if ((w = word_at(s, "pre"))     ) rank = 3;
        else if ((w = word_at(s, "rc"))      ) rank = 3;
        else if ((w = word_at(s, "post"))    ) slot = &v->post;
        else if ((w = word_at(s, "rev"))     ) slot = &v->post;
        else if ((w = word_at(s, "dev"))     ) slot = &v->dev;
        else if ((w = word_at(s, "a"))       ) rank = 1;
        else if ((w = word_at(s, "b"))       ) rank = 2;
        else if ((w = word_at(s, "c"))       ) rank = 3;
        else if ((w = word_at(s, "r"))       ) slot = &v->post;
        else if (implicit_post && isdigit((unsigned char)*s)) {
            w = 0;
            slot = &v->post;
        }
        else return -1;                             /* unknown qualifier */
        s += w;

        /* The qualifier's number may be glued ("b2") or separated by one
         * '.'/'-'/'_' ("beta.2", "post-1"); both normalise identically. */
        const char *digits = s;
        if ((*digits == '.' || *digits == '-' || *digits == '_') &&
            isdigit((unsigned char)digits[1]))
            digits++;
        long num = 0;
        if (isdigit((unsigned char)*digits)) {
            char *end;
            num = strtol(digits, &end, 10);
            s = end;
        } else if (slot == &v->post && w == 0) {
            return -1;                              /* bare '-' with no digits */
        }

        if (slot) {
            *slot = num;
        } else {
            if (have_pre)
                return -1;                          /* two pre segments */
            have_pre    = 1;
            v->pre_rank = rank;
            v->pre_num  = num;
        }
    }

    /* Dev release with no pre-release sorts below every pre-release. */
    if (!have_pre && v->dev != LONG_MAX && v->post == -1)
        v->pre_rank = -1;

    return 0;                                       /* "+local" tail ignored */
}

/* ── Ordering ────────────────────────────────────────────────────────────── */

static long rel_at(const Pep440 *v, int i)
{
    return i < v->n ? v->rel[i] : 0;                /* zero-pad short tuples */
}

static int cmp_long(long a, long b)
{
    return (a > b) - (a < b);
}

static int pep440_cmp_parsed(const Pep440 *a, const Pep440 *b)
{
    int rc;
    if ((rc = cmp_long(a->epoch, b->epoch)) != 0) return rc;
    for (int i = 0; i < PEP440_MAX_REL; i++)
        if ((rc = cmp_long(rel_at(a, i), rel_at(b, i))) != 0) return rc;
    if ((rc = cmp_long(a->pre_rank, b->pre_rank)) != 0) return rc;
    if (a->pre_rank != 99 && a->pre_rank != -1 &&
        (rc = cmp_long(a->pre_num, b->pre_num)) != 0) return rc;
    if ((rc = cmp_long(a->post, b->post)) != 0) return rc;
    return cmp_long(a->dev, b->dev);
}

int pep440_valid(const char *v)
{
    Pep440 p;
    return pep440_parse(v, &p) == 0;
}

int pep440_cmp(const char *a, const char *b)
{
    Pep440 va, vb;
    if (pep440_parse(a, &va) != 0 || pep440_parse(b, &vb) != 0)
        return strcmp(a, b);
    return pep440_cmp_parsed(&va, &vb);
}

int pep440_is_prerelease(const char *v)
{
    Pep440 p;
    if (pep440_parse(v, &p) != 0)
        return 0;
    return (p.pre_rank != 99 && p.post == -1) || p.dev != LONG_MAX;
}

/* ── Specifier matching ──────────────────────────────────────────────────── */

/*
 * clause_satisfied — evaluate one specifier clause against parsed version
 * `v` (with `vstr` available for "===").  Returns 1/0, or -1 unparseable.
 */
static int clause_satisfied(const Pep440 *v, const char *vstr,
                            const char *clause, size_t len)
{
    while (len > 0 && (*clause == ' ' || *clause == '\t')) clause++, len--;
    while (len > 0 && (clause[len - 1] == ' ' || clause[len - 1] == '\t')) len--;
    if (len == 0)
        return -1;

    char op[4] = {0};
    size_t oi = 0;
    while (oi < 3 && oi < len &&
           (clause[oi] == '=' || clause[oi] == '!' ||
            clause[oi] == '<' || clause[oi] == '>' || clause[oi] == '~'))
        op[oi] = clause[oi], oi++;

    const char *operand = clause + oi;
    size_t      olen    = len - oi;
    while (olen > 0 && (*operand == ' ' || *operand == '\t')) operand++, olen--;
    if (olen == 0 || olen >= 64)
        return -1;

    char ostr[64];
    memcpy(ostr, operand, olen);
    ostr[olen] = '\0';

    /* Arbitrary equality: textual comparison, no version semantics. */
    if (strcmp(op, "===") == 0)
        return strcmp(vstr, ostr) == 0;

    /* Trailing ".*" wildcard (== and != only): prefix-match the release. */
    int wildcard = 0;
    if (olen > 2 && strcmp(ostr + olen - 2, ".*") == 0) {
        if (strcmp(op, "==") != 0 && strcmp(op, "!=") != 0)
            return -1;
        ostr[olen - 2] = '\0';
        wildcard = 1;
    }

    Pep440 o;
    if (pep440_parse(ostr, &o) != 0)
        return -1;

    if (wildcard) {
        int match = v->epoch == o.epoch;
        for (int i = 0; match && i < o.n; i++)
            if (rel_at(v, i) != o.rel[i])
                match = 0;
        return (op[0] == '=') ? match : !match;
    }

    if (strcmp(op, "~=") == 0) {
        /* ~=X.Y[.Z] → >= operand AND ==X[.Y].* (operand minus last part). */
        if (o.n < 2)
            return -1;
        if (pep440_cmp_parsed(v, &o) < 0)
            return 0;
        if (v->epoch != o.epoch)
            return 0;
        for (int i = 0; i < o.n - 1; i++)
            if (rel_at(v, i) != o.rel[i])
                return 0;
        return 1;
    }

    int c = pep440_cmp_parsed(v, &o);
    if (strcmp(op, "==") == 0) return c == 0;
    if (strcmp(op, "!=") == 0) return c != 0;
    if (strcmp(op, ">=") == 0) return c >= 0;
    if (strcmp(op, "<=") == 0) return c <= 0;
    if (strcmp(op, ">")  == 0) return c >  0;
    if (strcmp(op, "<")  == 0) return c <  0;
    return -1;
}

int pep440_satisfies(const char *version, const char *spec)
{
    if (!spec || !*spec)
        return 1;

    Pep440 v;
    if (pep440_parse(version, &v) != 0)
        return -1;

    const char *s = spec;
    while (*s) {
        const char *comma = strchr(s, ',');
        size_t      len   = comma ? (size_t)(comma - s) : strlen(s);

        int rc = clause_satisfied(&v, version, s, len);
        if (rc != 1)
            return rc;                      /* 0 (fails) or -1 (unknown) */

        s = comma ? comma + 1 : s + len;
    }
    return 1;
}

int pep440_spec_admits_prerelease(const char *spec)
{
    if (!spec)
        return 0;

    const char *s = spec;
    while (*s) {
        const char *comma = strchr(s, ',');
        size_t      len   = comma ? (size_t)(comma - s) : strlen(s);

        /* Slice the operand out of the clause and test it. */
        char clause[64];
        size_t start = 0, end = len;
        while (start < end && !isdigit((unsigned char)s[start]) &&
               s[start] != 'v' && s[start] != 'V')
            start++;
        if (end - start > 0 && end - start < sizeof(clause)) {
            memcpy(clause, s + start, end - start);
            clause[end - start] = '\0';
            if (pep440_is_prerelease(clause))
                return 1;
        }
        s = comma ? comma + 1 : s + len;
    }
    return 0;
}
