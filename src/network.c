#include "network.h"
#include "utils.h"

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
    return fwrite(contents, size, nmemb, fp);
}

/* ── Shared curl configuration ────────────────────────────────────────────── */

static void configure_common(CURL *curl, char *error_buf)
{
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER,    error_buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,  1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS,       5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,  15L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     "packmule/0.1 libcurl/" LIBCURL_VERSION);
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
}

/* ── Retry policy ─────────────────────────────────────────────────────────── */

#define MAX_ATTEMPTS 3

/*
 * attempt_should_retry — decide whether a finished curl attempt failed
 * transiently (worth retrying) and sleep with backoff if another attempt
 * remains.  Permanent failures (HTTP 4xx) and success return 0.
 * `attempt` is 0-based; backoff is 1s then 3s.
 */
static int attempt_should_retry(CURLcode res, long http_code, int attempt)
{
    int transient = (res != CURLE_OK) || http_code >= 500;
    if (!transient || attempt + 1 >= MAX_ATTEMPTS)
        return 0;
    sleep(attempt == 0 ? 1 : 3);
    return 1;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

int network_init(void)
{
    CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
    return (rc == CURLE_OK) ? 0 : -1;
}

void network_cleanup(void)
{
    curl_global_cleanup();
}

/*
 * fetch_json — GET `url` and return the response body as a heap-allocated
 * NUL-terminated string.
 *
 * CALLER OWNS the returned buffer and must free it with pm_free().
 * Returns NULL on network error or non-200 HTTP response.
 */
char *fetch_json(const char *url)
{
    CURL        *curl = NULL;
    CURLcode     res;
    WriteBuffer  buf  = { NULL, 0 };
    char         curl_error[CURL_ERROR_SIZE];

    curl_error[0] = '\0';

    curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "packmule: curl_easy_init() failed\n");
        return NULL;
    }

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
        buf.size    = 0;
        buf.data[0] = '\0';
        http_code   = 0;

        res = curl_easy_perform(curl);
        if (res == CURLE_OK)
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        if (res == CURLE_OK && http_code == 200) {
            curl_easy_cleanup(curl);
            return buf.data;
        }
        if (!attempt_should_retry(res, http_code, attempt))
            break;
        fprintf(stderr, "packmule: retrying %s ...\n", url);
    }

    if (res != CURLE_OK)
        fprintf(stderr, "packmule: network error fetching %s: %s\n",
                url,
                curl_error[0] ? curl_error : curl_easy_strerror(res));
    else if (http_code == 404)
        fprintf(stderr, "packmule: 404 not found: %s\n", url);
    else
        fprintf(stderr, "packmule: HTTP %ld for %s\n", http_code, url);

    pm_free(buf.data);
    curl_easy_cleanup(curl);
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

/* ── Download ─────────────────────────────────────────────────────────────── */

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
        fclose(fp);
        if (res == CURLE_OK)
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        if (res == CURLE_OK && http_code == 200) {
            curl_easy_cleanup(curl);
            return 0;
        }
        if (!attempt_should_retry(res, http_code, attempt))
            break;
        fprintf(stderr, "packmule: retrying %s ...\n", url);
    }

    if (res != CURLE_OK)
        fprintf(stderr, "packmule: download error for %s: %s\n",
                url,
                curl_error[0] ? curl_error : curl_easy_strerror(res));
    else
        fprintf(stderr, "packmule: HTTP %ld downloading %s\n", http_code, url);

    /* Do not leave a partial/garbage file (e.g. an HTML error body) behind. */
    remove(dest_path);
    curl_easy_cleanup(curl);
    return -1;
}
