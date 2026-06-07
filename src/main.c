#include "bundle.h"
#include "hash.h"
#include "network.h"
#include "package.h"
#include "registry.h"
#include "version.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>

/*
 * parse_dep_name — extract the bare package name from a PEP 508 specifier.
 *
 * Stops at the first '[', '(', ';', '>', '<', '!', '=', '~', or whitespace
 * and lowercases the result so case-insensitive dedup works correctly.
 */
static void parse_dep_name(const char *spec, char *out, size_t out_size)
{
    size_t i = 0;
    while (i < out_size - 1 && spec[i] &&
           spec[i] != '[' && spec[i] != '(' &&
           spec[i] != ';' && spec[i] != '>' &&
           spec[i] != '<' && spec[i] != '!' &&
           spec[i] != '=' && spec[i] != '~' &&
           spec[i] != ' ' && spec[i] != '\t')
    {
        out[i] = (char)tolower((unsigned char)spec[i]);
        i++;
    }
    out[i] = '\0';
}

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
            "\nUsage: %s -r <manifest> [-o <dir>] [-t <type>] [-a <arch>] [-u <url>] [-n]\n"
            "\n"
            "Options:\n"
            "  -r <file>         Path to the package manifest (required)\n"
            "  -o <dir>          Output directory for downloads (default: .)\n"
            "  -t <type>         Registry backend (default: pypi)\n"
            "  -a <arch>         Target CPU architecture for package selection\n"
            "                    (default: current machine, e.g. x86_64, aarch64, arm64)\n"
            "                    Use 'any' to prefer universal/source packages only\n"
            "  -u <url>          Repository base URL\n"
            "                    Required for rpm; optional for pypi/npm (overrides public endpoint)\n"
            "                    e.g. https://dl.fedoraproject.org/pub/fedora/linux/releases/40/Everything/x86_64/os\n"
            "                    e.g. https://artifactory.example.com/artifactory/api/pypi/pypi-virtual/pypi\n"
            "  -b, --bundle      After downloading, write manifest.json and\n"
            "                    install.sh into <dir>, then create <dir>.tar.gz\n"
            "  -n, --dry-run     Resolve packages and print what would be\n"
            "                    downloaded without writing any files\n"
            "  -V                Print version and exit\n"
            "  -h, --help        Show this help and exit\n"
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

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return EXIT_SUCCESS;
        }

        if (strcmp(argv[i], "-V") == 0 || strcmp(argv[i], "--version") == 0) {
            puts("packmule " PACKMULE_VERSION);
            return EXIT_SUCCESS;
        }

        if (strcmp(argv[i], "-r") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "packmule: -r requires an argument\n");
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            requirements_file = argv[++i];
            continue;
        }

        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "packmule: -o requires an argument\n");
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            output_dir = argv[++i];
            continue;
        }

        if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--type") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "packmule: -t requires an argument\n");
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            registry_type = argv[++i];
            continue;
        }

        if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--arch") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "packmule: -a requires an argument\n");
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            arch = argv[++i];
            continue;
        }

        if (strcmp(argv[i], "-u") == 0 || strcmp(argv[i], "--repo-url") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "packmule: -u requires an argument\n");
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            repo_url = argv[++i];
            continue;
        }

        if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--bundle") == 0) {
            do_bundle = 1;
            continue;
        }

        if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--dry-run") == 0) {
            dry_run = 1;
            continue;
        }

        fprintf(stderr, "packmule: unrecognised option: %s\n", argv[i]);
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

        printf("  [%zu/%zu] %s%s%s",
               i + 1, reqs->count,
               pkg->name,
               pkg->version ? "==" : "",
               pkg->version ? pkg->version : "");
        fflush(stdout);

        if (reg->resolve(reg, pkg) != 0) {
            printf(" -- FAILED\n");
            fprintf(stderr, "  packmule: could not resolve %s\n", pkg->name);
            exit_code = EXIT_FAILURE;
            continue;
        }

        if (!was_pinned && pkg->version)
            printf(" (resolved: %s)", pkg->version);
        putchar('\n');

        /* Enqueue transitive deps discovered via requires_dist. */
        if (pkg->requires_dist) {
            for (char **rd = pkg->requires_dist; *rd; rd++) {
                char dep[256];
                parse_dep_name(*rd, dep, sizeof(dep));
                if (dep[0] && !package_list_contains_name(reqs, dep))
                    package_list_add(reqs, package_create(dep, NULL));
            }
        }

        if (dry_run) {
            printf("         file  : %s\n"
                   "         url   : %s\n"
                   "         sha256: %s\n\n",
                   pkg->filename, pkg->url, pkg->sha256);
        } else {
            char dest[4096];
            int nw = snprintf(dest, sizeof(dest), "%s/%s",
                              output_dir, pkg->filename);
            if (nw < 0 || nw >= (int)sizeof(dest)) {
                fprintf(stderr, "  packmule: destination path too long for %s\n",
                        pkg->filename);
                exit_code = EXIT_FAILURE;
                ++resolved;
                continue;
            }

            printf("         -> %s\n", dest);
            fflush(stdout);

            if (download_file(pkg->url, dest, 1) != 0) {
                fprintf(stderr, "  packmule: download failed for %s\n", pkg->name);
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

            printf("            sha256: OK\n\n");
            ++downloaded;
        }

        ++resolved;
    }

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
