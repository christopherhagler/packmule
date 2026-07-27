/*
 * rpm_repo.h — an indexed view over a decompressed primary.xml.
 *
 * The backend used to answer every question by scanning the whole document
 * with strstr.  That is fine for a handful of explicitly named packages, but
 * transitive resolution asks thousands of questions ("who provides
 * libssl.so.3()(64bit)?"), and primary.xml for a large repository is a few
 * hundred megabytes.  So the document is indexed once: package blocks are
 * located, and every capability they provide is hashed to the blocks that
 * provide it.
 *
 * Nothing is copied out of the XML.  Names are (pointer, length) slices into
 * the caller's buffer, which must outlive the RpmRepo.
 */

#ifndef PACKMULE_RPM_REPO_H
#define PACKMULE_RPM_REPO_H

#include <stddef.h>

#include "hash.h"

/* A capability reference: "name [flags epoch:ver-rel]". */
typedef struct {
    const char *name;
    size_t      name_len;
    char        flags[4];   /* "EQ", "GE", "GT", "LE", "LT", or "" */
    int         epoch;
    char       *ver;        /* heap, or NULL */
    char       *rel;        /* heap, or NULL */
} RpmCap;

/* One <package> block, as located in the document. */
typedef struct {
    const char *start;      /* at "<package " */
    const char *end;        /* at "</package>" */
} RpmBlock;

typedef struct RpmRepo RpmRepo;

/*
 * rpm_repo_index — build an index over `primary_xml`.
 *
 * `primary_xml` must remain valid and unmodified for the lifetime of the
 * returned repo.  Returns NULL on allocation failure or an unparseable
 * document.  Free with rpm_repo_free().
 */
RpmRepo *rpm_repo_index(const char *primary_xml);

void rpm_repo_free(RpmRepo *repo);

/* Number of <package> blocks found. */
size_t rpm_repo_count(const RpmRepo *repo);

/* The i-th block. */
const RpmBlock *rpm_repo_block(const RpmRepo *repo, size_t i);

/*
 * rpm_repo_find_by_name — indices of every block whose <name> is `name`.
 *
 * Writes up to `max` indices into `out` and returns how many were written,
 * or -1 if there are none.
 */
int rpm_repo_find_by_name(const RpmRepo *repo, const char *name,
                          size_t *out, int max);

/*
 * rpm_repo_find_providers — indices of every block providing capability
 * `cap` (a NUL-terminated capability name, including file paths).
 *
 * Writes up to `max` indices into `out` and returns how many were written.
 */
int rpm_repo_find_providers(const RpmRepo *repo, const char *cap,
                            size_t *out, int max);

/*
 * rpm_block_requires — parse the <rpm:requires> entries of a block.
 *
 * Returns a heap array of `*n_out` capabilities (free with
 * rpm_caps_free()).  rpmlib()/config() pseudo-capabilities and rich/boolean
 * dependencies are omitted: the first two are satisfied by rpm itself, and
 * the third cannot be evaluated without a real depsolver.  `*n_rich_out`
 * receives the number of rich dependencies skipped, so the caller can say so.
 */
RpmCap *rpm_block_requires(const RpmBlock *b, size_t *n_out,
                           size_t *n_rich_out);

/* Free an array returned by rpm_block_requires(), including the array. */
void rpm_caps_free(RpmCap *caps, size_t n);

/*
 * rpm_cap_clear — release only the heap members of a single RpmCap, leaving
 * the struct itself alone.  This is the one to use for a stack-allocated cap
 * such as the output of rpm_block_provides_matching(); rpm_caps_free() would
 * additionally free the struct and abort on a stack address.
 */
void rpm_cap_clear(RpmCap *c);

/* ── Field accessors (each returns a heap copy, or NULL) ─────────────────── */

char *rpm_block_name(const RpmBlock *b);
char *rpm_block_arch(const RpmBlock *b);
char *rpm_block_href(const RpmBlock *b);

/* Version triple; `epoch` defaults to 0 when the attribute is absent. */
int rpm_block_evr(const RpmBlock *b, int *epoch, char **ver, char **rel);

/* Whichever checksum the repository publishes.  Returns 0 on success. */
int rpm_block_digest(const RpmBlock *b, Digest *out);

/*
 * rpm_cap_satisfied_by — does the provides-entry `prov` satisfy the
 * requires-entry `req`?  An unversioned requirement is satisfied by any
 * provider; otherwise the comparison follows RPM's EVR ordering.
 */
int rpm_cap_satisfied_by(const RpmCap *req, const RpmCap *prov);

/*
 * rpm_block_provides_matching — find the block's provides entry for
 * capability `name` and write it to `out`.  Returns 1 when found.
 * `out->ver`/`out->rel` are heap-allocated; release with rpm_cap_clear(out).
 */
int rpm_block_provides_matching(const RpmBlock *b, const char *name,
                                RpmCap *out);

#endif /* PACKMULE_RPM_REPO_H */
