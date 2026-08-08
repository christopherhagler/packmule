/*
 * verify.c — bundle checksum verification and offline install checks.
 * See verify.h for what each of these is actually proving.
 */

#include "verify.h"
#include "hash.h"
#include "utils.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* ── SHA256SUMS ──────────────────────────────────────────────────────────── */

int bundle_verify_checksums(const char *dir)
{
    char sums_path[4096];
    snprintf(sums_path, sizeof(sums_path), "%s/SHA256SUMS", dir);

    FILE *fp = fopen(sums_path, "r");
    if (!fp) {
        fprintf(stderr,
                "packmule: cannot read %s\n"
                "          Without it there is nothing to verify the bundle "
                "against.\n", sums_path);
        return -1;
    }

    size_t checked = 0, bad = 0;
    char   line[4608];

    while (fgets(line, (int)sizeof(line), fp)) {
        char *s = pm_strtrim(line);
        if (!*s || *s == '#')
            continue;

        /* coreutils format: "<hex>  <name>" (two spaces, or " *" for binary). */
        char *sep = strchr(s, ' ');
        if (!sep) {
            fprintf(stderr, "packmule: malformed SHA256SUMS line: %s\n", s);
            bad++;
            continue;
        }
        *sep = '\0';
        char *name = sep + 1;
        while (*name == ' ' || *name == '*')
            name++;
        if (!*name) {
            fprintf(stderr, "packmule: malformed SHA256SUMS line: %s\n", s);
            bad++;
            continue;
        }

        /* The names come from a file that travelled with the bundle: refuse
         * any path component so a doctored SHA256SUMS cannot walk the
         * verifier out of the bundle directory. */
        if (strchr(name, '/')) {
            fprintf(stderr, "packmule: refusing path in SHA256SUMS: %s\n", name);
            bad++;
            continue;
        }

        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", dir, name);

        Digest d = {0};
        digest_set(&d, DIGEST_SHA256, DIGEST_ENC_HEX, s);
        int ok = digest_matches_file(path, &d);
        digest_clear(&d);

        checked++;
        if (!ok) {
            fprintf(stderr, "packmule: FAILED %s\n", name);
            bad++;
        }
    }
    fclose(fp);

    if (checked == 0 && bad == 0) {
        fprintf(stderr, "packmule: %s lists no files\n", sums_path);
        return -1;
    }

    if (bad) {
        fprintf(stderr,
                "packmule: %zu of %zu file(s) failed verification.\n"
                "          Do not install this bundle; transfer it again.\n",
                bad, checked);
        return -1;
    }

    printf("packmule: %zu file(s) verified against SHA256SUMS\n", checked);
    return 0;
}

/* ── Subprocess helpers ──────────────────────────────────────────────────── */

/*
 * run_capture — run `cmd` under /bin/sh and capture the first line of its
 * stdout into `out`.  Returns 0 when the command exited 0.
 */
static int run_capture(const char *cmd, char *out, size_t outsz)
{
    FILE *fp = popen(cmd, "r");
    if (!fp)
        return -1;
    out[0] = '\0';
    if (!fgets(out, (int)outsz, fp)) {
        pclose(fp);
        return -1;
    }
    return pclose(fp) == 0 ? 0 : -1;
}

/*
 * pip_supports_dry_run — pip gained `install --dry-run` in 22.2.  Knowing
 * this up front is what lets a failed check be a real failure: without the
 * version probe, "pip is too old" and "the bundle is broken" both surface as
 * a non-zero exit and the result has to be downgraded to a warning.
 */
static int pip_supports_dry_run(void)
{
    char buf[256];
    if (run_capture("python3 -m pip --version 2>/dev/null", buf, sizeof(buf)) != 0)
        return 0;

    /* "pip 24.0 from /usr/lib/python3/dist-packages/pip (python 3.12)" */
    const char *p = buf;
    while (*p && !isdigit((unsigned char)*p))
        p++;
    if (!*p)
        return 0;

    int major = atoi(p);
    while (isdigit((unsigned char)*p)) p++;
    int minor = (*p == '.') ? atoi(p + 1) : 0;

    return major > 22 || (major == 22 && minor >= 2);
}

/* detect_python_minor — the local CPython 3.x minor, or 0. */
static int detect_python_minor(void)
{
    char buf[16];
    if (run_capture("python3 -c 'import sys; print(sys.version_info[1])'"
                    " 2>/dev/null", buf, sizeof(buf)) != 0)
        return 0;
    int minor = atoi(buf);
    return minor > 0 ? minor : 0;
}

/* ── pypi ────────────────────────────────────────────────────────────────── */

BundleCheckResult bundle_check_pypi(const char *output_dir, const char *arch,
                                    const char *host_arch,
                                    const char *target_os, const char *host_os,
                                    int py_minor)
{
    int host_py = detect_python_minor();
    if (host_py <= 0) {
        printf("packmule: skipping offline install check (no local python3)\n");
        return BUNDLE_CHECK_SKIPPED;
    }
    if ((py_minor > 0 && py_minor != host_py) ||
        (target_os && (!host_os || strcmp(target_os, host_os) != 0)) ||
        (arch && strcmp(arch, host_arch) != 0)) {
        printf("packmule: skipping offline install check (bundle targets a "
               "different os/arch/python than this machine)\n");
        return BUNDLE_CHECK_SKIPPED;
    }
    if (!pip_supports_dry_run()) {
        printf("packmule: skipping offline install check "
               "(local pip is older than 22.2, which added --dry-run)\n");
        return BUNDLE_CHECK_SKIPPED;
    }

    printf("packmule: checking the bundle installs offline (pip --dry-run) ...\n");
    fflush(stdout);

    char *req   = pm_asprintf("%s/requirements.txt", output_dir);
    char *links = pm_asprintf("--find-links=%s", output_dir);

    BundleCheckResult result = BUNDLE_CHECK_FAILED;
    pid_t pid = fork();
    if (pid == 0) {
        /*
         * --ignore-installed is what makes this a check of the bundle rather
         * than of this machine.  pip satisfies a requirement from an already
         * installed distribution before it ever consults --find-links, so
         * without it every package that happens to be installed here is
         * reported "already satisfied" and never looked for in the bundle —
         * and building from inside the project's own virtualenv, which is the
         * obvious thing to do, makes the check pass on an empty directory.
         * The target machine has none of those packages, which is precisely
         * the situation this is supposed to be reproducing.
         *
         * It does not affect the build environment: --no-build-isolation
         * still lets an sdist's metadata be prepared with the setuptools
         * installed here.
         */
        execlp("python3", "python3", "-m", "pip", "install", "--dry-run",
               "--no-index", "--quiet", "--ignore-installed", links,
               "--no-build-isolation", "-r", req, (char *)NULL);
        _exit(127);
    } else if (pid > 0) {
        int st;
        if (waitpid(pid, &st, 0) == pid && WIFEXITED(st) &&
            WEXITSTATUS(st) == 0)
            result = BUNDLE_CHECK_PASSED;
    }
    pm_free(req);
    pm_free(links);

    return result;
}

/* ── npm ─────────────────────────────────────────────────────────────────── */

BundleCheckResult bundle_check_npm(const char *output_dir)
{
    char abs[PATH_MAX];
    if (!realpath(output_dir, abs) || strchr(abs, '\'')) {
        printf("packmule: skipping offline install check "
               "(cannot resolve output path)\n");
        return BUNDLE_CHECK_SKIPPED;
    }

    printf("packmule: checking the bundle installs offline (install.sh) ...\n");
    fflush(stdout);

    /*
     * Exit 127 is reserved for "a prerequisite is missing" so a machine
     * without node/npm reports SKIPPED rather than failing the build.
     */
    char *cmd = pm_asprintf(
        "command -v npm  >/dev/null 2>&1 || exit 127; "
        "command -v node >/dev/null 2>&1 || exit 127; "
        "d=$(mktemp -d) || exit 1; cd \"$d\" || exit 1; "
        "export npm_config_cache=\"$d/cache\" npm_config_ignore_scripts=true; "
        "if [ -f '%s/package-lock.json' ]; then "
            "node -e '"
                "const fs=require(\"fs\");"
                "const l=JSON.parse(fs.readFileSync(process.argv[1],\"utf8\"));"
                "const r=(l.packages||{})[\"\"]||{};"
                "fs.writeFileSync(\"package.json\",JSON.stringify({"
                    "name:r.name||\"packmule-verify\","
                    "version:r.version||\"0.0.0\","
                    "dependencies:r.dependencies||{},"
                    "devDependencies:r.devDependencies||{},"
                    "optionalDependencies:r.optionalDependencies||{},"
                    "peerDependencies:r.peerDependencies||{}}));"
            "' '%s/package-lock.json' || exit 1; "
        "else printf '{}' > package.json; fi; "
        "sh '%s/install.sh' >/dev/null; "
        "rc=$?; cd /; rm -rf \"$d\"; "
        "[ $rc -eq 127 ] && rc=1; exit $rc", abs, abs, abs);

    BundleCheckResult result = BUNDLE_CHECK_FAILED;
    pid_t pid = fork();
    if (pid == 0) {
        /* Unroutable registry: any resolution gap fails fast instead of
         * silently succeeding via the network. */
        setenv("npm_config_registry", "http://127.0.0.1:9/", 1);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    } else if (pid > 0) {
        int st;
        if (waitpid(pid, &st, 0) == pid && WIFEXITED(st)) {
            if (WEXITSTATUS(st) == 0)        result = BUNDLE_CHECK_PASSED;
            else if (WEXITSTATUS(st) == 127) result = BUNDLE_CHECK_SKIPPED;
        }
    }
    pm_free(cmd);

    if (result == BUNDLE_CHECK_SKIPPED)
        printf("packmule: skipping offline install check "
               "(node/npm not available)\n");
    return result;
}
