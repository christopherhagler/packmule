/*
 * test_toml.c — unit tests for the TOML reader (pure parsing, no I/O).
 *
 * The cases here are the shapes real lockfiles use, plus the malformed inputs
 * that must fail loudly rather than yield a half-read document: a lock we
 * only partly understood would become a bundle that is quietly incomplete.
 */
#include "toml.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_scalars(void)
{
    TomlValue *r = toml_parse_string(
        "# a comment\n"
        "name = \"packmule\"\n"
        "version = 1\n"
        "ratio = 1.5\n"
        "enabled = true\n"
        "disabled = false\n"
        "empty = \"\"\n"
        "literal = 'no \\escapes here'\n"
        "when = 2025-01-25T11:30:10.164985Z\n", "t.toml");
    assert(r);

    assert(strcmp(toml_table_string(r, "name"), "packmule") == 0);
    assert(toml_get(r, "version")->type == TOML_INTEGER);
    assert(toml_get(r, "version")->u.integer == 1);
    assert(toml_get(r, "ratio")->type == TOML_FLOAT);
    assert(toml_get(r, "enabled")->u.boolean == 1);
    assert(toml_get(r, "disabled")->u.boolean == 0);
    assert(strcmp(toml_table_string(r, "empty"), "") == 0);
    /* A literal string is bytes, not escapes — markers depend on this. */
    assert(strcmp(toml_table_string(r, "literal"), "no \\escapes here") == 0);
    assert(toml_get(r, "when")->type == TOML_DATETIME);
    assert(strcmp(toml_table_string(r, "when"),
                  "2025-01-25T11:30:10.164985Z") == 0);

    /* Absent keys and wrong types are NULL, never a crash. */
    assert(toml_get(r, "nope") == NULL);
    assert(toml_table_string(r, "version") == NULL);
    assert(toml_get(NULL, "x") == NULL);

    toml_free(r);
}

static void test_escapes(void)
{
    TomlValue *r = toml_parse_string(
        "s = \"tab\\there\\nline\\\"quote\\\\back\"\n"
        "u = \"\\u00e9\\u0041\"\n", "t.toml");
    assert(r);
    assert(strcmp(toml_table_string(r, "s"),
                  "tab\there\nline\"quote\\back") == 0);
    assert(strcmp(toml_table_string(r, "u"), "\xc3\xa9" "A") == 0);
    toml_free(r);
}

static void test_multiline_strings(void)
{
    TomlValue *r = toml_parse_string(
        "a = \"\"\"\nfirst\nsecond\"\"\"\n"
        "b = '''\nraw \\n stays'''\n", "t.toml");
    assert(r);
    /* The newline right after the opening delimiter is not part of the value. */
    assert(strcmp(toml_table_string(r, "a"), "first\nsecond") == 0);
    assert(strcmp(toml_table_string(r, "b"), "raw \\n stays") == 0);
    toml_free(r);
}

static void test_arrays_and_inline_tables(void)
{
    TomlValue *r = toml_parse_string(
        "empty = []\n"
        "names = [\"a\", \"b\", \"c\"]\n"
        "multi = [\n"
        "    1,\n"
        "    2,   # trailing comma and comments are fine\n"
        "]\n"
        "point = { x = 1, y = 2 }\n"
        "nested = [{ name = \"certifi\" }, { name = \"idna\" }]\n", "t.toml");
    assert(r);

    assert(toml_array_len(toml_get(r, "empty")) == 0);
    assert(toml_array_len(toml_get(r, "names")) == 3);
    assert(strcmp(toml_string(toml_array_at(toml_get(r, "names"), 1)), "b") == 0);
    assert(toml_array_len(toml_get(r, "multi")) == 2);
    assert(toml_get(toml_get(r, "point"), "y")->u.integer == 2);

    const TomlValue *nested = toml_get(r, "nested");
    assert(toml_array_len(nested) == 2);
    assert(strcmp(toml_table_string(toml_array_at(nested, 1), "name"),
                  "idna") == 0);

    /* Out-of-range and wrong-type access is NULL, not undefined behaviour. */
    assert(toml_array_at(nested, 99) == NULL);
    assert(toml_array_len(toml_get(r, "point")) == 0);

    toml_free(r);
}

static void test_tables_and_dotted_keys(void)
{
    TomlValue *r = toml_parse_string(
        "top = 1\n"
        "[tool]\n"
        "name = \"uv\"\n"
        "[tool.nested]\n"
        "deep = \"yes\"\n"
        "a.b.c = \"dotted\"\n"
        "[\"quoted key\"]\n"
        "v = 2\n", "t.toml");
    assert(r);

    assert(toml_get(r, "top")->u.integer == 1);
    assert(strcmp(toml_table_string(toml_get(r, "tool"), "name"), "uv") == 0);

    const TomlValue *nested = toml_get(toml_get(r, "tool"), "nested");
    assert(strcmp(toml_table_string(nested, "deep"), "yes") == 0);

    /* A dotted key builds intermediate tables inside the current one. */
    const TomlValue *c = toml_get(toml_get(toml_get(nested, "a"), "b"), "c");
    assert(strcmp(toml_string(c), "dotted") == 0);

    assert(toml_get(toml_get(r, "quoted key"), "v")->u.integer == 2);
    toml_free(r);
}

/*
 * The load-bearing case for lockfiles: a [table] header after an
 * [[array of tables]] must attach to the LAST element of that array, which is
 * what makes uv's [package.metadata] and PEP 751's [packages.sdist] land on
 * the package they follow rather than on the first one in the file.
 */
static void test_array_of_tables(void)
{
    TomlValue *r = toml_parse_string(
        "[[package]]\n"
        "name = \"certifi\"\n"
        "[package.metadata]\n"
        "note = \"first\"\n"
        "\n"
        "[[package]]\n"
        "name = \"requests\"\n"
        "dependencies = [{ name = \"certifi\" }]\n"
        "[package.metadata]\n"
        "note = \"second\"\n", "t.toml");
    assert(r);

    const TomlValue *pkgs = toml_get(r, "package");
    assert(toml_array_len(pkgs) == 2);

    const TomlValue *p0 = toml_array_at(pkgs, 0);
    const TomlValue *p1 = toml_array_at(pkgs, 1);
    assert(strcmp(toml_table_string(p0, "name"), "certifi") == 0);
    assert(strcmp(toml_table_string(p1, "name"), "requests") == 0);

    assert(strcmp(toml_table_string(toml_get(p0, "metadata"), "note"),
                  "first") == 0);
    assert(strcmp(toml_table_string(toml_get(p1, "metadata"), "note"),
                  "second") == 0);
    assert(toml_array_len(toml_get(p1, "dependencies")) == 1);

    toml_free(r);
}

static void test_bom_and_crlf(void)
{
    /* Split so "\xBF" does not absorb the following 'a' as a hex digit. */
    TomlValue *r = toml_parse_string("\xEF\xBB\xBF" "a = 1\r\nb = 2\r\n",
                                     "t.toml");
    assert(r);
    assert(toml_get(r, "a")->u.integer == 1);
    assert(toml_get(r, "b")->u.integer == 2);
    toml_free(r);
}

/* Every one of these must return NULL rather than a partial document. */
static void test_malformed_is_rejected(void)
{
    static const char *const bad[] = {
        "a = \n",                        /* no value                    */
        "a 1\n",                         /* no '='                      */
        "a = \"unterminated\n",           /* newline in a basic string   */
        "a = 'unterminated\n",            /* newline in a literal string */
        "a = [1, 2\n",                   /* unclosed array              */
        "a = { x = 1\n",                 /* unclosed inline table       */
        "[table\n",                      /* unclosed header             */
        "[[table]\n",                    /* mismatched header brackets  */
        "a = 1\na = 2\n",                /* duplicate key               */
        "[t]\nx = 1\n[t]\ny = 2\n",      /* table defined twice         */
        "a = \"\\q\"\n",                 /* unknown escape              */
        "a = \"\\uZZZZ\"\n",             /* malformed unicode escape    */
        "a = 1 garbage\n",               /* trailing text after a value */
        "a = notavalue\n",               /* unparseable scalar          */
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        /* Errors print to stderr by design; the test only asserts refusal. */
        TomlValue *r = toml_parse_string(bad[i], "bad.toml");
        if (r) {
            fprintf(stdout, "case %zu should have failed: %s\n", i, bad[i]);
            assert(r == NULL);
        }
    }
    assert(toml_parse_string(NULL, "x") == NULL);
}

int main(void)
{
    printf("Running TOML reader tests...\n");
    test_scalars();
    test_escapes();
    test_multiline_strings();
    test_arrays_and_inline_tables();
    test_tables_and_dotted_keys();
    test_array_of_tables();
    test_bom_and_crlf();
    printf("  (the following parse errors are expected)\n");
    test_malformed_is_rejected();
    printf("All TOML reader tests passed.\n");
    return 0;
}
