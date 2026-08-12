#include "auth.h"
#include "bundle.h"
#include "hash.h"
#include "network.h"
#include "package.h"
#include "pylock.h"
#include "registry.h"
#include "registry_internal.h"
#include "resolve.h"
#include "sbom.h"
#include "utils.h"
#include "verify.h"
#include "version.h"

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

/* ── Terminal presentation ───────────────────────────────────────────────── */

/*
 * Colour and in-place redraws only make sense on a terminal; when stdout is a
 * pipe or a CI log we emit plain, permanent lines only.
 */
static struct {
    int         tty;
    const char *grn, *red, *ylw, *rst;
} ui;

static void ui_init(void)
{
    ui.tty = isatty(fileno(stdout));
    ui.grn = ui.tty ? "\033[32m" : "";
    ui.red = ui.tty ? "\033[31m" : "";
    ui.ylw = ui.tty ? "\033[33m" : "";
    ui.rst = ui.tty ? "\033[0m"  : "";
}

/* Erase the transient status line, if there is one. */
static void ui_clear_line(void)
{
    if (ui.tty)
        fputs("\r\033[K", stdout);
}

static void print_registry_list(void)
{
    const char *const *names = registry_names();
    for (int i = 0; names[i]; i++) {
        const Registry *reg = registry_find(names[i]);
        fprintf(stderr, "  %-6s (%s)%s\n",
                names[i],
                reg && reg->manifest_name ? reg->manifest_name : "?",
                i == 0 ? "  [default]" : "");
    }
}

/*
 * bundle_clobbers_manifest — guard against a destructive `-o .` (or any output
 * dir that is the manifest's own directory): bundling writes generated files
 * into the output dir, which would silently overwrite the user's input
 * manifest if it shares one of those names and lives there.  Returns the
 * colliding generated name, or NULL if safe.
 */
static const char *bundle_clobbers_manifest(const char *manifest_file,
                                            const char *output_dir)
{
    static const char *const generated[] = { "manifest.json", "install.sh",
                                             "requirements.txt", "SHA256SUMS" };

    const char *mbase = pm_basename(manifest_file);

    const char *match = NULL;
    for (size_t i = 0; i < sizeof(generated) / sizeof(generated[0]); i++)
        if (strcmp(mbase, generated[i]) == 0) { match = generated[i]; break; }
    if (!match)
        return NULL;

    /* Names collide — do they also land in the same directory? */
    char mdir[PATH_MAX];
    snprintf(mdir, sizeof(mdir), "%s", manifest_file);
    char *ms = strrchr(mdir, '/');
    if (ms) *ms = '\0';
    else    snprintf(mdir, sizeof(mdir), ".");

    char rmdir[PATH_MAX], rodir[PATH_MAX];
    if (!realpath(mdir, rmdir) || !realpath(output_dir, rodir))
        return NULL;               /* can't resolve → don't block */
    return strcmp(rmdir, rodir) == 0 ? match : NULL;
}

/*
 * parse_python_minor — parse a --python value into a CPython 3.x minor number.
 * Accepts "3.12", "3.12.2", "312", or "cp312"; returns the minor (e.g. 12) or
 * -1 if the value is not a CPython 3.x spec we can target.
 */
static int parse_python_minor(const char *s)
{
    if (!s) return -1;
    if (strncmp(s, "cp", 2) == 0) s += 2;   /* "cp312" → "312" */

    if (s[0] != '3') return -1;
    s++;
    if (*s == '.') s++;                      /* "3.12" → "12"; "312" → "12" */
    if (!isdigit((unsigned char)*s)) return -1;
    return atoi(s);
}

/*
 * detect_python_minor — ask the local python3 for its minor version so wheel
 * selection defaults to the interpreter on this machine.  Returns the minor
 * (e.g. 12) or 0 if no usable python3 is found.
 */
static int detect_python_minor(void)
{
    FILE *fp = popen("python3 -c 'import sys; print(sys.version_info[1])'"
                     " 2>/dev/null", "r");
    if (!fp) return 0;
    char buf[16] = {0};
    char *got = fgets(buf, sizeof(buf), fp);
    pclose(fp);
    if (!got) return 0;
    int minor = atoi(buf);
    return minor > 0 ? minor : 0;
}

/*
 * normalize_os — map a user-supplied --os value (or a uname sysname) to the
 * canonical family wheel selection uses: "linux", "macos", or "windows".
 * Returns NULL for anything unrecognised.
 */
static const char *normalize_os(const char *s)
{
    if (!s) return NULL;
    if (strcasecmp(s, "linux")   == 0)                                return "linux";
    if (strcasecmp(s, "macos")   == 0 || strcasecmp(s, "mac")    == 0 ||
        strcasecmp(s, "darwin")  == 0 || strcasecmp(s, "osx")    == 0) return "macos";
    if (strcasecmp(s, "windows") == 0 || strcasecmp(s, "win")    == 0 ||
        strcasecmp(s, "win32")   == 0)                                return "windows";
    return NULL;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "packmule " PACKMULE_VERSION " -- air-gapped package bundler\n"
            "\nUsage: %s -f <manifest> [-o <dir>] [-t <type>] [-a <arch>] [-s <os>] [-p <ver>] [-u <url>] [-b] [-n]\n"
            "       %s verify <bundle-dir>\n"
            "\n"
            "Options:\n"
            "  -f, --manifest <file>      Path to the package manifest (required)\n"
            "                             pypi: requirements.txt, or a lockfile\n"
            "                             (uv.lock, pylock.toml, pyproject.toml\n"
            "                             with one beside it) for the exact tree\n"
            "  -o <dir>                   Output directory for downloads (default: .)\n"
            "  -t, --type <type>          Registry backend (default: auto-detect\n"
            "                             from the manifest filename, else pypi)\n"
            "  -a, --arch <arch>          Target CPU architecture (default: current machine)\n"
            "                             e.g. x86_64, aarch64, arm64; use 'any' for universal only\n"
            "  -s, --os <os>              Target OS for wheels (pypi only): linux, macos,\n"
            "                             windows, or any (default: the host OS)\n"
            "  -p, --python <ver>         Target CPython version for wheels (pypi only)\n"
            "                             e.g. 3.12 (default: the local python3)\n"
            "  -u, --repo-url <url>       Repository base URL\n"
            "                             Required for rpm; optional override for pypi/npm\n"
            "      --index <mode>         PyPI index API: auto (default), simple, or json.\n"
            "                             'auto' picks the better one: pypi.org's JSON API\n"
            "                             normally, the PEP 503 simple API when --repo-url is\n"
            "                             set (private indexes rarely implement JSON)\n"
            "  -b, --bundle               Write manifest.json + install.sh + SHA256SUMS,\n"
            "                             then create <dir>.tar.gz\n"
            "  -n, --dry-run              Resolve and print what would be downloaded; no files written\n"
            "  -j, --jobs <n>             Parallel downloads (1-%d, default %d)\n"
            "      --no-verify            Skip the post-bundle offline install check\n"
            "      --sbom <format>        Also write an SBOM: cyclonedx, spdx, or both.\n"
            "                             Lands inside the bundle, covered by SHA256SUMS\n"
            "      --rpm-deps <mode>      rpm transitive deps: resolve (default) or none.\n"
            "                             'resolve' bundles the full closure from the repo,\n"
            "                             including base packages the target may already have\n"
            "  -V, --version              Print version and exit\n"
            "  -h, --help                 Show this help and exit\n"
            "\n"
            "Commands:\n"
            "  verify <dir>               Re-check an extracted bundle against its SHA256SUMS\n"
            "\n"
            "Authentication (private indexes: JFrog Artifactory, Nexus, devpi, ...):\n"
            "  Credentials come from the environment, never from the command line, and are\n"
            "  sent only to the host named by --repo-url.  Works for pypi, npm and rpm.\n"
            "    PACKMULE_USERNAME + PACKMULE_PASSWORD   HTTP Basic\n"
            "    PACKMULE_USERNAME + PACKMULE_TOKEN      HTTP Basic (API key / identity token)\n"
            "    PACKMULE_TOKEN                          Bearer token\n"
            "    PACKMULE_AUTH_HEADER                    literal header for any other scheme,\n"
            "                                            e.g. 'X-JFrog-Art-Api: <key>'\n"
            "    PACKMULE_AUTH_HOSTS                     extra hosts, comma-separated, for\n"
            "                                            indexes that serve files elsewhere\n"
            "    PACKMULE_AUTH_INSECURE=1                allow credentials over plain http\n"
            "\n"
            "Corporate TLS (no option to disable verification; name the CA instead):\n"
            "    PACKMULE_CA_BUNDLE                      CA file or directory\n"
            "                                            (CURL_CA_BUNDLE, SSL_CERT_FILE honoured)\n"
            "    PACKMULE_CLIENT_CERT / _KEY / _KEY_PASSWORD    client cert for mutual TLS\n"
            "  Proxies need no setup: http_proxy / https_proxy / no_proxy are honoured.\n"
            "\n"
            "Available registry types:\n",
            prog, prog, NETWORK_MAX_JOBS, NETWORK_DEFAULT_JOBS);
    print_registry_list();
    fprintf(stderr,
            "\nExamples:\n"
            "  %s -f requirements.txt -o ./vendor\n"
            "  %s -f uv.lock          -o ./vendor -a x86_64 -p 3.12\n"
            "  %s -f requirements.txt -n\n"
            "  %s -f package.json     -o ./vendor -t npm\n"
            "  %s -f packages.txt     -o ./vendor -t rpm -a x86_64 \\\n"
            "      -u https://dl.fedoraproject.org/pub/fedora/linux/releases/40/Everything/x86_64/os\n"
            "  %s verify ./vendor\n"
            "\n"
            "  export PACKMULE_TOKEN=...   # private Artifactory PyPI repository\n"
            "  %s -f requirements.txt -o ./vendor \\\n"
            "      -u https://art.corp/artifactory/api/pypi/pypi-virtual/simple\n",
            prog, prog, prog, prog, prog, prog, prog);
}

/* ── Resolution progress ─────────────────────────────────────────────────── */

static int g_dry_run;

static void on_resolve_progress(const Package *pkg, size_t index, size_t total,
                                int round)
{
    if (g_dry_run || !ui.tty)
        return;
    /* Transient: the next status line or the phase summary overwrites it. */
    printf("\r  [%zu/%zu] resolving %-.40s%s\033[K",
           index + 1, total, pkg->name, round > 1 ? " (revisit)" : "");
    fflush(stdout);
}

/* ── Dry run report ──────────────────────────────────────────────────────── */

static void print_dry_run(const PackageList *reqs)
{
    for (size_t i = 0; i < reqs->count; i++) {
        const Package *p = reqs->items[i];

        printf("  [%zu/%zu] %s", i + 1, reqs->count, p->name);
        if (p->constraint)
            printf(" %s", p->constraint);

        if (p->state != PKG_RESOLVED) {
            printf(" -- %sFAILED%s\n", ui.red, ui.rst);
            continue;
        }

        printf(" -> %s%s\n", p->version ? p->version : "?",
               p->user_pinned ? " (pinned)" : "");

        char *dg = digest_to_string(&p->digest);
        printf("         file  : %s\n"
               "         url   : %s\n"
               "         digest: %s\n\n",
               p->filename ? p->filename : "?",
               p->url ? p->url : "?",
               dg ? dg : "(none)");
        pm_free(dg);
    }
}

/* ── Download phase ──────────────────────────────────────────────────────── */

typedef struct {
    size_t             downloaded;
    size_t             cached;
    size_t             failed;
    unsigned long long total_bytes;
} DownloadStats;

/* Transient "n/m downloading" line while transfers overlap. */
static void on_download_done(const DownloadJob *job, size_t completed,
                             size_t total)
{
    (void)job;
    if (!ui.tty)
        return;
    printf("\r  downloading ... %zu/%zu\033[K", completed, total);
    fflush(stdout);
}

/*
 * download_all — fetch every resolved package into `output_dir`.
 *
 * Downloading only after resolution has reached a fixed point matters: the
 * resolver may revise its choice of version for a package more than once, and
 * fetching eagerly would leave the earlier candidates on disk and in the
 * bundle.
 *
 * Files already present with the right digest are skipped first, so a re-run
 * is cheap and interrupted runs resume.  The rest are fetched with several
 * transfers in flight, then verified serially.
 */
static int download_all(const PackageList *reqs, const char *output_dir,
                        int jobs_n, DownloadStats *out)
{
    DownloadStats st = {0};
    int rc = 0;

    /* Per-package destination paths, kept alive for the whole download. */
    char **dests = pm_calloc(reqs->count, sizeof(char *));
    DownloadJob *jobs = pm_calloc(reqs->count, sizeof(DownloadJob));
    Package **job_pkg = pm_calloc(reqs->count, sizeof(Package *));
    size_t njobs = 0;

    for (size_t i = 0; i < reqs->count; i++) {
        Package *pkg = reqs->items[i];
        if (pkg->state != PKG_RESOLVED)
            continue;

        /* Defense in depth: the backends already basename their filenames,
         * but never trust a registry-supplied name with path components. */
        dests[i] = pm_asprintf("%s/%s", output_dir, pm_basename(pkg->filename));

        /* Verified cache: if the file is already on disk and matches the
         * expected digest, skip the download — re-runs become resumable and
         * idempotent.  A missing, truncated, or stale file falls through to a
         * fresh download that overwrites it. */
        /*
         * On an index without PEP 658, resolution had to download this very
         * artifact to read its dependency metadata.  Move it into place rather
         * than fetching it a second time — for a large wheel that is the
         * difference between one transfer and two.  A cross-filesystem rename
         * fails harmlessly and the normal download takes over.
         */
        const char *staged = pypi_cached_artifact(pkg->filename);
        if (staged && digest_matches_file(staged, &pkg->digest))
            rename(staged, dests[i]);

        struct stat cst;
        if (stat(dests[i], &cst) == 0 &&
            digest_matches_file(dests[i], &pkg->digest)) {
            char size_str[16] = "?";
            pm_human_size((double)cst.st_size, size_str, sizeof(size_str));
            st.total_bytes += (unsigned long long)cst.st_size;
            printf("  %s✓%s %s  (%s, cached)\n",
                   ui.grn, ui.rst, pkg->filename, size_str);
            st.cached++;
            continue;
        }

        jobs[njobs].url       = pkg->url;
        jobs[njobs].dest_path = dests[i];
        jobs[njobs].label     = pkg->filename;
        job_pkg[njobs]        = pkg;
        njobs++;
    }

    if (njobs > 0) {
        /* One transfer at a time keeps the familiar per-file progress bar;
         * beyond that a bar per file would fight over the same line, so the
         * aggregate counter takes over. */
        if (jobs_n == 1) {
            for (size_t j = 0; j < njobs; j++) {
                jobs[j].rc = download_file(jobs[j].url, jobs[j].dest_path,
                                           jobs[j].label, ui.tty);
                ui_clear_line();
            }
        } else {
            download_many(jobs, njobs, jobs_n, on_download_done);
            ui_clear_line();
        }

        for (size_t j = 0; j < njobs; j++) {
            Package *pkg = job_pkg[j];

            if (jobs[j].rc != 0) {
                printf("  %s✗%s %s -- download failed\n",
                       ui.red, ui.rst, pkg->name);
                st.failed++;
                rc = -1;
                continue;
            }
            if (digest_verify_file(jobs[j].dest_path, &pkg->digest) != 0) {
                remove(jobs[j].dest_path);
                printf("  %s✗%s %s -- checksum mismatch\n",
                       ui.red, ui.rst, pkg->filename);
                st.failed++;
                rc = -1;
                continue;
            }

            char        size_str[16] = "?";
            struct stat sb;
            if (stat(jobs[j].dest_path, &sb) == 0) {
                pm_human_size((double)sb.st_size, size_str, sizeof(size_str));
                st.total_bytes += (unsigned long long)sb.st_size;
            }
            printf("  %s✓%s %s  (%s)\n",
                   ui.grn, ui.rst, pkg->filename, size_str);
            st.downloaded++;
        }
    }

    for (size_t i = 0; i < reqs->count; i++)
        pm_free(dests[i]);
    pm_free(dests);
    pm_free(jobs);
    pm_free(job_pkg);

    *out = st;
    return rc;
}

/* ── verify subcommand ──────────────────────────────────────────────────── */

static int cmd_verify(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s verify <bundle-dir>\n", argv[0]);
        return EXIT_FAILURE;
    }
    return bundle_verify_checksums(argv[2]) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    ui_init();

    /* Subcommands are dispatched before getopt so their arguments are not
     * mistaken for option values. */
    if (argc > 1 && argv[1][0] != '-' && strcmp(argv[1], "verify") == 0)
        return cmd_verify(argc, argv);

    const char *manifest_file = NULL;
    const char *output_dir    = ".";
    const char *registry_type = NULL; /* NULL → auto-detect from filename */
    const char *repo_url      = NULL;
    int         dry_run       = 0;
    int         do_bundle     = 0;
    int         no_verify     = 0;
    int         jobs_n        = NETWORK_DEFAULT_JOBS;
    int         rpm_deps      = 1;   /* --rpm-deps resolve */

    /* Detect the current machine architecture as the default target.
     * Sized to hold any platform's utsname.machine (Linux 65, BSD/macOS 256)
     * so the copy below can never truncate. */
    static char detected_arch[256];
    const char *host_os   = NULL;   /* this machine's OS family, fixed */
    const char *target_os = NULL;   /* host OS family, overridable by --os */
    {
        struct utsname uts;
        if (uname(&uts) == 0) {
            snprintf(detected_arch, sizeof(detected_arch), "%s", uts.machine);
            host_os   = normalize_os(uts.sysname);
            target_os = host_os;
        }
    }
    const char *arch = detected_arch[0] ? detected_arch : NULL;

    /* Target CPython minor for wheel selection: -1 means "not set yet"; it is
     * resolved below to --python (if given) or the local python3. */
    int py_minor = -1;

    PypiIndexMode index_mode   = PYPI_INDEX_AUTO;
    int           sbom_formats = SBOM_NONE;

    enum { OPT_NO_VERIFY = 1000, OPT_RPM_DEPS, OPT_INDEX, OPT_SBOM };
    static const struct option LONG_OPTS[] = {
        { "help",      no_argument,       NULL, 'h' },
        { "manifest",  required_argument, NULL, 'f' },
        { "version",   no_argument,       NULL, 'V' },
        { "type",      required_argument, NULL, 't' },
        { "arch",      required_argument, NULL, 'a' },
        { "os",        required_argument, NULL, 's' },
        { "python",    required_argument, NULL, 'p' },
        { "repo-url",  required_argument, NULL, 'u' },
        { "bundle",    no_argument,       NULL, 'b' },
        { "dry-run",   no_argument,       NULL, 'n' },
        { "jobs",      required_argument, NULL, 'j' },
        { "no-verify", no_argument,       NULL, OPT_NO_VERIFY },
        { "rpm-deps",  required_argument, NULL, OPT_RPM_DEPS  },
        { "index",     required_argument, NULL, OPT_INDEX     },
        { "sbom",      required_argument, NULL, OPT_SBOM      },
        { NULL,        0,                 NULL,  0  },
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "hVf:o:t:a:s:p:u:j:bn",
                              LONG_OPTS, NULL)) != -1) {
        switch (opt) {
        case 'h': usage(argv[0]); return EXIT_SUCCESS;
        case 'V': puts("packmule " PACKMULE_VERSION); return EXIT_SUCCESS;
        case 'f': manifest_file = optarg; break;
        case 'o': output_dir    = optarg; break;
        case 't': registry_type = optarg; break;
        case 'a': arch          = optarg; break;
        case 'u': repo_url      = optarg; break;
        case 'b': do_bundle     = 1;      break;
        case 'n': dry_run       = 1;      break;
        case OPT_NO_VERIFY: no_verify = 1; break;
        case OPT_SBOM:
            sbom_formats = sbom_parse_format(optarg);
            if (sbom_formats == SBOM_NONE) {
                fprintf(stderr,
                        "packmule: invalid --sbom value '%s'"
                        " (expected 'cyclonedx', 'spdx', or 'both')\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case OPT_INDEX:
            if      (strcmp(optarg, "auto")   == 0) index_mode = PYPI_INDEX_AUTO;
            else if (strcmp(optarg, "simple") == 0) index_mode = PYPI_INDEX_SIMPLE;
            else if (strcmp(optarg, "json")   == 0) index_mode = PYPI_INDEX_JSON;
            else {
                fprintf(stderr,
                        "packmule: invalid --index value '%s'"
                        " (expected 'auto', 'simple', or 'json')\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case OPT_RPM_DEPS:
            if      (strcmp(optarg, "resolve") == 0) rpm_deps = 1;
            else if (strcmp(optarg, "none")    == 0) rpm_deps = 0;
            else {
                fprintf(stderr,
                        "packmule: invalid --rpm-deps value '%s'"
                        " (expected 'resolve' or 'none')\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'j': {
            char *endp = NULL;
            long  v    = strtol(optarg, &endp, 10);
            if (!endp || *endp || v < 1 || v > NETWORK_MAX_JOBS) {
                fprintf(stderr,
                        "packmule: invalid --jobs value '%s' (expected 1-%d)\n",
                        optarg, NETWORK_MAX_JOBS);
                return EXIT_FAILURE;
            }
            jobs_n = (int)v;
            break;
        }
        case 's':
            if (strcasecmp(optarg, "any") == 0) {
                target_os = NULL;
            } else {
                target_os = normalize_os(optarg);
                if (!target_os) {
                    fprintf(stderr,
                            "packmule: invalid --os value '%s'"
                            " (expected linux, macos, windows, or any)\n", optarg);
                    return EXIT_FAILURE;
                }
            }
            break;
        case 'p':
            py_minor = parse_python_minor(optarg);
            if (py_minor < 0) {
                fprintf(stderr,
                        "packmule: invalid --python value '%s'"
                        " (expected a CPython 3.x version, e.g. 3.12)\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        default:
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    /* No explicit --python: default to the local python3 so wheels match the
     * interpreter on this machine.  0 (none found) disables Python filtering. */
    if (py_minor < 0)
        py_minor = detect_python_minor();

    if (optind < argc) {
        fprintf(stderr, "packmule: unexpected argument: %s\n", argv[optind]);
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (!manifest_file) {
        fprintf(stderr, "packmule: -f <manifest> is required\n");
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    /* "any" is a user-supplied sentinel meaning "no arch preference".
     * Normalise it before anything reads it, so no code path can observe the
     * literal string. */
    if (arch && strcmp(arch, "any") == 0)
        arch = NULL;

    /* Resolve the backend: an explicit --type wins; otherwise infer it from
     * the manifest filename (requirements.txt → pypi, package.json → npm, …),
     * falling back to pypi when the name is unrecognised. */
    int auto_detected = 0;
    const Registry *base_reg;
    if (registry_type) {
        base_reg = registry_find(registry_type);
        if (!base_reg) {
            fprintf(stderr, "packmule: unknown registry type '%s'\n"
                            "Available types:\n", registry_type);
            print_registry_list();
            return EXIT_FAILURE;
        }
    } else {
        base_reg = registry_detect(manifest_file);
        if (!base_reg)
            base_reg = registry_find("pypi"); /* sensible default */
        else
            auto_detected = 1;
    }

    /* Shallow-copy the static registry so we can inject runtime config without
     * mutating the shared constant. */
    Registry  reg_inst = *base_reg;
    RpmConfig rpm_cfg  = { arch, rpm_deps, NULL };
    int       is_rpm   = (strcmp(reg_inst.name, "rpm") == 0);

    /* ctx is the backend's own configuration.  pypi and npm need only the
     * target architecture; rpm needs a struct (see RpmConfig). */
    if (is_rpm)
        reg_inst.ctx = &rpm_cfg;
    else
        memcpy(&reg_inst.ctx, &arch, sizeof(arch));  /* const char* → void* */

    reg_inst.repo_url   = repo_url;
    reg_inst.py_minor   = py_minor;
    reg_inst.target_os  = target_os;
    reg_inst.index_mode = index_mode;
    const Registry *reg = &reg_inst;

    /*
     * Credentials must be settled before the first request goes out, and a
     * misconfiguration is fatal rather than a silent fall back to an anonymous
     * fetch — that would quietly build a bundle from the wrong index.
     */
    if (auth_init(repo_url) != 0)
        return EXIT_FAILURE;

    /*
     * Registered rather than called at each return: the pypi backend may leave
     * a temporary directory of downloaded artifacts behind, and every early
     * exit below (resolution failure, dry run, bad output dir) must still
     * clean it up.  auth_cleanup() wipes the secrets from memory.
     */
    atexit(pypi_backend_cleanup);
    atexit(auth_cleanup);

    if (network_init() != 0) {
        fprintf(stderr, "packmule: failed to initialise libcurl\n");
        return EXIT_FAILURE;
    }

    PackageList *reqs = reg->parse_manifest(reg, manifest_file);
    if (!reqs) {
        network_cleanup();
        return EXIT_FAILURE;
    }

    printf("packmule: backend   : %s%s\n",
           reg->name, auto_detected ? " (auto-detected)" : "");
    printf("packmule: arch      : %s\n",
           arch ? arch : "any (universal/source packages only)");
    if (strcmp(reg->name, "pypi") == 0) {
        printf("packmule: os        : %s\n", target_os ? target_os : "any");
        if (py_minor > 0)
            printf("packmule: python    : 3.%d\n", py_minor);
        else
            printf("packmule: python    : any (no python3 found; "
                   "arch-only wheel matching)\n");
        /* Which API we chose is not obvious from --index auto, and it decides
         * what a failure means: say it up front. */
        printf("packmule: index     : %s\n",
               (index_mode == PYPI_INDEX_AUTO && repo_url) ? "simple (PEP 503)"
             : (index_mode == PYPI_INDEX_SIMPLE)           ? "simple (PEP 503)"
                                                           : "json");
    }
    if (repo_url)
        printf("packmule: repo url  : %s\n", repo_url);
    if (auth_configured())
        printf("packmule: auth      : %s credentials for %s\n",
               auth_scheme_name(), auth_scope_description());
    printf("packmule: manifest  : %s (%zu package(s))\n",
           manifest_file, reqs->count);
    if (dry_run)
        printf("packmule: mode      : DRY RUN -- resolve only, no files written\n");
    else
        printf("packmule: output dir: %s\n", output_dir);
    putchar('\n');

    if (!dry_run) {
        if (pm_mkdir_p(output_dir, 0755) != 0) {
            fprintf(stderr, "packmule: cannot create output directory '%s': %s\n",
                    output_dir, strerror(errno));
            package_list_destroy(reqs);
            network_cleanup();
            return EXIT_FAILURE;
        }

        /* Refuse a bundle that would overwrite the input manifest before we
         * spend time downloading anything. */
        if (do_bundle) {
            const char *clobber = bundle_clobbers_manifest(manifest_file, output_dir);
            if (clobber) {
                fprintf(stderr,
                        "packmule: --bundle would overwrite your input manifest '%s'\n"
                        "          (the bundle writes '%s' into the output directory).\n"
                        "          Use a dedicated output directory, e.g. -o bundle\n",
                        manifest_file, clobber);
                package_list_destroy(reqs);
                network_cleanup();
                return EXIT_FAILURE;
            }
        }
    }

    /* ── Phase 1: resolve to a fixed point ───────────────────────────────── */

    g_dry_run = dry_run;
    ResolveStats rs;
    int resolve_rc = resolve_all(reg, reqs, on_resolve_progress, &rs);
    ui_clear_line();

    if (dry_run) {
        print_dry_run(reqs);
        printf("\npackmule: dry run complete -- %zu/%zu package(s) resolved "
               "in %d round(s), 0 downloaded\n",
               rs.resolved, reqs->count, rs.rounds);
        package_list_destroy(reqs);
        network_cleanup();
        return resolve_rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (resolve_rc != 0) {
        fprintf(stderr,
                "\npackmule: resolution failed -- %zu of %zu package(s) could "
                "not be resolved.\n"
                "          No files were downloaded.\n",
                rs.failed, reqs->count);
        package_list_destroy(reqs);
        network_cleanup();
        return EXIT_FAILURE;
    }

    printf("packmule: resolved %zu package(s) in %d round(s)\n\n",
           rs.resolved, rs.rounds);

    /* ── Phase 2: download ───────────────────────────────────────────────── */

    DownloadStats ds;
    int dl_rc = download_all(reqs, output_dir, jobs_n, &ds);

    char total_str[16];
    pm_human_size((double)ds.total_bytes, total_str, sizeof(total_str));
    printf("\npackmule: %zu/%zu package(s) ready in %s (%s)",
           ds.downloaded + ds.cached, reqs->count, output_dir, total_str);
    if (ds.cached)
        printf(" -- %zu downloaded, %zu cached", ds.downloaded, ds.cached);
    putchar('\n');

    int exit_code = (dl_rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;

    /*
     * An SBOM without --bundle still belongs next to the packages it
     * describes.  With --bundle, bundle_create emits it instead, so that it is
     * written before SHA256SUMS and ends up covered by it and inside the
     * archive.
     */
    if (sbom_formats != SBOM_NONE && !do_bundle &&
        exit_code == EXIT_SUCCESS &&
        sbom_write(output_dir, reg, reqs, sbom_formats) != 0)
        exit_code = EXIT_FAILURE;

    /* ── Phase 3: bundle ─────────────────────────────────────────────────── */

    if (do_bundle) {
        if (exit_code != EXIT_SUCCESS) {
            fprintf(stderr,
                    "packmule: skipping bundle -- one or more packages failed\n");
        } else {
            BundleOptions bopts;
            bopts.output_dir    = output_dir;
            bopts.registry_name = reg->name;
            bopts.packages      = reqs;
            bopts.aux_file      = NULL;
            bopts.aux_name      = NULL;
            bopts.sbom_formats  = sbom_formats;
            bopts.registry      = reg;

            /* An npm bundle built from a lockfile must carry that lock:
             * install.sh replays its exact tree with `npm ci --offline`. */
            char *npm_lock = NULL;
            if (strcmp(reg->name, "npm") == 0 &&
                (npm_lock = npm_effective_lockfile(manifest_file)) != NULL) {
                bopts.aux_file = npm_lock;
                bopts.aux_name = "package-lock.json";
            }

            /*
             * A pypi bundle built from uv.lock / pylock.toml carries the lock
             * too, though for a different reason: install.sh installs from the
             * generated requirements.txt, so nothing at the destination needs
             * uv.  The lock travels as provenance — the record of what was
             * resolved, checksummed by SHA256SUMS like everything else.
             */
            char *py_lock = NULL;
            if (strcmp(reg->name, "pypi") == 0 &&
                (py_lock = pylock_effective(manifest_file)) != NULL) {
                bopts.aux_file = py_lock;
                bopts.aux_name = pm_basename(py_lock);
            }

            if (bundle_create(&bopts) != 0) {
                exit_code = EXIT_FAILURE;
            } else if (!no_verify) {
                BundleCheckResult cr = BUNDLE_CHECK_SKIPPED;
                if (strcmp(reg->name, "pypi") == 0)
                    cr = bundle_check_pypi(output_dir, arch, detected_arch,
                                           target_os, host_os, py_minor);
                else if (strcmp(reg->name, "npm") == 0)
                    cr = bundle_check_npm(output_dir);

                /*
                 * A failed check means the package manager itself rejected
                 * this closure.  That bundle will not install on the target,
                 * so the build fails here rather than shipping it — SKIPPED
                 * (prerequisites missing) is a different answer and is not
                 * treated as a failure.
                 */
                if (cr == BUNDLE_CHECK_PASSED) {
                    printf("packmule: offline install check PASSED\n");
                } else if (cr == BUNDLE_CHECK_FAILED) {
                    fprintf(stderr,
                            "packmule: offline install check FAILED -- the "
                            "package manager could not install this bundle "
                            "without an index.\n"
                            "          The bundle would not install on the "
                            "target machine.  Re-run with --no-verify to "
                            "build it anyway.\n");
                    exit_code = EXIT_FAILURE;
                }
            }
            pm_free(npm_lock);
            pm_free(py_lock);
        }
    }

    package_list_destroy(reqs);
    if (is_rpm)
        rpm_backend_cleanup(&rpm_cfg);
    network_cleanup();

    return exit_code;
}
