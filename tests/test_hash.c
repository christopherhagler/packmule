/*
 * test_hash.c — unit tests for hash.h (sha256_file, verify_file).
 *
 * Uses known content written to a temp file; the expected digests are derived
 * from sha256_file() itself (oracle pattern) so the tests remain valid even if
 * the test content changes.  The SHA-512 SRI expected value is computed via
 * OpenSSL directly inside the test so no hard-coded base64 string is needed.
 */
#include "hash.h"

#include <openssl/evp.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TMP = "test_hash_tmp.bin";

static void write_file(const char *path, const char *contents)
{
    FILE *fp = fopen(path, "wb");
    assert(fp != NULL);
    fputs(contents, fp);
    fclose(fp);
}

static void cleanup(void) { remove(TMP); }

/* Build the "sha512-<base64>" SRI string for the content of `path`. */
static char *make_sha512_sri(const char *path)
{
    FILE          *fp         = fopen(path, "rb");
    EVP_MD_CTX    *ctx        = EVP_MD_CTX_new();
    unsigned char  buf[4096];
    unsigned char  digest[EVP_MAX_MD_SIZE];
    unsigned int   digest_len = 0;
    size_t         n;

    assert(fp && ctx);
    assert(EVP_DigestInit_ex(ctx, EVP_sha512(), NULL) == 1);
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
        assert(EVP_DigestUpdate(ctx, buf, n) == 1);
    assert(EVP_DigestFinal_ex(ctx, digest, &digest_len) == 1);
    EVP_MD_CTX_free(ctx);
    fclose(fp);

    unsigned char b64[128];
    int b64_len = EVP_EncodeBlock(b64, digest, (int)digest_len);
    b64[b64_len] = '\0';

    /* "sha512-" (7) + b64_len + NUL */
    char *sri = malloc(7 + (size_t)b64_len + 1);
    assert(sri != NULL);
    memcpy(sri, "sha512-", 7);
    memcpy(sri + 7, b64, (size_t)b64_len + 1);
    return sri;
}

/* ── sha256_file ─────────────────────────────────────────────────────────── */

static void test_sha256_returns_64_hex_chars(void)
{
    write_file(TMP, "hello world\n");
    char hex[65];
    assert(sha256_file(TMP, hex) == 0);
    assert(strlen(hex) == 64);
    for (int i = 0; i < 64; i++)
        assert((hex[i] >= '0' && hex[i] <= '9') ||
               (hex[i] >= 'a' && hex[i] <= 'f'));
    cleanup();
}

static void test_sha256_known_value(void)
{
    /* echo -n "" | sha256sum → e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855 */
    write_file(TMP, "");
    char hex[65];
    assert(sha256_file(TMP, hex) == 0);
    assert(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb924"
                       "27ae41e4649b934ca495991b7852b855") == 0);
    cleanup();
}

static void test_sha256_missing_file(void)
{
    char hex[65];
    assert(sha256_file("/nonexistent/file.bin", hex) == -1);
}

/* ── verify_file — SHA-256 path ──────────────────────────────────────────── */

static void test_verify_sha256_match(void)
{
    write_file(TMP, "packmule test content");
    char hex[65];
    assert(sha256_file(TMP, hex) == 0);
    Digest d = {0};
    digest_set(&d, DIGEST_SHA256, DIGEST_ENC_HEX, hex);
    assert(digest_verify_file(TMP, &d) == 0);
    digest_clear(&d);
    cleanup();
}

static void test_verify_sha256_mismatch(void)
{
    write_file(TMP, "packmule test content");
    /* All-zero hash — will never match real content. */
    const char *wrong = "0000000000000000000000000000000000000000000000000000000000000000";
    Digest d = {0};
    digest_set(&d, DIGEST_SHA256, DIGEST_ENC_HEX, wrong);
    assert(digest_verify_file(TMP, &d) == -1);
    digest_clear(&d);
    cleanup();
}

/* ── verify_file — SHA-512 SRI path ─────────────────────────────────────── */

static void test_verify_sha512_sri_match(void)
{
    write_file(TMP, "npm tarball content");
    char *sri = make_sha512_sri(TMP);
    Digest d = {0};
    assert(digest_parse_sri(&d, sri) == 0);
    assert(digest_verify_file(TMP, &d) == 0);
    digest_clear(&d);
    free(sri);
    cleanup();
}

static void test_verify_sha512_sri_mismatch(void)
{
    write_file(TMP, "npm tarball content");
    /* Syntactically valid SRI prefix with wrong base64 payload. */
    Digest d = {0};
    assert(digest_parse_sri(&d, "sha512-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA") == 0);
    assert(digest_verify_file(TMP, &d) == -1);
    digest_clear(&d);
    cleanup();
}

static void test_verify_missing_file(void)
{
    Digest d = {0};
    digest_set(&d, DIGEST_SHA256, DIGEST_ENC_HEX,
               "0000000000000000000000000000000000000000"
               "0000000000000000000000000000");
    assert(digest_verify_file("/nonexistent/file.bin", &d) == -1);
    digest_clear(&d);
}

/* ── determinism: same content → same digest ─────────────────────────────── */

static void test_sha256_deterministic(void)
{
    write_file(TMP, "deterministic input");
    char hex1[65], hex2[65];
    assert(sha256_file(TMP, hex1) == 0);
    assert(sha256_file(TMP, hex2) == 0);
    assert(strcmp(hex1, hex2) == 0);
    cleanup();
}

/* ── Digest typing ───────────────────────────────────────────────────────── */

static void test_unset_digest_is_a_failure_not_a_pass(void)
{
    /* A file with no digest to check against must never be accepted: the
     * previous string-based API dereferenced NULL here instead. */
    write_file(TMP, "content");
    Digest empty = {0};
    assert(digest_verify_file(TMP, &empty) == -1);
    assert(digest_matches_file(TMP, &empty) == 0);
    cleanup();
}

static void test_algorithm_is_explicit_not_sniffed(void)
{
    assert(digest_algo_from_name("sha256") == DIGEST_SHA256);
    assert(digest_algo_from_name("SHA512") == DIGEST_SHA512);
    assert(digest_algo_from_name("sha")    == DIGEST_SHA1);  /* repodata */
    assert(digest_algo_from_name("sha1")   == DIGEST_SHA1);
    assert(digest_algo_from_name("md5")    == DIGEST_NONE);
    assert(digest_algo_from_name(NULL)     == DIGEST_NONE);

    /* An unsupported SRI algorithm is rejected rather than assumed. */
    Digest d = {0};
    assert(digest_parse_sri(&d, "md5-abcdef") == -1);
    assert(digest_parse_sri(&d, "notsri")     == -1);
    assert(digest_parse_sri(&d, "sha512-Zm9v") == 0);
    assert(d.algo == DIGEST_SHA512 && d.enc == DIGEST_ENC_BASE64);
    assert(strcmp(d.value, "Zm9v") == 0);
    digest_clear(&d);
}

static void test_sha1_and_sha512_hex_verify(void)
{
    /* RPM repositories publish sha1 or sha512 hex, not only sha256. */
    write_file(TMP, "rpm payload");

    char hex[DIGEST_HEX_MAX];
    assert(digest_file_hex(TMP, DIGEST_SHA1, hex, sizeof(hex)) == 0);
    assert(strlen(hex) == 40);
    Digest d = {0};
    digest_set(&d, DIGEST_SHA1, DIGEST_ENC_HEX, hex);
    assert(digest_verify_file(TMP, &d) == 0);
    digest_clear(&d);

    assert(digest_file_hex(TMP, DIGEST_SHA512, hex, sizeof(hex)) == 0);
    assert(strlen(hex) == 128);
    digest_set(&d, DIGEST_SHA512, DIGEST_ENC_HEX, hex);
    assert(digest_verify_file(TMP, &d) == 0);
    digest_clear(&d);

    cleanup();
}

static void test_hex_compare_is_case_insensitive(void)
{
    /* Some repositories publish uppercase hex. */
    write_file(TMP, "case test");
    char hex[DIGEST_HEX_MAX];
    assert(digest_file_hex(TMP, DIGEST_SHA256, hex, sizeof(hex)) == 0);
    for (char *p = hex; *p; p++)
        if (*p >= 'a' && *p <= 'f') *p = (char)(*p - 'a' + 'A');

    Digest d = {0};
    digest_set(&d, DIGEST_SHA256, DIGEST_ENC_HEX, hex);
    assert(digest_verify_file(TMP, &d) == 0);
    digest_clear(&d);
    cleanup();
}

int main(void)
{
    test_unset_digest_is_a_failure_not_a_pass();
    test_algorithm_is_explicit_not_sniffed();
    test_sha1_and_sha512_hex_verify();
    test_hex_compare_is_case_insensitive();
    test_sha256_returns_64_hex_chars();
    test_sha256_known_value();
    test_sha256_missing_file();
    test_verify_sha256_match();
    test_verify_sha256_mismatch();
    test_verify_sha512_sri_match();
    test_verify_sha512_sri_mismatch();
    test_verify_missing_file();
    test_sha256_deterministic();
    return 0;
}
