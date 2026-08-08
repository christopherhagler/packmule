/*
 * test_network.c — regression tests for the parallel download scheduler.
 *
 * These exercise the paths that fail *before* any network activity, so the
 * tests need no network and no server: start_transfer() opens the destination
 * file first, and an unopenable destination is a realistic failure (a full
 * disk or a read-only mount on the removable media a bundle is being written
 * to) that used to leave the scheduler with nothing to do and no way to
 * notice.
 *
 * A hang is the failure being guarded against, so this test is registered with
 * a ctest TIMEOUT: if download_many() ever fails to return, the suite fails
 * rather than running forever.
 */
#include "network.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* A directory that cannot exist, so fopen() of anything under it fails. */
#define BAD_DIR "/packmule-nonexistent-dir-for-tests"

static size_t g_done_calls;

static void count_done(const DownloadJob *job, size_t completed, size_t total)
{
    (void)job;
    assert(completed >= 1 && completed <= total);
    g_done_calls++;
}

/*
 * Every destination is unopenable and there are more jobs than slots, so no
 * slot can ever be armed.  The scheduler must retire every job and return,
 * rather than looping with nothing in flight and jobs still queued.
 */
static void test_all_destinations_unopenable(void)
{
    DownloadJob jobs[5];
    memset(jobs, 0, sizeof(jobs));
    for (size_t i = 0; i < 5; i++) {
        jobs[i].url       = "http://127.0.0.1:9/pkg";
        jobs[i].dest_path = BAD_DIR "/pkg";
        jobs[i].label     = "pkg";
    }

    g_done_calls = 0;
    int rc = download_many(jobs, 5, 2, count_done);

    assert(rc == -1);
    /* Every job must be accounted for: an unattempted job left at rc == 0
     * would be read as a successful download by the caller. */
    for (size_t i = 0; i < 5; i++)
        assert(jobs[i].rc == -1);
    /* And each one must have been reported exactly once. */
    assert(g_done_calls == 5);

    printf("  test_all_destinations_unopenable: ok\n");
}

/*
 * The same failure on a queue that fits entirely in the initial slots: this is
 * the case the loop condition alone happened to survive, so it guards against
 * a fix that only handles the re-arm path.
 */
static void test_unopenable_fits_in_slots(void)
{
    DownloadJob jobs[2];
    memset(jobs, 0, sizeof(jobs));
    for (size_t i = 0; i < 2; i++) {
        jobs[i].url       = "http://127.0.0.1:9/pkg";
        jobs[i].dest_path = BAD_DIR "/pkg";
        jobs[i].label     = "pkg";
    }

    g_done_calls = 0;
    int rc = download_many(jobs, 2, 4, count_done);

    assert(rc == -1);
    assert(jobs[0].rc == -1 && jobs[1].rc == -1);
    assert(g_done_calls == 2);

    printf("  test_unopenable_fits_in_slots: ok\n");
}

/* An empty queue is a no-op, not an error. */
static void test_empty_queue(void)
{
    assert(download_many(NULL, 0, 4, NULL) == 0);
    printf("  test_empty_queue: ok\n");
}

int main(void)
{
    assert(network_init() == 0);

    test_empty_queue();
    test_unopenable_fits_in_slots();
    test_all_destinations_unopenable();

    network_cleanup();
    printf("test_network: all tests passed\n");
    return 0;
}
