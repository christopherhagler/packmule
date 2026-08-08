/*
 * test_verify.c — regression test for the pypi offline install check.
 *
 * The check exists to answer one question: can this bundle install on a
 * machine that has none of these packages?  pip satisfies a requirement from
 * an already-installed distribution before it consults --find-links, so
 * without --ignore-installed the check answers a different and useless
 * question — can it install *here* — and passes on a bundle with no wheels in
 * it at all.
 *
 * The test builds exactly that bundle: a requirements.txt naming a
 * distribution that is certainly installed (pip itself, since the check needs
 * pip to run) and a directory containing no wheels.  bundle_check_pypi() must
 * not report PASSED.
 */
#include "verify.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *DIR = "test_verify_bundle_tmp";

static void cleanup(void)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/requirements.txt", DIR);
    remove(path);
    rmdir(DIR);
}

int main(void)
{
    cleanup();
    assert(mkdir(DIR, 0755) == 0);

    char path[512];
    snprintf(path, sizeof(path), "%s/requirements.txt", DIR);
    FILE *fp = fopen(path, "w");
    assert(fp != NULL);
    /* Installed on any machine that can run this check at all — which is what
     * makes it the exact shape of the bug. */
    fputs("pip\n", fp);
    fclose(fp);

    /*
     * NULL arch/target_os and py_minor 0 mean "no target preference", so the
     * host-matches-target gate lets the check actually run.  A machine with no
     * python3, or with pip older than 22.2, reports SKIPPED — that is a
     * different answer from PASSED and the assertion below still holds.
     */
    BundleCheckResult r = bundle_check_pypi(DIR, NULL, "x86_64", NULL, NULL, 0);

    if (r == BUNDLE_CHECK_SKIPPED) {
        printf("test_verify: skipped (no usable python3/pip on this machine)\n");
        cleanup();
        return 0;
    }

    /* The bundle contains no distributions whatsoever; claiming it installs
     * offline is the bug. */
    assert(r == BUNDLE_CHECK_FAILED);

    cleanup();
    printf("test_verify: all tests passed\n");
    return 0;
}
