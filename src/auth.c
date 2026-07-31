#include "auth.h"
#include "utils.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* ── Credential store ─────────────────────────────────────────────────────── */

typedef struct {
    AuthScheme   scheme;
    char        *userpwd;       /* "user:secret", AUTH_BASIC only */
    char        *bearer_header; /* header line, AUTH_BEARER / AUTH_HEADER */
    char       **hosts;         /* NULL-terminated, lowercased host[:port] */
    char        *scope_desc;    /* the same hosts, comma-joined, for messages */
    char        *scheme_label;  /* header field name, AUTH_HEADER only */

    /* Transport trust.  Not host-scoped: these describe how this machine
     * establishes a TLS connection at all, not who it proves itself to. */
    char        *ca_bundle;
    int          ca_is_dir;
    char        *client_cert;
    char        *client_key;
    char        *client_key_password;
} AuthStore;

static AuthStore g_auth;

/* ── URL authority parsing ────────────────────────────────────────────────── */

/*
 * scheme_default_port — the port that is redundant for `url`'s scheme, or 0
 * when the scheme is not one we handle.
 */
static int scheme_default_port(const char *url, size_t *scheme_len)
{
    if (strncasecmp(url, "https://", 8) == 0) { *scheme_len = 8; return 443; }
    if (strncasecmp(url, "http://",  7) == 0) { *scheme_len = 7; return 80;  }
    return 0;
}

/* The authority runs from after "scheme://" to the first '/', '?' or '#'. */
static const char *authority_end(const char *authority)
{
    const char *p = authority;
    while (*p && *p != '/' && *p != '?' && *p != '#')
        p++;
    return p;
}

int auth_url_has_userinfo(const char *url)
{
    size_t scheme_len = 0;
    if (!url || scheme_default_port(url, &scheme_len) == 0)
        return 0;

    const char *authority = url + scheme_len;
    const char *end       = authority_end(authority);

    for (const char *p = authority; p < end; p++)
        if (*p == '@')
            return 1;
    return 0;
}

char *auth_url_host(const char *url)
{
    size_t scheme_len   = 0;
    int    default_port = url ? scheme_default_port(url, &scheme_len) : 0;
    if (default_port == 0)
        return NULL;

    const char *authority = url + scheme_len;
    const char *end       = authority_end(authority);

    /* Userinfo may itself contain ':' and (percent-encoded) other delimiters,
     * so the host starts after the LAST '@' in the authority, not the first. */
    const char *host = authority;
    for (const char *p = authority; p < end; p++)
        if (*p == '@')
            host = p + 1;

    if (host >= end)
        return NULL;

    /*
     * Find the port separator.  A ':' inside an IPv6 literal is part of the
     * address, so only a ':' after the closing bracket counts.
     */
    const char *port_sep = NULL;
    if (*host == '[') {
        const char *close = memchr(host, ']', (size_t)(end - host));
        if (!close)
            return NULL;                     /* unterminated IPv6 literal */
        if (close + 1 < end && close[1] == ':')
            port_sep = close + 1;
    } else {
        for (const char *p = host; p < end; p++)
            if (*p == ':') {
                port_sep = p;
                break;
            }
    }

    const char *host_end = port_sep ? port_sep : end;
    if (host_end == host)
        return NULL;                         /* empty host */

    /* Drop the port when it is the scheme's default, so that
     * "https://art.corp" and "https://art.corp:443" are one host. */
    int explicit_port = 0;
    if (port_sep) {
        const char *digits = port_sep + 1;
        if (digits == end)
            return NULL;                     /* "host:" with no port */
        for (const char *p = digits; p < end; p++)
            if (!isdigit((unsigned char)*p))
                return NULL;
        explicit_port = atoi(digits);
        if (explicit_port == default_port)
            port_sep = NULL;
    }

    size_t len = port_sep ? (size_t)(end - host) : (size_t)(host_end - host);
    char  *out = pm_strndup(host, len);
    for (char *p = out; *p; p++)
        *p = (char)tolower((unsigned char)*p);
    return out;
}

/* ── Host scope ───────────────────────────────────────────────────────────── */

/* Append `host` (already heap-allocated and lowercased) to the scope list. */
static void scope_add(char ***hosts, size_t *n, char *host)
{
    for (size_t i = 0; i < *n; i++) {
        if (strcmp((*hosts)[i], host) == 0) {
            pm_free(host);
            return;
        }
    }
    *hosts        = pm_realloc(*hosts, (*n + 2) * sizeof(char *));
    (*hosts)[*n]  = host;
    (*hosts)[++*n] = NULL;
}

/*
 * scope_add_extra — parse PACKMULE_AUTH_HOSTS, a comma-separated list of
 * additional authorities.
 *
 * Some Artifactory and Nexus deployments hand out download URLs on a separate
 * CDN or storage hostname, which the --repo-url host alone would not cover.
 * Entries may be bare ("art-cdn.corp", "art-cdn.corp:8443") or full URLs.
 */
static int scope_add_extra(const char *spec, char ***hosts, size_t *n)
{
    char *work = pm_strdup(spec);
    char *save = work;
    int   rc   = 0;

    for (char *tok = strtok(work, ","); tok; tok = strtok(NULL, ",")) {
        char *trimmed = pm_strtrim(tok);
        if (!*trimmed)
            continue;

        /* Accept a full URL as a convenience; otherwise it is a bare host and
         * we borrow the parser by giving it a scheme to chew on. */
        char *host;
        if (strncasecmp(trimmed, "http://",  7) == 0 ||
            strncasecmp(trimmed, "https://", 8) == 0) {
            host = auth_url_host(trimmed);
        } else {
            char *as_url = pm_asprintf("https://%s", trimmed);
            host = auth_url_host(as_url);
            pm_free(as_url);
        }

        if (!host) {
            fprintf(stderr,
                    "packmule: PACKMULE_AUTH_HOSTS entry '%s' is not a valid "
                    "host\n", trimmed);
            rc = -1;
            break;
        }
        scope_add(hosts, n, host);
    }

    pm_free(save);
    return rc;
}

/* ── Initialisation ───────────────────────────────────────────────────────── */

/* An environment variable that is set but empty counts as unset. */
static const char *env_or_null(const char *name)
{
    const char *v = getenv(name);
    return (v && *v) ? v : NULL;
}

/*
 * validate_header_line — check a caller-supplied "Name: value" header.
 *
 * The escape hatch that makes this tool work against a registry nobody here
 * has seen is also the one place where the environment dictates raw protocol
 * bytes, so it is checked rather than trusted: a CR or LF would end the header
 * and let anything after it be injected as further headers, or as a second
 * request entirely.
 *
 * Returns 0 if usable, -1 otherwise (reason printed to stderr).
 */
static int validate_header_line(const char *line)
{
    for (const char *p = line; *p; p++) {
        if (*p == '\r' || *p == '\n') {
            fprintf(stderr,
                    "packmule: PACKMULE_AUTH_HEADER must be a single line; it "
                    "contains a newline.\n");
            return -1;
        }
        /* A header field-name/value is ASCII; a stray control byte is a
         * mistake (a stray shell escape, a copied terminal sequence). */
        if ((unsigned char)*p < 0x20 && *p != '\t') {
            fprintf(stderr,
                    "packmule: PACKMULE_AUTH_HEADER contains a control "
                    "character.\n");
            return -1;
        }
    }

    const char *colon = strchr(line, ':');
    if (!colon || colon == line) {
        fprintf(stderr,
                "packmule: PACKMULE_AUTH_HEADER must look like "
                "\"Name: value\" (no colon found).\n"
                "          Example: PACKMULE_AUTH_HEADER='X-JFrog-Art-Api: "
                "<key>'\n");
        return -1;
    }

    /* A name with a space in it is almost always a missing colon, e.g.
     * "Bearer abc" — which would be silently sent as a bogus header. */
    for (const char *p = line; p < colon; p++) {
        if (*p == ' ' || *p == '\t') {
            fprintf(stderr,
                    "packmule: PACKMULE_AUTH_HEADER field name '%.*s' contains "
                    "whitespace.\n"
                    "          Did you mean PACKMULE_TOKEN for a bearer "
                    "token?\n", (int)(colon - line), line);
            return -1;
        }
    }

    const char *value = colon + 1;
    while (*value == ' ' || *value == '\t')
        value++;
    if (!*value) {
        fprintf(stderr, "packmule: PACKMULE_AUTH_HEADER has an empty value.\n");
        return -1;
    }

    return 0;
}

/*
 * init_transport — resolve the TLS trust settings.
 *
 * Checked here, at startup, rather than left to fail on the first request: a
 * mistyped CA path otherwise surfaces as a TLS handshake error, which reads
 * like a broken server rather than a broken setting.
 */
static int init_transport(void)
{
    /* CURL_CA_BUNDLE and SSL_CERT_FILE are what curl and OpenSSL-based tools
     * already read, so a machine configured for a corporate CA works with no
     * packmule-specific setup. */
    const char *ca = env_or_null("PACKMULE_CA_BUNDLE");
    if (!ca) ca = env_or_null("CURL_CA_BUNDLE");
    if (!ca) ca = env_or_null("SSL_CERT_FILE");

    if (ca) {
        struct stat st;
        if (stat(ca, &st) != 0) {
            fprintf(stderr,
                    "packmule: CA bundle '%s' does not exist.\n", ca);
            return -1;
        }
        g_auth.ca_bundle = pm_strdup(ca);
        g_auth.ca_is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    }

    const char *cert = env_or_null("PACKMULE_CLIENT_CERT");
    const char *key  = env_or_null("PACKMULE_CLIENT_KEY");

    if (!cert && key) {
        fprintf(stderr,
                "packmule: PACKMULE_CLIENT_KEY is set but "
                "PACKMULE_CLIENT_CERT is not.\n");
        return -1;
    }

    if (cert) {
        struct stat st;
        if (stat(cert, &st) != 0) {
            fprintf(stderr,
                    "packmule: client certificate '%s' does not exist.\n", cert);
            return -1;
        }
        if (key && stat(key, &st) != 0) {
            fprintf(stderr,
                    "packmule: client key '%s' does not exist.\n", key);
            return -1;
        }
        g_auth.client_cert = pm_strdup(cert);
        /* A key is optional: a PEM or PKCS#12 file may carry both. */
        if (key)
            g_auth.client_key = pm_strdup(key);

        const char *pw = env_or_null("PACKMULE_CLIENT_KEY_PASSWORD");
        if (pw)
            g_auth.client_key_password = pm_strdup(pw);
    }

    return 0;
}

int auth_init(const char *repo_url)
{
    const char *user     = env_or_null("PACKMULE_USERNAME");
    const char *password = env_or_null("PACKMULE_PASSWORD");
    const char *token    = env_or_null("PACKMULE_TOKEN");
    const char *raw_hdr  = env_or_null("PACKMULE_AUTH_HEADER");
    const char *extra    = env_or_null("PACKMULE_AUTH_HOSTS");
    const char *insecure = env_or_null("PACKMULE_AUTH_INSECURE");

    /*
     * Release anything a previous call left behind rather than zeroing over
     * it.  main.c calls this once, but the failure paths below return without
     * unwinding, so a second call after a rejected configuration would
     * otherwise orphan whatever the first had already allocated.
     */
    auth_cleanup();

    /* TLS trust is independent of whether any credential is configured: a
     * private CA is needed to reach an anonymous internal mirror too. */
    if (init_transport() != 0)
        return -1;

    /*
     * Reject userinfo in --repo-url regardless of whether credentials are set.
     * It would work — libcurl honours it — but the URL is echoed in progress
     * output and error messages, so the secret would end up in logs and CI
     * transcripts.  Failing loudly is better than leaking quietly.
     */
    if (repo_url && auth_url_has_userinfo(repo_url)) {
        fprintf(stderr,
                "packmule: --repo-url must not embed credentials "
                "(user:password@host).\n"
                "          The URL appears in progress and error output, so a "
                "secret there leaks into logs.\n"
                "          Set PACKMULE_USERNAME and PACKMULE_PASSWORD (or "
                "PACKMULE_TOKEN) instead.\n");
        return -1;
    }

    if (!user && !password && !token && !raw_hdr)
        return 0;                       /* no credentials; stay inert */

    /*
     * A raw header and a built-in scheme together are ambiguous — both would
     * set Authorization, and which one wins is not something the operator
     * should have to guess.
     */
    if (raw_hdr && (user || password || token)) {
        fprintf(stderr,
                "packmule: PACKMULE_AUTH_HEADER cannot be combined with "
                "PACKMULE_USERNAME/PASSWORD/TOKEN.\n"
                "          Use the raw header on its own, or drop it and use "
                "the built-in schemes.\n");
        return -1;
    }

    if (raw_hdr && validate_header_line(raw_hdr) != 0)
        return -1;

    /* Half a credential is a configuration mistake, not a reason to fall back
     * to anonymous: the run would fail later with an opaque 401. */
    if (password && !user) {
        fprintf(stderr,
                "packmule: PACKMULE_PASSWORD is set but PACKMULE_USERNAME is "
                "not.\n");
        return -1;
    }
    if (user && !password && !token) {
        fprintf(stderr,
                "packmule: PACKMULE_USERNAME is set but neither "
                "PACKMULE_PASSWORD nor PACKMULE_TOKEN is.\n");
        return -1;
    }

    if (!repo_url) {
        fprintf(stderr,
                "packmule: credentials are set in the environment but no "
                "--repo-url was given.\n"
                "          There is no private index to send them to, and they "
                "will not be sent to the\n"
                "          public registry.  Pass -u <url>, or unset "
                "PACKMULE_USERNAME/PASSWORD/TOKEN.\n");
        return -1;
    }

    char *host = auth_url_host(repo_url);
    if (!host) {
        fprintf(stderr,
                "packmule: cannot determine a host from --repo-url '%s'; "
                "credentials need an\n"
                "          absolute http(s) URL to be scoped to.\n", repo_url);
        return -1;
    }

    /*
     * Plain http hands the credential to anyone on the path.  Artifactory
     * behind a corporate TLS-terminating proxy is a real deployment, so allow
     * an explicit opt-out — but never silently.
     */
    if (strncasecmp(repo_url, "http://", 7) == 0) {
        if (!insecure || strcmp(insecure, "1") != 0) {
            fprintf(stderr,
                    "packmule: refusing to send credentials to an http:// URL "
                    "(%s).\n"
                    "          Anyone on the network path can read them.  Use "
                    "https, or set\n"
                    "          PACKMULE_AUTH_INSECURE=1 if this link is "
                    "genuinely trusted.\n", host);
            pm_free(host);
            return -1;
        }
        fprintf(stderr,
                "packmule: warning: sending credentials in cleartext to %s "
                "(PACKMULE_AUTH_INSECURE=1)\n", host);
    }

    char  **hosts = NULL;
    size_t  nhosts = 0;
    scope_add(&hosts, &nhosts, host);

    if (extra && scope_add_extra(extra, &hosts, &nhosts) != 0) {
        for (size_t i = 0; i < nhosts; i++)
            pm_free(hosts[i]);
        pm_free(hosts);
        return -1;
    }

    /*
     * Scheme selection mirrors how Artifactory is actually used.  A username
     * alongside the secret is Basic — that covers a password, an API key, and
     * an identity token equally.  A token on its own is a bearer access token,
     * which is the only form that carries no username.
     */
    if (raw_hdr) {
        g_auth.scheme        = AUTH_HEADER;
        g_auth.bearer_header = pm_strdup(raw_hdr);
        g_auth.scheme_label  = pm_strndup(raw_hdr,
                                          (size_t)(strchr(raw_hdr, ':') - raw_hdr));
    } else if (user) {
        g_auth.scheme  = AUTH_BASIC;
        g_auth.userpwd = pm_asprintf("%s:%s", user, password ? password : token);
    } else {
        g_auth.scheme        = AUTH_BEARER;
        g_auth.bearer_header = pm_asprintf("Authorization: Bearer %s", token);
    }

    g_auth.hosts = hosts;

    size_t desc_len = 1;
    for (size_t i = 0; i < nhosts; i++)
        desc_len += strlen(hosts[i]) + 2;
    g_auth.scope_desc = pm_malloc(desc_len);
    g_auth.scope_desc[0] = '\0';
    for (size_t i = 0; i < nhosts; i++) {
        if (i)
            strcat(g_auth.scope_desc, ", ");
        strcat(g_auth.scope_desc, hosts[i]);
    }

    return 0;
}

void auth_cleanup(void)
{
    /* Overwrite the secrets before releasing the pages holding them.  This is
     * not a defence against a determined attacker with process access, but it
     * does keep the token out of a core dump written after this point. */
    if (g_auth.userpwd) {
        memset(g_auth.userpwd, 0, strlen(g_auth.userpwd));
        pm_free(g_auth.userpwd);
    }
    if (g_auth.bearer_header) {
        memset(g_auth.bearer_header, 0, strlen(g_auth.bearer_header));
        pm_free(g_auth.bearer_header);
    }
    if (g_auth.client_key_password) {
        memset(g_auth.client_key_password, 0,
               strlen(g_auth.client_key_password));
        pm_free(g_auth.client_key_password);
    }
    if (g_auth.hosts) {
        for (char **h = g_auth.hosts; *h; h++)
            pm_free(*h);
        pm_free(g_auth.hosts);
    }
    pm_free(g_auth.scope_desc);
    pm_free(g_auth.scheme_label);
    pm_free(g_auth.ca_bundle);
    pm_free(g_auth.client_cert);
    pm_free(g_auth.client_key);
    memset(&g_auth, 0, sizeof(g_auth));
}

/* ── Queries ──────────────────────────────────────────────────────────────── */

int auth_configured(void)
{
    return g_auth.scheme != AUTH_NONE;
}

AuthScheme auth_scheme_for_url(const char *url)
{
    if (g_auth.scheme == AUTH_NONE || !g_auth.hosts)
        return AUTH_NONE;

    char *host = auth_url_host(url);
    if (!host)
        return AUTH_NONE;

    AuthScheme scheme = AUTH_NONE;
    for (char **h = g_auth.hosts; *h; h++) {
        if (strcmp(*h, host) == 0) {
            scheme = g_auth.scheme;
            break;
        }
    }

    pm_free(host);
    return scheme;
}

const char *auth_userpwd(void)
{
    return g_auth.userpwd;
}

const char *auth_bearer_header(void)
{
    return g_auth.bearer_header;
}

const char *auth_scope_description(void)
{
    return g_auth.scope_desc;
}

const char *auth_scheme_name(void)
{
    switch (g_auth.scheme) {
    case AUTH_BASIC:  return "basic";
    case AUTH_BEARER: return "bearer";
    case AUTH_HEADER:
        /* Name the header rather than saying "custom": when a run fails with a
         * 401 the first useful question is which scheme was actually tried. */
        return g_auth.scheme_label;
    case AUTH_NONE:
    default:          return NULL;
    }
}

const char *auth_ca_bundle(void)          { return g_auth.ca_bundle; }
int         auth_ca_bundle_is_dir(void)   { return g_auth.ca_is_dir; }
const char *auth_client_cert(void)        { return g_auth.client_cert; }
const char *auth_client_key(void)         { return g_auth.client_key; }
const char *auth_client_key_password(void){ return g_auth.client_key_password; }
