/*
 * network.h — generic libcurl wrappers.
 *
 * This layer knows nothing about specific registries.  URL construction is
 * the responsibility of each registry backend (registry_pypi.c, etc.).
 *
 * Call network_init() once at program start and network_cleanup() on exit.
 *
 * Ownership: functions that return (char *) transfer ownership to the caller,
 *            who must free the result with pm_free().  NULL is returned on
 *            any network or HTTP error; the error is logged to stderr.
 */

#ifndef PACKMULE_NETWORK_H
#define PACKMULE_NETWORK_H

/*
 * Initialise the global libcurl state.
 * Must be called before any other network function.
 * Returns 0 on success, -1 on failure.
 */
int network_init(void);

/* Release all global libcurl resources.  Call once at program exit. */
void network_cleanup(void);

/*
 * fetch_json — perform an HTTP GET request and return the response body.
 *
 * `url` must be a fully-formed URL including scheme.
 *
 * Returns a heap-allocated, NUL-terminated string on success (HTTP 200).
 * CALLER OWNS the buffer and must free it with pm_free().
 * Returns NULL on network error or non-200 HTTP response.
 */
char *fetch_json(const char *url);

/*
 * download_file — fetch `url` and write the response body verbatim to
 * `dest_path`, creating or overwriting the file.
 *
 * Does not verify the file's integrity — the caller should hash the result
 * and compare against the registry-provided digest before trusting it.
 *
 * Returns 0 on success, -1 on failure.
 */
int download_file(const char *url, const char *dest_path);

#endif /* PACKMULE_NETWORK_H */
