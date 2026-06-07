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
    assert(verify_file(TMP, hex) == 0);
    cleanup();
}

static void test_verify_sha256_mismatch(void)
{
    write_file(TMP, "packmule test content");
    /* All-zero hash — will never match real content. */
    const char *wrong = "0000000000000000000000000000000000000000000000000000000000000000";
    assert(verify_file(TMP, wrong) == -1);
    cleanup();
}

/* ── verify_file — SHA-512 SRI path ─────────────────────────────────────── */

static void test_verify_sha512_sri_match(void)
{
    write_file(TMP, "npm tarball content");
    char *sri = make_sha512_sri(TMP);
    assert(verify_file(TMP, sri) == 0);
    free(sri);
    cleanup();
}

static void test_verify_sha512_sri_mismatch(void)
{
    write_file(TMP, "npm tarball content");
    /* Syntactically valid SRI prefix with wrong base64 payload. */
    assert(verify_file(TMP, "sha512-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA") == -1);
    cleanup();
}

static void test_verify_missing_file(void)
{
    assert(verify_file("/nonexistent/file.bin",
                       "0000000000000000000000000000000000000000"
                       "0000000000000000000000000000") == -1);
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

int main(void)
{
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
