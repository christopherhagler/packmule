#include "hash.h"

#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define READ_CHUNK 65536

/* ── Internal helper ─────────────────────────────────────────────────────── */

static int compute_digest(const char *path, const EVP_MD *md,
                          unsigned char *out, unsigned int *out_len)
{
    FILE          *fp  = fopen(path, "rb");
    EVP_MD_CTX    *ctx = NULL;
    unsigned char  buf[READ_CHUNK];
    size_t         n;
    int            ret = -1;

    if (!fp) {
        fprintf(stderr, "packmule: cannot open '%s' for hashing\n", path);
        return -1;
    }
    ctx = EVP_MD_CTX_new();
    if (!ctx) {
        fprintf(stderr, "packmule: EVP_MD_CTX_new() failed\n");
        goto cleanup;
    }
    if (EVP_DigestInit_ex(ctx, md, NULL) != 1) {
        fprintf(stderr, "packmule: EVP_DigestInit_ex() failed\n");
        goto cleanup;
    }
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (EVP_DigestUpdate(ctx, buf, n) != 1) {
            fprintf(stderr, "packmule: EVP_DigestUpdate() failed\n");
            goto cleanup;
        }
    }
    if (ferror(fp)) {
        fprintf(stderr, "packmule: read error while hashing '%s'\n", path);
        goto cleanup;
    }
    if (EVP_DigestFinal_ex(ctx, out, out_len) != 1) {
        fprintf(stderr, "packmule: EVP_DigestFinal_ex() failed\n");
        goto cleanup;
    }
    ret = 0;
cleanup:
    EVP_MD_CTX_free(ctx);
    fclose(fp);
    return ret;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

int sha256_file(const char *path, char out_hex[65])
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  digest_len = 0;

    if (compute_digest(path, EVP_sha256(), digest, &digest_len) != 0)
        return -1;

    for (unsigned int i = 0; i < digest_len; i++)
        snprintf(out_hex + i * 2, 3, "%02x", (unsigned int)digest[i]);
    out_hex[digest_len * 2] = '\0';
    return 0;
}

/*
 * Shared core for verify_file / file_matches_hash.  Detects the algorithm from
 * the format of `expected` and compares.  When `quiet` is non-zero, mismatches
 * are reported only via the return value (no stderr) — used by the download
 * cache probe, where a mismatch is an expected, benign "re-download" signal.
 */
static int verify_file_impl(const char *path, const char *expected, int quiet)
{
    if (strncmp(expected, "sha512-", 7) == 0) {
        /*
         * npm SRI integrity: "sha512-<base64>"
         * Compute SHA-512, base64-encode, compare against the expected string.
         */
        const char   *b64_expected = expected + 7;
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int  digest_len = 0;

        if (compute_digest(path, EVP_sha512(), digest, &digest_len) != 0)
            return -1;

        /* EVP_EncodeBlock: SHA-512 (64 bytes) → 88 base64 chars + NUL */
        unsigned char b64[128];
        int b64_len = EVP_EncodeBlock(b64, digest, (int)digest_len);
        b64[b64_len] = '\0';

        if (strcmp((char *)b64, b64_expected) != 0) {
            if (!quiet)
                fprintf(stderr,
                        "packmule: SHA-512 integrity mismatch for '%s'\n"
                        "         expected: sha512-%s\n"
                        "         got:      sha512-%s\n",
                        path, b64_expected, (char *)b64);
            return -1;
        }
        return 0;
    }

    /* Default: SHA-256 hex (PyPI and RPM).  Compare case-insensitively: some
     * repositories publish uppercase hex digests. */
    char computed[65];
    if (sha256_file(path, computed) != 0)
        return -1;
    if (strcasecmp(computed, expected) != 0) {
        if (!quiet)
            fprintf(stderr,
                    "packmule: SHA-256 mismatch for '%s'\n"
                    "         expected: %s\n"
                    "         got:      %s\n",
                    path, expected, computed);
        return -1;
    }
    return 0;
}

int verify_file(const char *path, const char *expected)
{
    return verify_file_impl(path, expected, 0);
}

int file_matches_hash(const char *path, const char *expected)
{
    /* Never claim a match we can't actually check: an absent expected hash
     * means the file must be (re)downloaded, not trusted on filename alone. */
    if (!expected || !*expected)
        return 0;
    return verify_file_impl(path, expected, 1) == 0;
}
