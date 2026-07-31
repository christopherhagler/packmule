#include "network.h"
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

static void configure_common(CURL *curl, char *error_buf)
{
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER,    error_buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,  1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS,       5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,  15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL,        1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     "packmule/" PACKMULE_VERSION " libcurl/" LIBCURL_VERSION);
    restrict_protocols(curl);
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
    curl_easy_setopt(curl, CURLOPT_URL,            url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,   write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,       &buf);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    long http_code = 0;
    for (int attempt = 0; attempt < MAX_ATTEMPTS; attempt++) {
        /* Discard any partial body from a failed previous attempt. */
        buf.size     = 0;
        buf.data[0]  = '\0';
        buf.overflow = 0;
        http_code    = 0;

        res = curl_easy_perform(curl);
        if (res == CURLE_OK)
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        if (res == CURLE_OK && http_code == 200)
            return buf.data;

        if (buf.overflow)
            break;   /* a too-large body will be too large next time too */
        if (!attempt_should_retry(res, http_code, attempt))
            break;
        fprintf(stderr, "packmule: retrying %s ...\n", url);
    }

    if (buf.overflow)
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
    curl_easy_setopt(curl, CURLOPT_URL,           url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file_callback);

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
        /* (Re)open with "wb" so a retry truncates the failed attempt's bytes. */
        FILE *fp = fopen(dest_path, "wb");
        if (!fp) {
            fprintf(stderr, "packmule: cannot open %s for writing\n", dest_path);
            curl_easy_cleanup(curl);
            return -1;
        }
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);

        if (show_progress) {
            ps.last_draw = 0.0;
            ps.last_pct  = -1;
            clock_gettime(CLOCK_MONOTONIC, &ps.start);
        }

        http_code = 0;
        res = curl_easy_perform(curl);

        /* fclose is where buffered bytes actually reach the disk; a full disk
         * surfaces here and nowhere else. */
        io_error = (fclose(fp) != 0);
        if (res == CURLE_OK)
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        if (res == CURLE_OK && http_code == 200 && !io_error) {
            curl_easy_cleanup(curl);
            return 0;
        }
        if (io_error)
            break;
        if (!attempt_should_retry(res, http_code, attempt))
            break;
        fprintf(stderr, "packmule: retrying %s ...\n", url);
    }

    if (io_error)
        fprintf(stderr, "packmule: write error saving %s\n", dest_path);
    else if (res != CURLE_OK)
        fprintf(stderr, "packmule: download error for %s: %s\n",
                url, curl_error[0] ? curl_error : curl_easy_strerror(res));
    else
        fprintf(stderr, "packmule: HTTP %ld downloading %s\n", http_code, url);

    /* Do not leave a partial/garbage file (e.g. an HTML error body) behind. */
    remove(dest_path);
    curl_easy_cleanup(curl);
    return -1;
}

/* ── Parallel downloads ───────────────────────────────────────────────────── */

typedef struct {
    DownloadJob *job;
    FILE        *fp;
    CURL        *easy;
    int          attempt;      /* 0-based */
    char         error[CURL_ERROR_SIZE];
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

    curl_easy_reset(t->easy);
    configure_download(t->easy, t->error);
    curl_easy_setopt(t->easy, CURLOPT_URL,           t->job->url);
    curl_easy_setopt(t->easy, CURLOPT_WRITEFUNCTION, write_file_callback);
    curl_easy_setopt(t->easy, CURLOPT_WRITEDATA,     t->fp);
    curl_easy_setopt(t->easy, CURLOPT_NOPROGRESS,    1L);
    curl_easy_setopt(t->easy, CURLOPT_PRIVATE,       t);

    curl_multi_add_handle(multi, t->easy);
    return 0;
}

/* finish_transfer — close the file and decide success, failure, or retry. */
static int finish_transfer(Transfer *t, CURLcode res)
{
    long http_code = 0;
    int  io_error  = (t->fp && fclose(t->fp) != 0);
    t->fp = NULL;

    if (res == CURLE_OK)
        curl_easy_getinfo(t->easy, CURLINFO_RESPONSE_CODE, &http_code);

    if (res == CURLE_OK && http_code == 200 && !io_error)
        return 0;                       /* done, succeeded */

    if (!io_error && is_transient(res, http_code) &&
        t->attempt + 1 < MAX_ATTEMPTS)
        return 1;                       /* retry */

    if (io_error)
        fprintf(stderr, "packmule: write error saving %s\n", t->job->dest_path);
    else if (res != CURLE_OK)
        fprintf(stderr, "packmule: download error for %s: %s\n",
                t->job->url,
                t->error[0] ? t->error : curl_easy_strerror(res));
    else
        fprintf(stderr, "packmule: HTTP %ld downloading %s\n",
                http_code, t->job->url);

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
    size_t next = 0, completed = 0;
    int    failures = 0;

    /* Prime the pipeline. */
    for (int i = 0; i < concurrency && next < n; i++) {
        slots[i].job     = &jobs[next++];
        slots[i].attempt = 0;
        if (start_transfer(multi, &slots[i]) != 0) {
            slots[i].job->rc = -1;
            failures++;
            completed++;
            slots[i].job = NULL;
        }
    }

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

        CURLMsg *msg;
        int      left = 0;
        while ((msg = curl_multi_info_read(multi, &left)) != NULL) {
            if (msg->msg != CURLMSG_DONE)
                continue;

            Transfer *t = NULL;
            curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, (char **)&t);
            curl_multi_remove_handle(multi, msg->easy_handle);

            int verdict = finish_transfer(t, msg->data.result);

            if (verdict == 1) {
                /* Transient: back off briefly and re-arm the same slot. */
                t->attempt++;
                sleep(t->attempt == 1 ? 1 : 3);
                fprintf(stderr, "packmule: retrying %s ...\n", t->job->url);
                if (start_transfer(multi, t) == 0)
                    continue;
                verdict = -1;
            }

            t->job->rc = (verdict == 0) ? 0 : -1;
            if (verdict != 0)
                failures++;
            completed++;
            if (on_done)
                on_done(t->job, completed, n);

            /* Feed the slot its next job. */
            if (next < n) {
                t->job     = &jobs[next++];
                t->attempt = 0;
                if (start_transfer(multi, t) != 0) {
                    t->job->rc = -1;
                    failures++;
                    completed++;
                    if (on_done)
                        on_done(t->job, completed, n);
                    t->job = NULL;
                }
            } else {
                t->job = NULL;
            }
        }
    } while (running > 0 || next < n);

    for (int i = 0; i < concurrency; i++)
        if (slots[i].easy)
            curl_easy_cleanup(slots[i].easy);
    pm_free(slots);
    curl_multi_cleanup(multi);

    return failures == 0 ? 0 : -1;
}
