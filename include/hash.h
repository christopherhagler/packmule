/*
 * hash.h — File digest helpers backed by OpenSSL libcrypto.
 *
 * Ownership: callers supply output buffers; this module has no allocations.
 */

#ifndef PACKMULE_HASH_H
#define PACKMULE_HASH_H

/*
 * sha256_file — compute the SHA-256 digest of the file at `path`.
 *
 * On success writes a lowercase NUL-terminated 64-char hex string into
 * out_hex (≥ 65 bytes) and returns 0.  Returns -1 on any error.
 */
int sha256_file(const char *path, char out_hex[65]);

/*
 * verify_file — verify `path` against `expected`, detecting the algorithm
 * from the format of `expected`:
 *
 *   "sha512-<base64>"  →  SHA-512 SRI (npm dist.integrity)
 *   64 lowercase hex   →  SHA-256 hex  (PyPI)
 *
 * Logs a human-readable mismatch message to stderr on failure.
 * Returns 0 on match, -1 on mismatch or I/O error.
 */
int verify_file(const char *path, const char *expected);

/*
 * file_matches_hash — quiet predicate form of verify_file for cache probes.
 *
 * Returns 1 only if the file at `path` exists, is readable, and matches
 * `expected` (same algorithm detection as verify_file).  Returns 0 on any
 * mismatch, I/O error, or when `expected` is NULL/empty — and never logs to
 * stderr, since a miss here just means "download it".
 */
int file_matches_hash(const char *path, const char *expected);

#endif /* PACKMULE_HASH_H */
