/*
 * pypi_metadata.h — reading a distribution's dependency list.
 *
 * The PyPI JSON API hands over info.requires_dist directly.  The PEP 503
 * simple API does not carry dependency metadata at all, so on a private index
 * the same information has to come from one of two places:
 *
 *   1. PEP 658/714: the index advertises data-core-metadata on the anchor and
 *      serves the wheel's METADATA at "<file-url>.metadata".  One small GET.
 *   2. Otherwise: the artifact itself.  A wheel carries
 *      "<dist>-<ver>.dist-info/METADATA"; an sdist usually carries only
 *      PKG-INFO, which for anything built with a modern backend does not list
 *      run-time requirements.
 *
 * Both paths end in the same RFC 822-style header block, which is what
 * pypi_metadata_requires() parses.
 */

#ifndef PACKMULE_PYPI_METADATA_H
#define PACKMULE_PYPI_METADATA_H

/*
 * pypi_metadata_requires — collect every "Requires-Dist:" value from a
 * METADATA / PKG-INFO document.
 *
 * Values are returned in the same PEP 508 form the JSON API uses
 * ("idna<4,>=2.5", "colorama; sys_platform == \"win32\""), so the caller can
 * feed them to the existing dependency walk unchanged.  Folded continuation
 * lines are joined.
 *
 * Returns a NULL-terminated array (empty if the document declares no
 * dependencies), which the caller owns and frees with
 * pypi_metadata_free_specs().  Returns NULL only when `text` is NULL.
 */
char **pypi_metadata_requires(const char *text);

void pypi_metadata_free_specs(char **specs);

/*
 * pypi_metadata_has_requires_header — whether the document contains any
 * dependency-bearing header at all.
 *
 * An sdist's PKG-INFO with no Requires-Dist is ambiguous: it can mean "no
 * dependencies" or "this metadata was never generated".  Distinguishing the
 * two is what lets the backend warn about a bundle that may be incomplete
 * instead of silently shipping one.
 */
int pypi_metadata_has_requires_header(const char *text);

/*
 * pypi_metadata_license — the declared licence from a METADATA / PKG-INFO
 * document, for the SBOM.
 *
 * Prefers "License-Expression" (PEP 639, already an SPDX expression), then a
 * "License ::" Trove classifier, then the free-text "License" header — the
 * last only when it is short enough to be a name rather than the licence body
 * pasted in whole, which is common enough to guard against.
 *
 * Returns a heap string the caller frees with pm_free(), or NULL.
 */
char *pypi_metadata_license(const char *text);

/*
 * pypi_metadata_from_archive — extract the metadata document from a
 * distribution file on disk.
 *
 * Understands wheels (zip, a METADATA file inside the .dist-info directory)
 * and source distributions (tar.gz / tar.bz2 / tar.xz / zip, a PKG-INFO file,
 * shallowest match wins).
 *
 * Returns a heap string the caller frees with pm_free(), or NULL when the file
 * cannot be read or holds no metadata.  Errors are printed to stderr.
 */
char *pypi_metadata_from_archive(const char *path);

#endif /* PACKMULE_PYPI_METADATA_H */
