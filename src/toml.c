/*
 * toml.c — the TOML subset described in toml.h.
 *
 * A single recursive-descent pass over a NUL-terminated buffer.  The parser is
 * strict: the first thing it cannot make sense of ends the parse with a line
 * number.  That matters more here than in a general-purpose reader, because
 * the caller is turning a lockfile into a list of files to ship across an air
 * gap — a partially-understood lock is the one failure mode that must not be
 * possible.
 */

#include "toml.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils.h"

/* Largest lockfile we will read into memory.  uv.lock for a very large
 * workspace runs to a few megabytes; past this the input is not a lockfile. */
#define TOML_MAX_FILE_BYTES ((size_t)32 * 1024 * 1024)

/* Deepest nesting of arrays/inline tables.  Lockfiles reach 3; the limit
 * exists so a hostile file cannot drive the recursive value parser into a
 * stack overflow. */
#define TOML_MAX_DEPTH 32

/* Most components in a dotted key ("a.b.c").  Lockfiles use at most 3. */
#define TOML_MAX_KEY_PARTS 16

typedef struct {
    const char *p;        /* cursor */
    const char *origin;   /* filename for diagnostics, or NULL */
    int         line;
    int         failed;   /* an error has already been reported */
} Parser;

/* ── Diagnostics ─────────────────────────────────────────────────────────── */

/*
 * fail — report the first error and latch.  Later calls are ignored so a
 * single syntax error does not cascade into a wall of noise as the parser
 * unwinds.
 */
static void fail(Parser *ps, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)));
static void fail(Parser *ps, const char *fmt, ...)
#endif
{
    if (ps->failed)
        return;
    ps->failed = 1;

    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "packmule: %s:%d: ", ps->origin ? ps->origin : "<toml>",
            ps->line);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ── Value construction ──────────────────────────────────────────────────── */

static TomlValue *val_new(TomlType type)
{
    TomlValue *v = pm_calloc(1, sizeof(*v));
    v->type = type;
    return v;
}

void toml_free(TomlValue *v)
{
    if (!v)
        return;

    switch (v->type) {
    case TOML_STRING:
    case TOML_DATETIME:
        pm_free(v->u.string);
        break;
    case TOML_ARRAY:
        for (size_t i = 0; i < v->u.array.count; i++)
            toml_free(v->u.array.items[i]);
        pm_free(v->u.array.items);
        break;
    case TOML_TABLE:
        for (size_t i = 0; i < v->u.table.count; i++) {
            pm_free(v->u.table.keys[i]);
            toml_free(v->u.table.values[i]);
        }
        pm_free(v->u.table.keys);
        pm_free(v->u.table.values);
        break;
    case TOML_INTEGER:
    case TOML_FLOAT:
    case TOML_BOOLEAN:
        break;
    }
    pm_free(v);
}

static void array_push(TomlValue *arr, TomlValue *item)
{
    if (arr->u.array.count == arr->u.array.capacity) {
        size_t cap = arr->u.array.capacity ? arr->u.array.capacity * 2 : 8;
        arr->u.array.items =
            pm_realloc(arr->u.array.items, cap * sizeof(*arr->u.array.items));
        arr->u.array.capacity = cap;
    }
    arr->u.array.items[arr->u.array.count++] = item;
}

/* Borrowed lookup that allows mutation of the stored value. */
static TomlValue *table_find(TomlValue *tbl, const char *key)
{
    if (!tbl || tbl->type != TOML_TABLE)
        return NULL;
    for (size_t i = 0; i < tbl->u.table.count; i++)
        if (strcmp(tbl->u.table.keys[i], key) == 0)
            return tbl->u.table.values[i];
    return NULL;
}

/* Takes ownership of both `key` and `val`.  The caller must have established
 * that `key` is not already present. */
static void table_set(TomlValue *tbl, char *key, TomlValue *val)
{
    if (tbl->u.table.count == tbl->u.table.capacity) {
        size_t cap = tbl->u.table.capacity ? tbl->u.table.capacity * 2 : 8;
        tbl->u.table.keys =
            pm_realloc(tbl->u.table.keys, cap * sizeof(*tbl->u.table.keys));
        tbl->u.table.values =
            pm_realloc(tbl->u.table.values, cap * sizeof(*tbl->u.table.values));
        tbl->u.table.capacity = cap;
    }
    tbl->u.table.keys[tbl->u.table.count]     = key;
    tbl->u.table.values[tbl->u.table.count++] = val;
}

/* ── Character-level helpers ─────────────────────────────────────────────── */

static void skip_blanks(Parser *ps)
{
    while (*ps->p == ' ' || *ps->p == '\t')
        ps->p++;
}

/* Consume one newline (LF or CRLF).  Returns 1 if one was consumed. */
static int skip_newline(Parser *ps)
{
    if (*ps->p == '\r' && ps->p[1] == '\n') {
        ps->p += 2;
        ps->line++;
        return 1;
    }
    if (*ps->p == '\n') {
        ps->p++;
        ps->line++;
        return 1;
    }
    return 0;
}

static void skip_comment(Parser *ps)
{
    if (*ps->p != '#')
        return;
    while (*ps->p && *ps->p != '\n' && *ps->p != '\r')
        ps->p++;
}

/* Skip whitespace, comments and newlines: the gap between statements, and the
 * gap between elements of a multi-line array. */
static void skip_gap(Parser *ps)
{
    for (;;) {
        skip_blanks(ps);
        skip_comment(ps);
        if (!skip_newline(ps))
            return;
    }
}

static int is_bare_key_char(int c)
{
    return isalnum((unsigned char)c) || c == '_' || c == '-';
}

/* ── Strings ─────────────────────────────────────────────────────────────── */

/* A growable byte buffer for the string being unescaped. */
typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} StrBuf;

static void sb_putc(StrBuf *sb, char c)
{
    if (sb->len + 1 >= sb->cap) {
        sb->cap = sb->cap ? sb->cap * 2 : 32;
        sb->buf = pm_realloc(sb->buf, sb->cap);
    }
    sb->buf[sb->len++] = c;
}

static char *sb_finish(StrBuf *sb)
{
    if (!sb->buf)
        sb->buf = pm_malloc(1);
    sb->buf[sb->len] = '\0';
    return sb->buf;
}

/* Encode one Unicode scalar as UTF-8.  Invalid scalars are rejected by the
 * caller; surrogates are written as-is rather than being silently replaced,
 * which no lockfile will ever exercise but keeps the function total. */
static void sb_put_utf8(StrBuf *sb, unsigned long cp)
{
    if (cp < 0x80) {
        sb_putc(sb, (char)cp);
    } else if (cp < 0x800) {
        sb_putc(sb, (char)(0xC0 | (cp >> 6)));
        sb_putc(sb, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        sb_putc(sb, (char)(0xE0 | (cp >> 12)));
        sb_putc(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_putc(sb, (char)(0x80 | (cp & 0x3F)));
    } else {
        sb_putc(sb, (char)(0xF0 | (cp >> 18)));
        sb_putc(sb, (char)(0x80 | ((cp >> 12) & 0x3F)));
        sb_putc(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_putc(sb, (char)(0x80 | (cp & 0x3F)));
    }
}

/* Read `digits` hex digits as a code point.  Returns -1 on a short or
 * malformed sequence. */
static int read_hex_escape(Parser *ps, int digits, unsigned long *out)
{
    unsigned long cp = 0;
    for (int i = 0; i < digits; i++) {
        int c = (unsigned char)ps->p[i];
        if (!isxdigit(c))
            return -1;
        cp = cp * 16 + (unsigned long)(isdigit(c) ? c - '0'
                                                  : (tolower(c) - 'a' + 10));
    }
    ps->p += digits;
    *out = cp;
    return 0;
}

/*
 * parse_escape — handle the sequence after a backslash in a basic string.
 *
 * `multiline` enables the line-ending backslash, which swallows the newline
 * and all leading whitespace of the following line.
 */
static int parse_escape(Parser *ps, StrBuf *sb, int multiline)
{
    char c = *ps->p;
    switch (c) {
    case 'b':  ps->p++; sb_putc(sb, '\b'); return 0;
    case 't':  ps->p++; sb_putc(sb, '\t'); return 0;
    case 'n':  ps->p++; sb_putc(sb, '\n'); return 0;
    case 'f':  ps->p++; sb_putc(sb, '\f'); return 0;
    case 'r':  ps->p++; sb_putc(sb, '\r'); return 0;
    case '"':  ps->p++; sb_putc(sb, '"');  return 0;
    case '\\': ps->p++; sb_putc(sb, '\\'); return 0;
    case 'u':
    case 'U': {
        int digits = (c == 'u') ? 4 : 8;
        ps->p++;
        unsigned long cp;
        if (read_hex_escape(ps, digits, &cp) != 0 || cp > 0x10FFFF) {
            fail(ps, "invalid \\%c escape in string", c);
            return -1;
        }
        sb_put_utf8(sb, cp);
        return 0;
    }
    default:
        break;
    }

    /* A backslash at end of line inside a multi-line string is a continuation:
     * drop the newline and the next line's indentation. */
    if (multiline) {
        const char *save = ps->p;
        skip_blanks(ps);
        if (skip_newline(ps)) {
            skip_gap(ps);
            return 0;
        }
        ps->p = save;
    }

    fail(ps, "unknown escape sequence \\%c", isprint((unsigned char)c) ? c : '?');
    return -1;
}

/*
 * parse_basic_string — a "…" or """…""" string, with escapes applied.
 * The opening delimiter has not yet been consumed.
 */
static char *parse_basic_string(Parser *ps)
{
    int multiline = (ps->p[0] == '"' && ps->p[1] == '"' && ps->p[2] == '"');
    ps->p += multiline ? 3 : 1;

    /* A newline immediately after the opening """ is not part of the value. */
    if (multiline) {
        skip_blanks(ps);
        skip_newline(ps);
    }

    StrBuf sb = { NULL, 0, 0 };
    for (;;) {
        char c = *ps->p;
        if (c == '\0') {
            fail(ps, "unterminated string");
            pm_free(sb.buf);
            return NULL;
        }
        if (c == '"') {
            if (!multiline) {
                ps->p++;
                return sb_finish(&sb);
            }
            if (ps->p[1] == '"' && ps->p[2] == '"') {
                ps->p += 3;
                /* A multi-line string may end with up to two extra quotes;
                 * those belong to the value, not to the delimiter. */
                for (int extra = 0; extra < 2 && *ps->p == '"'; extra++) {
                    sb_putc(&sb, '"');
                    ps->p++;
                }
                return sb_finish(&sb);
            }
            sb_putc(&sb, c);
            ps->p++;
            continue;
        }
        if (c == '\\') {
            ps->p++;
            if (parse_escape(ps, &sb, multiline) != 0) {
                pm_free(sb.buf);
                return NULL;
            }
            continue;
        }
        if (c == '\n' || c == '\r') {
            if (!multiline) {
                fail(ps, "unterminated string (newline in a single-line string)");
                pm_free(sb.buf);
                return NULL;
            }
            if (skip_newline(ps))
                sb_putc(&sb, '\n');
            continue;
        }
        sb_putc(&sb, c);
        ps->p++;
    }
}

/*
 * parse_literal_string — a '…' or '''…''' string.  No escape processing at
 * all: the bytes are the value.  Markers in lockfiles arrive this way
 * ('sys_platform == "win32"'), which is exactly why it must not be skipped.
 */
static char *parse_literal_string(Parser *ps)
{
    int multiline = (ps->p[0] == '\'' && ps->p[1] == '\'' && ps->p[2] == '\'');
    ps->p += multiline ? 3 : 1;

    if (multiline) {
        skip_blanks(ps);
        skip_newline(ps);
    }

    StrBuf sb = { NULL, 0, 0 };
    for (;;) {
        char c = *ps->p;
        if (c == '\0') {
            fail(ps, "unterminated string");
            pm_free(sb.buf);
            return NULL;
        }
        if (c == '\'') {
            if (!multiline) {
                ps->p++;
                return sb_finish(&sb);
            }
            if (ps->p[1] == '\'' && ps->p[2] == '\'') {
                ps->p += 3;
                for (int extra = 0; extra < 2 && *ps->p == '\''; extra++) {
                    sb_putc(&sb, '\'');
                    ps->p++;
                }
                return sb_finish(&sb);
            }
            sb_putc(&sb, c);
            ps->p++;
            continue;
        }
        if (c == '\n' || c == '\r') {
            if (!multiline) {
                fail(ps, "unterminated string (newline in a single-line string)");
                pm_free(sb.buf);
                return NULL;
            }
            if (skip_newline(ps))
                sb_putc(&sb, '\n');
            continue;
        }
        sb_putc(&sb, c);
        ps->p++;
    }
}

/* ── Keys ────────────────────────────────────────────────────────────────── */

/* One component of a key: bare, or a quoted string.  Returns a heap string. */
static char *parse_key_part(Parser *ps)
{
    if (*ps->p == '"')
        return parse_basic_string(ps);
    if (*ps->p == '\'')
        return parse_literal_string(ps);

    const char *start = ps->p;
    while (is_bare_key_char((unsigned char)*ps->p))
        ps->p++;
    if (ps->p == start) {
        fail(ps, "expected a key");
        return NULL;
    }
    return pm_strndup(start, (size_t)(ps->p - start));
}

/*
 * parse_dotted_key — "a.b.c" into `parts`.  Returns the number of components,
 * or -1 on error.  The caller owns every returned string.
 */
static int parse_dotted_key(Parser *ps, char **parts)
{
    int n = 0;
    for (;;) {
        if (n == TOML_MAX_KEY_PARTS) {
            fail(ps, "key has more than %d components", TOML_MAX_KEY_PARTS);
            goto error;
        }
        char *part = parse_key_part(ps);
        if (!part)
            goto error;
        parts[n++] = part;

        skip_blanks(ps);
        if (*ps->p != '.')
            return n;
        ps->p++;
        skip_blanks(ps);
    }

error:
    for (int i = 0; i < n; i++)
        pm_free(parts[i]);
    return -1;
}

/* ── Values ──────────────────────────────────────────────────────────────── */

static TomlValue *parse_value(Parser *ps, int depth);

static TomlValue *parse_array(Parser *ps, int depth)
{
    ps->p++;    /* '[' */
    TomlValue *arr = val_new(TOML_ARRAY);

    for (;;) {
        skip_gap(ps);
        if (*ps->p == ']') {
            ps->p++;
            return arr;
        }
        if (*ps->p == '\0') {
            fail(ps, "unterminated array");
            toml_free(arr);
            return NULL;
        }

        TomlValue *item = parse_value(ps, depth + 1);
        if (!item) {
            toml_free(arr);
            return NULL;
        }
        array_push(arr, item);

        skip_gap(ps);
        if (*ps->p == ',') {
            ps->p++;
            continue;
        }
        if (*ps->p == ']') {
            ps->p++;
            return arr;
        }
        fail(ps, "expected ',' or ']' in array");
        toml_free(arr);
        return NULL;
    }
}

static TomlValue *parse_inline_table(Parser *ps, int depth)
{
    ps->p++;    /* '{' */
    TomlValue *tbl = val_new(TOML_TABLE);

    skip_blanks(ps);
    if (*ps->p == '}') {
        ps->p++;
        return tbl;
    }

    for (;;) {
        skip_blanks(ps);

        char *parts[TOML_MAX_KEY_PARTS];
        int   n = parse_dotted_key(ps, parts);
        if (n < 0) {
            toml_free(tbl);
            return NULL;
        }

        skip_blanks(ps);
        if (*ps->p != '=') {
            fail(ps, "expected '=' after key in inline table");
            goto key_error;
        }
        ps->p++;
        skip_blanks(ps);

        TomlValue *val = parse_value(ps, depth + 1);
        if (!val)
            goto key_error;

        /* Walk the dotted path, creating intermediate tables. */
        TomlValue *dst = tbl;
        for (int i = 0; i < n - 1; i++) {
            TomlValue *next = table_find(dst, parts[i]);
            if (!next) {
                next = val_new(TOML_TABLE);
                table_set(dst, parts[i], next);
                parts[i] = NULL;    /* ownership moved into the table */
            } else if (next->type != TOML_TABLE) {
                fail(ps, "'%s' is not a table", parts[i]);
                toml_free(val);
                goto key_error;
            }
            dst = next;
        }
        if (table_find(dst, parts[n - 1])) {
            fail(ps, "duplicate key '%s' in inline table", parts[n - 1]);
            toml_free(val);
            goto key_error;
        }
        table_set(dst, parts[n - 1], val);
        parts[n - 1] = NULL;

        for (int i = 0; i < n; i++)
            pm_free(parts[i]);

        skip_blanks(ps);
        if (*ps->p == ',') {
            ps->p++;
            continue;
        }
        if (*ps->p == '}') {
            ps->p++;
            return tbl;
        }
        fail(ps, "expected ',' or '}' in inline table");
        toml_free(tbl);
        return NULL;

    key_error:
        for (int i = 0; i < n; i++)
            pm_free(parts[i]);
        toml_free(tbl);
        return NULL;
    }
}

/* 1 when `tok` starts with YYYY-MM-DD or HH:MM:SS. */
static int looks_like_datetime(const char *tok, size_t len)
{
    if (len >= 10 && isdigit((unsigned char)tok[0]) &&
        isdigit((unsigned char)tok[1]) && isdigit((unsigned char)tok[2]) &&
        isdigit((unsigned char)tok[3]) && tok[4] == '-')
        return 1;
    if (len >= 8 && isdigit((unsigned char)tok[0]) &&
        isdigit((unsigned char)tok[1]) && tok[2] == ':')
        return 1;
    return 0;
}

/*
 * parse_scalar — numbers, booleans and dates: everything whose extent is
 * "until a delimiter".  PEP 751's `upload-time` is an unquoted date-time, so
 * this path is not optional even though a lockfile reader never reads one.
 */
static TomlValue *parse_scalar(Parser *ps)
{
    const char *start = ps->p;
    while (*ps->p && *ps->p != ',' && *ps->p != ']' && *ps->p != '}' &&
           *ps->p != '\n' && *ps->p != '\r' && *ps->p != '#')
        ps->p++;

    /* Trim trailing blanks that belong to the layout, not the value. */
    const char *stop = ps->p;
    while (stop > start && (stop[-1] == ' ' || stop[-1] == '\t'))
        stop--;

    size_t len = (size_t)(stop - start);
    if (len == 0) {
        fail(ps, "expected a value");
        return NULL;
    }

    if (len == 4 && strncmp(start, "true", 4) == 0) {
        TomlValue *v = val_new(TOML_BOOLEAN);
        v->u.boolean = 1;
        return v;
    }
    if (len == 5 && strncmp(start, "false", 5) == 0) {
        TomlValue *v = val_new(TOML_BOOLEAN);
        v->u.boolean = 0;
        return v;
    }

    if (looks_like_datetime(start, len)) {
        TomlValue *v = val_new(TOML_DATETIME);
        v->u.string  = pm_strndup(start, len);
        return v;
    }

    char *text = pm_strndup(start, len);
    char *endp = NULL;

    /* A float is anything a strict integer scan does not consume whole. */
    long long ival = strtoll(text, &endp, 10);
    if (endp && *endp == '\0') {
        pm_free(text);
        TomlValue *v = val_new(TOML_INTEGER);
        v->u.integer = ival;
        return v;
    }

    double dval = strtod(text, &endp);
    if (endp && *endp == '\0' && endp != text) {
        pm_free(text);
        TomlValue *v = val_new(TOML_FLOAT);
        v->u.number  = dval;
        return v;
    }

    fail(ps, "cannot parse value '%s'", text);
    pm_free(text);
    return NULL;
}

static TomlValue *parse_value(Parser *ps, int depth)
{
    if (depth > TOML_MAX_DEPTH) {
        fail(ps, "value nesting deeper than %d levels", TOML_MAX_DEPTH);
        return NULL;
    }

    switch (*ps->p) {
    case '"':
    case '\'': {
        char *s = (*ps->p == '"') ? parse_basic_string(ps)
                                  : parse_literal_string(ps);
        if (!s)
            return NULL;
        TomlValue *v = val_new(TOML_STRING);
        v->u.string  = s;
        return v;
    }
    case '[':
        return parse_array(ps, depth);
    case '{':
        return parse_inline_table(ps, depth);
    default:
        return parse_scalar(ps);
    }
}

/* ── Document structure ──────────────────────────────────────────────────── */

/*
 * navigate — walk `parts[0 .. n-2]` from `root`, creating implicit tables as
 * it goes, and return the table that should hold the final component.
 *
 * Descending through an array-of-tables lands on its LAST element, which is
 * what makes `[package.metadata]` after `[[package]]` attach to the package
 * just opened rather than to the first one.
 */
static TomlValue *navigate(Parser *ps, TomlValue *root, char **parts, int n)
{
    TomlValue *cur = root;
    for (int i = 0; i < n - 1; i++) {
        TomlValue *next = table_find(cur, parts[i]);
        if (!next) {
            next = val_new(TOML_TABLE);
            table_set(cur, pm_strdup(parts[i]), next);
        } else if (next->type == TOML_ARRAY) {
            if (next->u.array.count == 0 ||
                next->u.array.items[next->u.array.count - 1]->type !=
                    TOML_TABLE) {
                fail(ps, "'%s' is not an array of tables", parts[i]);
                return NULL;
            }
            next = next->u.array.items[next->u.array.count - 1];
        } else if (next->type != TOML_TABLE) {
            fail(ps, "'%s' is not a table", parts[i]);
            return NULL;
        }
        cur = next;
    }
    return cur;
}

/*
 * parse_header — a `[table]` or `[[array of tables]]` line.  Returns the table
 * that subsequent key/value pairs belong to, or NULL on error.
 */
static TomlValue *parse_header(Parser *ps, TomlValue *root)
{
    int array_of_tables = (ps->p[1] == '[');
    ps->p += array_of_tables ? 2 : 1;
    skip_blanks(ps);

    char *parts[TOML_MAX_KEY_PARTS];
    int   n = parse_dotted_key(ps, parts);
    if (n < 0)
        return NULL;

    skip_blanks(ps);
    if (*ps->p != ']') {
        fail(ps, "expected ']' to close a table header");
        goto error;
    }
    ps->p++;
    if (array_of_tables) {
        if (*ps->p != ']') {
            fail(ps, "expected ']]' to close an array-of-tables header");
            goto error;
        }
        ps->p++;
    }

    TomlValue *parent = navigate(ps, root, parts, n);
    if (!parent)
        goto error;

    const char *last = parts[n - 1];
    TomlValue  *cur  = NULL;
    TomlValue  *slot = table_find(parent, last);

    if (array_of_tables) {
        if (!slot) {
            slot = val_new(TOML_ARRAY);
            table_set(parent, pm_strdup(last), slot);
        } else if (slot->type != TOML_ARRAY) {
            fail(ps, "'%s' was already defined as a table", last);
            goto error;
        }
        cur = val_new(TOML_TABLE);
        cur->u.table.defined = 1;
        array_push(slot, cur);
    } else {
        if (!slot) {
            cur = val_new(TOML_TABLE);
            cur->u.table.defined = 1;
            table_set(parent, pm_strdup(last), cur);
        } else if (slot->type == TOML_TABLE && !slot->u.table.defined) {
            /* Created implicitly by an earlier [a.b]; this defines it. */
            slot->u.table.defined = 1;
            cur = slot;
        } else {
            fail(ps, "table '%s' is defined more than once", last);
            goto error;
        }
    }

    for (int i = 0; i < n; i++)
        pm_free(parts[i]);
    return cur;

error:
    for (int i = 0; i < n; i++)
        pm_free(parts[i]);
    return NULL;
}

/* A `key = value` line assigned into `cur`.  Returns 0 on success. */
static int parse_keyval(Parser *ps, TomlValue *cur)
{
    char *parts[TOML_MAX_KEY_PARTS];
    int   n = parse_dotted_key(ps, parts);
    if (n < 0)
        return -1;

    skip_blanks(ps);
    if (*ps->p != '=') {
        fail(ps, "expected '=' after key");
        goto error;
    }
    ps->p++;
    skip_blanks(ps);

    TomlValue *val = parse_value(ps, 0);
    if (!val)
        goto error;

    TomlValue *dst = navigate(ps, cur, parts, n);
    if (!dst) {
        toml_free(val);
        goto error;
    }
    if (table_find(dst, parts[n - 1])) {
        fail(ps, "duplicate key '%s'", parts[n - 1]);
        toml_free(val);
        goto error;
    }
    table_set(dst, pm_strdup(parts[n - 1]), val);

    for (int i = 0; i < n; i++)
        pm_free(parts[i]);
    return 0;

error:
    for (int i = 0; i < n; i++)
        pm_free(parts[i]);
    return -1;
}

/* After a value, only blanks and a comment may precede the line break. */
static int expect_line_end(Parser *ps)
{
    skip_blanks(ps);
    skip_comment(ps);
    if (*ps->p == '\0' || skip_newline(ps))
        return 0;
    fail(ps, "unexpected trailing text after a value");
    return -1;
}

TomlValue *toml_parse_string(const char *src, const char *origin)
{
    if (!src)
        return NULL;

    Parser ps = { src, origin, 1, 0 };

    /* A UTF-8 BOM is legal at the start of a TOML file. */
    if ((unsigned char)ps.p[0] == 0xEF && (unsigned char)ps.p[1] == 0xBB &&
        (unsigned char)ps.p[2] == 0xBF)
        ps.p += 3;

    TomlValue *root = val_new(TOML_TABLE);
    root->u.table.defined = 1;
    TomlValue *cur = root;

    for (;;) {
        skip_gap(&ps);
        if (*ps.p == '\0')
            break;

        if (*ps.p == '[') {
            cur = parse_header(&ps, root);
            if (!cur)
                goto error;
            if (expect_line_end(&ps) != 0)
                goto error;
            continue;
        }

        if (parse_keyval(&ps, cur) != 0)
            goto error;
        if (expect_line_end(&ps) != 0)
            goto error;
    }
    return root;

error:
    toml_free(root);
    return NULL;
}

TomlValue *toml_parse_file(const char *path, size_t max_bytes)
{
    if (max_bytes == 0)
        max_bytes = TOML_MAX_FILE_BYTES;

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "packmule: cannot open %s\n", path);
        return NULL;
    }

    long fsize = -1;
    if (fseek(fp, 0, SEEK_END) == 0)
        fsize = ftell(fp);
    /* fseek back rather than rewind(): rewind cannot report failure, and a
     * silent one here would read the file from the wrong offset. */
    if (fsize < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fprintf(stderr, "packmule: cannot determine size of %s\n", path);
        fclose(fp);
        return NULL;
    }
    if ((size_t)fsize > max_bytes) {
        fprintf(stderr,
                "packmule: %s is larger than %zu MB; refusing to parse it\n",
                path, max_bytes / ((size_t)1024 * 1024));
        fclose(fp);
        return NULL;
    }

    size_t want  = (size_t)fsize;
    char  *buf   = pm_malloc(want + 1);
    size_t nread = fread(buf, 1, want, fp);
    if (nread > want)           /* unreachable; keeps the bound explicit */
        nread = want;
    /* buf holds want+1 bytes and nread <= want, so this index is in range.
     * The analyzer cannot see the size pm_malloc was called with (it lives in
     * another translation unit and returns void *), so it treats the file
     * size as an unbounded tainted value. */
    buf[nread] = '\0';          /* NOLINT(clang-analyzer-security.ArrayBound) */
    fclose(fp);

    TomlValue *root = toml_parse_string(buf, pm_basename(path));
    pm_free(buf);
    return root;
}

/* ── Accessors ───────────────────────────────────────────────────────────── */

const TomlValue *toml_get(const TomlValue *v, const char *key)
{
    if (!v || v->type != TOML_TABLE || !key)
        return NULL;
    for (size_t i = 0; i < v->u.table.count; i++)
        if (strcmp(v->u.table.keys[i], key) == 0)
            return v->u.table.values[i];
    return NULL;
}

size_t toml_array_len(const TomlValue *v)
{
    return (v && v->type == TOML_ARRAY) ? v->u.array.count : 0;
}

const TomlValue *toml_array_at(const TomlValue *v, size_t i)
{
    if (!v || v->type != TOML_ARRAY || i >= v->u.array.count)
        return NULL;
    return v->u.array.items[i];
}

const char *toml_string(const TomlValue *v)
{
    if (!v || (v->type != TOML_STRING && v->type != TOML_DATETIME))
        return NULL;
    return v->u.string;
}

const char *toml_table_string(const TomlValue *v, const char *key)
{
    return toml_string(toml_get(v, key));
}
