#include "sbom.h"
#include "hash.h"
#include "pep508.h"
#include "registry.h"
#include "utils.h"
#include "version.h"

#include <cjson/cJSON.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ── Small helpers ────────────────────────────────────────────────────────── */

int sbom_parse_format(const char *s)
{
    if (!s)                          return SBOM_NONE;
    if (strcmp(s, "cyclonedx") == 0) return SBOM_CYCLONEDX;
    if (strcmp(s, "spdx")      == 0) return SBOM_SPDX;
    if (strcmp(s, "both")      == 0) return SBOM_CYCLONEDX | SBOM_SPDX;
    return SBOM_NONE;
}

static int file_exists(const char *dir, const char *filename)
{
    char        path[4096];
    struct stat st;
    snprintf(path, sizeof(path), "%s/%s", dir, filename);
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/*
 * timestamp_utc — ISO-8601 UTC, honouring SOURCE_DATE_EPOCH.
 *
 * An SBOM is an artifact like any other, and a build that wants to be
 * reproducible needs its one free-running field pinned.
 */
static void timestamp_utc(char *buf, size_t bufsz)
{
    time_t      now = time(NULL);
    const char *sde = getenv("SOURCE_DATE_EPOCH");

    if (sde && *sde) {
        char     *end = NULL;
        long long v   = strtoll(sde, &end, 10);
        if (end && !*end && v >= 0)
            now = (time_t)v;
    }

    struct tm *utc = gmtime(&now);
    strftime(buf, bufsz, "%Y-%m-%dT%H:%M:%SZ", utc);
}

/*
 * document_uuid — a RFC 4122 v4 UUID for the document identity that both
 * formats require (CycloneDX serialNumber, SPDX documentNamespace).
 *
 * Falls back to time and pid when /dev/urandom is unavailable; a duplicate
 * identifier is a nuisance for SBOM consumers, not a security problem, so the
 * fallback does not need to be unpredictable.
 */
static void document_uuid(char *buf, size_t bufsz)
{
    unsigned char b[16];
    FILE         *f  = fopen("/dev/urandom", "rb");
    int           ok = f && fread(b, 1, sizeof(b), f) == sizeof(b);
    if (f)
        fclose(f);

    if (!ok) {
        unsigned long long seed =
            (unsigned long long)time(NULL) ^ ((unsigned long long)getpid() << 20);
        for (size_t i = 0; i < sizeof(b); i++) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            b[i] = (unsigned char)(seed >> 33);
        }
    }

    b[6] = (unsigned char)((b[6] & 0x0F) | 0x40);   /* version 4  */
    b[8] = (unsigned char)((b[8] & 0x3F) | 0x80);   /* variant 10 */

    snprintf(buf, bufsz,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
             "%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

/* ── Package URLs ─────────────────────────────────────────────────────────── */

/*
 * purl_escape — percent-encode a purl component.
 *
 * The unreserved set from RFC 3986 passes through; everything else is encoded.
 * '/' is encoded too, because a namespace separator that arrives inside a name
 * would silently change what the identifier refers to.
 */
static char *purl_escape(const char *s)
{
    size_t n   = strlen(s);
    char  *out = pm_malloc(n * 3 + 1);
    size_t k   = 0;

    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
            out[k++] = (char)c;
        } else {
            static const char HEX[] = "0123456789ABCDEF";
            out[k++] = '%';
            out[k++] = HEX[c >> 4];
            out[k++] = HEX[c & 0x0F];
        }
    }
    out[k] = '\0';
    return out;
}

/* PEP 503 normalisation, which the purl spec mandates for the pypi type. */
static char *purl_pypi_name(const char *name)
{
    size_t n   = strlen(name);
    char  *out = pm_malloc(n + 1);
    size_t k   = 0;

    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (c == '-' || c == '_' || c == '.') {
            if (k > 0 && out[k - 1] == '-')
                continue;
            out[k++] = '-';
        } else {
            out[k++] = (char)tolower((unsigned char)c);
        }
    }
    while (k > 0 && out[k - 1] == '-')
        k--;
    out[k] = '\0';
    return out;
}

char *sbom_purl(const char *registry_name, const Package *pkg)
{
    if (!pkg->name || !pkg->version || !*pkg->version)
        return NULL;

    if (strcmp(registry_name, "pypi") == 0) {
        char *norm = purl_pypi_name(pkg->name);
        char *n    = purl_escape(norm);
        char *v    = purl_escape(pkg->version);
        char *out  = pm_asprintf("pkg:pypi/%s@%s", n, v);
        pm_free(norm); pm_free(n); pm_free(v);
        return out;
    }

    if (strcmp(registry_name, "npm") == 0) {
        char *v = purl_escape(pkg->version);
        char *out;

        /* A scoped name is a purl namespace plus a name: "@babel/core"
         * becomes namespace "@babel" (encoded) and name "core". */
        const char *slash = (pkg->name[0] == '@') ? strchr(pkg->name, '/')
                                                  : NULL;
        if (slash) {
            char *scope = pm_strndup(pkg->name, (size_t)(slash - pkg->name));
            char *ns    = purl_escape(scope);
            char *nm    = purl_escape(slash + 1);
            out = pm_asprintf("pkg:npm/%s/%s@%s", ns, nm, v);
            pm_free(scope); pm_free(ns); pm_free(nm);
        } else {
            char *n = purl_escape(pkg->name);
            out = pm_asprintf("pkg:npm/%s@%s", n, v);
            pm_free(n);
        }
        pm_free(v);
        return out;
    }

    if (strcmp(registry_name, "rpm") == 0) {
        /*
         * packmule renders an RPM version as "epoch:ver-rel" when the package
         * carries an epoch.  purl keeps the epoch as a qualifier instead, so
         * split it back out.
         */
        const char *ver   = pkg->version;
        const char *colon = strchr(ver, ':');
        char       *epoch = NULL;
        if (colon) {
            epoch = pm_strndup(ver, (size_t)(colon - ver));
            ver   = colon + 1;
        }

        /* The architecture is the trailing field of the filename
         * ("name-ver-rel.<arch>.rpm"), which is where it survives resolution. */
        char *arch = NULL;
        if (pkg->filename) {
            const char *dot_rpm = strstr(pkg->filename, ".rpm");
            if (dot_rpm) {
                const char *p = dot_rpm;
                while (p > pkg->filename && p[-1] != '.')
                    p--;
                if (p < dot_rpm)
                    arch = pm_strndup(p, (size_t)(dot_rpm - p));
            }
        }

        char *n = purl_escape(pkg->name);
        char *v = purl_escape(ver);
        char *out;
        if (arch && epoch)
            out = pm_asprintf("pkg:rpm/%s@%s?arch=%s&epoch=%s", n, v, arch, epoch);
        else if (arch)
            out = pm_asprintf("pkg:rpm/%s@%s?arch=%s", n, v, arch);
        else if (epoch)
            out = pm_asprintf("pkg:rpm/%s@%s?epoch=%s", n, v, epoch);
        else
            out = pm_asprintf("pkg:rpm/%s@%s", n, v);

        pm_free(n); pm_free(v); pm_free(arch); pm_free(epoch);
        return out;
    }

    /* An unknown backend still gets a well-formed identifier, using its own
     * name as the purl type — better than omitting the field entirely. */
    char *t = purl_escape(registry_name);
    char *n = purl_escape(pkg->name);
    char *v = purl_escape(pkg->version);
    char *out = pm_asprintf("pkg:%s/%s@%s", t, n, v);
    pm_free(t); pm_free(n); pm_free(v);
    return out;
}

/* ── Dependency edges ─────────────────────────────────────────────────────── */

/*
 * dep_spec_name — the package name inside a raw dependency specifier.
 *
 * The specifier syntax belongs to the registry, so this is the one place the
 * SBOM has to know which backend produced the data.  rpm is absent on purpose:
 * its dep_specs are capability names ("libc.so.6", "/bin/sh"), which do not
 * correspond to package names and would produce edges that are wrong rather
 * than merely missing.
 *
 * Returns a heap string, or NULL when no edge can be derived.
 */
static char *dep_spec_name(const char *registry_name, const char *spec)
{
    if (strcmp(registry_name, "pypi") == 0) {
        char name[256];
        pep508_spec_name(spec, name, sizeof(name));
        return name[0] ? pm_strdup(name) : NULL;
    }

    if (strcmp(registry_name, "npm") == 0) {
        /* "name@range"; a scoped name's own '@' is only ever at index 0. */
        const char *at = strchr(spec + (spec[0] == '@' ? 1 : 0), '@');
        return at ? pm_strndup(spec, (size_t)(at - spec)) : pm_strdup(spec);
    }

    return NULL;
}

/* ── Component collection ─────────────────────────────────────────────────── */

typedef struct {
    const Package *pkg;
    char          *purl;      /* owned; may be NULL */
    char          *sha256;    /* owned; hex of the file on disk */
    char           spdx_id[64];
} Component;

static void components_free(Component *c, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        pm_free(c[i].purl);
        pm_free(c[i].sha256);
    }
    pm_free(c);
}

/*
 * collect — build the component list: every resolved package whose file is
 * actually present, hashed as it sits on disk.
 */
static Component *collect(const char *output_dir, const Registry *reg,
                          const PackageList *packages, size_t *out_n)
{
    Component *comps = pm_calloc(packages->count, sizeof(Component));
    size_t     n     = 0;

    for (size_t i = 0; i < packages->count; i++) {
        const Package *p = packages->items[i];
        if (!p->filename || !file_exists(output_dir, p->filename))
            continue;

        char path[4096], hex[DIGEST_HEX_MAX];
        snprintf(path, sizeof(path), "%s/%s", output_dir, p->filename);
        if (digest_file_hex(path, DIGEST_SHA256, hex, sizeof(hex)) != 0) {
            fprintf(stderr, "packmule: cannot hash %s for the SBOM\n", path);
            components_free(comps, n);
            return NULL;
        }

        comps[n].pkg    = p;
        comps[n].purl   = sbom_purl(reg->name, p);
        comps[n].sha256 = pm_strdup(hex);
        snprintf(comps[n].spdx_id, sizeof(comps[n].spdx_id),
                 "SPDXRef-Package-%zu", n);
        n++;
    }

    *out_n = n;
    return comps;
}

/* Index of the component named `name`, or (size_t)-1. */
static size_t find_component(const Component *c, size_t n, const char *name,
                             const Registry *reg)
{
    for (size_t i = 0; i < n; i++)
        if (reg->name_equal(c[i].pkg->name, name))
            return i;
    return (size_t)-1;
}

/* ── Serialisation ────────────────────────────────────────────────────────── */

static int write_json(const char *output_dir, const char *filename, cJSON *root)
{
    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    if (!text) {
        fprintf(stderr, "packmule: failed to serialise %s\n", filename);
        return -1;
    }

    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", output_dir, filename);

    FILE *fp = fopen(path, "w");
    if (!fp) {
        fprintf(stderr, "packmule: cannot write %s\n", path);
        free(text);
        return -1;
    }

    size_t len = strlen(text);
    int    ok  = fwrite(text, 1, len, fp) == len && fputc('\n', fp) != EOF;
    if (fclose(fp) != 0)
        ok = 0;
    free(text);   /* cJSON_Print allocates with malloc, not pm_malloc */

    if (!ok) {
        fprintf(stderr, "packmule: failed writing %s\n", path);
        return -1;
    }
    return 0;
}

/* Attach a licence to a CycloneDX component, when one was captured. */
static void cdx_add_license(cJSON *comp, const char *license)
{
    if (!license || !*license)
        return;

    cJSON *arr  = cJSON_AddArrayToObject(comp, "licenses");
    cJSON *item = cJSON_CreateObject();
    cJSON *lic  = cJSON_CreateObject();

    /*
     * "name" rather than "id": an SPDX id has to come from the SPDX list, and
     * what registries publish is free text ("BSD-3-Clause", but also "MIT
     * License", "Apache 2.0").  Declaring free text as an id would produce a
     * document that fails validation.
     */
    cJSON_AddStringToObject(lic, "name", license);
    cJSON_AddItemToObject(item, "license", lic);
    cJSON_AddItemToArray(arr, item);
}

static int write_cyclonedx(const char *output_dir, const Registry *reg,
                           const Component *comps, size_t n,
                           const char *timestamp, const char *uuid)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "bomFormat",  "CycloneDX");
    cJSON_AddStringToObject(root, "specVersion", "1.5");
    char serial[64];
    snprintf(serial, sizeof(serial), "urn:uuid:%s", uuid);
    cJSON_AddStringToObject(root, "serialNumber", serial);
    cJSON_AddNumberToObject(root, "version", 1);

    cJSON *meta = cJSON_AddObjectToObject(root, "metadata");
    cJSON_AddStringToObject(meta, "timestamp", timestamp);

    cJSON *tools = cJSON_AddArrayToObject(meta, "tools");
    cJSON *tool  = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "vendor",  "packmule");
    cJSON_AddStringToObject(tool, "name",    "packmule");
    cJSON_AddStringToObject(tool, "version", PACKMULE_VERSION);
    cJSON_AddItemToArray(tools, tool);

    /* The subject of the document: the bundle itself. */
    cJSON *subject = cJSON_AddObjectToObject(meta, "component");
    cJSON_AddStringToObject(subject, "type",     "application");
    cJSON_AddStringToObject(subject, "bom-ref",  "packmule-bundle");
    cJSON_AddStringToObject(subject, "name",     "packmule-bundle");
    cJSON_AddStringToObject(subject, "version",  PACKMULE_VERSION);
    cJSON_AddStringToObject(subject, "description",
                            "Offline dependency bundle produced by packmule");

    cJSON *components = cJSON_AddArrayToObject(root, "components");
    for (size_t i = 0; i < n; i++) {
        const Package *p = comps[i].pkg;

        cJSON *c = cJSON_CreateObject();
        cJSON_AddStringToObject(c, "type", "library");
        cJSON_AddStringToObject(c, "bom-ref",
                                comps[i].purl ? comps[i].purl
                                              : comps[i].spdx_id);
        cJSON_AddStringToObject(c, "name",    p->name);
        cJSON_AddStringToObject(c, "version", p->version ? p->version : "");
        if (comps[i].purl)
            cJSON_AddStringToObject(c, "purl", comps[i].purl);

        cdx_add_license(c, p->license);

        cJSON *hashes = cJSON_AddArrayToObject(c, "hashes");
        cJSON *h      = cJSON_CreateObject();
        cJSON_AddStringToObject(h, "alg",     "SHA-256");
        cJSON_AddStringToObject(h, "content", comps[i].sha256);
        cJSON_AddItemToArray(hashes, h);

        /* The filename and the URL it came from are what make a bundle
         * auditable after the fact; neither has a first-class CycloneDX
         * field, so they go in properties. */
        cJSON *props = cJSON_AddArrayToObject(c, "properties");
        cJSON *pf    = cJSON_CreateObject();
        cJSON_AddStringToObject(pf, "name",  "packmule:filename");
        cJSON_AddStringToObject(pf, "value", p->filename);
        cJSON_AddItemToArray(props, pf);
        if (p->url) {
            cJSON *pu = cJSON_CreateObject();
            cJSON_AddStringToObject(pu, "name",  "packmule:sourceUrl");
            cJSON_AddStringToObject(pu, "value", p->url);
            cJSON_AddItemToArray(props, pu);
        }

        cJSON_AddItemToArray(components, c);
    }

    /*
     * Dependency edges.  CycloneDX requires every bom-ref to appear here, so
     * a component with no known dependencies still gets an empty dependsOn.
     */
    cJSON *deps = cJSON_AddArrayToObject(root, "dependencies");

    cJSON *root_entry = cJSON_CreateObject();
    cJSON_AddStringToObject(root_entry, "ref", "packmule-bundle");
    cJSON *root_on = cJSON_AddArrayToObject(root_entry, "dependsOn");
    for (size_t i = 0; i < n; i++)
        cJSON_AddItemToArray(root_on, cJSON_CreateString(
            comps[i].purl ? comps[i].purl : comps[i].spdx_id));
    cJSON_AddItemToArray(deps, root_entry);

    for (size_t i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "ref",
                                comps[i].purl ? comps[i].purl
                                              : comps[i].spdx_id);
        cJSON *on = cJSON_AddArrayToObject(e, "dependsOn");

        for (char **d = comps[i].pkg->dep_specs; d && *d; d++) {
            char *dn = dep_spec_name(reg->name, *d);
            if (!dn)
                continue;
            size_t j = find_component(comps, n, dn, reg);
            pm_free(dn);
            if (j != (size_t)-1 && j != i)
                cJSON_AddItemToArray(on, cJSON_CreateString(
                    comps[j].purl ? comps[j].purl : comps[j].spdx_id));
        }
        cJSON_AddItemToArray(deps, e);
    }

    return write_json(output_dir, SBOM_FILE_CYCLONEDX, root);
}

/*
 * spdx_license — a value acceptable in licenseConcluded / licenseDeclared.
 *
 * Those fields take an SPDX licence *expression*, not free text, and a
 * validator rejects anything else.  Registries publish free text, so anything
 * that does not look like an expression becomes NOASSERTION — which is the
 * defined way to say "not determined" and keeps the document valid.  The raw
 * string is still preserved verbatim in the CycloneDX output.
 */
static const char *spdx_license(const char *license)
{
    if (!license || !*license)
        return "NOASSERTION";

    for (const char *p = license; *p; p++) {
        if (!isalnum((unsigned char)*p) &&
            *p != '-' && *p != '.' && *p != '+' &&
            *p != ' ' && *p != '(' && *p != ')')
            return "NOASSERTION";
    }

    /* A bare "MIT License" or "Apache 2.0" passes the charset test but is not
     * an SPDX identifier.  Require the shape of one: no spaces unless they
     * join expression keywords. */
    for (const char *p = license; *p; p++) {
        if (*p != ' ')
            continue;
        if (strncmp(p, " AND ", 5) != 0 && strncmp(p, " OR ", 4) != 0 &&
            strncmp(p, " WITH ", 6) != 0)
            return "NOASSERTION";
    }

    return license;
}

static int write_spdx(const char *output_dir, const Registry *reg,
                      const Component *comps, size_t n,
                      const char *timestamp, const char *uuid)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "spdxVersion", "SPDX-2.3");
    cJSON_AddStringToObject(root, "dataLicense", "CC0-1.0");
    cJSON_AddStringToObject(root, "SPDXID",      "SPDXRef-DOCUMENT");
    cJSON_AddStringToObject(root, "name",        "packmule-bundle");

    char ns[128];
    snprintf(ns, sizeof(ns), "https://packmule.invalid/spdx/%s", uuid);
    cJSON_AddStringToObject(root, "documentNamespace", ns);

    cJSON *ci = cJSON_AddObjectToObject(root, "creationInfo");
    cJSON_AddStringToObject(ci, "created", timestamp);
    cJSON *creators = cJSON_AddArrayToObject(ci, "creators");
    cJSON_AddItemToArray(creators,
                         cJSON_CreateString("Tool: packmule-" PACKMULE_VERSION));

    cJSON *packages = cJSON_AddArrayToObject(root, "packages");
    for (size_t i = 0; i < n; i++) {
        const Package *p = comps[i].pkg;

        cJSON *sp = cJSON_CreateObject();
        cJSON_AddStringToObject(sp, "SPDXID",      comps[i].spdx_id);
        cJSON_AddStringToObject(sp, "name",        p->name);
        cJSON_AddStringToObject(sp, "versionInfo", p->version ? p->version : "");
        cJSON_AddStringToObject(sp, "downloadLocation",
                                p->url ? p->url : "NOASSERTION");
        cJSON_AddStringToObject(sp, "packageFileName", p->filename);
        /* We hash the archive, we do not unpack and enumerate its contents. */
        cJSON_AddBoolToObject(sp, "filesAnalyzed", 0);

        cJSON *sums = cJSON_AddArrayToObject(sp, "checksums");
        cJSON *sum  = cJSON_CreateObject();
        cJSON_AddStringToObject(sum, "algorithm",     "SHA256");
        cJSON_AddStringToObject(sum, "checksumValue", comps[i].sha256);
        cJSON_AddItemToArray(sums, sum);

        /* Declared is what the registry published; concluded would mean
         * packmule had audited the source, which it has not. */
        cJSON_AddStringToObject(sp, "licenseConcluded", "NOASSERTION");
        cJSON_AddStringToObject(sp, "licenseDeclared", spdx_license(p->license));
        cJSON_AddStringToObject(sp, "copyrightText",   "NOASSERTION");

        if (comps[i].purl) {
            cJSON *refs = cJSON_AddArrayToObject(sp, "externalRefs");
            cJSON *ref  = cJSON_CreateObject();
            cJSON_AddStringToObject(ref, "referenceCategory", "PACKAGE-MANAGER");
            cJSON_AddStringToObject(ref, "referenceType",     "purl");
            cJSON_AddStringToObject(ref, "referenceLocator",  comps[i].purl);
            cJSON_AddItemToArray(refs, ref);
        }

        cJSON_AddItemToArray(packages, sp);
    }

    cJSON *rels = cJSON_AddArrayToObject(root, "relationships");
    for (size_t i = 0; i < n; i++) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "spdxElementId",      "SPDXRef-DOCUMENT");
        cJSON_AddStringToObject(r, "relatedSpdxElement", comps[i].spdx_id);
        cJSON_AddStringToObject(r, "relationshipType",   "DESCRIBES");
        cJSON_AddItemToArray(rels, r);
    }

    for (size_t i = 0; i < n; i++) {
        for (char **d = comps[i].pkg->dep_specs; d && *d; d++) {
            char *dn = dep_spec_name(reg->name, *d);
            if (!dn)
                continue;
            size_t j = find_component(comps, n, dn, reg);
            pm_free(dn);
            if (j == (size_t)-1 || j == i)
                continue;

            cJSON *r = cJSON_CreateObject();
            cJSON_AddStringToObject(r, "spdxElementId",      comps[i].spdx_id);
            cJSON_AddStringToObject(r, "relatedSpdxElement", comps[j].spdx_id);
            cJSON_AddStringToObject(r, "relationshipType",   "DEPENDS_ON");
            cJSON_AddItemToArray(rels, r);
        }
    }

    return write_json(output_dir, SBOM_FILE_SPDX, root);
}

/* ── Entry point ──────────────────────────────────────────────────────────── */

int sbom_write(const char *output_dir, const Registry *reg,
               const PackageList *packages, int formats)
{
    if (formats == SBOM_NONE)
        return 0;

    size_t     n     = 0;
    Component *comps = collect(output_dir, reg, packages, &n);
    if (!comps)
        return -1;

    char timestamp[32], uuid[40];
    timestamp_utc(timestamp, sizeof(timestamp));
    document_uuid(uuid, sizeof(uuid));

    int rc = 0;

    if (formats & SBOM_CYCLONEDX) {
        printf("packmule: writing %s ...\n", SBOM_FILE_CYCLONEDX);
        if (write_cyclonedx(output_dir, reg, comps, n, timestamp, uuid) != 0)
            rc = -1;
    }
    if (rc == 0 && (formats & SBOM_SPDX)) {
        printf("packmule: writing %s ...\n", SBOM_FILE_SPDX);
        if (write_spdx(output_dir, reg, comps, n, timestamp, uuid) != 0)
            rc = -1;
    }

    components_free(comps, n);
    return rc;
}
