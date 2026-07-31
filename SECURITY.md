# Security Policy

## Reporting a vulnerability

Please report vulnerabilities **privately**, not as a public issue.

Use GitHub's private reporting: go to the repository's **Security** tab and
choose **Report a vulnerability**. That opens a draft advisory visible only to
you and the maintainers.

Please include what you are reporting, how to reproduce it, and what an
attacker gains. A proof-of-concept manifest or a captured registry response is
worth more than a description.

You should get an initial response within a week. If a fix is warranted, the
advisory will be published alongside it with credit, unless you prefer
otherwise.

## Supported versions

packmule is pre-1.0. Only the latest release receives fixes. There are no
backports.

## Threat model

packmule fetches metadata and packages from a registry over the network,
verifies them, and writes a bundle intended to be carried into an air-gapped
environment and installed there.

### What is untrusted

Everything that arrives over the network. Registry JSON, `repomd.xml`,
`primary.xml`, version strings, PEP 508 specifiers, wheel filenames, and
package names are all parsed by hand, in C, before anything has been verified.

**Memory-safety bugs in that parsing are the highest-severity findings in this
project, and they are in scope even when they require a hostile or compromised
mirror to trigger.** A crash, an out-of-bounds read, or anything worse reached
from a malformed registry response is a vulnerability, not a robustness issue.

### What packmule enforces

Verified against the code, not aspirational:

- **TLS** at libcurl's defaults for every request. There is no flag to disable
  certificate or hostname verification, and none will be added.
- **`http` and `https` only**, for the request itself and for anything a
  redirect points at, with redirects capped at 5. An index cannot redirect a
  download to `file://` or `scp://`.
- **Typed digest verification.** The algorithm comes from the metadata; there
  is no length-sniffing. **A file with no digest to check against is refused**
  — an absent digest is a hard failure, never a silent pass.
- **npm requires SHA-512 SRI.** `dist.integrity` must carry a `sha512-` value;
  packages offering only the legacy SHA-1 `dist.shasum` are rejected rather
  than accepted with a weaker hash.
- **RPM chain of trust.** `primary.xml` is verified against the digest
  published in `repomd.xml` before any package digest inside it is trusted.
- **Bundle integrity.** `SHA256SUMS` records every bundled file; `install.sh`
  and `packmule verify` check it. Entry names containing `/` are rejected.
- **Resource limits**, so a hostile response cannot exhaust memory or disk:
  256 MB per metadata response, 256 MB per npm manifest, 1 GB decompressed
  `primary.xml`, 8 GB per download; a 60 s metadata timeout, and an abort after
  30 s below 1 KB/s.

### What packmule does *not* do

Please read this before relying on it:

- **It trusts the registry.** There is no TUF (PEP 458/480), no npm signature
  verification, and no check of package *provenance*. Digests confirm you
  received what the index advertised — not that the index is honest. A
  compromised registry, or a successful account takeover of a package you
  depend on, is not something packmule detects.
- **It does not verify RPM GPG signatures.** `install_rpm.sh` deliberately
  leaves dnf's `gpgcheck` setting alone so the target's own policy applies.
  **Leave `gpgcheck` enabled on the target.**
- **`SHA256SUMS` is unsigned.** It gives you integrity, not authenticity:
  it proves the bundle was not corrupted, not that it came from you. Anyone who
  can modify the bundle can regenerate it. Sign the `.tar.gz` out of band and
  verify that signature before installing.
- **It does not inspect package contents.** A package that is malicious but
  correctly published will be bundled and installed faithfully. packmule is a
  transport, not a scanner.
- **Installation runs with the operator's privileges.** `install.sh` executes
  pip, npm, or dnf on the target. Read it before running it as root.
- **Local input is trusted.** Manifests, lockfiles, and existing bundle
  directories are treated as coming from the operator.

## Recommended practice

For an air-gapped deployment:

1. Bundle from a **private mirror** you control rather than the public index.
2. **Sign the resulting `.tar.gz`** and verify the signature on the target.
   This is the gap `SHA256SUMS` does not close.
3. Run `packmule verify <dir>` after extracting, before installing.
4. Keep **`gpgcheck` enabled** for RPM installs.
5. Read `install.sh` before running it with elevated privileges.
