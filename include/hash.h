/*
 * hash.h — cryptographic digests, as a typed value rather than a bare string.
 *
 * A registry publishes a digest in whatever algorithm and encoding it likes:
 * PyPI serves SHA-256 hex, npm serves SHA-512 base64 in Subresource-Integrity
 * form ("sha512-<base64>"), and RPM repodata serves whatever createrepo_c was
 * configured with (sha1, sha256 or sha512 hex).  Carrying that as one char*
 * and sniffing the algorithm from its shape — the previous design — silently
 * mis-verifies the moment a repository picks an algorithm the sniffer does
 * not recognise.  Digest makes the algorithm explicit, so an unsupported one
 * is a clear error instead of a wrong answer.
 *
 * Ownership: Digest owns `value`.  Use digest_set()/digest_clear(); never
 * assign the field directly.
 */

#ifndef PACKMULE_HASH_H
#define PACKMULE_HASH_H

#include <stddef.h>

typedef enum {
    DIGEST_NONE = 0,
    DIGEST_SHA1,
    DIGEST_SHA256,
    DIGEST_SHA512,
} DigestAlgo;

typedef enum {
    DIGEST_ENC_HEX = 0,
    DIGEST_ENC_BASE64,      /* npm SRI payload */
} DigestEncoding;

typedef struct {
    DigestAlgo     algo;
    DigestEncoding enc;
    char          *value;   /* NULL when algo == DIGEST_NONE */
} Digest;

/* Longest hex digest this module produces (SHA-512), plus NUL. */
#define DIGEST_HEX_MAX 129

/* Human-readable algorithm name ("sha256"), or "none". */
const char *digest_algo_name(DigestAlgo algo);

/*
 * digest_algo_from_name — map a repodata/registry algorithm string
 * ("sha", "sha1", "sha256", "sha512") to a DigestAlgo.  Returns DIGEST_NONE
 * for anything unrecognised.  ("sha" is RPM repodata's spelling of sha1.)
 */
DigestAlgo digest_algo_from_name(const char *name);

/*
 * digest_set — replace `d`'s contents with a copy of `value`.
 * Passing DIGEST_NONE or a NULL/empty value clears the digest.
 */
void digest_set(Digest *d, DigestAlgo algo, DigestEncoding enc,
                const char *value);

/*
 * digest_parse_sri — parse an npm Subresource-Integrity string
 * ("sha512-<base64>", "sha256-<base64>").  Only the first entry of a
 * space-separated list is used.  Returns 0 on success, -1 when the string is
 * not SRI or names an algorithm we do not support.
 */
int digest_parse_sri(Digest *d, const char *sri);

/* Release `d`'s value and reset it to DIGEST_NONE.  Safe with NULL. */
void digest_clear(Digest *d);

/* 1 when `d` carries a usable digest. */
int digest_is_set(const Digest *d);

/*
 * digest_to_string — "<algo>-<value>" (e.g. "sha256-abc…"), heap-allocated,
 * for manifest.json and diagnostics.  Returns NULL when unset.
 * Caller frees with pm_free().
 */
char *digest_to_string(const Digest *d);

/*
 * digest_file_hex — compute `algo` over the file at `path` and write it to
 * `out` as lowercase hex.  `outsz` must be at least DIGEST_HEX_MAX.
 * Returns 0 on success, -1 on I/O or OpenSSL error.
 */
int digest_file_hex(const char *path, DigestAlgo algo, char *out, size_t outsz);

/*
 * digest_verify_file — verify `path` against `d`.
 *
 * Returns 0 on match, -1 on mismatch, I/O error, or an unset digest.  An
 * unset digest is a FAILURE, not a pass: packmule never keeps a file it
 * cannot check.  Mismatches are reported to stderr.
 */
int digest_verify_file(const char *path, const Digest *d);

/*
 * digest_matches_file — quiet predicate form used by the download cache
 * probe, where a miss just means "download it".  Returns 1 only on a
 * confirmed match; never writes to stderr.
 */
int digest_matches_file(const char *path, const Digest *d);

/*
 * sha256_file — lowercase SHA-256 hex of `path` into out_hex (>= 65 bytes).
 * Convenience wrapper over digest_file_hex.
 * Returns 0 on success, -1 on error.
 */
int sha256_file(const char *path, char out_hex[65]);

#endif /* PACKMULE_HASH_H */
