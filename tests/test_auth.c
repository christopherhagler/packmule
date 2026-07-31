/*
 * test_auth.c — credential scoping for private indexes.
 *
 * The rules that matter here are the ones that decide whether a secret leaves
 * the machine, so most of these tests are about the cases where it must not.
 * auth_init() prints diagnostics to stderr by design; the failing cases below
 * are expected to be noisy.
 */
#include "auth.h"
#include "utils.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Put the environment in a known state before each case. */
static void clear_env(void)
{
    unsetenv("PACKMULE_USERNAME");
    unsetenv("PACKMULE_PASSWORD");
    unsetenv("PACKMULE_TOKEN");
    unsetenv("PACKMULE_AUTH_HEADER");
    unsetenv("PACKMULE_AUTH_HOSTS");
    unsetenv("PACKMULE_AUTH_INSECURE");
    unsetenv("PACKMULE_CA_BUNDLE");
    unsetenv("CURL_CA_BUNDLE");
    unsetenv("SSL_CERT_FILE");
    unsetenv("PACKMULE_CLIENT_CERT");
    unsetenv("PACKMULE_CLIENT_KEY");
    unsetenv("PACKMULE_CLIENT_KEY_PASSWORD");
    auth_cleanup();
}

static void host_is(const char *url, const char *want)
{
    char *got = auth_url_host(url);
    if (!want) {
        assert(got == NULL);
        return;
    }
    assert(got != NULL);
    assert(strcmp(got, want) == 0);
    pm_free(got);
}

static void test_url_host(void)
{
    host_is("https://art.corp/artifactory/api/pypi/pypi/", "art.corp");
    host_is("http://art.corp/x",                           "art.corp");
    host_is("https://ART.Corp/x",                          "art.corp");

    /* A default port is redundant: both spellings must compare equal, or a
     * download URL that names it explicitly would silently lose its token. */
    host_is("https://art.corp:443/x", "art.corp");
    host_is("http://art.corp:80/x",   "art.corp");
    host_is("https://art.corp:8443/x", "art.corp:8443");
    host_is("http://art.corp:443/x",   "art.corp:443");

    /* Userinfo is not part of the host, and the host is what follows the LAST
     * '@' — an '@' inside the password must not shift the boundary. */
    host_is("https://user@art.corp/x",            "art.corp");
    host_is("https://user:pw@art.corp/x",         "art.corp");
    host_is("https://user:p@ss@art.corp/x",       "art.corp");

    host_is("https://[2001:db8::1]/x",      "[2001:db8::1]");
    host_is("https://[2001:db8::1]:8443/x", "[2001:db8::1]:8443");
    host_is("https://[2001:db8::1]:443/x",  "[2001:db8::1]");

    /* No path at all is still a valid authority. */
    host_is("https://art.corp",  "art.corp");
    host_is("https://art.corp?q", "art.corp");

    host_is("ftp://art.corp/x", NULL);
    host_is("art.corp/x",       NULL);
    host_is("https://",         NULL);
    host_is("https://:443/x",   NULL);
    host_is("https://art.corp:/x",   NULL);
    host_is("https://art.corp:xy/x", NULL);
}

static void test_userinfo_detection(void)
{
    assert(auth_url_has_userinfo("https://u:p@art.corp/x"));
    assert(auth_url_has_userinfo("https://u@art.corp/x"));
    assert(!auth_url_has_userinfo("https://art.corp/x"));
    /* An '@' in the path is not userinfo. */
    assert(!auth_url_has_userinfo("https://art.corp/a@b"));
    assert(!auth_url_has_userinfo("https://art.corp/?x=a@b"));
}

static void test_basic(void)
{
    clear_env();
    setenv("PACKMULE_USERNAME", "ci-bot", 1);
    setenv("PACKMULE_PASSWORD", "s3cret", 1);

    assert(auth_init("https://art.corp/artifactory/api/pypi/pypi/") == 0);
    assert(auth_configured());
    assert(strcmp(auth_userpwd(), "ci-bot:s3cret") == 0);
    assert(auth_bearer_header() == NULL);

    assert(auth_scheme_for_url("https://art.corp/anything") == AUTH_BASIC);
    /* Port-equivalent spelling of the same host. */
    assert(auth_scheme_for_url("https://art.corp:443/anything") == AUTH_BASIC);

    /* The whole point: a file URL on another host gets nothing. */
    assert(auth_scheme_for_url("https://files.pythonhosted.org/x.whl")
           == AUTH_NONE);
    /* Not even a subdomain of the in-scope host. */
    assert(auth_scheme_for_url("https://evil.art.corp/x") == AUTH_NONE);
    /* Nor the same name on a different port. */
    assert(auth_scheme_for_url("https://art.corp:8443/x") == AUTH_NONE);

    clear_env();
}

static void test_username_plus_token_is_basic(void)
{
    clear_env();
    setenv("PACKMULE_USERNAME", "ci-bot", 1);
    setenv("PACKMULE_TOKEN",    "AKCp8k", 1);

    /* Artifactory's "username + API key / identity token" is Basic, not
     * Bearer; getting this wrong yields a 401 that reads like a bad token. */
    assert(auth_init("https://art.corp/x") == 0);
    assert(auth_scheme_for_url("https://art.corp/x") == AUTH_BASIC);
    assert(strcmp(auth_userpwd(), "ci-bot:AKCp8k") == 0);

    clear_env();
}

static void test_bearer(void)
{
    clear_env();
    setenv("PACKMULE_TOKEN", "eyJ2ZXIiOiIy", 1);

    assert(auth_init("https://art.corp/artifactory/api/npm/npm/") == 0);
    assert(auth_scheme_for_url("https://art.corp/x") == AUTH_BEARER);
    assert(auth_userpwd() == NULL);
    assert(strcmp(auth_bearer_header(),
                  "Authorization: Bearer eyJ2ZXIiOiIy") == 0);

    clear_env();
}

static void test_extra_hosts(void)
{
    clear_env();
    setenv("PACKMULE_TOKEN",      "tok", 1);
    setenv("PACKMULE_AUTH_HOSTS", "art-cdn.corp, https://art-eu.corp:8443/ignored", 1);

    assert(auth_init("https://art.corp/x") == 0);
    assert(auth_scheme_for_url("https://art.corp/x")           == AUTH_BEARER);
    assert(auth_scheme_for_url("https://art-cdn.corp/x")       == AUTH_BEARER);
    assert(auth_scheme_for_url("https://art-eu.corp:8443/x")   == AUTH_BEARER);
    assert(auth_scheme_for_url("https://art-eu.corp/x")        == AUTH_NONE);
    assert(auth_scheme_for_url("https://elsewhere.corp/x")     == AUTH_NONE);

    clear_env();
}

static void test_misconfiguration_is_fatal(void)
{
    /* Credentials with nowhere to send them: silently falling back to an
     * anonymous fetch of the public index would produce a bundle from the
     * wrong registry. */
    clear_env();
    setenv("PACKMULE_TOKEN", "tok", 1);
    assert(auth_init(NULL) == -1);

    /* Half a credential pair. */
    clear_env();
    setenv("PACKMULE_PASSWORD", "pw", 1);
    assert(auth_init("https://art.corp/x") == -1);

    clear_env();
    setenv("PACKMULE_USERNAME", "u", 1);
    assert(auth_init("https://art.corp/x") == -1);

    /* Cleartext transport, without and with the explicit opt-out. */
    clear_env();
    setenv("PACKMULE_TOKEN", "tok", 1);
    assert(auth_init("http://art.corp/x") == -1);
    setenv("PACKMULE_AUTH_INSECURE", "1", 1);
    assert(auth_init("http://art.corp/x") == 0);
    assert(auth_scheme_for_url("http://art.corp/x") == AUTH_BEARER);

    /* A secret in the URL would be echoed into logs. */
    clear_env();
    assert(auth_init("https://u:p@art.corp/x") == -1);
    /* …and that check does not depend on credentials being configured. */
    setenv("PACKMULE_TOKEN", "tok", 1);
    assert(auth_init("https://u:p@art.corp/x") == -1);

    /* A repo URL with no usable host. */
    clear_env();
    setenv("PACKMULE_TOKEN", "tok", 1);
    assert(auth_init("file:///srv/mirror") == -1);

    clear_env();
}

static void test_inert_without_credentials(void)
{
    clear_env();
    assert(auth_init("https://art.corp/x") == 0);
    assert(!auth_configured());
    assert(auth_scheme_for_url("https://art.corp/x") == AUTH_NONE);

    assert(auth_init(NULL) == 0);
    assert(!auth_configured());

    /* An empty variable is not a credential. */
    setenv("PACKMULE_TOKEN", "", 1);
    assert(auth_init("https://art.corp/x") == 0);
    assert(!auth_configured());

    clear_env();
}

/* ── Custom header (the escape hatch for unknown registries) ─────────────── */

static void test_custom_header(void)
{
    clear_env();
    setenv("PACKMULE_AUTH_HEADER", "X-JFrog-Art-Api: AKCp8kabc", 1);

    assert(auth_init("https://art.corp/x") == 0);
    assert(auth_configured());
    assert(auth_scheme_for_url("https://art.corp/x") == AUTH_HEADER);
    assert(strcmp(auth_bearer_header(), "X-JFrog-Art-Api: AKCp8kabc") == 0);
    assert(auth_userpwd() == NULL);
    /* The banner names the header, so a 401 says which scheme was tried. */
    assert(strcmp(auth_scheme_name(), "X-JFrog-Art-Api") == 0);

    /* Still host-scoped — this is the scheme libcurl would NOT have stripped
     * across a redirect, so the scoping is the only thing protecting it. */
    assert(auth_scheme_for_url("https://cdn.example.com/x") == AUTH_NONE);

    clear_env();
}

static void test_custom_header_validation(void)
{
    /* Header injection: a newline would let the value smuggle further headers
     * or an entire second request. */
    clear_env();
    setenv("PACKMULE_AUTH_HEADER", "X-Api-Key: abc\r\nX-Evil: 1", 1);
    assert(auth_init("https://art.corp/x") == -1);

    clear_env();
    setenv("PACKMULE_AUTH_HEADER", "X-Api-Key: abc\nX-Evil: 1", 1);
    assert(auth_init("https://art.corp/x") == -1);

    /* Missing colon, and the common mistake of pasting a bearer value. */
    clear_env();
    setenv("PACKMULE_AUTH_HEADER", "just-a-token", 1);
    assert(auth_init("https://art.corp/x") == -1);

    clear_env();
    setenv("PACKMULE_AUTH_HEADER", "Bearer abc123", 1);
    assert(auth_init("https://art.corp/x") == -1);

    clear_env();
    setenv("PACKMULE_AUTH_HEADER", "X-Api-Key:   ", 1);
    assert(auth_init("https://art.corp/x") == -1);

    /* Ambiguous: two schemes both wanting to set Authorization. */
    clear_env();
    setenv("PACKMULE_AUTH_HEADER", "X-Api-Key: abc", 1);
    setenv("PACKMULE_TOKEN",       "tok", 1);
    assert(auth_init("https://art.corp/x") == -1);

    clear_env();
}

/* ── Transport trust ─────────────────────────────────────────────────────── */

static void test_ca_bundle(void)
{
    clear_env();
    assert(auth_ca_bundle() == NULL);

    /* A mistyped path must fail at startup, not as a confusing TLS error on
     * the first request. */
    setenv("PACKMULE_CA_BUNDLE", "/nonexistent/corp-ca.pem", 1);
    assert(auth_init("https://art.corp/x") == -1);

    /* A real file is accepted, and is not treated as a directory. */
    clear_env();
    setenv("PACKMULE_CA_BUNDLE", "/etc/hosts", 1);
    assert(auth_init("https://art.corp/x") == 0);
    assert(strcmp(auth_ca_bundle(), "/etc/hosts") == 0);
    assert(!auth_ca_bundle_is_dir());

    /* A directory selects CURLOPT_CAPATH instead. */
    clear_env();
    setenv("PACKMULE_CA_BUNDLE", "/etc", 1);
    assert(auth_init("https://art.corp/x") == 0);
    assert(auth_ca_bundle_is_dir());

    /* The conventions other tools already use are honoured, in order. */
    clear_env();
    setenv("CURL_CA_BUNDLE", "/etc/hosts", 1);
    assert(auth_init("https://art.corp/x") == 0);
    assert(strcmp(auth_ca_bundle(), "/etc/hosts") == 0);

    clear_env();
    setenv("SSL_CERT_FILE", "/etc/hosts", 1);
    assert(auth_init("https://art.corp/x") == 0);
    assert(strcmp(auth_ca_bundle(), "/etc/hosts") == 0);

    /* TLS trust is independent of credentials: an anonymous internal mirror
     * behind a private CA still needs it. */
    assert(!auth_configured());

    clear_env();
}

static void test_client_cert(void)
{
    clear_env();
    setenv("PACKMULE_CLIENT_CERT", "/nonexistent/client.pem", 1);
    assert(auth_init("https://art.corp/x") == -1);

    clear_env();
    setenv("PACKMULE_CLIENT_KEY", "/etc/hosts", 1);   /* key without cert */
    assert(auth_init("https://art.corp/x") == -1);

    clear_env();
    setenv("PACKMULE_CLIENT_CERT", "/etc/hosts", 1);
    setenv("PACKMULE_CLIENT_KEY",  "/etc/hosts", 1);
    setenv("PACKMULE_CLIENT_KEY_PASSWORD", "pw", 1);
    assert(auth_init("https://art.corp/x") == 0);
    assert(strcmp(auth_client_cert(), "/etc/hosts") == 0);
    assert(strcmp(auth_client_key(),  "/etc/hosts") == 0);
    assert(strcmp(auth_client_key_password(), "pw") == 0);

    /* A combined PEM/PKCS#12 carries both, so the key is optional. */
    clear_env();
    setenv("PACKMULE_CLIENT_CERT", "/etc/hosts", 1);
    assert(auth_init("https://art.corp/x") == 0);
    assert(auth_client_key() == NULL);

    clear_env();
}

int main(void)
{
    test_url_host();
    test_userinfo_detection();
    test_basic();
    test_username_plus_token_is_basic();
    test_bearer();
    test_extra_hosts();
    test_misconfiguration_is_fatal();
    test_inert_without_credentials();
    test_custom_header();
    test_custom_header_validation();
    test_ca_bundle();
    test_client_cert();

    printf("test_auth: all tests passed\n");
    return 0;
}
