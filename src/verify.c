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
#include <strings.h>          /* strcasecmp — not pulled in by string.h here */
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

/*
 * run_quiet — run `cmd` under /bin/sh, discarding stdout, and return its exit
 * status (or -1 if it could not be run or died on a signal).
 */
static int run_quiet(const char *cmd)
{
    int st = system(cmd);
    if (st == -1 || !WIFEXITED(st))
        return -1;
    return WEXITSTATUS(st);
}

/* ── Report ──────────────────────────────────────────────────────────────── */

void bundle_check_report_clear(BundleCheckReport *rep)
{
    if (!rep)
        return;
    pm_free(rep->method);
    pm_free(rep->reason);
    rep->method = rep->reason = NULL;
}

static void report_set(BundleCheckReport *rep, BundleCheckResult result,
                       char *method, char *reason)
{
    if (!rep) {
        pm_free(method);
        pm_free(reason);
        return;
    }
    pm_free(rep->method);
    pm_free(rep->reason);
    rep->result = result;
    rep->method = method;
    rep->reason = reason;
}

/* ── Containerised checking ──────────────────────────────────────────────── */

/*
 * container_engine — podman or docker, whichever is on PATH.
 *
 * podman first: it is the one that tends to be present on the RHEL-family
 * build hosts this tool is aimed at, and it needs no daemon.
 */
static const char *container_engine(void)
{
    static const char *cached;
    static int         probed;

    if (!probed) {
        char buf[PATH_MAX];
        probed = 1;

        /*
         * An explicit off switch, for environments where reaching a registry
         * for an image is not allowed even though an engine is installed.
         * Distribution builds set it in %check: a package build must never
         * touch the network, and relying on the buildroot merely not having
         * podman in it is a weaker guarantee than saying so.
         */
        const char *off = getenv("PACKMULE_NO_CONTAINER_VERIFY");
        if (off && *off && strcmp(off, "0") != 0)
            return NULL;

        if (run_capture("command -v podman 2>/dev/null", buf, sizeof(buf)) == 0)
            cached = "podman";
        else if (run_capture("command -v docker 2>/dev/null", buf, sizeof(buf)) == 0)
            cached = "docker";
    }
    return cached;
}

/*
 * container_platform — the OCI platform string for a target architecture.
 *
 * Only the two architectures with real manylinux wheel coverage are mapped.
 * Anything else returns NULL and the check is skipped rather than run against
 * a platform that is not the target.
 */
static const char *container_platform(const char *arch)
{
    if (!arch)
        return NULL;
    if (strcasecmp(arch, "aarch64") == 0 || strcasecmp(arch, "arm64") == 0)
        return "linux/arm64";
    if (strcasecmp(arch, "x86_64") == 0 || strcasecmp(arch, "amd64") == 0)
        return "linux/amd64";
    return NULL;
}

/*
 * verify_image — the image the check runs in.
 *
 * Fully qualified because podman refuses to guess a registry.  Overridable so
 * a site with no docker.io access can point at its own mirror — the same
 * escape-hatch approach as PACKMULE_CA_BUNDLE and friends.
 */
static char *verify_image(int py_minor)
{
    const char *env = getenv("PACKMULE_VERIFY_IMAGE");
    if (env && *env)
        return pm_strdup(env);
    if (py_minor > 0)
        return pm_asprintf("docker.io/library/python:3.%d-slim", py_minor);
    return pm_strdup("docker.io/library/python:3-slim");
}

/*
 * check_pypi_container — run the same pip check inside a container built for
 * the target platform.
 *
 * The distinction that makes a failure trustworthy is drawn by the probe: the
 * image is pulled and a trivial command run in it first, so "this machine
 * cannot run linux/arm64 containers" is SKIPPED, and only a container that
 * demonstrably works can return FAILED.  Without that, a build host lacking
 * binfmt emulation would report every cross-architecture bundle as broken.
 */
static BundleCheckResult check_pypi_container(const char *output_dir,
                                              const char *arch,
                                              const char *target_os,
                                              int py_minor,
                                              const char *host_blocker,
                                              BundleCheckReport *rep)
{
    if (target_os && strcmp(target_os, "linux") != 0) {
        report_set(rep, BUNDLE_CHECK_SKIPPED, NULL,
                   pm_asprintf("%s, and a container can only check a linux "
                               "target (this bundle targets %s)",
                               host_blocker, target_os));
        return BUNDLE_CHECK_SKIPPED;
    }

    const char *engine = container_engine();
    if (!engine) {
        report_set(rep, BUNDLE_CHECK_SKIPPED, NULL,
                   pm_asprintf("%s, and neither podman nor docker is on PATH "
                               "to check it in a container", host_blocker));
        return BUNDLE_CHECK_SKIPPED;
    }

    const char *platform = container_platform(arch);
    if (!platform) {
        report_set(rep, BUNDLE_CHECK_SKIPPED, NULL,
                   pm_asprintf("%s, and target architecture '%s' has no "
                               "container platform to check it on",
                               host_blocker, arch ? arch : "any"));
        return BUNDLE_CHECK_SKIPPED;
    }

    /* The bundle is mounted by absolute path; a quote in it would break out of
     * the shell quoting below. */
    char abs[PATH_MAX];
    if (!realpath(output_dir, abs) || strchr(abs, '\'')) {
        report_set(rep, BUNDLE_CHECK_SKIPPED, NULL,
                   pm_asprintf("%s, and the bundle path cannot be passed to a "
                               "container safely", host_blocker));
        return BUNDLE_CHECK_SKIPPED;
    }

    char             *image  = verify_image(py_minor);
    BundleCheckResult result = BUNDLE_CHECK_SKIPPED;

    printf("packmule: fetching %s for the check (first run only) ...\n", image);
    fflush(stdout);

    char *pull = pm_asprintf("%s pull --platform %s '%s' >/dev/null 2>&1",
                             engine, platform, image);
    int   rc   = run_quiet(pull);
    pm_free(pull);

    if (rc != 0) {
        report_set(rep, BUNDLE_CHECK_SKIPPED, NULL,
                   pm_asprintf("%s, and image %s could not be fetched for a "
                               "container check (set PACKMULE_VERIFY_IMAGE to "
                               "a reachable mirror)", host_blocker, image));
        pm_free(image);
        return BUNDLE_CHECK_SKIPPED;
    }

    /* Can this machine actually execute that platform at all? */
    char *probe = pm_asprintf(
        "%s run --rm --platform %s --network none '%s' "
        "python3 -c 'pass' >/dev/null 2>&1", engine, platform, image);
    rc = run_quiet(probe);
    pm_free(probe);

    if (rc != 0) {
        report_set(rep, BUNDLE_CHECK_SKIPPED, NULL,
                   pm_asprintf("%s, and this machine cannot run %s containers "
                               "(binfmt/qemu emulation may be missing)",
                               host_blocker, platform));
        pm_free(image);
        return BUNDLE_CHECK_SKIPPED;
    }

    printf("packmule: checking the bundle installs offline "
           "(%s, %s, %s) ...\n", engine, image, platform);
    fflush(stdout);

    /*
     * The container runs the bundle's own install.sh, not a pip command of our
     * own devising — the same thing the npm check does, and for the same
     * reason: what has to work on the target is that script, so approximating
     * it here can only test something else.  It matters concretely.  install.sh
     * installs the bundled setuptools and wheel before anything that needs
     * building, and a hand-written `pip install --no-build-isolation` does not:
     * against a slim image, which ships no setuptools, every bundle containing
     * an sdist would fail a check it should pass.
     *
     * The host check stays a --dry-run because it runs on the user's machine
     * and must not install anything there.  A throwaway container has no such
     * constraint, which is what makes it the better rehearsal.
     *
     * --network none is what makes this a real air-gap rehearsal rather than a
     * hopeful one: pip cannot reach an index even if --no-index were wrong.
     *
     * PACKMULE_SKIP_VERIFY is set because SHA256SUMS does not exist yet — it
     * is written after this check, so that it can cover the verdict.  What
     * that file proves (the bytes survived the transfer) is not what is being
     * asked here anyway, and `packmule verify` covers it at the destination.
     */
    char *run = pm_asprintf(
        "%s run --rm --platform %s --network none -e PACKMULE_SKIP_VERIFY=1 "
        "-v '%s':/bundle:ro '%s' sh /bundle/install.sh",
        engine, platform, abs, image);
    rc = run_quiet(run);
    pm_free(run);

    result = (rc == 0) ? BUNDLE_CHECK_PASSED : BUNDLE_CHECK_FAILED;
    report_set(rep, result,
               pm_asprintf("container (%s, %s, %s)", engine, image, platform),
               NULL);
    pm_free(image);
    return result;
}

/* ── pypi ────────────────────────────────────────────────────────────────── */

/*
 * host_check_blocker — why this machine cannot answer for the bundle's target,
 * or NULL when it can.  The returned string is heap-owned.
 */
static char *host_check_blocker(const char *arch, const char *host_arch,
                                const char *target_os, const char *host_os,
                                int py_minor)
{
    if (detect_python_minor() <= 0)
        return pm_strdup("this machine has no python3");

    int host_py = detect_python_minor();

    if (py_minor > 0 && py_minor != host_py)
        return pm_asprintf("this machine runs python 3.%d and the bundle "
                           "targets 3.%d", host_py, py_minor);

    if (target_os && (!host_os || strcmp(target_os, host_os) != 0))
        return pm_asprintf("this machine runs %s and the bundle targets %s",
                           host_os ? host_os : "an unknown OS", target_os);

    if (arch && host_arch && strcmp(arch, host_arch) != 0)
        return pm_asprintf("this machine is %s and the bundle targets %s",
                           host_arch, arch);

    if (!pip_supports_dry_run())
        return pm_strdup("the local pip predates --dry-run (22.2)");

    return NULL;
}

/* run_host_check — the pip dry-run, on this machine. */
static BundleCheckResult run_host_check(const char *output_dir)
{
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

BundleCheckResult bundle_check_pypi(const char *output_dir, const char *arch,
                                    const char *host_arch,
                                    const char *target_os, const char *host_os,
                                    int py_minor, BundleCheckReport *rep)
{
    char *blocker = host_check_blocker(arch, host_arch, target_os, host_os,
                                       py_minor);
    if (!blocker) {
        BundleCheckResult r = run_host_check(output_dir);
        report_set(rep, r, pm_strdup("host"), NULL);
        return r;
    }

    /*
     * The host cannot answer for this target — which is the normal case for an
     * air-gapped build, not an edge case — so ask a container that can.
     */
    printf("packmule: %s\n", blocker);

    BundleCheckResult r = check_pypi_container(output_dir, arch, target_os,
                                               py_minor, blocker, rep);
    pm_free(blocker);
    return r;
}

/* ── npm ─────────────────────────────────────────────────────────────────── */

BundleCheckResult bundle_check_npm(const char *output_dir,
                                   BundleCheckReport *rep)
{
    char abs[PATH_MAX];
    if (!realpath(output_dir, abs) || strchr(abs, '\'')) {
        printf("packmule: skipping offline install check "
               "(cannot resolve output path)\n");
        report_set(rep, BUNDLE_CHECK_SKIPPED, NULL,
                   pm_strdup("the bundle path cannot be passed to a shell "
                             "safely"));
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
        /* SHA256SUMS is written after this check runs, so that it can cover
         * the verdict; there is nothing for install.sh to verify against yet,
         * and the bytes have not been anywhere to need it. */
        setenv("PACKMULE_SKIP_VERIFY", "1", 1);
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

    if (result == BUNDLE_CHECK_SKIPPED) {
        printf("packmule: skipping offline install check "
               "(node/npm not available)\n");
        report_set(rep, result, NULL,
                   pm_strdup("node and npm are needed to check an npm bundle "
                             "and are not on PATH"));
    } else {
        report_set(rep, result, pm_strdup("host"), NULL);
    }
    return result;
}
