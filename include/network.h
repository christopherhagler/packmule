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
 *
 * Every transfer is restricted to HTTP and HTTPS and capped in size.  URLs
 * here come from registry responses (dist.tarball, PyPI urls[].url, RPM
 * location href), so they are attacker-influenced input in the threat model
 * that matters to an air-gap tool: an index that can name a scheme or an
 * unbounded body can do more than serve the wrong package.
 */

#ifndef PACKMULE_NETWORK_H
#define PACKMULE_NETWORK_H

#include <stddef.h>

/*
 * Largest metadata document we will hold in memory.  RPM primary.xml for a
 * large repository is tens of MB; well beyond that is a broken or hostile
 * server, and growing the buffer forever just turns into an OOM abort.
 */
#define NETWORK_MAX_RESPONSE_BYTES ((size_t)256 * 1024 * 1024)

/* Largest single artifact we will write to disk (8 GiB). */
#define NETWORK_MAX_DOWNLOAD_BYTES (8ull * 1024 * 1024 * 1024)

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
 * `url` must be a fully-formed http/https URL.
 *
 * Connections are pooled across calls: resolution issues one request per
 * package, and a fresh TLS handshake for each of several hundred packages
 * dominates the wall-clock time of a run.
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
 * When `show_progress` is nonzero a single-line progress bar is drawn to
 * stdout, labelled with `label`.  The bar is transient: it is left on the
 * current line without a trailing newline, so the caller is expected to erase
 * or overwrite it.  Callers should only enable this when stdout is a terminal.
 *
 * Returns 0 on success, -1 on failure.
 */
int download_file(const char *url, const char *dest_path,
                  const char *label, int show_progress);

/* ── Parallel downloads ──────────────────────────────────────────────────── */

typedef struct {
    const char *url;
    const char *dest_path;
    const char *label;      /* shown in progress output; may be NULL */
    int         rc;         /* filled in by download_many: 0 ok, -1 failed */
} DownloadJob;

/* Default and maximum number of transfers in flight. */
#define NETWORK_DEFAULT_JOBS 4
#define NETWORK_MAX_JOBS     16

/*
 * download_many — run `n` jobs with at most `concurrency` in flight.
 *
 * Registry CDNs are latency-bound far more than bandwidth-bound for the small
 * files that dominate a dependency closure, so overlapping transfers is worth
 * considerably more here than a faster single stream.
 *
 * `on_done` (may be NULL) is called once per finished job, in completion
 * order, with the number completed so far.  Failed jobs leave no partial file
 * behind and are reported through job->rc.
 *
 * Returns 0 when every job succeeded, -1 otherwise.
 */
int download_many(DownloadJob *jobs, size_t n, int concurrency,
                  void (*on_done)(const DownloadJob *job, size_t completed,
                                  size_t total));

#endif /* PACKMULE_NETWORK_H */
