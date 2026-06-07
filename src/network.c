#include "network.h"
#include "utils.h"

#include <curl/curl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ── Internal types ───────────────────────────────────────────────────────── */

typedef struct {
    char  *data;
    size_t size;
} WriteBuffer;

/*
 * ProgressCtx — state carried into the CURLOPT_XFERINFOFUNCTION callback.
 *
 * start       : monotonic time at the moment curl_easy_perform() was called
 * last_update : monotonic time of the previous bar render (rate-limiting)
 * is_tty      : 1 when stdout is an interactive terminal
 */
typedef struct {
    struct timespec start;
    struct timespec last_update;
    int             is_tty;
} ProgressCtx;

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

/* ── Progress bar ─────────────────────────────────────────────────────────── */

static void fmt_bytes(double n, char *out, size_t sz)
{
    if (n >= 1048576.0)
        snprintf(out, sz, "%.1f MB", n / 1048576.0);
    else if (n >= 1024.0)
        snprintf(out, sz, "%.1f KB", n / 1024.0);
    else
        snprintf(out, sz, "%.0f B", n);
}

/*
 * xferinfo_cb — called by libcurl after each received chunk.
 *
 * Rate-limited to ≈10 redraws per second; always redraws on completion
 * (dlnow == dltotal && dltotal > 0).  Uses \r to overwrite the current
 * terminal line in-place.
 */
static int xferinfo_cb(void *userdata, curl_off_t dltotal, curl_off_t dlnow,
                        curl_off_t ultotal, curl_off_t ulnow)
{
    (void)ultotal; (void)ulnow;

    ProgressCtx *ctx = (ProgressCtx *)userdata;
    if (!ctx->is_tty || dlnow < 0)
        return 0;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    int is_done = (dltotal > 0 && dlnow >= dltotal);

    /* Throttle to 10 Hz unless this is the final update. */
    if (!is_done) {
        double since = (double)(now.tv_sec  - ctx->last_update.tv_sec) +
                       (double)(now.tv_nsec - ctx->last_update.tv_nsec) * 1e-9;
        if (since < 0.1)
            return 0;
    }
    ctx->last_update = now;

    double elapsed = (double)(now.tv_sec  - ctx->start.tv_sec) +
                     (double)(now.tv_nsec - ctx->start.tv_nsec) * 1e-9;
    double speed   = (elapsed > 0.001) ? (double)dlnow / elapsed : 0.0;

    char speed_str[24], down_str[24], total_str[24];
    fmt_bytes(speed,          speed_str, sizeof(speed_str));
    fmt_bytes((double)dlnow,  down_str,  sizeof(down_str));

    if (dltotal > 0) {
        int pct    = (int)((double)dlnow * 100.0 / (double)dltotal);
        if (pct > 100) pct = 100;
        int filled = (pct * 20) / 100;

        char bar[21];
        for (int i = 0; i < 20; i++) {
            if (i < filled)
                bar[i] = '=';
            else if (i == filled && !is_done)
                bar[i] = '>';
            else
                bar[i] = ' ';
        }
        bar[20] = '\0';

        fmt_bytes((double)dltotal, total_str, sizeof(total_str));

        fprintf(stdout, "\r  [%s] %3d%%  %s / %s  %s/s     ",
                bar, pct, down_str, total_str, speed_str);
    } else {
        /* Content-Length unknown — show amount + speed without a bar. */
        fprintf(stdout, "\r  %s downloaded  %s/s          ",
                down_str, speed_str);
    }
    fflush(stdout);
    return 0;
}

/* ── Shared curl configuration ────────────────────────────────────────────── */

static void configure_common(CURL *curl, char *error_buf)
{
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER,    error_buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,  1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS,       5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,         60L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,  15L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     "packmule/0.1 libcurl/" LIBCURL_VERSION);
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

    configure_common(curl, curl_error);
    curl_easy_setopt(curl, CURLOPT_URL,            url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,   write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,       &buf);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        fprintf(stderr, "packmule: network error fetching %s: %s\n",
                url,
                curl_error[0] ? curl_error : curl_easy_strerror(res));
        pm_free(buf.data);
        buf.data = NULL;
        goto cleanup;
    }

    {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code == 404) {
            fprintf(stderr, "packmule: 404 not found: %s\n", url);
            pm_free(buf.data);
            buf.data = NULL;
        } else if (http_code != 200) {
            fprintf(stderr, "packmule: HTTP %ld for %s\n", http_code, url);
            pm_free(buf.data);
            buf.data = NULL;
        }
    }

cleanup:
    curl_easy_cleanup(curl);
    return buf.data;
}

int download_file(const char *url, const char *dest_path, int show_progress)
{
    CURL    *curl = NULL;
    CURLcode res;
    FILE    *fp   = NULL;
    char     curl_error[CURL_ERROR_SIZE];
    int      ret  = -1;

    curl_error[0] = '\0';

    fp = fopen(dest_path, "wb");
    if (!fp) {
        fprintf(stderr, "packmule: cannot open %s for writing\n", dest_path);
        return -1;
    }

    curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "packmule: curl_easy_init() failed\n");
        fclose(fp);
        return -1;
    }

    ProgressCtx ctx;
    ctx.is_tty = show_progress && isatty(fileno(stdout));
    clock_gettime(CLOCK_MONOTONIC, &ctx.start);
    ctx.last_update = ctx.start;

    configure_common(curl, curl_error);
    curl_easy_setopt(curl, CURLOPT_URL,              url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,    write_file_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,        fp);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS,       0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferinfo_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA,     &ctx);

    res = curl_easy_perform(curl);

    /* End the progress line before printing any error messages. */
    if (ctx.is_tty)
        putchar('\n');

    if (res != CURLE_OK) {
        fprintf(stderr, "packmule: download error for %s: %s\n",
                url,
                curl_error[0] ? curl_error : curl_easy_strerror(res));
    } else {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code == 200)
            ret = 0;
        else
            fprintf(stderr, "packmule: HTTP %ld downloading %s\n", http_code, url);
    }

    curl_easy_cleanup(curl);
    fclose(fp);
    return ret;
}
