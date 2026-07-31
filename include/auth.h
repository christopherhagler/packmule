/*
 * auth.h — credentials and TLS trust for private package indexes.
 *
 * The design goal is to work against a registry this code has never seen.
 * There are many products (JFrog Artifactory, Sonatype Nexus, devpi, GitLab,
 * Azure Artifacts, Cloudsmith, a plain nginx directory) and more than one
 * authentication convention each, so rather than encode a table of vendors,
 * this module offers the primitives every one of them is built from — and a
 * raw-header escape hatch for the ones it does not anticipate.
 *
 * Credentials are read from the environment, never from the command line: an
 * argv token is visible in `ps` output to every user on the machine and lands
 * in shell history, and the whole point of a token is that it stays secret.
 *
 *   PACKMULE_USERNAME + PACKMULE_PASSWORD   HTTP Basic
 *   PACKMULE_USERNAME + PACKMULE_TOKEN      HTTP Basic ("user + API key")
 *   PACKMULE_TOKEN                          Bearer token
 *   PACKMULE_AUTH_HEADER                    a literal header line, for any
 *                                           scheme not covered above (e.g.
 *                                           "X-JFrog-Art-Api: …",
 *                                           "PRIVATE-TOKEN: …")
 *   PACKMULE_AUTH_HOSTS                     extra hosts, comma-separated
 *   PACKMULE_AUTH_INSECURE=1                permit credentials over plain http
 *
 * Transport trust, for registries behind a corporate PKI:
 *
 *   PACKMULE_CA_BUNDLE                      CA file or directory to verify the
 *                                           server against (also honours
 *                                           CURL_CA_BUNDLE and SSL_CERT_FILE)
 *   PACKMULE_CLIENT_CERT / _KEY / _KEY_PASSWORD
 *                                           client certificate for mutual TLS
 *
 * There is deliberately no option to disable certificate verification.  An
 * internal CA is a trust decision packmule can act on; turning verification
 * off is not.
 *
 * Credentials are scoped to the host of --repo-url and to nothing else.  That
 * restriction is the reason this module exists rather than a curl_easy_setopt
 * call at each site: an index decides the download URLs, so a registry that
 * returns a file URL on another host — or an attacker who can make it do so —
 * would otherwise be handed the token.  A request whose host is not in scope
 * is sent unauthenticated.  TLS settings, by contrast, are not scoped: they
 * describe how this machine establishes any connection at all.
 *
 * This module is deliberately free of any libcurl dependency so the scoping
 * rules can be unit-tested; network.c translates the decisions into handle
 * options.
 */

#ifndef PACKMULE_AUTH_H
#define PACKMULE_AUTH_H

typedef enum {
    AUTH_NONE = 0,   /* send nothing */
    AUTH_BASIC,      /* Authorization: Basic (curl builds it from userpwd) */
    AUTH_BEARER,     /* Authorization: Bearer <token> */
    AUTH_HEADER      /* a caller-supplied literal header line */
} AuthScheme;

/*
 * auth_init — read the environment and fix the credential scope to the host of
 * `repo_url` (which may be NULL when the user did not pass --repo-url).
 *
 * Must be called once, before any network request.  Calling it with no
 * credentials in the environment is fine and leaves the module inert.
 *
 * Returns 0 on success, -1 on a misconfiguration that must stop the run:
 * an incomplete credential pair, credentials with no --repo-url to scope them
 * to, credentials aimed at a plain-http URL, or a --repo-url that embeds
 * userinfo.  The reason is printed to stderr.
 */
int auth_init(const char *repo_url);

/* Release the credential store.  Safe to call when auth_init() was not. */
void auth_cleanup(void);

/* Nonzero when usable credentials were configured. */
int auth_configured(void);

/*
 * auth_scheme_for_url — the scheme to use for `url`, or AUTH_NONE when the
 * URL's host is out of scope (or no credentials are configured).
 */
AuthScheme auth_scheme_for_url(const char *url);

/*
 * auth_userpwd — the "user:secret" string for AUTH_BASIC, or NULL.
 * Owned by this module; valid until auth_cleanup().
 */
const char *auth_userpwd(void);

/*
 * auth_bearer_header — the header line to send for AUTH_BEARER or
 * AUTH_HEADER, or NULL.  Owned by this module.
 */
const char *auth_bearer_header(void);

/* ── Transport trust (not host-scoped; see the note at the top) ──────────── */

/*
 * auth_ca_bundle — path to a CA certificate file or directory to verify
 * servers against, or NULL to use libcurl's default trust store.
 *
 * Reads PACKMULE_CA_BUNDLE, then CURL_CA_BUNDLE, then SSL_CERT_FILE; the
 * latter two are the conventions curl and OpenSSL-based tooling already use,
 * so a machine already configured for a corporate CA needs no packmule-
 * specific setup.  Owned by this module.
 */
const char *auth_ca_bundle(void);

/*
 * auth_ca_bundle_is_dir — nonzero when auth_ca_bundle() names a directory
 * (an OpenSSL c_rehash-style CA path) rather than a single bundle file.
 */
int auth_ca_bundle_is_dir(void);

/*
 * Client certificate for mutual TLS, or NULL.  Some enterprises require a
 * client cert instead of, or in addition to, a token.  Owned by this module.
 */
const char *auth_client_cert(void);
const char *auth_client_key(void);
const char *auth_client_key_password(void);

/*
 * auth_scope_description — the in-scope hosts, comma-separated, for the
 * startup banner (e.g. "art.corp, art-cdn.corp").  NULL when unconfigured.
 * Owned by this module.
 */
const char *auth_scope_description(void);

/*
 * auth_scheme_name — "basic", "bearer", or the field name of a custom header
 * (e.g. "X-JFrog-Art-Api"), for the startup banner.  NULL when unconfigured.
 * Owned by this module.
 */
const char *auth_scheme_name(void);

/* ── Exposed for unit tests ──────────────────────────────────────────────── */

/*
 * auth_url_host — extract the host[:port] authority from an absolute http(s)
 * URL, lowercased, with a redundant default port (:80 on http, :443 on https)
 * removed so that the two spellings of one host compare equal.  Userinfo is
 * skipped.  IPv6 literals keep their brackets.
 *
 * Returns a heap string the caller frees with pm_free(), or NULL when `url`
 * is not an absolute http/https URL.
 */
char *auth_url_host(const char *url);

/*
 * auth_url_has_userinfo — nonzero when `url`'s authority contains a "user@"
 * or "user:pass@" prefix.
 */
int auth_url_has_userinfo(const char *url);

#endif /* PACKMULE_AUTH_H */
