/*
 * semver.c — SemVer 2.0 comparison and node-semver range matching.
 *
 * See semver.h for the supported grammar.  Everything here is pure string
 * processing with no allocation beyond the stack, so it is cheap to call in
 * a loop over a packument's version keys.
 */

#include "semver.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ── Parsed version ──────────────────────────────────────────────────────── */

typedef struct {
    long maj, min, pat;
    int  n;              /* concrete numeric components present (0–3) */
    char pre[64];        /* prerelease identifiers, "" when none */
} SemVer;

/*
 * sv_parse — parse "v1.2.3-rc.1+build" into `v`.  Missing or 'x'/'*'
 * components stop the count (n reflects how many were concrete), so "1.2"
 * parses with n=2 and "1.x" with n=1.  Build metadata (+…) is ignored.
 * Returns 0 on success, -1 when `s` does not start like a version.
 */
static int sv_parse(const char *s, SemVer *v)
{
    v->maj = v->min = v->pat = 0;
    v->n      = 0;
    v->pre[0] = '\0';

    while (*s == ' ' || *s == '\t') s++;
    if (*s == 'v' || *s == '=') s++;

    long *comp[3] = { &v->maj, &v->min, &v->pat };
    for (int i = 0; i < 3; i++) {
        if (*s == 'x' || *s == 'X' || *s == '*') {
            s++;
            break;                        /* x-range: stop counting here */
        }
        if (!isdigit((unsigned char)*s)) {
            if (i == 0 && *s == '\0')
                return 0;                 /* empty / "*" alone: n = 0, valid */
            return i == 0 ? -1 : 0;       /* partial version: fine after 1+ */
        }
        char *end;
        *comp[i] = strtol(s, &end, 10);
        s        = end;
        v->n     = i + 1;
        if (*s != '.')
            break;
        s++;
    }

    if (*s == '-') {
        size_t k = 0;
        s++;
        while (*s && *s != '+' && *s != ' ' && k < sizeof(v->pre) - 1)
            v->pre[k++] = *s++;
        v->pre[k] = '\0';
    }
    return 0;
}

/*
 * pre_cmp — compare two prerelease strings per SemVer 2.0 rules:
 * dot-separated identifiers, numeric identifiers compare numerically and
 * rank below alphanumeric ones, and a longer identifier list wins a tie.
 * An empty string means "no prerelease" and ranks ABOVE any prerelease.
 */
static int pre_cmp(const char *a, const char *b)
{
    if (a[0] == '\0' && b[0] == '\0') return 0;
    if (a[0] == '\0') return 1;   /* release > prerelease */
    if (b[0] == '\0') return -1;

    while (*a || *b) {
        if (!*a) return -1;       /* fewer identifiers ranks lower */
        if (!*b) return 1;

        const char *ea = a; while (*ea && *ea != '.') ea++;
        const char *eb = b; while (*eb && *eb != '.') eb++;
        size_t la = (size_t)(ea - a), lb = (size_t)(eb - b);

        int na = 1, nb = 1;
        for (size_t i = 0; i < la; i++)
            if (!isdigit((unsigned char)a[i])) { na = 0; break; }
        for (size_t i = 0; i < lb; i++)
            if (!isdigit((unsigned char)b[i])) { nb = 0; break; }

        if (na && nb) {
            long va = strtol(a, NULL, 10), vb = strtol(b, NULL, 10);
            if (va != vb) return va > vb ? 1 : -1;
        } else if (na != nb) {
            return nb ? 1 : -1;   /* numeric < alphanumeric */
        } else {
            int rc = strncmp(a, b, la < lb ? la : lb);
            if (rc != 0) return rc > 0 ? 1 : -1;
            if (la != lb) return la > lb ? 1 : -1;
        }

        a = *ea ? ea + 1 : ea;
        b = *eb ? eb + 1 : eb;
    }
    return 0;
}

static int sv_cmp(const SemVer *a, const SemVer *b)
{
    if (a->maj != b->maj) return a->maj > b->maj ? 1 : -1;
    if (a->min != b->min) return a->min > b->min ? 1 : -1;
    if (a->pat != b->pat) return a->pat > b->pat ? 1 : -1;
    return pre_cmp(a->pre, b->pre);
}

int semver_cmp(const char *a, const char *b)
{
    SemVer va, vb;
    if (sv_parse(a, &va) != 0 || sv_parse(b, &vb) != 0)
        return strcmp(a, b);      /* not versions: any stable order will do */
    return sv_cmp(&va, &vb);
}

/* ── Comparator evaluation ───────────────────────────────────────────────── */

/*
 * One primitive comparator from a range clause, normalised to an operator
 * plus operand.  `op` is one of: '^', '~', '>', '<', 'g' (>=), 'l' (<=),
 * '=' (exact or x-range).
 */
typedef struct {
    char   op;
    SemVer v;
} Comparator;

/* bump — the smallest version strictly above every version the partial
 * operand covers: bump(1) = 2.0.0, bump(1.2) = 1.3.0, bump(1.2.3) = 1.2.4. */
static SemVer bump(const SemVer *c)
{
    SemVer r = { c->maj, c->min, c->pat, 3, "" };
    if (c->n <= 1)      { r.maj++; r.min = r.pat = 0; }
    else if (c->n == 2) { r.min++; r.pat = 0; }
    else                { r.pat++; }
    return r;
}

/* caret_upper — exclusive upper bound of "^operand" per node-semver. */
static SemVer caret_upper(const SemVer *c)
{
    SemVer r = { 0, 0, 0, 3, "" };
    if (c->maj > 0 || c->n <= 1) {
        r.maj = c->maj + 1;                      /* ^1.2.3, ^0, ^* → next maj */
    } else if (c->min > 0 || c->n == 2) {
        r.min = c->min + 1;                      /* ^0.2.3, ^0.0 → next minor */
    } else {
        r.pat = c->pat + 1;                      /* ^0.0.3 → next patch */
    }
    return r;
}

/* sat_one — does version `v` satisfy a single comparator? (Ignores the
 * prerelease admission rule, which is applied per-clause by the caller.) */
static int sat_one(const SemVer *v, const Comparator *c)
{
    SemVer lo, hi;

    switch (c->op) {
    case '^':
        if (c->v.n == 0) return 1;               /* ^* → anything */
        lo = c->v; lo.n = 3;
        hi = caret_upper(&c->v);
        return sv_cmp(v, &lo) >= 0 && sv_cmp(v, &hi) < 0;
    case '~':
        if (c->v.n == 0) return 1;
        lo = c->v; lo.n = 3;
        hi = (c->v.n >= 2)
           ? (SemVer){ c->v.maj, c->v.min + 1, 0, 3, "" }
           : (SemVer){ c->v.maj + 1, 0, 0, 3, "" };
        return sv_cmp(v, &lo) >= 0 && sv_cmp(v, &hi) < 0;
    case '=':
        if (c->v.n == 0) return 1;               /* "*" / "x" */
        if (c->v.n == 3)                         /* exact (incl. prerelease) */
            return sv_cmp(v, &c->v) == 0;
        lo = c->v;                               /* x-range: [lo, bump(lo)) */
        hi = bump(&c->v);
        return sv_cmp(v, &lo) >= 0 && sv_cmp(v, &hi) < 0;
    case 'g':                                    /* >= */
        lo = c->v;
        return sv_cmp(v, &lo) >= 0;
    case '>':
        if (c->v.n < 3) {                        /* >1.2 → >=1.3.0 */
            hi = bump(&c->v);
            return sv_cmp(v, &hi) >= 0;
        }
        return sv_cmp(v, &c->v) > 0;
    case 'l':                                    /* <= */
        if (c->v.n < 3) {                        /* <=1.2 → <1.3.0 */
            hi = bump(&c->v);
            return sv_cmp(v, &hi) < 0;
        }
        return sv_cmp(v, &c->v) <= 0;
    case '<':
        return sv_cmp(v, &c->v) < 0;
    default:
        return 0;
    }
}

/* ── Clause tokenizer ────────────────────────────────────────────────────── */

#define MAX_COMPARATORS 16

/*
 * parse_clause — tokenise one AND-clause (no "||") into comparators.
 * Handles operators glued to or separated from their operand (">=1.2.3",
 * ">= 1.2.3") and hyphen ranges ("1.2.3 - 2.0.0").
 * Returns the comparator count, or -1 when any token is unparseable.
 */
static int parse_clause(const char *s, Comparator *out)
{
    int n = 0;

    while (*s) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s)
            break;
        if (n >= MAX_COMPARATORS)
            return -1;

        char op = '=';
        if      (s[0] == '>' && s[1] == '=') { op = 'g'; s += 2; }
        else if (s[0] == '<' && s[1] == '=') { op = 'l'; s += 2; }
        else if (s[0] == '>')                { op = '>'; s += 1; }
        else if (s[0] == '<')                { op = '<'; s += 1; }
        else if (s[0] == '^')                { op = '^'; s += 1; }
        else if (s[0] == '~')                { op = '~'; s += 1; }

        while (*s == ' ' || *s == '\t') s++;   /* ">= 1.2.3" */

        /* Slice out the operand token. */
        const char *tok = s;
        while (*s && *s != ' ' && *s != '\t') s++;
        char operand[128];
        size_t len = (size_t)(s - tok);
        if (len == 0 || len >= sizeof(operand))
            return -1;
        memcpy(operand, tok, len);
        operand[len] = '\0';

        if (sv_parse(operand, &out[n].v) != 0)
            return -1;
        out[n].op = op;
        n++;

        /* Hyphen range: "<lower> - <upper>" → >=lower AND <=upper. */
        const char *peek = s;
        while (*peek == ' ' || *peek == '\t') peek++;
        if (peek[0] == '-' && (peek[1] == ' ' || peek[1] == '\t')) {
            if (op != '=' || n >= MAX_COMPARATORS)
                return -1;
            out[n - 1].op = 'g';
            s = peek + 1;
            while (*s == ' ' || *s == '\t') s++;
            tok = s;
            while (*s && *s != ' ' && *s != '\t') s++;
            len = (size_t)(s - tok);
            if (len == 0 || len >= sizeof(operand))
                return -1;
            memcpy(operand, tok, len);
            operand[len] = '\0';
            if (sv_parse(operand, &out[n].v) != 0)
                return -1;
            out[n].op = 'l';
            n++;
        }
    }
    return n;
}

/*
 * clause_satisfied — all comparators hold, plus the node-semver prerelease
 * rule: a prerelease version is only admitted when some comparator's operand
 * is a prerelease of the same major.minor.patch tuple.
 */
static int clause_satisfied(const SemVer *v, const Comparator *comps, int n)
{
    if (n == 0)
        return 1;                                /* "" / "*" clause */

    if (v->pre[0]) {
        int admitted = 0;
        for (int i = 0; i < n; i++) {
            if (comps[i].v.pre[0] &&
                comps[i].v.maj == v->maj &&
                comps[i].v.min == v->min &&
                comps[i].v.pat == v->pat) {
                admitted = 1;
                break;
            }
        }
        if (!admitted)
            return 0;
    }

    for (int i = 0; i < n; i++)
        if (!sat_one(v, &comps[i]))
            return 0;
    return 1;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

int semver_satisfies(const char *version, const char *range)
{
    SemVer v;
    if (sv_parse(version, &v) != 0 || v.n == 0)
        return 0;                                /* not a concrete version */

    int unparseable = 0;
    const char *s = range;

    while (s) {
        const char *bar = strstr(s, "||");
        size_t      len = bar ? (size_t)(bar - s) : strlen(s);

        char clause[256];
        if (len >= sizeof(clause)) {
            unparseable = 1;
        } else {
            memcpy(clause, s, len);
            clause[len] = '\0';

            Comparator comps[MAX_COMPARATORS];
            int n = parse_clause(clause, comps);
            if (n < 0)
                unparseable = 1;
            else if (clause_satisfied(&v, comps, n))
                return 1;
        }
        s = bar ? bar + 2 : NULL;
    }

    return unparseable ? -1 : 0;
}
