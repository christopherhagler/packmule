#include "bundle.h"
#include "hash.h"
#include "network.h"
#include "package.h"
#include "registry.h"
#include "version.h"

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>

static void print_registry_list(void)
{
    const char *const *names = registry_names();
    for (int i = 0; names[i]; i++)
        fprintf(stderr, "  %s%s\n", names[i], i == 0 ? "  (default)" : "");
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "packmule " PACKMULE_VERSION " -- air-gapped package bundler\n"
            "\nUsage: %s -r <manifest> [-o <dir>] [-t <type>] [-a <arch>] [-u <url>] [-b] [-n]\n"
            "\n"
            "Options:\n"
            "  -r <file>                  Path to the package manifest (required)\n"
            "  -o <dir>                   Output directory for downloads (default: .)\n"
            "  -t, --type <type>          Registry backend (default: pypi)\n"
            "  -a, --arch <arch>          Target CPU architecture (default: current machine)\n"
            "                             e.g. x86_64, aarch64, arm64; use 'any' for universal only\n"
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
            "  %s -r requirements.txt -o ./vendor\n"
            "  %s -r requirements.txt -o ./vendor -a x86_64\n"
            "  %s -r requirements.txt -n\n"
            "  %s -r package.json     -o ./vendor -t npm\n"
            "  %s -r packages.txt     -o ./vendor -t rpm -a x86_64 \\\n"
            "      -u https://dl.fedoraproject.org/pub/fedora/linux/releases/40/Everything/x86_64/os\n",
            prog, prog, prog, prog, prog);
}

int main(int argc, char *argv[])
{
    const char *requirements_file = NULL;
    const char *output_dir        = ".";
    const char *registry_type     = "pypi";
    const char *repo_url          = NULL;
    int         dry_run           = 0;
    int         do_bundle         = 0;

    /* Detect the current machine architecture as the default target. */
    static char detected_arch[65];
    {
        struct utsname uts;
        if (uname(&uts) == 0)
            snprintf(detected_arch, sizeof(detected_arch), "%s", uts.machine);
    }
    char *arch = detected_arch[0] ? detected_arch : NULL;

    static const struct option LONG_OPTS[] = {
        { "help",     no_argument,       NULL, 'h' },
        { "version",  no_argument,       NULL, 'V' },
        { "type",     required_argument, NULL, 't' },
        { "arch",     required_argument, NULL, 'a' },
        { "repo-url", required_argument, NULL, 'u' },
        { "bundle",   no_argument,       NULL, 'b' },
        { "dry-run",  no_argument,       NULL, 'n' },
        { NULL,       0,                 NULL,  0  },
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "hVr:o:t:a:u:bn", LONG_OPTS, NULL)) != -1) {
        switch (opt) {
        case 'h': usage(argv[0]); return EXIT_SUCCESS;
        case 'V': puts("packmule " PACKMULE_VERSION); return EXIT_SUCCESS;
        case 'r': requirements_file = optarg; break;
        case 'o': output_dir        = optarg; break;
        case 't': registry_type     = optarg; break;
        case 'a': arch              = optarg; break;
        case 'u': repo_url          = optarg; break;
        case 'b': do_bundle         = 1;      break;
        case 'n': dry_run           = 1;      break;
        default:
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (optind < argc) {
        fprintf(stderr, "packmule: unexpected argument: %s\n", argv[optind]);
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (!requirements_file) {
        fprintf(stderr, "packmule: -r <manifest> is required\n");
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    const Registry *base_reg = registry_find(registry_type);
    if (!base_reg) {
        fprintf(stderr, "packmule: unknown registry type '%s'\n"
                        "Available types:\n", registry_type);
        print_registry_list();
        return EXIT_FAILURE;
    }

    /* Shallow-copy the static registry so we can inject runtime config without
     * mutating the shared constant. */
    Registry       reg_inst  = *base_reg;
    reg_inst.ctx             = (void *)arch;
    reg_inst.repo_url        = repo_url;
    const Registry *reg      = &reg_inst;

    if (network_init() != 0) {
        fprintf(stderr, "packmule: failed to initialise libcurl\n");
        return EXIT_FAILURE;
    }

    PackageList *reqs = reg->parse_manifest(reg, requirements_file);
    if (!reqs) {
        network_cleanup();
        return EXIT_FAILURE;
    }

    /* "any" is a user-supplied sentinel meaning "no arch preference". */
    if (arch && strcmp(arch, "any") == 0) {
        arch          = NULL;
        reg_inst.ctx  = NULL;
    }

    printf("packmule: backend   : %s\n", reg->name);
    printf("packmule: arch      : %s\n",
           arch ? arch : "any (universal/source packages only)");
    printf("packmule: manifest  : %s (%zu package(s))\n",
           requirements_file, reqs->count);
    if (dry_run)
        printf("packmule: mode      : DRY RUN -- resolve only, no files written\n");
    else
        printf("packmule: output dir: %s\n", output_dir);
    putchar('\n');

    if (!dry_run) {
        if (mkdir(output_dir, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "packmule: cannot create output directory '%s': %s\n",
                    output_dir, strerror(errno));
            package_list_destroy(reqs);
            network_cleanup();
            return EXIT_FAILURE;
        }
    }

    int    exit_code = EXIT_SUCCESS;
    size_t resolved   = 0;
    size_t downloaded = 0;

    /* Iterate with reqs->count as the upper bound so that transitive deps
     * appended inside the loop are picked up automatically. */
    for (size_t i = 0; i < reqs->count; i++) {
        Package *pkg    = reqs->items[i];
        int  was_pinned = (pkg->version != NULL);

        /* In download mode, overwrite the same status line using \r so the
         * terminal does not scroll.  In dry-run mode, let output flow freely
         * since the user is reviewing the full list. */
        if (!dry_run) {
            printf("\r  [%zu/%-4zu] resolving %-40.40s\033[K",
                   i + 1, reqs->count, pkg->name);
            fflush(stdout);
        } else {
            printf("  [%zu/%zu] %s%s%s",
                   i + 1, reqs->count,
                   pkg->name,
                   pkg->version ? "==" : "",
                   pkg->version ? pkg->version : "");
            fflush(stdout);
        }

        if (reg->resolve(reg, pkg) != 0) {
            if (dry_run)
                printf(" -- FAILED\n");
            else
                fprintf(stdout, "\n");
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
        } else {
            char dest[4096];
            int nw = snprintf(dest, sizeof(dest), "%s/%s",
                              output_dir, pkg->filename);
            if (nw < 0 || nw >= (int)sizeof(dest)) {
                fprintf(stderr, "\n  packmule: destination path too long for %s\n",
                        pkg->filename);
                exit_code = EXIT_FAILURE;
                ++resolved;
                continue;
            }

            printf("\r  [%zu/%-4zu] downloading %-38.38s\033[K",
                   i + 1, reqs->count, pkg->filename);
            fflush(stdout);

            if (download_file(pkg->url, dest, 1) != 0) {
                fprintf(stderr, "\n  packmule: download failed for %s\n", pkg->name);
                exit_code = EXIT_FAILURE;
                ++resolved;
                continue;
            }

            if (verify_file(dest, pkg->sha256) != 0) {
                remove(dest);
                exit_code = EXIT_FAILURE;
                ++resolved;
                continue;
            }

            ++downloaded;
        }

        ++resolved;
    }

    /* End the in-place status line with a newline before the summary. */
    if (!dry_run)
        putchar('\n');

    if (dry_run) {
        printf("packmule: dry run complete -- %zu/%zu package(s) resolved"
               ", 0 downloaded\n", resolved, reqs->count);
    } else {
        printf("packmule: %zu/%zu package(s) downloaded to %s\n",
               downloaded, reqs->count, output_dir);
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
