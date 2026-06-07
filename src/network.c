#include "network.h"
#include "utils.h"

#include <curl/curl.h>
#include <stdio.h>
#include <string.h>

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

int download_file(const char *url, const char *dest_path)
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

    configure_common(curl, curl_error);
    curl_easy_setopt(curl, CURLOPT_URL,           url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     fp);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS,    1L);

    res = curl_easy_perform(curl);

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
