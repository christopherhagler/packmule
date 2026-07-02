#include "bundle.h"
#include "hash.h"
#include "network.h"
#include "package.h"
#include "registry.h"
#include "utils.h"
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
 * dir that is the manifest's own directory): bundling writes manifest.json,
 * install.sh, and requirements.txt into the output dir, which would silently
 * overwrite the user's input manifest if it shares one of those names and
 * lives there.  Returns the colliding generated name, or NULL if safe.
 */
static const char *bundle_clobbers_manifest(const char *manifest_file,
                                            const char *output_dir)
{
    static const char *generated[] = { "manifest.json", "install.sh",
                                        "requirements.txt" };

    const char *slash = strrchr(manifest_file, '/');
    const char *mbase = slash ? slash + 1 : manifest_file;

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
    FILE *fp = popen("python3 -c 'import sys; print(sys.version_info[1])' 2>/dev/null", "r");
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
            "\n"
            "Options:\n"
            "  -f, --manifest <file>      Path to the package manifest (required)\n"
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
            "  -b, --bundle               Write manifest.json + install.sh, then create <dir>.tar.gz\n"
            "  -n, --dry-run              Resolve and print what would be downloaded; no files written\n"
            "  -V, --version              Print version and exit\n"
            "  -h, --help                 Show this help and exit\n"
            "\n"
            "Available registry types:\n",
            prog);
    print_registry_list();
    fprintf(stderr,
            "\nExamples:\n"
            "  %s -f requirements.txt -o ./vendor\n"
            "  %s -f requirements.txt -o ./vendor -a x86_64\n"
            "  %s -f requirements.txt -n\n"
            "  %s -f package.json     -o ./vendor -t npm\n"
            "  %s -f packages.txt     -o ./vendor -t rpm -a x86_64 \\\n"
            "      -u https://dl.fedoraproject.org/pub/fedora/linux/releases/40/Everything/x86_64/os\n",
            prog, prog, prog, prog, prog);
}

int main(int argc, char *argv[])
{
    const char *manifest_file     = NULL;
    const char *output_dir        = ".";
    const char *registry_type     = NULL; /* NULL → auto-detect from filename */
    const char *repo_url          = NULL;
    int         dry_run           = 0;
    int         do_bundle         = 0;

    /* Detect the current machine architecture as the default target.
     * Sized to hold any platform's utsname.machine (Linux 65, BSD/macOS 256)
     * so the copy below can never truncate. */
    static char detected_arch[256];
    const char *target_os = NULL;   /* host OS family, overridable by --os */
    {
        struct utsname uts;
        if (uname(&uts) == 0) {
            snprintf(detected_arch, sizeof(detected_arch), "%s", uts.machine);
            target_os = normalize_os(uts.sysname);
        }
    }
    char *arch = detected_arch[0] ? detected_arch : NULL;

    /* Target CPython minor for wheel selection: -1 means "not set yet"; it is
     * resolved below to --python (if given) or the local python3. */
    int py_minor = -1;

    static const struct option LONG_OPTS[] = {
        { "help",     no_argument,       NULL, 'h' },
        { "manifest", required_argument, NULL, 'f' },
        { "version",  no_argument,       NULL, 'V' },
        { "type",     required_argument, NULL, 't' },
        { "arch",     required_argument, NULL, 'a' },
        { "os",       required_argument, NULL, 's' },
        { "python",   required_argument, NULL, 'p' },
        { "repo-url", required_argument, NULL, 'u' },
        { "bundle",   no_argument,       NULL, 'b' },
        { "dry-run",  no_argument,       NULL, 'n' },
        { NULL,       0,                 NULL,  0  },
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "hVf:o:t:a:s:p:u:bn", LONG_OPTS, NULL)) != -1) {
        switch (opt) {
        case 'h': usage(argv[0]); return EXIT_SUCCESS;
        case 'V': puts("packmule " PACKMULE_VERSION); return EXIT_SUCCESS;
        case 'f': manifest_file     = optarg; break;
        case 'o': output_dir        = optarg; break;
        case 't': registry_type     = optarg; break;
        case 'a': arch              = optarg; break;
        case 'u': repo_url          = optarg; break;
        case 'b': do_bundle         = 1;      break;
        case 'n': dry_run           = 1;      break;
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
    Registry       reg_inst  = *base_reg;
    reg_inst.ctx             = (void *)arch;
    reg_inst.repo_url        = repo_url;
    reg_inst.py_minor        = py_minor;
    reg_inst.target_os       = target_os;
    const Registry *reg      = &reg_inst;

    if (network_init() != 0) {
        fprintf(stderr, "packmule: failed to initialise libcurl\n");
        return EXIT_FAILURE;
    }

    PackageList *reqs = reg->parse_manifest(reg, manifest_file);
    if (!reqs) {
        network_cleanup();
        return EXIT_FAILURE;
    }

    /* "any" is a user-supplied sentinel meaning "no arch preference". */
    if (arch && strcmp(arch, "any") == 0) {
        arch          = NULL;
        reg_inst.ctx  = NULL;
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
    }
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

    int    exit_code = EXIT_SUCCESS;
    size_t resolved   = 0;
    size_t downloaded = 0;
    size_t cached     = 0;
    unsigned long long total_bytes = 0;

    /* Colour and in-place redraws only make sense on a terminal; when stdout
     * is a pipe or CI log we emit plain, permanent lines only. */
    int         tty   = isatty(fileno(stdout));
    const char *c_grn = tty ? "\033[32m" : "";
    const char *c_red = tty ? "\033[31m" : "";
    const char *c_rst = tty ? "\033[0m"  : "";

    /* Iterate with reqs->count as the upper bound so that transitive deps
     * appended inside the loop are picked up automatically. */
    for (size_t i = 0; i < reqs->count; i++) {
        Package *pkg    = reqs->items[i];
        int  was_pinned = (pkg->version != NULL);

        /* In download mode show a transient "resolving" line (terminal only)
         * that the download bar / result line overwrites.  In dry-run mode let
         * output flow freely since the user is reviewing the full list. */
        if (dry_run) {
            printf("  [%zu/%zu] %s%s%s",
                   i + 1, reqs->count,
                   pkg->name,
                   pkg->version ? "==" : "",
                   pkg->version ? pkg->version : "");
            fflush(stdout);
        } else if (tty) {
            printf("\r  [%zu/%zu] resolving %-.50s\033[K",
                   i + 1, reqs->count, pkg->name);
            fflush(stdout);
        }

        if (reg->resolve(reg, pkg) != 0) {
            if (dry_run) {
                printf(" -- FAILED\n");
            } else {
                if (tty) fputs("\r\033[K", stdout);
                printf("  %s✗%s [%zu/%zu] %s -- could not resolve\n",
                       c_red, c_rst, i + 1, reqs->count, pkg->name);
            }
            fprintf(stderr, "  packmule: could not resolve %s\n", pkg->name);
            exit_code = EXIT_FAILURE;
            continue;
        }

        /* Enqueue transitive deps via the registry's own get_deps hook.
         * All format-specific filtering is the registry's responsibility. */
        if (reg->get_deps)
            reg->get_deps(reg, pkg, reqs, reqs);

        if (dry_run) {
            if (!was_pinned && pkg->version)
                printf(" (resolved: %s)", pkg->version);
            putchar('\n');
            printf("         file  : %s\n"
                   "         url   : %s\n"
                   "         sha256: %s\n\n",
                   pkg->filename, pkg->url, pkg->sha256);
            ++resolved;
            continue;
        }

        char dest[4096];
        /* Defense in depth: the backends already basename their filenames,
         * but never trust a registry-supplied name with path components. */
        int nw = snprintf(dest, sizeof(dest), "%s/%s",
                          output_dir, pm_basename(pkg->filename));
        if (nw < 0 || nw >= (int)sizeof(dest)) {
            if (tty) fputs("\r\033[K", stdout);
            printf("  %s✗%s [%zu/%zu] %s -- destination path too long\n",
                   c_red, c_rst, i + 1, reqs->count, pkg->filename);
            exit_code = EXIT_FAILURE;
            ++resolved;
            continue;
        }

        /* Verified cache: if the file is already on disk and matches the
         * expected hash, skip the download — re-runs become resumable and
         * idempotent.  A missing, truncated, or stale file (hash mismatch, or
         * no hash to check against) falls through to a fresh download that
         * overwrites it. */
        struct stat cst;
        if (stat(dest, &cst) == 0 && file_matches_hash(dest, pkg->sha256)) {
            char size_str[16] = "?";
            pm_human_size((double)cst.st_size, size_str, sizeof(size_str));
            total_bytes += (unsigned long long)cst.st_size;
            if (tty) fputs("\r\033[K", stdout); /* erase the transient line */
            printf("  %s✓%s [%zu/%zu] %s  (%s, cached)\n",
                   c_grn, c_rst, i + 1, reqs->count, pkg->filename, size_str);
            ++cached;
            ++resolved;
            continue;
        }

        int dl_rc = download_file(pkg->url, dest, pkg->filename, tty);
        if (tty) fputs("\r\033[K", stdout); /* erase the transient progress bar */

        if (dl_rc != 0) {
            printf("  %s✗%s [%zu/%zu] %s -- download failed\n",
                   c_red, c_rst, i + 1, reqs->count, pkg->name);
            exit_code = EXIT_FAILURE;
            ++resolved;
            continue;
        }

        if (verify_file(dest, pkg->sha256) != 0) {
            remove(dest);
            printf("  %s✗%s [%zu/%zu] %s -- checksum mismatch\n",
                   c_red, c_rst, i + 1, reqs->count, pkg->filename);
            exit_code = EXIT_FAILURE;
            ++resolved;
            continue;
        }

        /* Success: leave a permanent record with the on-disk size. */
        char        size_str[16] = "?";
        struct stat st;
        if (stat(dest, &st) == 0) {
            pm_human_size((double)st.st_size, size_str, sizeof(size_str));
            total_bytes += (unsigned long long)st.st_size;
        }
        printf("  %s✓%s [%zu/%zu] %s  (%s)\n",
               c_grn, c_rst, i + 1, reqs->count, pkg->filename, size_str);

        ++downloaded;
        ++resolved;
    }

    if (dry_run) {
        printf("\npackmule: dry run complete -- %zu/%zu package(s) resolved"
               ", 0 downloaded\n", resolved, reqs->count);
    } else {
        char total_str[16];
        pm_human_size((double)total_bytes, total_str, sizeof(total_str));
        printf("\npackmule: %zu/%zu package(s) ready in %s (%s)",
               downloaded + cached, reqs->count, output_dir, total_str);
        if (cached)
            printf(" -- %zu downloaded, %zu cached", downloaded, cached);
        putchar('\n');
    }

    if (do_bundle && !dry_run) {
        if (exit_code != EXIT_SUCCESS) {
            fprintf(stderr,
                    "packmule: skipping bundle -- one or more packages failed\n");
        } else {
            BundleOptions bopts;
            bopts.output_dir    = output_dir;
            bopts.registry_name = reg->name;
            bopts.packages      = reqs;
            if (bundle_create(&bopts) != 0)
                exit_code = EXIT_FAILURE;
        }
    }

    package_list_destroy(reqs);
    network_cleanup();

    return exit_code;
}
