#include "network.h"
#include "auth.h"
#include "utils.h"
#include "version.h"

#include <curl/curl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* ── Internal types ───────────────────────────────────────────────────────── */

typedef struct {
    char  *data;
    size_t size;
    int    overflow;   /* set when the body exceeded the response cap */
} WriteBuffer;

/* ── libcurl callbacks ────────────────────────────────────────────────────── */

static size_t write_callback(void *contents, size_t size, size_t nmemb,
                              void *userp)
{
    WriteBuffer *buf = (WriteBuffer *)userp;

    if (size != 0 && nmemb > ((size_t)-1) / size)
        return 0;

    size_t realsize = size * nmemb;

    if (buf->size + realsize + 1 < buf->size)
        return 0;

    /* Refuse to keep growing for a server that will not stop talking. */
    if (buf->size + realsize > NETWORK_MAX_RESPONSE_BYTES) {
        buf->overflow = 1;
        return 0;      /* aborts the transfer with CURLE_WRITE_ERROR */
    }

    buf->data = pm_realloc(buf->data, buf->size + realsize + 1);
    memcpy(buf->data + buf->size, contents, realsize);
    buf->size           += realsize;
    buf->data[buf->size] = '\0';

    return realsize;
}

static size_t write_file_callback(void *contents, size_t size, size_t nmemb,
                                   void *userp)
{
    FILE *fp = (FILE *)userp;
    /* curl compares the return against size*nmemb; returning the item count
     * is only equal to that because curl always calls with size == 1.  Return
     * bytes explicitly so a short write is reported as one. */
    size_t items = fwrite(contents, size, nmemb, fp);
    return items * size;
}

/* ── Shared curl configuration ────────────────────────────────────────────── */

/*
 * restrict_protocols — allow http and https only, for the request itself and
 * for anything a redirect points at.
 *
 * libcurl's defaults are broader than this tool ever needs, and the URLs
 * being fetched are supplied by the registry rather than by the user.  Left
 * unrestricted, an index could name file:// or scp:// and turn "download a
 * package" into something else entirely.
 */
static void restrict_protocols(CURL *curl)
{
#if LIBCURL_VERSION_NUM >= 0x075500   /* 7.85.0 */
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR,       "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    long allowed = CURLPROTO_HTTP | CURLPROTO_HTTPS;
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS,       allowed);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, allowed);
#endif
}

/*
 * configure_tls — apply the machine's trust configuration.
 *
 * An internal registry almost always presents a certificate from a corporate
 * CA, and a TLS-terminating proxy re-signs everything else too, so without a
 * way to name that CA the tool is unusable in exactly the environments it is
 * built for.  Supplying the right trust anchor is a different thing from
 * turning verification off, which remains impossible.
 *
 * Applied to every request rather than only to in-scope hosts: this describes
 * how this machine establishes a connection at all, not who it authenticates
 * to.  (A corporate bundle is normally a full replacement trust store, which
 * is why naming one is safe for public hosts as well.)
 */
static void configure_tls(CURL *curl)
{
    const char *ca = auth_ca_bundle();
    if (ca) {
        if (auth_ca_bundle_is_dir())
            curl_easy_setopt(curl, CURLOPT_CAPATH, ca);
        else
            curl_easy_setopt(curl, CURLOPT_CAINFO, ca);
    }

    const char *cert = auth_client_cert();
    if (cert) {
        curl_easy_setopt(curl, CURLOPT_SSLCERT, cert);
        if (auth_client_key())
            curl_easy_setopt(curl, CURLOPT_SSLKEY, auth_client_key());
        if (auth_client_key_password())
            curl_easy_setopt(curl, CURLOPT_KEYPASSWD,
                             auth_client_key_password());
    }
}

static void configure_common(CURL *curl, char *error_buf)
{
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER,    error_buf);
    /*
     * Redirects are followed by hand (see next_redirect).  libcurl's automatic
     * following drops the Authorization header when a redirect crosses to
     * another host, but it does not touch a custom header — so an operator
     * using PACKMULE_AUTH_HEADER ("X-JFrog-Art-Api: …") would have that
     * credential follow the redirect to wherever the index pointed.  Walking
     * the chain ourselves lets the same host-scoping rule govern every scheme,
     * and stops the guarantee depending on which libcurl version is installed.
     */
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,  0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,  15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL,        1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     "packmule/" PACKMULE_VERSION " libcurl/" LIBCURL_VERSION);
    restrict_protocols(curl);
    configure_tls(curl);
    /* CURLOPT_PROXY is deliberately not set: leaving it unset lets libcurl
     * honour http_proxy / https_proxy / no_proxy from the environment, which
     * is how corporate egress is already configured on these machines. */
}

/*
 * Metadata requests (registry JSON, repomd.xml) are small, so a total-time
 * cap is a clean way to bound them.
 */
static void configure_metadata(CURL *curl, char *error_buf)
{
    configure_common(curl, error_buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
}

/*
 * Package downloads can be arbitrarily large (an ML wheel is hundreds of MB),
 * so a total-time cap would abort legitimate slow transfers.  Instead abort
 * only when the transfer stalls: under 1 KB/s for 30 consecutive seconds.
 */
static void configure_download(CURL *curl, char *error_buf)
{
    configure_common(curl, error_buf);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,  30L);
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE,
                     (curl_off_t)NETWORK_MAX_DOWNLOAD_BYTES);
}

/* ── Authentication ───────────────────────────────────────────────────────── */

/*
 * apply_auth — attach credentials to `curl` if `url`'s host is in scope.
 *
 * Must be called after configure_*() and after any curl_easy_reset(), both of
 * which clear the options set here.
 *
 * Returns a header list that the caller must keep alive for the duration of
 * the transfer and then release with curl_slist_free_all(), or NULL when there
 * is nothing to free.
 *
 * Redirects are the interesting case.  Artifactory and Nexus commonly answer a
 * download with a 302 to a pre-signed S3 or CDN URL on a different host, which
 * must NOT receive the credential — S3 rejects a request carrying both a
 * pre-signed query and an Authorization header, and more importantly the token
 * is none of that host's business.
 *
 * libcurl is not relied on to get that right.  It drops CURLOPT_USERPWD and
 * (since 7.58.0) a caller-supplied Authorization header across a cross-host
 * redirect, but it does nothing about a custom header, which is exactly what
 * PACKMULE_AUTH_HEADER produces.  So redirects are followed by hand and this
 * function is called afresh for each hop with that hop's URL; every scheme is
 * then governed by the same host-scoping rule.  See next_redirect().
 */
static struct curl_slist *apply_auth(CURL *curl, const char *url)
{
    switch (auth_scheme_for_url(url)) {
    case AUTH_BASIC:
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
        curl_easy_setopt(curl, CURLOPT_USERPWD,  auth_userpwd());
        return NULL;

    case AUTH_BEARER:
    case AUTH_HEADER: {
        /*
         * Set by hand rather than with CURLOPT_XOAUTH2_BEARER: that option
         * only emits the header for schemes negotiated via CURLAUTH_BEARER,
         * which needs libcurl 7.61 *and* a server that advertises it.  A
         * literal header is what registries actually expect.
         *
         * AUTH_HEADER takes the same path with an operator-supplied line
         * ("X-JFrog-Art-Api: …", "PRIVATE-TOKEN: …"), which is what lets this
         * work against a product whose convention we do not know.  Note that
         * libcurl only strips *Authorization* across a cross-host redirect, so
         * a custom-named header would otherwise follow the redirect — see the
         * explicit check in apply_auth's caller contract: we only ever attach
         * it to an in-scope URL, and re-evaluate scope after each transfer
         * begins rather than reusing a handle across hosts.
         */
        struct curl_slist *hdrs = curl_slist_append(NULL, auth_bearer_header());
        if (hdrs)
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
        return hdrs;
    }

    case AUTH_NONE:
    default:
        return NULL;
    }
}

/* ── Redirects ────────────────────────────────────────────────────────────── */

#define MAX_REDIRECTS 5

static int is_redirect(long http_code)
{
    return http_code == 301 || http_code == 302 || http_code == 303 ||
           http_code == 307 || http_code == 308;
}

/*
 * next_redirect — the URL a 3xx points at, or NULL to stop.
 *
 * libcurl resolves the Location header against the request URL for us even
 * with CURLOPT_FOLLOWLOCATION off, so this does not re-implement RFC 3986.
 * What it must do itself is enforce the scheme restriction that
 * CURLOPT_REDIR_PROTOCOLS would have applied: an index that answers a
 * download with a redirect to file:///etc/shadow gets refused here.
 *
 * Returns a heap copy the caller frees with pm_free().
 */
static char *next_redirect(CURL *curl, long http_code, int hop)
{
    if (!is_redirect(http_code))
        return NULL;

    if (hop >= MAX_REDIRECTS) {
        fprintf(stderr, "packmule: too many redirects (limit %d)\n",
                MAX_REDIRECTS);
        return NULL;
    }

    char *location = NULL;
    curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &location);
    if (!location || !*location) {
        fprintf(stderr, "packmule: HTTP %ld with no usable Location header\n",
                http_code);
        return NULL;
    }

    /* auth_url_host() returns NULL for anything that is not absolute http(s),
     * which is exactly the test needed here. */
    char *host = auth_url_host(location);
    if (!host) {
        fprintf(stderr,
                "packmule: refusing a redirect to a non-http(s) location\n");
        return NULL;
    }
    pm_free(host);

    return pm_strdup(location);
}

/*
 * report_auth_failure — explain a 401/403.
 *
 * The three causes need three different fixes, and the bare status code sends
 * people looking at the wrong one: no credentials at all, credentials that
 * exist but are scoped to a different host than the one that refused us, and
 * credentials the server rejected.
 */
static void report_auth_failure(const char *url, long http_code)
{
    fprintf(stderr, "packmule: HTTP %ld (unauthorised) for %s\n",
            http_code, url);

    if (!auth_configured()) {
        fprintf(stderr,
                "          This index requires authentication.  Set "
                "PACKMULE_USERNAME and\n"
                "          PACKMULE_PASSWORD, or PACKMULE_TOKEN for a bearer "
                "token.\n");
        return;
    }

    if (auth_scheme_for_url(url) == AUTH_NONE) {
        fprintf(stderr,
                "          Credentials were NOT sent: this URL's host is "
                "outside the authenticated\n"
                "          scope (%s).  If the index serves files from another "
                "host, add it to\n"
                "          PACKMULE_AUTH_HOSTS.\n",
                auth_scope_description());
        return;
    }

    fprintf(stderr,
            "          Credentials were sent and rejected.  Check the token or "
            "password, and that\n"
            "          the account may read this repository.\n");
}

/* ── Retry policy ─────────────────────────────────────────────────────────── */

#define MAX_ATTEMPTS 3

/* Is this failure worth another attempt? */
static int is_transient(CURLcode res, long http_code)
{
    /* 429 (rate limited) is transient too: PyPI and npm both rate-limit, and
     * a large manifest can trip it mid-run. */
    return (res != CURLE_OK) || http_code >= 500 || http_code == 429;
}

/*
 * attempt_should_retry — decide whether a finished curl attempt failed
 * transiently and sleep with backoff if another attempt remains.
 * `attempt` is 0-based; backoff is 1s then 3s.
 */
static int attempt_should_retry(CURLcode res, long http_code, int attempt)
{
    if (!is_transient(res, http_code) || attempt + 1 >= MAX_ATTEMPTS)
        return 0;
    sleep(attempt == 0 ? 1 : 3);
    return 1;
}

/* ── Global state ─────────────────────────────────────────────────────────── */

/*
 * A single reusable handle for metadata requests.  Resolution makes one
 * request per package, and a fresh connection (DNS + TCP + TLS) for each of
 * several hundred packages costs more than everything else in the run put
 * together.  packmule is single-threaded, so one handle is enough.
 */
static CURL *g_meta_handle;

int network_init(void)
{
    CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (rc != CURLE_OK)
        return -1;
    g_meta_handle = curl_easy_init();
    if (!g_meta_handle) {
        curl_global_cleanup();
        return -1;
    }
    return 0;
}

void network_cleanup(void)
{
    if (g_meta_handle) {
        curl_easy_cleanup(g_meta_handle);
        g_meta_handle = NULL;
    }
    curl_global_cleanup();
}

/* ── Metadata fetch ───────────────────────────────────────────────────────── */

char *fetch_json(const char *url)
{
    if (!g_meta_handle) {
        fprintf(stderr, "packmule: network not initialised\n");
        return NULL;
    }

    CURL        *curl = g_meta_handle;
    CURLcode     res  = CURLE_OK;
    WriteBuffer  buf  = { NULL, 0, 0 };
    char         curl_error[CURL_ERROR_SIZE];

    curl_error[0] = '\0';

    /* Reset per-request options but keep the connection cache. */
    curl_easy_reset(curl);

    buf.data    = pm_malloc(1);
    buf.data[0] = '\0';

    configure_metadata(curl, curl_error);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,   write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,       &buf);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    struct curl_slist *auth_hdrs = NULL;
    char *current = pm_strdup(url);   /* the URL of the hop in flight */

    long http_code = 0;
    for (int attempt = 0; attempt < MAX_ATTEMPTS; attempt++) {
        /* Each attempt restarts the redirect chain from the original URL. */
        pm_free(current);
        current = pm_strdup(url);

        for (int hop = 0; ; hop++) {
            /* Discard any partial body from the previous hop or attempt. */
            buf.size     = 0;
            buf.data[0]  = '\0';
            buf.overflow = 0;
            http_code    = 0;

            /* Credentials are decided per hop, from the host actually about to
             * be contacted — that is what keeps a redirect from carrying them
             * somewhere they do not belong. */
            curl_slist_free_all(auth_hdrs);
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, NULL);
            curl_easy_setopt(curl, CURLOPT_USERPWD,    NULL);
            curl_easy_setopt(curl, CURLOPT_URL,        current);
            auth_hdrs = apply_auth(curl, current);

            res = curl_easy_perform(curl);
            if (res == CURLE_OK)
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

            char *next = (res == CURLE_OK)
                       ? next_redirect(curl, http_code, hop) : NULL;
            if (!next)
                break;                  /* final response, or a refused hop */
            pm_free(current);
            current = next;
        }

        if (res == CURLE_OK && http_code == 200) {
            curl_slist_free_all(auth_hdrs);
            pm_free(current);
            return buf.data;
        }

        if (buf.overflow)
            break;   /* a too-large body will be too large next time too */
        /* 401/403 will not become 200 on a second identical request. */
        if (http_code == 401 || http_code == 403)
            break;
        if (!attempt_should_retry(res, http_code, attempt))
            break;
        fprintf(stderr, "packmule: retrying %s ...\n", url);
    }

    /*
     * Diagnose before releasing anything: an unauthorised response is
     * reported against `current`, the hop that actually refused us, which is
     * not the original URL once a redirect has been followed.
     */
    if (http_code == 401 || http_code == 403)
        report_auth_failure(current, http_code);
    else if (buf.overflow)
        fprintf(stderr,
                "packmule: response from %s exceeds %zu MB; refusing it\n",
                url, NETWORK_MAX_RESPONSE_BYTES / ((size_t)1024 * 1024));
    else if (res != CURLE_OK)
        fprintf(stderr, "packmule: network error fetching %s: %s\n",
                url, curl_error[0] ? curl_error : curl_easy_strerror(res));
    else if (http_code == 404)
        fprintf(stderr, "packmule: 404 not found: %s\n", url);
    else
        fprintf(stderr, "packmule: HTTP %ld for %s\n", http_code, url);

    curl_slist_free_all(auth_hdrs);
    pm_free(current);
    pm_free(buf.data);
    return NULL;
}

/* ── Progress bar ─────────────────────────────────────────────────────────── */

typedef struct {
    const char     *label;     /* subject shown on the bar (e.g. filename) */
    struct timespec start;     /* transfer start, for speed calculation */
    double          last_draw; /* seconds-since-start at the last redraw */
    long            last_pct;  /* last percentage drawn (-1 = none yet) */
    int             cols;      /* terminal width, queried once */
} ProgressState;

/* Monotonic seconds elapsed since `start`. */
static double secs_since(const struct timespec *start)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - start->tv_sec)
         + (double)(now.tv_nsec - start->tv_nsec) / 1e9;
}

/* Best-effort terminal width; defaults to 80 when unavailable. */
static int terminal_cols(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
}

/*
 * progress_cb — libcurl CURLOPT_XFERINFOFUNCTION callback.
 *
 * Draws a single in-place line (leading '\r', trailing CSI-K erase, never a
 * newline) sized to fit the terminal, so it overwrites itself instead of
 * scrolling.  Redraws are throttled to avoid flooding the terminal.
 */
static int progress_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow)
{
    (void)ultotal;
    (void)ulnow;

    ProgressState *ps      = (ProgressState *)clientp;
    double         elapsed = secs_since(&ps->start);
    long           pct     = (dltotal > 0)
                           ? (long)((dlnow * 100) / dltotal)
                           : -1;

    /* Throttle: redraw only on a percent change or every ~100 ms. */
    if (pct == ps->last_pct && elapsed - ps->last_draw < 0.1)
        return 0;
    ps->last_pct  = pct;
    ps->last_draw = elapsed;

    /* Sized to hold rate (up to 15 chars) + "/s" + NUL without truncation,
     * which GCC's -Wformat-truncation verifies at compile time. */
    char speed[24] = "";
    if (elapsed > 0.001) {
        char rate[16];
        pm_human_size((double)dlnow / elapsed, rate, sizeof(rate));
        snprintf(speed, sizeof(speed), "%s/s", rate);
    }

    if (dltotal <= 0) {
        /* Unknown length: show transferred bytes only, no bar. */
        char got[16];
        pm_human_size((double)dlnow, got, sizeof(got));
        printf("\r  %-.40s  %8s  %11s\033[K", ps->label, got, speed);
        fflush(stdout);
        return 0;
    }

    /*
     * Lay the line out to fit `cols`.  Fixed overhead is the label field, the
     * brackets, the percentage and speed columns and their separators; the bar
     * gets whatever space is left (clamped to a sane range).
     */
    int label_w = 28;
    int overhead = 2 /*indent*/ + label_w + 2 + 2 /*[]*/ + 6 /*" 100%"*/
                 + 2 + 12 /*speed*/;
    int bar_w = ps->cols - overhead;
    if (bar_w < 10) bar_w = 10;
    if (bar_w > 40) bar_w = 40;

    int filled = (int)((dlnow * bar_w) / dltotal);
    if (filled > bar_w) filled = bar_w;

    char bar[64];
    for (int i = 0; i < bar_w; i++)
        bar[i] = (i < filled) ? '#' : '-';
    bar[bar_w] = '\0';

    printf("\r  %-*.*s  [%s] %3ld%%  %11s\033[K",
           label_w, label_w, ps->label, bar, pct, speed);
    fflush(stdout);
    return 0;
}

/* ── Single download ──────────────────────────────────────────────────────── */

int download_file(const char *url, const char *dest_path,
                  const char *label, int show_progress)
{
    CURL    *curl = NULL;
    CURLcode res  = CURLE_OK;
    char     curl_error[CURL_ERROR_SIZE];

    curl_error[0] = '\0';

    curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "packmule: curl_easy_init() failed\n");
        return -1;
    }

    configure_download(curl, curl_error);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file_callback);

    struct curl_slist *auth_hdrs = NULL;
    char *current = pm_strdup(url);

    ProgressState ps;
    if (show_progress) {
        ps.label     = label ? label : "";
        ps.cols      = terminal_cols();
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA,     &ps);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS,       0L);
    } else {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS,       1L);
    }

    long http_code = 0;
    int  io_error  = 0;

    for (int attempt = 0; attempt < MAX_ATTEMPTS; attempt++) {
        pm_free(current);
        current = pm_strdup(url);

        for (int hop = 0; ; hop++) {
            /* (Re)open with "wb" so a retry, or a redirect's discarded 3xx
             * body, does not accumulate in the destination file. */
            FILE *fp = fopen(dest_path, "wb");
            if (!fp) {
                fprintf(stderr, "packmule: cannot open %s for writing\n",
                        dest_path);
                curl_slist_free_all(auth_hdrs);
                pm_free(current);
                curl_easy_cleanup(curl);
                return -1;
            }
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);

            if (show_progress) {
                ps.last_draw = 0.0;
                ps.last_pct  = -1;
                clock_gettime(CLOCK_MONOTONIC, &ps.start);
            }

            /* Re-decide credentials from the host of this hop. */
            curl_slist_free_all(auth_hdrs);
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, NULL);
            curl_easy_setopt(curl, CURLOPT_USERPWD,    NULL);
            curl_easy_setopt(curl, CURLOPT_URL,        current);
            auth_hdrs = apply_auth(curl, current);

            http_code = 0;
            res = curl_easy_perform(curl);

            /* fclose is where buffered bytes actually reach the disk; a full
             * disk surfaces here and nowhere else. */
            io_error = (fclose(fp) != 0);
            if (res == CURLE_OK)
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

            char *next = (res == CURLE_OK && !io_error)
                       ? next_redirect(curl, http_code, hop) : NULL;
            if (!next)
                break;                  /* final response, or a refused hop */
            pm_free(current);
            current = next;
        }

        if (res == CURLE_OK && http_code == 200 && !io_error) {
            curl_slist_free_all(auth_hdrs);
            pm_free(current);
            curl_easy_cleanup(curl);
            return 0;
        }
        if (io_error)
            break;
        if (http_code == 401 || http_code == 403)
            break;
        if (!attempt_should_retry(res, http_code, attempt))
            break;
        fprintf(stderr, "packmule: retrying %s ...\n", url);
    }

    if (io_error)
        fprintf(stderr, "packmule: write error saving %s\n", dest_path);
    else if (res != CURLE_OK)
        fprintf(stderr, "packmule: download error for %s: %s\n",
                current, curl_error[0] ? curl_error : curl_easy_strerror(res));
    else if (http_code == 401 || http_code == 403)
        report_auth_failure(current, http_code);
    else
        fprintf(stderr, "packmule: HTTP %ld downloading %s\n",
                http_code, current);

    /* Do not leave a partial/garbage file (e.g. an HTML error body) behind. */
    remove(dest_path);
    curl_slist_free_all(auth_hdrs);
    pm_free(current);
    curl_easy_cleanup(curl);
    return -1;
}

/* ── Parallel downloads ───────────────────────────────────────────────────── */

typedef struct {
    DownloadJob       *job;
    FILE              *fp;
    CURL              *easy;
    struct curl_slist *auth_hdrs;  /* owned; freed when the slot is re-armed */
    char              *cur_url;    /* owned; the hop in flight, != job->url
                                    * once a redirect has been followed */
    int                attempt;    /* 0-based */
    int                hop;        /* redirects followed for this attempt */
    char               error[CURL_ERROR_SIZE];
} Transfer;

/* start_transfer — open the destination and hand the easy handle to `multi`. */
static int start_transfer(CURLM *multi, Transfer *t)
{
    t->fp = fopen(t->job->dest_path, "wb");
    if (!t->fp) {
        fprintf(stderr, "packmule: cannot open %s for writing\n",
                t->job->dest_path);
        return -1;
    }

    t->error[0] = '\0';
    if (!t->easy)
        t->easy = curl_easy_init();
    if (!t->easy) {
        fclose(t->fp);
        t->fp = NULL;
        return -1;
    }

    if (!t->cur_url)
        t->cur_url = pm_strdup(t->job->url);

    curl_easy_reset(t->easy);
    configure_download(t->easy, t->error);
    curl_easy_setopt(t->easy, CURLOPT_URL,           t->cur_url);
    curl_easy_setopt(t->easy, CURLOPT_WRITEFUNCTION, write_file_callback);
    curl_easy_setopt(t->easy, CURLOPT_WRITEDATA,     t->fp);
    curl_easy_setopt(t->easy, CURLOPT_NOPROGRESS,    1L);
    curl_easy_setopt(t->easy, CURLOPT_PRIVATE,       t);

    /* A slot is reused for retries, redirect hops and later jobs, and each of
     * those may target a different host: drop the previous header list before
     * building the one this URL calls for, and derive credentials from the
     * host actually about to be contacted.  curl_easy_reset() above has
     * already cleared the handle's pointer to it, so freeing cannot dangle. */
    curl_slist_free_all(t->auth_hdrs);
    t->auth_hdrs = apply_auth(t->easy, t->cur_url);

    curl_multi_add_handle(multi, t->easy);
    return 0;
}

/* Release a slot's per-job state before it takes on a different job. */
static void transfer_reset_url(Transfer *t, const char *url)
{
    pm_free(t->cur_url);
    t->cur_url = url ? pm_strdup(url) : NULL;
    t->hop     = 0;
}

/* Bookkeeping shared by the priming loop and the re-arm path. */
typedef struct {
    DownloadJob *jobs;
    size_t       n;
    size_t       next;       /* index of the first job not yet handed out */
    size_t       completed;
    int          failures;
    void       (*on_done)(const DownloadJob *job, size_t completed,
                          size_t total);
} Queue;

/*
 * arm_slot — give `t` the next job that can actually be started.
 *
 * start_transfer fails before any network activity when the destination
 * cannot be opened (a full disk, a read-only mount, too many open files).
 * That is a property of this job's destination, not of the queue, so the
 * failure is recorded and the slot moves on to the next job.  Retiring the
 * slot instead — as this once did — could idle every slot while jobs
 * remained, leaving the event loop with nothing in flight and nothing able
 * to advance it.
 *
 * On return the slot is either armed, or t->job is NULL because the queue is
 * empty.  Those are the only two states, which is what lets the loop below
 * treat "no transfers running" as "the queue is drained".
 */
static void arm_slot(CURLM *multi, Transfer *t, Queue *q)
{
    while (q->next < q->n) {
        t->job     = &q->jobs[q->next++];
        t->attempt = 0;
        transfer_reset_url(t, t->job->url);

        if (start_transfer(multi, t) == 0)
            return;

        t->job->rc = -1;
        q->failures++;
        q->completed++;
        if (q->on_done)
            q->on_done(t->job, q->completed, q->n);
    }
    t->job = NULL;
}

/*
 * finish_transfer — close the file and decide what happens next.
 *
 * Returns 0 done/succeeded, -1 done/failed, 1 retry, 2 follow a redirect
 * (t->cur_url has been advanced to the next hop).
 */
static int finish_transfer(Transfer *t, CURLcode res)
{
    long http_code = 0;
    int  io_error  = (t->fp && fclose(t->fp) != 0);
    t->fp = NULL;

    if (res == CURLE_OK)
        curl_easy_getinfo(t->easy, CURLINFO_RESPONSE_CODE, &http_code);

    if (res == CURLE_OK && http_code == 200 && !io_error)
        return 0;                       /* done, succeeded */

    if (res == CURLE_OK && !io_error) {
        char *next = next_redirect(t->easy, http_code, t->hop);
        if (next) {
            pm_free(t->cur_url);
            t->cur_url = next;
            t->hop++;
            return 2;                   /* follow it, re-deciding auth */
        }
    }

    if (!io_error && is_transient(res, http_code) &&
        t->attempt + 1 < MAX_ATTEMPTS)
        return 1;                       /* retry */

    if (io_error)
        fprintf(stderr, "packmule: write error saving %s\n", t->job->dest_path);
    else if (res != CURLE_OK)
        fprintf(stderr, "packmule: download error for %s: %s\n",
                t->job->url,
                t->error[0] ? t->error : curl_easy_strerror(res));
    else if (http_code == 401 || http_code == 403)
        report_auth_failure(t->cur_url, http_code);
    else
        fprintf(stderr, "packmule: HTTP %ld downloading %s\n",
                http_code, t->cur_url);

    remove(t->job->dest_path);
    return -1;                          /* done, failed */
}

/*
 * multi_wait — block until a transfer needs attention, or until timeout_ms.
 *
 * curl_multi_poll() is the call we want: unlike curl_multi_wait() it also
 * waits when there is no file descriptor to wait on yet — during name
 * resolution, or while a retry backs off.  It arrived in libcurl 7.66.0 and
 * RHEL/EPEL 8 ships 7.61, so fall back there and do the waiting by hand.
 *
 * The manual sleep is the whole point of the fallback: curl_multi_wait()
 * returns immediately with numfds == 0 in exactly those cases, so without it
 * the loop below spins on the CPU instead of waiting.
 */
static CURLMcode multi_wait(CURLM *multi, int timeout_ms)
{
#if LIBCURL_VERSION_NUM >= 0x074200   /* 7.66.0 */
    return curl_multi_poll(multi, NULL, 0, timeout_ms, NULL);
#else
    int       numfds = 0;
    CURLMcode mc     = curl_multi_wait(multi, NULL, 0, timeout_ms, &numfds);

    if (mc == CURLM_OK && numfds == 0) {
        struct timespec ts = { .tv_sec  =  timeout_ms / 1000,
                               .tv_nsec = (timeout_ms % 1000) * 1000000L };
        nanosleep(&ts, NULL);
    }
    return mc;
#endif
}

int download_many(DownloadJob *jobs, size_t n, int concurrency,
                  void (*on_done)(const DownloadJob *job, size_t completed,
                                  size_t total))
{
    if (n == 0)
        return 0;
    if (concurrency < 1)                  concurrency = 1;
    if (concurrency > NETWORK_MAX_JOBS)   concurrency = NETWORK_MAX_JOBS;
    if ((size_t)concurrency > n)          concurrency = (int)n;

    CURLM *multi = curl_multi_init();
    if (!multi) {
        fprintf(stderr, "packmule: curl_multi_init() failed\n");
        return -1;
    }

    Transfer *slots = pm_calloc((size_t)concurrency, sizeof(Transfer));
    Queue     q     = { jobs, n, 0, 0, 0, on_done };

    /* Prime the pipeline. */
    for (int i = 0; i < concurrency; i++)
        arm_slot(multi, &slots[i], &q);

    int running = 0;
    do {
        CURLMcode mc = curl_multi_perform(multi, &running);
        if (mc == CURLM_OK && running)
            mc = multi_wait(multi, 200);
        if (mc != CURLM_OK) {
            fprintf(stderr, "packmule: curl_multi error: %s\n",
                    curl_multi_strerror(mc));
            break;
        }

        /*
         * Nothing in flight while jobs remain would mean no message can
         * arrive and nothing can re-arm a slot: the loop condition below
         * would then spin on the CPU forever.  arm_slot() makes this
         * unreachable, but a busy loop is far too bad a failure mode to
         * leave one refactor away, so fail the remaining jobs and stop.
         */
        if (running == 0 && q.next < n) {
            fprintf(stderr,
                    "packmule: internal error: %zu download(s) left "
                    "unattempted\n", n - q.next);
            while (q.next < n) {
                jobs[q.next++].rc = -1;
                q.failures++;
            }
            break;
        }

        CURLMsg *msg;
        int      left = 0;
        while ((msg = curl_multi_info_read(multi, &left)) != NULL) {
            if (msg->msg != CURLMSG_DONE)
                continue;

            Transfer *t = NULL;
            curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, (char **)&t);
            curl_multi_remove_handle(multi, msg->easy_handle);

            int verdict = finish_transfer(t, msg->data.result);

            if (verdict == 2) {
                /* Redirect: re-arm the same slot on the next hop.  Not a
                 * retry, so the attempt counter is untouched. */
                if (start_transfer(multi, t) == 0)
                    continue;
                verdict = -1;
            }

            if (verdict == 1) {
                /* Transient: back off briefly and re-arm the same slot, from
                 * the original URL rather than wherever the chain ended. */
                t->attempt++;
                transfer_reset_url(t, t->job->url);
                sleep(t->attempt == 1 ? 1 : 3);
                fprintf(stderr, "packmule: retrying %s ...\n", t->job->url);
                if (start_transfer(multi, t) == 0)
                    continue;
                verdict = -1;
            }

            t->job->rc = (verdict == 0) ? 0 : -1;
            if (verdict != 0)
                q.failures++;
            q.completed++;
            if (on_done)
                on_done(t->job, q.completed, n);

            /* Feed the slot its next job. */
            arm_slot(multi, t, &q);
        }
    } while (running > 0 || q.next < n);

    for (int i = 0; i < concurrency; i++) {
        if (slots[i].easy)
            curl_easy_cleanup(slots[i].easy);
        curl_slist_free_all(slots[i].auth_hdrs);
        pm_free(slots[i].cur_url);
    }
    pm_free(slots);
    curl_multi_cleanup(multi);

    return q.failures == 0 ? 0 : -1;
}
