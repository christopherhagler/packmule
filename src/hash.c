/*
 * hash.c — Digest construction, comparison, and file verification.
 *
 * See hash.h for why digests are a typed value here rather than a string
 * whose algorithm is inferred from its shape.
 */

#include "hash.h"
#include "utils.h"

#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define READ_CHUNK 65536

/* ── Algorithm plumbing ──────────────────────────────────────────────────── */

const char *digest_algo_name(DigestAlgo algo)
{
    switch (algo) {
    case DIGEST_SHA1:   return "sha1";
    case DIGEST_SHA256: return "sha256";
    case DIGEST_SHA512: return "sha512";
    case DIGEST_NONE:   break;
    }
    return "none";
}

DigestAlgo digest_algo_from_name(const char *name)
{
    if (!name)
        return DIGEST_NONE;
    /* RPM repodata spells SHA-1 as plain "sha". */
    if (strcasecmp(name, "sha")    == 0 ||
        strcasecmp(name, "sha1")   == 0) return DIGEST_SHA1;
    if (strcasecmp(name, "sha256") == 0) return DIGEST_SHA256;
    if (strcasecmp(name, "sha512") == 0) return DIGEST_SHA512;
    return DIGEST_NONE;
}

static const EVP_MD *evp_for(DigestAlgo algo)
{
    switch (algo) {
    case DIGEST_SHA1:   return EVP_sha1();
    case DIGEST_SHA256: return EVP_sha256();
    case DIGEST_SHA512: return EVP_sha512();
    case DIGEST_NONE:   break;
    }
    return NULL;
}

/* ── Digest value ────────────────────────────────────────────────────────── */

void digest_clear(Digest *d)
{
    if (!d)
        return;
    pm_free(d->value);
    d->value = NULL;
    d->algo  = DIGEST_NONE;
    d->enc   = DIGEST_ENC_HEX;
}

void digest_set(Digest *d, DigestAlgo algo, DigestEncoding enc,
                const char *value)
{
    digest_clear(d);
    if (algo == DIGEST_NONE || !value || !*value)
        return;
    d->algo  = algo;
    d->enc   = enc;
    d->value = pm_strdup(value);
}

int digest_is_set(const Digest *d)
{
    return d && d->algo != DIGEST_NONE && d->value && d->value[0];
}

int digest_parse_sri(Digest *d, const char *sri)
{
    if (!sri)
        return -1;

    /* An SRI field may list several digests; the first is enough. */
    const char *dash = strchr(sri, '-');
    if (!dash || dash == sri)
        return -1;

    char algo_name[16];
    size_t alen = (size_t)(dash - sri);
    if (alen >= sizeof(algo_name))
        return -1;
    memcpy(algo_name, sri, alen);
    algo_name[alen] = '\0';

    DigestAlgo algo = digest_algo_from_name(algo_name);
    if (algo == DIGEST_NONE)
        return -1;

    const char *val = dash + 1;
    size_t vlen = strcspn(val, " \t");
    if (vlen == 0)
        return -1;

    char *copy = pm_strndup(val, vlen);
    digest_set(d, algo, DIGEST_ENC_BASE64, copy);
    pm_free(copy);
    return 0;
}

char *digest_to_string(const Digest *d)
{
    if (!digest_is_set(d))
        return NULL;
    return pm_asprintf("%s-%s", digest_algo_name(d->algo), d->value);
}

/* ── File digests ────────────────────────────────────────────────────────── */

/*
 * compute_digest — stream `path` through `md`.  `quiet` suppresses the
 * stderr message when the file simply cannot be opened, which the cache
 * probe treats as an ordinary miss.
 */
static int compute_digest(const char *path, const EVP_MD *md,
                          unsigned char *out, unsigned int *out_len, int quiet)
{
    FILE          *fp  = fopen(path, "rb");
    EVP_MD_CTX    *ctx = NULL;
    unsigned char  buf[READ_CHUNK];
    size_t         n;
    int            ret = -1;

    if (!fp) {
        if (!quiet)
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

static void hex_encode(const unsigned char *bytes, unsigned int len, char *out)
{
    static const char HEX[] = "0123456789abcdef";
    size_t n = len;   /* index in size_t so the *2 offsets cannot narrow */
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = HEX[bytes[i] >> 4];
        out[i * 2 + 1] = HEX[bytes[i] & 0x0f];
    }
    out[n * 2] = '\0';
}

int digest_file_hex(const char *path, DigestAlgo algo, char *out, size_t outsz)
{
    const EVP_MD *md = evp_for(algo);
    if (!md || outsz < DIGEST_HEX_MAX)
        return -1;

    unsigned char raw[EVP_MAX_MD_SIZE];
    unsigned int  raw_len = 0;
    if (compute_digest(path, md, raw, &raw_len, 0) != 0)
        return -1;

    hex_encode(raw, raw_len, out);
    return 0;
}

int sha256_file(const char *path, char out_hex[65])
{
    char buf[DIGEST_HEX_MAX];
    if (digest_file_hex(path, DIGEST_SHA256, buf, sizeof(buf)) != 0)
        return -1;
    memcpy(out_hex, buf, 65);
    return 0;
}

/* ── Verification ────────────────────────────────────────────────────────── */

/*
 * verify_impl — shared core.  `quiet` suppresses all stderr output, for the
 * cache probe where a mismatch is an expected, benign "re-download" signal.
 */
static int verify_impl(const char *path, const Digest *d, int quiet)
{
    if (!digest_is_set(d)) {
        if (!quiet)
            fprintf(stderr,
                    "packmule: refusing to accept '%s': no digest to verify "
                    "it against\n", path);
        return -1;
    }

    const EVP_MD *md = evp_for(d->algo);
    if (!md) {
        if (!quiet)
            fprintf(stderr, "packmule: unsupported digest algorithm for '%s'\n",
                    path);
        return -1;
    }

    unsigned char raw[EVP_MAX_MD_SIZE];
    unsigned int  raw_len = 0;
    if (compute_digest(path, md, raw, &raw_len, quiet) != 0)
        return -1;

    char got[DIGEST_HEX_MAX];
    if (d->enc == DIGEST_ENC_BASE64) {
        /* EVP_EncodeBlock: 64 raw bytes → 88 base64 chars + NUL, so a
         * DIGEST_HEX_MAX buffer is comfortably large enough. */
        unsigned char b64[DIGEST_HEX_MAX];
        int b64_len = EVP_EncodeBlock(b64, raw, (int)raw_len);
        if (b64_len < 0 || (size_t)b64_len >= sizeof(b64))
            return -1;
        b64[b64_len] = '\0';
        memcpy(got, b64, (size_t)b64_len + 1);
    } else {
        hex_encode(raw, raw_len, got);
    }

    /* Hex is compared case-insensitively (some repositories publish upper
     * case); base64 is case-significant. */
    int equal = (d->enc == DIGEST_ENC_BASE64)
              ? strcmp(got, d->value) == 0
              : strcasecmp(got, d->value) == 0;

    if (!equal) {
        if (!quiet)
            fprintf(stderr,
                    "packmule: %s mismatch for '%s'\n"
                    "         expected: %s\n"
                    "         got:      %s\n",
                    digest_algo_name(d->algo), path, d->value, got);
        return -1;
    }
    return 0;
}

int digest_verify_file(const char *path, const Digest *d)
{
    return verify_impl(path, d, 0);
}

int digest_matches_file(const char *path, const Digest *d)
{
    if (!digest_is_set(d))
        return 0;
    return verify_impl(path, d, 1) == 0;
}
