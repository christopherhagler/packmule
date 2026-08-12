/*
 * toml.h — a small TOML reader, sized for lockfiles.
 *
 * Python's lockfile formats are TOML: uv.lock, PEP 751's pylock.toml, and
 * poetry.lock.  cJSON covers every JSON input packmule reads, but no TOML
 * library is packaged widely enough to become a hard dependency of a tool
 * whose whole point is installing on machines that cannot fetch anything, so
 * this is hand-rolled in the same spirit as pep440.c, pep508.c and semver.c.
 *
 * What is supported is what lockfiles actually contain:
 *
 *   - comments, bare / quoted / dotted keys
 *   - [table] and [[array of tables]] headers, including dotted names
 *   - basic ("…") and literal ('…') strings, plus their multi-line ("""…""")
 *     forms and the standard escape set
 *   - integers, floats, booleans
 *   - arrays (multi-line, trailing comma allowed) and inline tables
 *   - dates and date-times, kept as their raw text
 *
 * What is deliberately NOT supported, because no lockfile emits it: mixed-type
 * array validation, integer bases other than 10, and underscores in numbers
 * are accepted but not interpreted beyond skipping.  Anything unparseable is a
 * hard error with a line number rather than a silently empty table — a lock we
 * cannot fully read must never become a bundle that is quietly incomplete.
 *
 * Ownership: toml_parse_* returns a TomlValue the caller owns and must release
 * with toml_free().  Every accessor returns a pointer BORROWED from that tree;
 * none of them allocate, and none of their results may be freed.
 */

#ifndef PACKMULE_TOML_H
#define PACKMULE_TOML_H

#include <stddef.h>

typedef enum {
    TOML_STRING = 0,
    TOML_INTEGER,
    TOML_FLOAT,
    TOML_BOOLEAN,
    TOML_DATETIME,  /* raw text; lockfiles only ever echo it back */
    TOML_ARRAY,
    TOML_TABLE,
} TomlType;

typedef struct TomlValue TomlValue;

struct TomlValue {
    TomlType type;
    union {
        char  *string;      /* TOML_STRING, TOML_DATETIME */
        long long integer;  /* TOML_INTEGER */
        double number;      /* TOML_FLOAT */
        int    boolean;     /* TOML_BOOLEAN */
        struct {
            TomlValue **items;
            size_t      count;
            size_t      capacity;
        } array;            /* TOML_ARRAY */
        struct {
            char      **keys;
            TomlValue **values;
            size_t      count;
            size_t      capacity;
            /*
             * 1 once a [header] has opened this table explicitly.  TOML
             * forbids defining the same table twice, and lockfile generators
             * do not do it, but tracking it turns a corrupt file into an
             * error instead of a last-write-wins merge.
             */
            int         defined;
        } table;            /* TOML_TABLE */
    } u;
};

/*
 * toml_parse_string — parse a NUL-terminated TOML document.
 *
 * Returns the root table, or NULL on a syntax error.  `origin` is used only
 * to label errors on stderr ("uv.lock:41: …") and may be NULL.
 */
TomlValue *toml_parse_string(const char *src, const char *origin);

/*
 * toml_parse_file — read and parse `path`, refusing files larger than
 * `max_bytes` (0 for the built-in default).  Returns NULL on an I/O or syntax
 * error, having reported it to stderr.
 */
TomlValue *toml_parse_file(const char *path, size_t max_bytes);

/* Release a tree returned by toml_parse_*.  Safe with NULL. */
void toml_free(TomlValue *v);

/* ── Accessors ───────────────────────────────────────────────────────────── */

/*
 * toml_get — look up `key` in a table.  Returns NULL when `v` is not a table
 * or the key is absent.  The result is borrowed from `v`.
 */
const TomlValue *toml_get(const TomlValue *v, const char *key);

/* Number of elements in an array (0 for any other type). */
size_t toml_array_len(const TomlValue *v);

/* Element `i` of an array, or NULL when out of range or not an array. */
const TomlValue *toml_array_at(const TomlValue *v, size_t i);

/*
 * toml_string — the text of a TOML_STRING (or TOML_DATETIME), else NULL.
 * Borrowed from the tree; do not free.
 */
const char *toml_string(const TomlValue *v);

/*
 * toml_table_string — toml_string(toml_get(v, key)), the single most common
 * lookup in a lockfile reader.  NULL when absent or not a string.
 */
const char *toml_table_string(const TomlValue *v, const char *key);

#endif /* PACKMULE_TOML_H */
