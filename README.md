# packmule

[![CI](https://github.com/christopherhagler/packmule/actions/workflows/ci.yml/badge.svg)](https://github.com/christopherhagler/packmule/actions/workflows/ci.yml)
[![RPM packaging](https://github.com/christopherhagler/packmule/actions/workflows/rpm.yml/badge.svg)](https://github.com/christopherhagler/packmule/actions/workflows/rpm.yml)

An air-gapped package bundler written in C.

packmule turns a package manifest into a **sealed, verifiable transfer
artifact**: one `.tar.gz` holding every file needed to install with no network,
a hash manifest, an SBOM, and an install script — proven to install offline
before it ever leaves the build host.

Supported registries: **PyPI**, **npm**, **RPM**. All three resolve transitive
dependencies. Every file is checked against its registry-published
SHA-256/SHA-512 at download time, the whole bundle against `SHA256SUMS` on
arrival, and the closure itself is test-installed offline at build time.

---

## Why not `pip download`, `dnf download`, or an npm cache?

If you just need a directory of wheels on your own laptop, use `pip download`.
It is the reference implementation, it tracks PEP changes for free, and it is
one line:

```bash
pip download -r requirements.txt --platform manylinux_2_17_x86_64 \
  --python-version 3.12 --only-binary=:all: -d ./wheels
```

packmule is for the case where the download is not the deliverable — where the
files have to cross a boundary and someone has to account for what crossed it.
Four things it gives you that the per-ecosystem tools do not:

**One artifact, one procedure, three ecosystems.** `pip download`,
`dnf download --resolve`, and npm's assorted offline workarounds produce three
different shapes of output with three different install incantations. packmule
produces the same bundle layout, the same `manifest.json`, the same
`install.sh` contract and the same `packmule verify` for all of them. That is
worth little to one developer and quite a lot to an organisation with a written
transfer procedure and a review step.

**An inventory that survives the trip.** `manifest.json` records name,
version, filename, upstream digest and computed SHA-256 for every package;
`SHA256SUMS` covers every file; `--sbom` emits CycloneDX 1.5 and/or SPDX 2.3
with a real dependency graph and per-package licences. The native tools hand
you a directory and no provenance record. When the question is "what exactly
is in this transfer, and did it arrive intact," a directory is not an answer.

**Proof that it installs offline, before it ships.** This is the one that
earns the tool. After bundling, packmule installs the bundle *for real* with
the network taken away — pip with `--dry-run --no-index --ignore-installed`
against only the bundled files, npm by running the generated `install.sh` in a
scratch project against an unroutable registry with an **empty** cache. Both
halves of that are load-bearing. `--ignore-installed` is pip's equivalent of
the empty cache: without it pip satisfies a requirement from an already
installed distribution before it ever looks at the bundle, so building from
inside the project's own virtualenv — the obvious thing to do — makes the
check pass on a directory with no wheels in it at all. The empty cache is the
whole point: a warm npm cache on the build host will happily "verify" a bundle
that is missing tarballs, and you find out three weeks later behind the wire,
where it cannot be fixed. A failed check is a build failure, not a warning.

**Private registries without a config file.** Credentials come from the
environment only (never written to disk, never read from `.npmrc`/`.netrc`),
are scoped to the index host so a token cannot leak to a CDN on redirect, and
cover custom auth headers, private CAs and mTLS — with one mechanism shared by
all three backends. See [Private registries](#private-registries).

### What you give up

packmule reimplements PEP 440, PEP 503, PEP 508 markers and wheel-tag matching
in C rather than shelling out to pip, and its resolver intersects constraints
and takes the highest satisfying version — it does **not** backtrack. A
requirement set that needs backtracking will resolve differently from pip, or
fail outright. That is a deliberate trade for a dependency-free static binary,
and it is exactly why the post-bundle check runs the real package manager
against the finished bundle and treats failure as fatal: pip and npm are the
oracle, packmule is the transport.

The way out of that trade is to not resolve at all. Point packmule at a
lockfile — `uv.lock` or `pylock.toml` for Python, `package-lock.json` for
Node — and it ships exactly what a real resolver already decided, hashes and
all. If your project has a lock, use it; this caveat then does not apply.

If you are not crossing an air gap, you do not need this. Use the native tool.

---

## Quick start

```
$ packmule -f requirements.txt -o ./vendor -a x86_64
packmule: backend   : pypi
packmule: arch      : x86_64
packmule: manifest  : requirements.txt (3 package(s))
packmule: output dir: ./vendor

  numpy-2.1.0-cp310-cp310-manyl  [#############-------------]  52%   16.8 MB/s
  ✓ [ 1/53] requests-2.31.0-py3-none-any.whl  (62.7 KB)
  ✓ [ 2/53] numpy-2.1.0-cp310-cp310-manylinux_2_17_x86_64.whl  (13.1 MB)
  ...
  ✓ [53/53] pycparser-2.22-py3-none-any.whl  (117.6 KB)

packmule: 53/53 package(s) downloaded to ./vendor (84.2 MB)
```

While a file is downloading on a terminal, packmule draws a single live
progress bar showing percent complete and transfer speed. The bar redraws
in place on one line — it never scrolls — and is replaced by a permanent
`✓` line recording the filename and on-disk size once the file is verified.
Failures leave a permanent `✗` line with the reason. The closing summary
reports how many packages were downloaded and the total size on disk.

When stdout is **not** a terminal (a pipe, file, or CI log), the live bar and
colours are suppressed automatically: only the plain permanent `✓`/`✗` lines
are emitted, so logs stay clean. Dry-run mode (`-n`) prints all resolved
packages in full without a progress bar.

The counter grows as transitive dependencies are discovered — the total shown
in brackets increases as each package's dependencies are queued. Each file is
verified against its registry-published digest before being kept; mismatches
are removed and reported on standard error.

---

## Requirements

| Dependency | Version | Purpose |
|---|---|---|
| libcurl | ≥ 7.61 | Registry API queries and file downloads |
| OpenSSL | ≥ 1.1 | SHA-256 and SHA-512 file verification |
| libarchive | ≥ 3.3.3 | RPM repository metadata decompression; bundle `.tar.gz` creation |
| cJSON | ≥ 1.7 | JSON registry response parsing |
| CMake | ≥ 3.15 | Build system |
| C compiler | C11 | GCC ≥ 7, Clang ≥ 6, Apple Clang |

These floors are enforced by CMake, so a too-old dependency fails at configure
time rather than at link time. The libcurl and libarchive floors are both set by
RHEL/CentOS 8 — a supported build target, which ships 7.61 and 3.3.3. Newer
versions are used where they help, but nothing requires them.

cJSON must be installed as a system package; the build does **not** fetch it
from the internet.

Optional at runtime: `podman` or `docker`, used only to run the post-bundle
offline install check on the target's platform when this machine cannot. It is
never needed to build packmule or to build a bundle — without it, a
cross-platform bundle is still produced and reported as unverified.

---

## Building

### Ubuntu / Debian

```bash
sudo apt-get install cmake gcc libcurl4-openssl-dev libssl-dev \
                     libarchive-dev libcjson-dev
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

### Fedora / RHEL

```bash
sudo dnf install cmake gcc libcurl-devel openssl-devel \
                 libarchive-devel cjson-devel
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

### macOS (Homebrew)

libcurl, libarchive, and OpenSSL are keg-only on Homebrew and not symlinked
into `/opt/homebrew`, so their prefixes must be passed to CMake explicitly.

```bash
brew install cmake curl openssl@3 libarchive cjson

cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$(brew --prefix curl);$(brew --prefix openssl@3);$(brew --prefix libarchive);/opt/homebrew"

cmake --build build --parallel
```

---

## Usage

```
packmule -f <manifest> [-o <dir>] [-t <type>] [-a <arch>] [-s <os>] [-p <ver>] [-u <url>] [-j <n>] [-b] [-n]
packmule verify <bundle-dir>
packmule -V | --version
packmule -h | --help
```

| Flag | Description |
|---|---|
| `-f <file>`, `--manifest` | Path to the package manifest (**required**). For `pypi` this may be a `requirements.txt`, a lockfile (`uv.lock`, `pylock.toml`), or a `pyproject.toml` with one beside it |
| `-o <dir>` | Output directory (default: `.`); created if absent |
| `-t <type>`, `--type` | Registry backend: `pypi`, `npm`, `rpm`. Auto-detected from the manifest filename when omitted (`requirements*.txt` / `uv.lock` / `pylock.toml` / `pyproject.toml` → `pypi`, `package.json` / `package-lock.json` / `npm-shrinkwrap.json` → `npm`, `packages.txt` → `rpm`); falls back to `pypi` for unrecognised names |
| `-a <arch>`, `--arch` | Target CPU architecture (default: current machine). Use `any` for universal/source only |
| `-s <os>`, `--os` | *(pypi only)* Target OS for wheel selection: `linux`, `macos`, `windows`, or `any` (default: the host OS) |
| `-p <ver>`, `--python` | *(pypi only)* Target CPython version for wheel selection and environment markers, e.g. `3.12` (default: the local `python3`) |
| `-u <url>`, `--repo-url` | Repository base URL — required for `rpm`; optional for `pypi`/`npm` (overrides public endpoint) |
| `--index <mode>` | *(pypi only)* Which index API to speak: `auto` (default), `simple`, or `json`. `auto` uses the PEP 503 simple API whenever `-u` is set, and pypi.org's JSON API otherwise |
| `-b`, `--bundle` | Write `manifest.json`, `SHA256SUMS` and `install.sh`, then compress output to `<dir>.tar.gz` |
| `-n`, `--dry-run` | Resolve and print what would be downloaded — no files written |
| `-j <n>`, `--jobs` | Parallel downloads, 1–16 (default 4). `-j 1` restores the per-file progress bar |
| `--no-verify` | Skip the post-bundle offline install check |
| `--sbom <fmt>` | Also emit an SBOM: `cyclonedx`, `spdx`, or `both` |
| `--rpm-deps <mode>` | *(rpm only)* `resolve` (default) follows transitive dependencies; `none` bundles only what the manifest names |
| `-V`, `--version` | Print version and exit |
| `-h`, `--help` | Print usage and exit |

| Command | Description |
|---|---|
| `verify <dir>` | Re-check an extracted bundle against its `SHA256SUMS` |

### Architecture selection

packmule detects the current machine's architecture via `uname(2)` and uses it
as the default target. Override with `-a`:

```bash
packmule -f requirements.txt -o ./vendor -a x86_64    # Linux x86-64
packmule -f requirements.txt -o ./vendor -a aarch64   # Linux ARM64
packmule -f requirements.txt -o ./vendor -a any       # universal/source only
```

`aarch64` and `arm64` are treated as equivalent (same CPU, different OS naming
convention). Package selection priority (highest first):

1. Architecture-specific package matching the `-a` target
2. Universal package (e.g. pure-Python `none-any` wheel, npm tarball)
3. Source archive

### Dry run

Use `-n` to audit what would be fetched before committing to a download:

```
$ packmule -n -f requirements.txt -a x86_64
packmule: backend   : pypi
packmule: arch      : x86_64
packmule: manifest  : requirements.txt (3 package(s))
packmule: mode      : DRY RUN -- resolve only, no files written

  [1/3] requests==2.31.0
         file  : requests-2.31.0-py3-none-any.whl
         url   : https://files.pythonhosted.org/packages/.../requests-2.31.0-py3-none-any.whl
         sha256: 58cd2187c01e70e6e26505bca751777aa9f2ee0b7f4300988b709f44e013003f

  [2/8] click==8.1.7
  ...

packmule: dry run complete -- 8/8 package(s) resolved, 0 downloaded
```

### Re-running after a failure

Re-runs are idempotent: before downloading, packmule checks whether the file
already exists in the output directory with a matching digest. Verified files
are skipped and marked `(cached)` in the output; missing, truncated, or
stale files (hash mismatch) are re-downloaded and overwritten. A run that
failed partway can simply be repeated — only the gap is fetched.

```
  ✓ [ 1/53] requests-2.31.0-py3-none-any.whl  (62.7 KB, cached)
  ✓ [ 2/53] numpy-2.1.0-cp310-cp310-manylinux_2_17_x86_64.whl  (13.1 MB)
```

### Bundling for transport

Add `--bundle` to compress the output directory into a single `.tar.gz` that
can be carried to the air-gapped machine in one step:

```
$ packmule -f requirements.txt -o ./vendor -a x86_64 --bundle
packmule: backend   : pypi
...
packmule: 10/10 package(s) downloaded to ./vendor (8.4 MB)
packmule: writing manifest.json ...
packmule: writing requirements.txt ...
packmule: writing install.sh ...
packmule: creating ./vendor.tar.gz ...
packmule: bundle ready: ./vendor.tar.gz
```

(`writing requirements.txt ...` appears for the `pypi` backend only.)

`vendor.tar.gz` extracts to a directory containing:

| File | Description |
|---|---|
| `*.whl` / `*.tgz` / `*.rpm` | Downloaded package files |
| `manifest.json` | Name, version, filename, upstream digest, and SHA-256 for every package |
| `SHA256SUMS` | `sha256sum`-format digests of every file in the bundle |
| `install.sh` | Pre-configured install script for the active backend |
| `requirements.txt` | *(pypi only)* pip-compatible requirements file used by `install.sh` |
| `package-lock.json` | *(npm lockfile bundles only)* the tree `npm ci` replays |
| `uv.lock` / `pylock.toml` | *(pypi lockfile bundles only)* the lock the bundle was built from, as provenance — `install.sh` does not need it |

On the air-gapped machine:

```bash
tar -xzf vendor.tar.gz
./vendor/install.sh
```

`install.sh` locates the bundle from its own path, so it can be invoked from
anywhere. For `pypi` and `rpm` that is the whole story: the working directory
is irrelevant, and `cd vendor && ./install.sh` works just as well.

**For `npm` the working directory is part of the input — run `install.sh` from
your project root**, the directory holding `package.json`:

```bash
cd /path/to/project
tar -xzf vendor.tar.gz      # → ./vendor
./vendor/install.sh         # not: cd vendor && ./install.sh
```

Running it from inside the bundle directory stops with

```
install.sh: run this from your project directory (no package.json here)
```

before anything is installed or modified. See
[why npm is different](#why-the-npm-install-runs-from-the-project-root).

`install.sh` verifies `SHA256SUMS` before installing anything and refuses to
continue if a file does not match. The digests packmule checked at download
time were about the registry; this one is about the trip here, which is the
only link in the chain nothing else validates. It needs nothing on the target
beyond `sha256sum` (or `shasum`).

You can also run the check on its own, without installing:

```bash
packmule verify ./vendor
```

Set `PACKMULE_SKIP_VERIFY=1` to bypass it — only reasonable on a target with
no coreutils at all.

Install scripts by backend:

| Backend | Command |
|---|---|
| `pypi` | `python3 -m pip install --no-index --find-links="$DIR" --no-build-isolation -r "$DIR/requirements.txt"` (installs bundled setuptools/wheel first when the bundle contains an sdist) |
| `npm` | **run from the project root.** Lockfile bundle: `npm ci --offline --omit=dev` replaying the bundled `package-lock.json` against the bundled tarballs; flat bundle: one `npm install --offline --omit=dev --no-save` over every `.tgz` (devDependencies hidden from npm for the duration and the manifest restored afterwards) |
| `rpm` | `dnf install -y --disablerepo='*' *.rpm`, falling back to `rpm -Uvh` |

Bundling is skipped if any package failed to download. The output directory
is always left intact alongside the archive.

After a bundle is created, packmule verifies it end-to-end so a bundle that
cannot install offline fails **here**, not on the air-gapped machine:

- **pypi, on this machine** — `pip install --dry-run --no-index
  --ignore-installed` against only the bundled files, used when this machine
  matches the bundle's target os/arch/python and pip is ≥ 22.2.
  `--ignore-installed` is what keeps this a check of the bundle rather than of
  the build machine's site-packages. It stays a dry run because it runs on
  your machine and must not install anything there.
- **pypi, in a container** — when this machine *cannot* answer for the target,
  which is the usual case when building for an air-gapped host, packmule runs
  the bundle's own `install.sh` inside `python:3.<minor>-slim` on the target
  platform with `--network none`. A throwaway container can do the real
  install, so this is the stronger check of the two: it exercises the exact
  script the destination will run, including the bundled setuptools/wheel step
  that an sdist needs. Requires `podman` or `docker` on `PATH`.
- **npm** — runs the generated `install.sh` in a scratch project with the
  registry pointed at an unroutable address and an empty npm cache, with
  scripts disabled. No container is needed: npm resolves by name and version
  rather than by platform, and a lockfile bundle already carries every
  platform's optional dependencies.

A failed check is a **build failure**: the package manager itself has said it
cannot install this closure offline, and shipping it anyway just moves the
failure somewhere it cannot be fixed. Use `--no-verify` to build without the
check.

When nothing can be checked — no container engine, an architecture with no
container platform, a non-linux target, no local pip/npm — packmule prints an
explicit **UNVERIFIED** warning rather than a quiet skip, and records the
verdict in `manifest.json`, so a bundle that was never checked cannot be
mistaken for one that passed:

```json
"install_check": {
  "result": "passed",
  "method": "container (podman, docker.io/library/python:3.12-slim, linux/arm64)"
}
```

`result` is `passed`, `failed`, or `unverified`; an `unverified` entry carries
a `reason`. The field is written before `SHA256SUMS`, so it is covered by it.

| Variable | Meaning |
|---|---|
| `PACKMULE_VERIFY_IMAGE` | Image to run the check in, for sites with no `docker.io` access (default `docker.io/library/python:3.<minor>-slim`) |
| `PACKMULE_NO_CONTAINER_VERIFY` | Set to `1` to never use a container, even when an engine is available |

### Private registries

All three backends accept a custom base URL via `-u`. Use this to point
packmule at a JFrog Artifactory, Sonatype Nexus, devpi or GitLab instance
instead of the public endpoints:

```bash
# Artifactory PyPI repository (note: the /simple endpoint)
packmule -f requirements.txt -o ./vendor \
  -u https://artifactory.example.com/artifactory/api/pypi/pypi-virtual/simple

# Nexus npm proxy
packmule -f package.json -o ./vendor -t npm \
  -u https://nexus.example.com/repository/npm-proxy

# Any DNF/YUM-compatible RPM repository
packmule -f packages.txt -o ./vendor -t rpm -a x86_64 \
  -u https://repo.example.com/fedora/40/x86_64/os
```

#### Authentication

Credentials are read **from the environment only** — never from the command
line, where they would be visible in `ps` output to every user on the machine
and recorded in shell history. They apply to all three backends.

| Variable | Effect |
|---|---|
| `PACKMULE_USERNAME` + `PACKMULE_PASSWORD` | HTTP Basic |
| `PACKMULE_USERNAME` + `PACKMULE_TOKEN` | HTTP Basic — the "username + API key / identity token" form |
| `PACKMULE_TOKEN` alone | `Authorization: Bearer <token>` |
| `PACKMULE_AUTH_HEADER` | A literal header line, for any scheme not covered above |
| `PACKMULE_AUTH_HOSTS` | Extra hosts to authenticate to, comma-separated (see below) |
| `PACKMULE_AUTH_INSECURE=1` | Permit credentials over plain `http://` |

`PACKMULE_AUTH_HEADER` is the escape hatch that keeps packmule working against
a registry it has never seen. If your product wants something other than Basic
or Bearer, spell the header out:

```bash
export PACKMULE_AUTH_HEADER='X-JFrog-Art-Api: <api-key>'   # older Artifactory
export PACKMULE_AUTH_HEADER='PRIVATE-TOKEN: <token>'       # GitLab
export PACKMULE_AUTH_HEADER='X-Api-Key: <key>'             # others
```

It is host-scoped like every other scheme. That matters more here than
elsewhere: libcurl only strips `Authorization` across a cross-host redirect, so
a custom header is protected by packmule's own scoping and nothing else.

```bash
export PACKMULE_TOKEN="$ARTIFACTORY_TOKEN"
packmule -f requirements.txt -o ./vendor -b \
  -u https://artifactory.example.com/artifactory/api/pypi/pypi-virtual/simple
```

**Credentials are scoped to the host named by `-u`, and to nothing else.** An
index decides the download URLs it hands back, so a repository that returns a
file URL on another host — or an attacker who can make it do so — would
otherwise be handed your token. Requests to any other host are sent
unauthenticated. If your index legitimately serves artifacts from a separate
CDN or storage hostname, name it explicitly:

```bash
export PACKMULE_AUTH_HOSTS="artifactory-cdn.example.com"
```

A 401 or 403 is reported with the reason it happened — no credentials
configured, credentials scoped to a different host, or credentials the server
rejected — because the fix differs in each case.

Two things are refused outright rather than handled quietly, because both leak
secrets: credentials embedded in the `-u` URL (`https://user:pass@host/…`,
which would be echoed into progress and error output), and credentials bound
for a plain-`http://` endpoint unless `PACKMULE_AUTH_INSECURE=1` says the link
is trusted. Credentials set with no `-u` at all are an error too — packmule
will not silently fall back to building your bundle from the public registry.

Nothing secret is written into a bundle: `manifest.json` records filenames,
versions and digests, and the generated `install.sh` installs from the bundled
files with the index disabled.

#### Corporate TLS and proxies

Internal registries almost always present a certificate from a private CA, and
a TLS-terminating proxy re-signs everything else too. Name the trust anchor —
there is deliberately no option to disable certificate verification:

| Variable | Effect |
|---|---|
| `PACKMULE_CA_BUNDLE` | CA bundle file, or a `c_rehash`-style directory, to verify servers against |
| `PACKMULE_CLIENT_CERT` / `_KEY` / `_KEY_PASSWORD` | Client certificate for mutual TLS |

`CURL_CA_BUNDLE` and `SSL_CERT_FILE` are honoured as fallbacks, so a machine
already configured for a corporate CA needs no packmule-specific setup. A
mistyped path fails at startup rather than as a confusing handshake error on
the first request.

Proxies need no configuration: packmule never sets libcurl's proxy options, so
the standard `http_proxy`, `https_proxy` and `no_proxy` variables (including a
`user:pass@` in them) work as they do for `curl`.

#### Known index URL shapes

packmule takes the index URL verbatim — it never guesses a vendor's layout, so
any product works as long as you point `-u` at the right endpoint. Some common
ones, for reference:

| Product | `-u` value |
|---|---|
| JFrog Artifactory (PyPI) | `https://<host>/artifactory/api/pypi/<repo>/simple` |
| JFrog Artifactory (npm) | `https://<host>/artifactory/api/npm/<repo>/` |
| JFrog Artifactory (RPM) | `https://<host>/artifactory/<repo>` |
| Sonatype Nexus (PyPI) | `https://<host>/repository/<repo>/simple` |
| Sonatype Nexus (npm) | `https://<host>/repository/<repo>/` |
| Sonatype Nexus (RPM/yum) | `https://<host>/repository/<repo>` |
| devpi | `https://<host>/<user>/<index>/+simple/` |
| GitLab (PyPI) | `https://<host>/api/v4/projects/<id>/packages/pypi/simple` |
| Azure Artifacts (PyPI) | `https://pkgs.dev.azure.com/<org>/_packaging/<feed>/pypi/simple` |
| Plain directory / mirror | whatever URL serves the index |

These are a starting point, not a supported list — if a product is not here,
pass its index URL and packmule will treat it the same way. If resolution
fails with a 404 on the index page, the URL is the first thing to check
(for PyPI, it usually needs to end in the index's `simple` endpoint).

#### PyPI index formats

packmule speaks two PyPI index APIs, and `--index auto` (the default) picks
the better one for the endpoint: the JSON API when talking to pypi.org, the
[PEP 503](https://peps.python.org/pep-0503/) *simple* API whenever `-u` is
given. Override with `--index simple` or `--index json` when your index is an
exception — a private index that does implement the JSON API, or pypi.org
itself when you want to exercise the simple path.

Neither is a legacy of the other. The JSON API (`/pypi/<name>/json`) is a
pypi.org extension that private indexes generally do not implement, so a
private index needs the simple API. Against pypi.org, though, JSON is the
better source: it answers in one request per package instead of two, and it
reports dependencies for **source distributions**, which the simple API cannot
(see below).

The simple API carries no dependency metadata, so packmule recovers it in one
of two ways:

1. **[PEP 658](https://peps.python.org/pep-0658/)/[714](https://peps.python.org/pep-0714/)**
   — if the index advertises `data-core-metadata`, the `METADATA` file is
   fetched on its own. One small request per package; this is the fast path and
   current Artifactory versions support it.
2. **Otherwise** — the wheel itself is downloaded during resolution and its
   `METADATA` read out of the archive. Those downloads are reused by the
   download phase rather than repeated, so nothing is fetched twice.

If neither yields metadata, packmule warns that dependencies were not followed
and the bundle may be incomplete — it does not silently ship one.

**Source distributions are the weak spot of the simple API.** pypi.org
advertises PEP 658 metadata for wheels but not for sdists, and most private
indexes advertise none at all, so an sdist falls through to route 2 — and an
sdist's `PKG-INFO` frequently declares no dependencies even when the package
has them. If a manifest resolves to sdists on a private index, check the
warnings and pin the missing transitive dependencies explicitly. Against
pypi.org this does not arise, because `--index auto` uses the JSON API, which
reports `requires_dist` for sdists too.

### SBOM output

A bundle is often the last point at which anyone can see what is about to
enter an air-gapped network, so it is the natural place to record an
inventory. `--sbom` emits one from the resolved package set — no extra network
requests, since resolution already established every name, exact version,
digest, source URL and dependency edge.

```bash
packmule -f requirements.txt -o ./vendor --bundle --sbom both
```

| Value | Output file | Format |
|---|---|---|
| `cyclonedx` | `sbom.cdx.json` | CycloneDX 1.5 JSON |
| `spdx` | `sbom.spdx.json` | SPDX 2.3 JSON |
| `both` | both of the above | — |

Both are generated from the same data, so asking for both costs only the
second file. CycloneDX is what vulnerability tooling ingests (Dependency-Track,
Grype, Trivy); SPDX is what licence-compliance and procurement processes
usually ask for.

Each component carries:

- a **package URL (purl)** — `pkg:pypi/requests@2.31.0`,
  `pkg:npm/%40babel/core@7.24.0`,
  `pkg:rpm/docker-ce@29.6.1-1.el9?arch=x86_64&epoch=3`. This is the identifier
  scanners match on; PyPI names are PEP 503-normalised and RPM epochs become a
  qualifier, both as the purl spec requires.
- the **SHA-256 of the file as it sits in the bundle** — the same hash
  `SHA256SUMS` and `packmule verify` check.
- the **source URL** it was fetched from, and the filename (CycloneDX
  `properties`; SPDX `downloadLocation` / `packageFileName`).
- the **declared licence**, where the registry publishes one.
- **dependency edges** (CycloneDX `dependencies`, SPDX `DEPENDS_ON`
  relationships) for pypi and npm, so the document is a graph rather than a
  flat list. RPM is excluded on purpose: its dependencies are capabilities
  (`libc.so.6`, `/bin/sh`) that do not map one-to-one onto package names, and
  guessing would produce edges that are wrong rather than merely absent.

With `--bundle`, the SBOM is written before `SHA256SUMS`, so it is covered by
it and travels inside the `.tar.gz` — an inventory that could be swapped out
undetected would not be worth much. Without `--bundle`, it is written into the
output directory alongside the packages.

Both documents are validated in development against the official CycloneDX
1.5 JSON schema and the SPDX reference validator (`spdx-tools`).

**On licences.** Registries publish free text — `BSD-3-Clause`, but also
`MIT License` and `Apache 2.0`. CycloneDX records whatever was published
verbatim (as a licence `name`, not an `id`, since an `id` must come from the
SPDX list). SPDX's `licenseDeclared` takes a licence *expression* and a
validator rejects anything else, so a value that is not plausibly one becomes
`NOASSERTION` rather than producing an invalid document. `licenseConcluded` is
always `NOASSERTION`: concluding a licence would mean packmule had audited the
source, which it has not.

`SOURCE_DATE_EPOCH` is honoured for the document timestamp. The document
identifier (CycloneDX `serialNumber`, SPDX `documentNamespace`) is a fresh
UUID per run, as both specifications require.

### Exit codes

| Code | Meaning |
|---|---|
| `0` | Everything resolved, downloaded, verified — and, with `--bundle`, proven to install offline |
| `1` | Something failed: an unsatisfiable requirement, a download or digest failure, or a bundle the package manager rejected |

Resolution failures are reported before anything is downloaded, so a manifest
that cannot be satisfied costs nothing but the metadata requests. Download
failures leave the packages that did succeed on disk, so a re-run resumes
rather than starting over.

---

## Backends

### pypi — Python Package Index

**Lockfile mode (preferred).** When the manifest is a `uv.lock` or a PEP 751
`pylock.toml` — or one sits next to the `pyproject.toml` you point at —
packmule ships exactly what the lock records and resolves nothing:

```sh
packmule -f uv.lock       -o ./vendor -a x86_64 -s linux -p 3.12
packmule -f pylock.toml   -o ./vendor
packmule -f pyproject.toml -o ./vendor   # uses the lock beside it
```

A lock is the output of a real backtracking resolver, with the URL and hash of
every artifact already decided. That removes the one caveat this backend
otherwise carries (see [What you give up](#what-you-give-up)): there is no
resolution left for packmule to do differently from pip, and no metadata to
fetch, so a lock-mode bundle needs only the downloads themselves.

The two formats differ in how they represent platforms, and packmule handles
each on its own terms:

- **`uv.lock`** locks for *every* platform at once and carries a dependency
  graph whose edges hold PEP 508 markers. The bundle is what is reachable from
  the workspace root once those markers are evaluated against your target, so a
  Windows-only package is pruned from a Linux bundle rather than shipped
  uselessly — or, worse, failing the build because it has no Linux wheel.
  Extras are followed through the graph: a root edge of `uvicorn[standard]`
  pulls in that extra's dependencies and nothing else. `dev-dependencies` are
  excluded, as `--omit=dev` excludes them for npm.
- **`pylock.toml`** is already flattened, so each package carries its own
  marker and selection is a filter rather than a walk.

Wheel selection is the same as everywhere else in this backend: among the
artifacts the lock records for a package, the best one for your `--arch`,
`--os` and `--python` wins. One lock therefore produces a correct bundle for
any target it covers.

The lock is copied into the bundle and covered by `SHA256SUMS`, as the record
of what was resolved. Nothing at the destination needs `uv`: `install.sh`
installs from the generated `requirements.txt` with `--no-index`, so plain
`pip` is enough.

These are all hard failures, never warnings, because each one produces a
bundle that fails on the far side of the wire:

| Condition | Why |
|---|---|
| A `git`, `directory`, `path` or `url` source | There is no artifact to carry across an air gap |
| A package with no hash in the lock | packmule never keeps a file it cannot verify |
| No artifact installable on the target, and no sdist | The bundle would be missing a package |
| One name locked at two versions that both apply | pip can install only one; guessing would ship the wrong one silently |
| A dependency the lock does not contain | The lock is inconsistent — regenerate it |

`poetry.lock` is **not** read. It records filenames and hashes but no URLs, so
resolving it needs a per-package index lookup — a different operation from
reading a self-contained lock. If your project uses Poetry, export a
`pylock.toml` (PEP 751 is the interchange format precisely so tools need not
read each other's native locks) and point packmule at that.

**Requirements files.** Without a lock, packmule reads a `requirements.txt`.
Supported syntax:

```
requests                   # unpinned — resolved to latest
requests==2.31.0           # exact pin
numpy>=1.20,<2.0           # PEP 440 range — highest satisfying release
uvicorn[standard]==0.29.0  # extras followed: their gated deps are bundled
-r more-requirements.txt   # includes are followed (relative to this file)
# comments and blank lines are ignored
```

`-c` constraint lines are skipped with a warning. Direct-URL requirements
(`pkg @ https://…`) and `-e` editable installs cannot be bundled and abort
the run rather than producing a silently incomplete bundle.

Transitive dependencies are resolved automatically by following
`requires_dist` from each package's registry metadata, honouring PEP 440
version ranges (`>=`, `<`, `~=`, `!=`, `==x.*`). Packages are deduplicated by
name (case-insensitive, with `-`, `_`, and `.` treated as equivalent per
PEP 503), and ranges from multiple dependents are intersected.

Resolution runs to a fixed point rather than in a single pass: when a
requirement discovered later narrows what an already-resolved package has to
satisfy, that package is resolved again with the fuller picture. The result
does not depend on the order entries appear in your manifest, and a
requirement that genuinely cannot be satisfied fails the build instead of
producing a bundle that will not install.

Extras-gated dependencies (e.g. `; extra == 'security'`) are followed only
when the parent was requested with that extra, and are never bundled
otherwise. OS and Python-version environment markers (`sys_platform`,
`python_version`, …) are evaluated against the target platform; markers that
cannot be fully evaluated keep the dependency rather than risk dropping one.

Wheel selection priority: architecture-specific wheel → universal wheel
(`none-any`) → sdist. Among matching manylinux wheels the lowest glibc floor
is preferred so the wheel installs on older targets. When only an sdist
exists, packmule warns and bundles `setuptools` + `wheel` alongside it so the
target machine can build it offline.

Integrity: SHA-256 hex digest, from the JSON API (`urls[].digests.sha256`) or,
on a simple index, from the `#sha256=` fragment on the file's link. An index
that publishes no digest for a file is a hard failure — packmule will not
bundle a file it cannot verify.

### npm — Node.js packages

**Lockfile mode (preferred).** When the manifest is a `package-lock.json` /
`npm-shrinkwrap.json` (lockfileVersion ≥ 2) — or a valid one sits next to
the given `package.json` — packmule downloads exactly the tarballs npm
itself resolved, including **multiple versions of one package** at
different nesting positions (something a flat per-name bundle cannot
represent). The lock ships inside the bundle, and `install.sh` replays the
exact tree with `npm ci --offline`, pointing every entry at its bundled
file and restoring the project's own lockfile afterwards.

**Range mode (no lockfile).** The `dependencies`, `optionalDependencies`,
and non-optional `peerDependencies` objects of `package.json` are walked
(npm 7+ installs peers by default, so they must be in the bundle;
`devDependencies` stay out — `install.sh` runs npm with `--omit=dev`).
Exact versions (`4.18.2`) are queried directly; range specifiers
(`^4.18.2`, `~1.2.3`, `>=1.0.0`, `1.x`, `a || b`) resolve to the highest
published version satisfying the range; dist-tags (`next`, `beta`) resolve
through the packument's `dist-tags`. Aliases (`"dep": "npm:real-pkg@^1.0"`)
resolve to the aliased package. When two dependents constrain the same
still-queued package, their ranges intersect; if the tree turns out to need
two versions of one package at once, the build fails with a pointer to
lockfile mode instead of shipping a bundle that cannot install.

In both modes, `git:`/`github:`/`file:`/`link:`/URL dependencies are a hard
build-time error — they have no registry tarball, and a bundle silently
missing them would only fail later, on the air-gapped machine.

#### Why the npm install runs from the project root

The npm bundle is the one case where the working directory at install time
matters. `pypi` and `rpm` are told everything explicitly — pip is handed
`--find-links="$DIR"` and `-r "$DIR/requirements.txt"` and installs into
whichever interpreter's environment is active; dnf is handed absolute `.rpm`
paths and installs into the system. Neither reads anything from the current
directory, so unpacking the bundle and running `install.sh` from inside it is
fine.

npm has no equivalent knob, because npm does not install *into a directory you
name* — it installs into a **project**. It finds that project by walking up
from the working directory to the closest `package.json`, reads the dependency
tree from that file and the `package-lock.json` beside it, and writes
`node_modules/` there. The bundle can supply the tarballs; it cannot supply
the project.

That matters more than it looks, because the lockfile install is not a single
npm command. `install.sh` has to:

1. back up the project's own `package-lock.json`,
2. write a rewritten copy in its place — same tree, but every `resolved` URL
   repointed at a bundled `.tgz` — under a trap that covers `INT`/`TERM`/`HUP`
   as well as `EXIT`,
3. run `npm ci --offline --omit=dev`,
4. restore the original lock, so later online npm use is unaffected.

Steps 1, 2 and 4 are ordinary file operations on the *project's* lockfile,
which is to say they are relative to the working directory. Step 3 is npm
finding the project by its own upward walk. Both have to land on the same
directory or the install is incoherent.

Run from inside the bundle directory, they don't. The bundle also contains a
`package-lock.json` (its copy of yours), so the backup and the rewrite land on
the *bundle's* file, while npm walks up past the bundle to your real project
and reads your original lock — still full of `https://registry.npmjs.org/`
URLs, none of them cached, with the network gone. You would get an obscure npm
error about uncached requests, having modified a file that `SHA256SUMS`
covers.

So the lockfile path checks for a `package.json` in the working directory
before it touches anything, and stops with a message if there is none. That
check runs ahead of the backup and the rewrite, so a run from the wrong
directory changes nothing — not your lockfile, not the bundle's.

The flat (no-lockfile) bundle wants the same working directory for the same
reason — `npm install --offline --no-save` installs into whatever project npm
walks up to, and the script temporarily strips `devDependencies` from the
project's `package.json` so npm does not try to resolve them from a registry
that isn't there — but it carries no such guard. Run from the bundle
directory it will appear to succeed and leave a `node_modules/` in the bundle
instead of in your project.

After `--bundle`, packmule verifies the result by running the generated
`install.sh` in a scratch project with the npm registry pointed at an
unroutable address and an **empty npm cache**, so a missing dependency
fails the build here rather than on the target.

Transitive dependencies are resolved from each version document's
dependency objects. Scoped packages (`@scope/name`) are supported; their
tarballs are prefixed with the scope (`babel-core-7.24.0.tgz`) to avoid
basename collisions.

Integrity: SHA-512 SRI from `dist.integrity` (`sha512-<base64>`). Packages
published before ~2017 that lack `dist.integrity` are refused rather than
silently falling back to the weaker SHA-1 `dist.shasum`.

```bash
packmule -f package-lock.json -o ./vendor -t npm   # exact tree (preferred)
packmule -f package.json      -o ./vendor -t npm   # uses sibling lock if present
```

### rpm — RPM repositories (DNF/YUM format)

Reads a plain text file with one package name per line, optionally with a
version suffix separated by `-` (split on last `-`):

```
bash
vim-9.0.0
python3-devel-3.11.0
# comments and blank lines are ignored
```

Requires a repository base URL via `-u`:

```bash
packmule -f packages.txt -o ./rpms -t rpm -a x86_64 \
  -u https://dl.fedoraproject.org/pub/fedora/linux/releases/40/Everything/x86_64/os
```

Resolution pipeline: fetch `repodata/repomd.xml` → locate `primary.xml.gz` →
download and decompress with libarchive → scan XML for the package by name and
arch (also matches `noarch` packages regardless of `-a`).

Integrity: the digest published in the primary database's `<checksum>`
element, in whichever algorithm the repository uses (sha1, sha256 or sha512).
`primary.xml` is itself verified against the checksum recorded in
`repomd.xml` before any package digest is read from it — otherwise every
package digest would come from an unauthenticated document.

Transitive dependencies are resolved from `<rpm:requires>` / `<rpm:provides>`
in the primary database, including capabilities named by file path
(`/bin/sh`) and by soname (`libssl.so.3()(64bit)`). Version flags on a
requirement (`>=`, `=`, …) are honoured against the provider's EVR, and a
package whose own name is the requested capability is preferred over one that
merely provides it.

```bash
packmule -f packages.txt -t rpm -u <repo> --rpm-deps resolve   # default
packmule -f packages.txt -t rpm -u <repo> --rpm-deps none      # manifest only
```

> **Scope:** `--rpm-deps resolve` bundles the full closure *within the
> repository you point it at*, which includes base-system packages the target
> almost certainly already has (glibc, bash, systemd). That is the safe
> default: `dnf` skips whatever is already installed, a redundant package only
> costs disk, and a missing one cannot be fixed on an air-gapped machine.
> Capabilities no package in the repository provides are reported as warnings
> rather than errors, since the target usually already satisfies them. Boolean
> and rich dependencies (`(a or b)`) are counted and reported but not
> evaluated — packmule is not a SAT solver.

---

## Running tests

```bash
ctest --test-dir build --output-on-failure
```

The unit suites are fully offline — no network access required:

| Suite | Coverage |
|---|---|
| `test_package` | Package lifecycle, PackageList grow, per-registry name equality, constraint/extras merging and dirty marking |
| `test_registry` | Registry dispatch table, name lookup, vtable integrity, `get_deps` and `name_equal` slots |
| `test_resolve` | Fixpoint resolution: late constraints, order independence, termination, pinned packages, failure handling |
| `test_rpm_repo` | primary.xml indexing: capability and file provides, requires filtering, EVR flag satisfaction |
| `test_semver` | node-semver ranges: `^`, `~`, comparators, x-ranges, hyphen ranges, `\|\|`, prereleases |
| `test_pep440` | PEP 440 ordering (dev/pre/post/epoch) and specifiers (`==`, `!=`, `<=`, `>=`, `~=`, `==x.*`) |
| `test_pep508` | Dependency-spec parsing (name/extras/pin/constraint) and environment-marker evaluation |
| `test_wheeltag` | Wheel filename classification, platform/arch/python tag matching, manylinux glibc floors |
| `test_registry_pypi` | requirements.txt parsing (includes, extras, constraints) and `get_deps`; wheel selection via `pypi_parse_response` |
| `test_registry_npm` | npm manifest/lockfile parsing and `get_deps`: aliases, peers, range intersection, dedup |
| `test_toml` | TOML reading: scalars, escapes, multi-line strings, arrays, inline tables, dotted keys, array-of-tables attachment, and the malformed inputs that must be refused |
| `test_pylock` | uv.lock / pylock.toml: marker-aware reachability and extras, per-target wheel selection, resolution-marker disambiguation, and every lock that must fail the build |
| `test_registry_rpm` | RPM manifest parsing: name-only, name-version, hyphenated names; EVR pins and selection |
| `test_hash` | Digest typing, sha1/sha256/sha512 hex and SHA-512 SRI, unset-digest refusal |
| `test_bundle` | Bundle creation for all three backends; skips packages missing from disk |
| `test_auth` | Credential scheme selection and host scoping: default-port equivalence, userinfo and IPv6 authorities, out-of-scope hosts, and every misconfiguration that must be fatal |
| `test_simple_index` | PEP 503 page parsing (hash fragments, yanking, PEP 658 attributes, entity decoding), RFC 3986 URL resolution, and version extraction from wheel/sdist filenames |
| `test_sbom` | purl construction per registry (PEP 503 normalisation, npm scopes, RPM epoch/arch qualifiers, encoding) and `--sbom` value parsing |
| `test_pypi_metadata` | `Requires-Dist` extraction: header/description boundary, CRLF, folded continuations, and reading METADATA out of a wheel or sdist |

### End-to-end tests

Network-dependent full-pipeline tests live in `tests/e2e/` and are opt-in:

```bash
cmake -B build -DPACKMULE_E2E_TESTS=ON
ctest --test-dir build -L e2e --output-on-failure
```

| Test | What it proves |
|---|---|
| `e2e_pypi` | two full cycles — requirements (range constraint, transitive deps, both SBOM formats) and lockfile (a generated `uv.lock` reproduced exactly, its win32-only package pruned, the lock shipped and checksummed) — each → bundle → extract → **offline install into a fresh venv** (`--no-index`) → import check |
| `e2e_npm` | two full cycles — flat (ranges, scoped package, devDeps excluded) and lockfile (a tree needing two `debug` versions) — bundle → extract → install with the registry pointed at an **unroutable address** → `require()` and nesting checks |
| `e2e_rpm` | repomd.xml → primary.xml decompress → package match → sha256/version extraction against a live DNF repo (dry run) |

A missing prerequisite (`python3`, `npm`) skips the test rather than failing it,
so the suite is safe in minimal CI images.

---

## Installing

Every tagged release carries prebuilt packages — a `.deb` and `.rpm`s for
Fedora and EPEL 8/9/10, each rebuilt in a clean `mock` chroot from its own
SRPM — plus a `SHA256SUMS` covering them:

```bash
# from https://github.com/christopherhagler/packmule/releases
sudo dnf install ./packmule-<version>-1.<dist>.x86_64.rpm
sudo dpkg -i  ./packmule_<version>_amd64.deb
```

Or install what you built:

```bash
cmake --install build --prefix /usr/local
# binary   → /usr/local/bin/packmule
# man page → /usr/local/share/man/man1/packmule.1.gz
```

### Building a .deb or .rpm

Needs the matching host tooling: `dpkg-deb` for `-G DEB`, `rpmbuild` for
`-G RPM`. Neither is present on macOS by default, where `cpack` reports
`Could not create CPack generator`.

```bash
cd build
cpack -G DEB   # → packmule_0.3.1_<arch>.deb
cpack -G RPM   # → packmule-0.3.1-<arch>.rpm
cpack          # all three: DEB, RPM, TGZ
```

For Fedora/EPEL, `packmule.spec` is the canonical packaging. The
[RPM packaging workflow](.github/workflows/rpm.yml) builds the SRPM and rebuilds
it in a clean `mock` chroot — the same mechanism Fedora's Koji uses — across
Fedora Rawhide and EPEL 8/9/10, then runs `rpmlint`. To reproduce locally on a
Fedora host:

```bash
version=$(rpmspec -q --qf '%{version}\n' packmule.spec | head -1)
git archive --format=tar.gz --prefix="packmule-${version}/" \
  -o ~/rpmbuild/SOURCES/packmule-${version}.tar.gz HEAD
rpmbuild -bs packmule.spec
mock --rebuild ~/rpmbuild/SRPMS/packmule-${version}-*.src.rpm
```

---

## Architecture

```
src/
  main.c              CLI argument parsing; the resolve → download → bundle
                      pipeline
  resolve.c           Fixpoint dependency resolution: re-resolves any package
                      whose requirements changed, until nothing changes
  verify.c            SHA256SUMS verification and the post-bundle offline
                      install checks
  network.c           libcurl wrappers — fetch_json() with connection reuse,
                      download_file(), download_many() (curl_multi); the single
                      point where credentials are attached to a request
  auth.c              Credentials from the environment, scoped to the
                      --repo-url host; no libcurl dependency, so the scoping
                      rules are unit-testable
  hash.c              Typed digests (algorithm + encoding) over OpenSSL EVP
  registry.c          Registry dispatch table and name lookup
  registry_pypi.c     PyPI backend — requirements.txt parser, distribution
                      selection, constraint-aware resolver, get_deps; speaks
                      either the JSON API or the PEP 503 simple API
  simple_index.c      PEP 503 index page parsing: anchors, digest fragments,
                      PEP 592 yanking, PEP 658 metadata attributes, and RFC
                      3986 URL resolution
  pylock.c            uv.lock and PEP 751 pylock.toml readers — marker-aware
                      reachability over uv's graph, artifact selection, and
                      the refusals that keep a lock from becoming an
                      incomplete bundle
  toml.c              A small TOML reader, sized for lockfiles.  Hand-rolled
                      for the same reason as the PEP parsers: no TOML library
                      is packaged widely enough to become a hard dependency
  pypi_metadata.c     Requires-Dist extraction from a METADATA document or
                      from inside a wheel/sdist (libarchive)
  registry_npm.c      npm backend — package.json / package-lock.json parser,
                      tarball resolution
  registry_rpm.c      RPM backend — packages.txt parser, repomd/primary.xml
                      resolve, capability-based depsolving
  rpm_repo.c          Indexed view of primary.xml: package blocks and a
                      capability → provider hash index
  semver.c            node-semver comparison and range matching (npm)
  pep440.c            PEP 440 version ordering and specifier matching (pypi)
  pep508.c            PEP 508 dependency-spec parsing and environment markers (pypi)
  wheeltag.c          Wheel/sdist filename classification and platform-tag
                      matching (pypi)
  package.c           Package and PackageList data structures
  bundle.c            manifest.json + requirements.txt + install.sh + .tar.gz;
                      install.sh is an embedded scripts/install_<name>.sh
  sbom.c              CycloneDX 1.5 and SPDX 2.3 output: purl construction per
                      registry, file hashes, licences, dependency edges
  utils.c             Abort-on-OOM allocators (pm_malloc, pm_free, …) and string
                      helpers (pm_strtrim, pm_asprintf, pm_human_size)
include/
  network.h
  auth.h              AuthScheme, credential lookup, host-scope query
  hash.h              Digest type, algorithms, and file verification
  registry.h          Registry vtable (name, manifest_name, name_equal,
                      parse_manifest, resolve, get_deps, ctx, repo_url)
  simple_index.h      SimpleFile / SimpleFileList and the PEP 503 parser
  sbom.h              SbomFormat flags, sbom_write(), sbom_purl()
  pypi_metadata.h     METADATA parsing and archive extraction
  registry_internal.h Backend internals exposed to the test suite only
  resolve.h / verify.h
  rpm_repo.h          primary.xml index API
  semver.h / pep440.h / pep508.h / wheeltag.h
  package.h           Package / PackageList types and state machine
  bundle.h            BundleOptions struct and bundle_create()
  version.h           PACKMULE_VERSION constant
  utils.h             Allocator wrappers and string helpers
scripts/
  install_pypi.sh     Per-backend offline install scripts (real, shellcheck-able);
  install_npm.sh        embedded into the binary at build time as byte arrays
  install_rpm.sh        (build/generated/bundle_scripts.h) and written verbatim
  verify_bundle.sh    Shared SHA256SUMS check, spliced into each install.sh
                      at its #__PACKMULE_VERIFY__ marker
                        into the bundle by bundle.c
cmake/
  embed_scripts.cmake   Build-time codegen: scripts/*.sh → bundle_scripts.h
tests/
  test_*.c            One offline suite per module (see Running tests)
  e2e/                Opt-in network end-to-end tests (see End-to-end tests)
  fixtures/           requirements.txt, package.json, packages.txt
man/
  packmule.1
packmule.spec         Fedora RPM spec
```

### Adding a new registry backend

Implement the `Registry` vtable from `include/registry.h`:

```c
struct Registry {
    const char  *name;           /* identifier passed to -t */
    const char  *manifest_name;  /* default manifest filename, shown in --help */
    package_name_equal_fn name_equal;  /* this registry's name identity rule */
    PackageList *(*parse_manifest)(const Registry *self, const char *path);
    int          (*detect)        (const char *basename);
    int          (*resolve)       (const Registry *self, Package *pkg);
    int          (*get_deps)      (const Registry *self, const Package *pkg,
                                   const PackageList *seen, PackageList *out);
    void        *ctx;            /* backend-specific config, injected by main.c */
    const char  *repo_url;       /* base URL, set from -u flag by main.c */
    int          py_minor;       /* target CPython minor (pypi) */
    const char  *target_os;      /* target OS family (pypi) */
    PypiIndexMode index_mode;    /* --index: auto / simple / json (pypi) */
};
```

1. Create `src/registry_<name>.c` with a `const Registry <name>_registry` instance.
2. Implement `parse_manifest` and `resolve`. Optionally implement `detect` (for
   filename auto-detection) and `get_deps` (transitive resolution); set either to
   `NULL` if not needed.
3. Set `name_equal`. Name identity is a registry rule, not a universal one —
   PyPI folds case and `-`/`_`/`.` (PEP 503), npm folds case only, RPM folds
   nothing. Pick `package_name_equal_pep503`, `..._casefold`, `..._exact`, or
   supply your own; there is no default, because inheriting another
   registry's rule silently merges packages that are not the same package.
4. Add an `extern` declaration and pointer entry in `src/registry.c`.
5. The new backend — and its `manifest_name` — appears automatically in `--help`
   and `--type`; `registry_names()` is derived from the dispatch table.
6. To support `--bundle`, add `scripts/install_<name>.sh` (with a
   `#__PACKMULE_VERIFY__` marker line) and a one-line entry in `script_for()`
   (`src/bundle.c`). The script is embedded into the binary at build time, so
   no runtime resource files are needed.

**Contract for `resolve`.** The resolver calls it again whenever a package is
marked dirty, so it must be idempotent and must honour `pkg->constraint` on
every call. Two invariants matter: `pkg->version` only ever holds a single
concrete version (ranges belong in `constraint`), and a package with
`user_pinned` set must never have its version changed. `get_deps` records what
it discovers by merging into existing entries — use `package_set_constraint()`
and `package_add_extras()`, which mark a resolved package dirty when the
requirement actually widens.

### Memory convention

`pm_*` allocators in `utils.h` abort on OOM — intentional for a CLI tool
where there is no meaningful recovery path. All heap-returning functions
document ownership; the caller is responsible for freeing via `pm_free()`.

---

## Roadmap

### Recently landed

- [x] Registry authentication — environment-supplied credentials (Basic,
      Bearer, or any custom header), scoped to the `--repo-url` host, plus a
      custom CA bundle and client certificates for corporate PKI. `~/.netrc`
      is deliberately not read: the environment is the single source
- [x] PEP 503 simple-index support — works against Artifactory, Nexus, devpi,
      GitLab and any other private PyPI index
- [x] SBOM output — CycloneDX 1.5 and SPDX 2.3 from the resolved package set
- [x] PyPI lockfile mode — exact-tree bundling from `uv.lock` and PEP 751
      `pylock.toml`, mirroring the npm lockfile mode.  `poetry.lock` is not
      read directly; export a `pylock.toml` instead

### Next up

- [ ] Debian/apt backend

### Planned

- [ ] HTTP Range resume for interrupted downloads

### Later

- [ ] Multi-arch bundles — one invocation targeting several architectures
- [ ] Delta bundles — diff against a previous bundle's `manifest.json` and
      ship only changed packages
- [ ] Signature verification beyond digests — GPG for RPMs, PyPI
      attestations, npm provenance
- [ ] More backends: Go modules, crates.io, Maven

---

## Contributing

### Development setup

Install the [dependencies](#requirements), then build with assertions live:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### The checks CI runs

Run these before opening a pull request. Every one of them gates merge.

**1. Build and test** — the Debug build above. Warnings are errors
(`PACKMULE_WERROR`, on by default).

**2. Sanitizers.** Most of this codebase is hand-rolled parsing of untrusted
input, which is exactly the shape of code that breaks in ways a passing test
suite does not reveal.

```sh
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DPACKMULE_WERROR=OFF \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer \
                     -fno-sanitize-recover=undefined -g"
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure
```

LeakSanitizer does not work on Apple silicon. On macOS add
`ASAN_OPTIONS=detect_leaks=0` and check for leaks with the platform tool
instead — `leaks --atExit -- ./build/packmule -n -f
tests/fixtures/requirements.txt`. CI runs LeakSanitizer on Ubuntu, so leaks are
caught there regardless.

**3. Static analysis** (local only — not run by CI). The generated header must
exist first.

```sh
cmake --build build --target bundle_scripts
clang-tidy -p build src/*.c
```

On macOS, add `-extra-arg="-isysroot$(xcrun --show-sdk-path)"`. This is worth
running before a change that touches parsing code, since most of this codebase
is hand-rolled parsing of untrusted input and the analyzer reaches error paths
the test suite does not. Prefer fixing what it reports over adding a
suppression; if a report is a genuine false positive, scope the suppression as
narrowly as possible — a `NOLINTBEGIN`/`NOLINTEND` pair around the one function
— and write down why.

Note that `--warnings-as-errors='*'` is deliberately absent. Some enabled
checks are heuristic and cannot express a provable bound —
`security.insecureAPI.strcpy` flags every `strcat` regardless of whether the
destination was just sized to fit — so treating the full check set as fatal
produces unavoidable failures. Read the output and judge it.

**4. Shell.** The install scripts ship inside every bundle; the e2e scripts are
the only thing exercising the pipeline end to end. Both are held to the same
standard as the C.

```sh
shellcheck -s sh scripts/*.sh tests/e2e/*.sh
```

**5. End-to-end** (optional, needs network) — see [End-to-end
tests](#end-to-end-tests). These hit real registries, so they are opt-in and run
nightly rather than per-push. They exit 77 (ctest's skip code) when a
prerequisite such as `npm` or `python3` is missing, rather than failing.

### Platform differences that bite

Most contributors develop on macOS; CI is mostly Linux. Several classes of bug
are invisible locally and fail only in CI. If CI disagrees with your machine,
**CI is authoritative** — do not assume it is flaky.

- **Missing POSIX headers.** macOS's `<string.h>` pulls in `<strings.h>`
  transitively; glibc under this project's strict `-std=c11` does not. Code
  calling `strcasecmp` without including `<strings.h>` builds cleanly on macOS
  and fails on every Linux job.
- **Library versions.** Homebrew is current; EL8 is not. An API introduced
  after the floor needs a `LIBCURL_VERSION_NUM` guard and a fallback — see
  `multi_wait()` in `src/network.c`.
- **clang-tidy findings can be platform-specific.** macOS's `<stdio.h>`
  macro-replaces some libc functions with `__builtin_*_chk` variants, and
  analyzer checks that match on the original name never engage. `pm_asprintf`
  in `src/utils.c` carries a suppression for a report that no clang-tidy
  version can reproduce on macOS. Since CI no longer runs the analyzer, a
  Linux-only report will not surface unless someone runs it there.

### Writing tests

Tests link `packmule_core`, a CMake `OBJECT` library, so they reach internal
functions without a second `main()`. They are compiled with `-UNDEBUG` so
`assert()` stays live — which means **no side effects inside `assert()`**.

Add tests in `tests/`, register them in `tests/CMakeLists.txt`. End-to-end
tests go under `tests/e2e/` and must carry the `e2e` label.

### Code style

- C11. Four-space indent, no tabs. Lines around 80 columns.
- `snake_case` for functions and variables; `PascalCase` for types.
- Static by default; non-static only when a test or another module needs it.
- The `pm_*` allocators abort on out-of-memory, so **do not write
  allocation-failure branches** — they are unreachable by construction.
- Errors propagate as return values. Library code does not call `exit()`;
  only `main.c` decides the process exit status.
- Comments explain *why*. The code already says what.

### Invariants reviewers will check

These are properties the compiler cannot enforce and the tests only sample.
Changes that touch resolution or credentials should be read against them:

- **The resolver contract** under [Adding a new registry
  backend](#adding-a-new-registry-backend) — `resolve` idempotent, `version`
  holding one concrete version, `user_pinned` never overridden, merging that
  narrows rather than overwrites. The fixpoint loop re-invokes `get_deps` for
  every dirty package, so anything with a side effect there runs more than
  once; see the one-shot warning set in `src/registry_rpm.c`.
- **A lockfile entry is final.** Packages built from a lock (`uv.lock`,
  `pylock.toml`, `package-lock.json`) must leave `parse_manifest` as
  `PKG_RESOLVED` with `user_pinned` set and `dirty` clear, which is what makes
  `needs_work()` in `src/resolve.c` skip them entirely. Anything that lets the
  resolver revisit them defeats the point of using a lock: the version, URL and
  digest were decided by a real backtracking resolver, and re-deciding any of
  them silently reintroduces the divergence the lock exists to prevent.
- **Name comparison is per-registry.** Each backend supplies `name_equal`;
  there is deliberately no default, because PyPI, npm and RPM disagree about
  what makes two names the same.
- **An unset digest is a failure, not a pass.** `digest_verify_file()` refuses
  a file it has nothing to check against.
- **Credentials go only to the `--repo-url` host**, and **redirects are
  followed by packmule, not by libcurl** — `CURLOPT_FOLLOWLOCATION` is
  deliberately off and the credential decision is remade at every hop from the
  host actually about to be contacted. See [Threat model](#threat-model) for
  why turning either of those around reintroduces a credential leak.
- **Nothing secret reaches the output directory.** `manifest.json`, the SBOM
  and `install.sh` are handed to whoever receives the bundle; a credential in
  any of them travels with it.

### Commits and pull requests

Write commit messages that explain the reasoning, not just the change. State
what was wrong, why the fix is right, and anything you verified or could not
verify. Keep the subject line short and in the imperative mood.

Pull requests should say how you tested the change, and call out anything you
could not check on your own platform.

---

## Security

### Reporting a vulnerability

Please report vulnerabilities **privately**, not as a public issue. Use
GitHub's private reporting: go to the repository's **Security** tab and choose
**Report a vulnerability**. That opens a draft advisory visible only to you and
the maintainers.

Please include what you are reporting, how to reproduce it, and what an
attacker gains. A proof-of-concept manifest or a captured registry response is
worth more than a description.

You should get an initial response within a week. If a fix is warranted, the
advisory will be published alongside it with credit, unless you prefer
otherwise.

packmule is pre-1.0: only the latest release receives fixes, and there are no
backports.

### Threat model

packmule fetches metadata and packages from a registry over the network,
verifies them, and writes a bundle intended to be carried into an air-gapped
environment and installed there.

**Everything that arrives over the network is untrusted.** Registry JSON, PEP
503 index HTML, `repomd.xml`, `primary.xml`, `METADATA` documents (including
those read out of a downloaded wheel), version strings, PEP 508 specifiers,
wheel filenames, and package names are all parsed by hand, in C, before
anything has been verified. **Memory-safety bugs in that parsing are the
highest-severity findings in this project, and they are in scope even when they
require a hostile or compromised mirror to trigger.** A crash, an out-of-bounds
read, or anything worse reached from a malformed registry response is a
vulnerability, not a robustness issue.

What packmule enforces — verified against the code, not aspirational:

- **TLS** at libcurl's defaults for every request. There is no flag to disable
  certificate or hostname verification, and none will be added. A private CA
  can be named (`PACKMULE_CA_BUNDLE`, or the conventional `CURL_CA_BUNDLE` /
  `SSL_CERT_FILE`) and mutual TLS configured, because supplying the correct
  trust anchor is a decision packmule can act on. Disabling verification is not.
- **`http` and `https` only**, for the request itself and for anything a
  redirect points at, with redirects capped at 5. An index cannot redirect a
  download to `file://` or `scp://`.
- **Typed digest verification.** The algorithm comes from the metadata; there
  is no length-sniffing. **A file with no digest to check against is refused**
  — an absent digest is a hard failure, never a silent pass. In lockfile mode
  the digest comes from the lock rather than from the index, so a registry that
  is compromised *after* you locked cannot substitute a file without the hash
  failing. A package with no hash in the lock is refused for the same reason.
- **npm requires SHA-512 SRI.** `dist.integrity` must carry a `sha512-` value;
  packages offering only the legacy SHA-1 `dist.shasum` are rejected rather
  than accepted with a weaker hash.
- **RPM chain of trust.** `primary.xml` is verified against the digest
  published in `repomd.xml` before any package digest inside it is trusted.
- **Bundle integrity.** `SHA256SUMS` records every bundled file; `install.sh`
  and `packmule verify` check it. Entry names containing `/` are rejected.
- **Credentials are host-scoped.** Index credentials are sent only to the host
  named by `--repo-url` (plus any host explicitly listed in
  `PACKMULE_AUTH_HOSTS`). An index chooses the download URLs it returns, so a
  repository that points a file at another host — or an attacker able to make
  it do so — receives an unauthenticated request, not your token.
- **Redirects are followed by packmule, not by libcurl**, and the credential
  decision is remade at each hop from the host actually about to be contacted.
  This is what makes an index's redirect to pre-signed S3 or CDN storage safe
  to follow. It is not merely defence in depth: libcurl drops only the
  `Authorization` header across a cross-host redirect, so a custom
  `PACKMULE_AUTH_HEADER` credential would otherwise follow the redirect to a
  third party.
- **A custom auth header is validated, not trusted.** `PACKMULE_AUTH_HEADER`
  is the one place the environment dictates raw protocol bytes; a CR or LF in
  it is rejected rather than sent, since it would let the value inject further
  headers or a second request.
- **Credentials come only from the environment.** They are never read from
  argv, where they would be visible in `ps` output to every user on the
  machine, and a `--repo-url` embedding `user:pass@` is rejected rather than
  honoured, because that URL is echoed in progress and error output.
  Credentials bound for a plain-`http://` endpoint are refused unless
  `PACKMULE_AUTH_INSECURE=1` is set explicitly. Credentials set without a
  `--repo-url` are an error, never a silent anonymous fetch of the public
  registry.
- **No credential reaches a bundle.** `manifest.json` records filenames,
  versions and digests; `install.sh` installs from the bundled files with the
  index disabled. A bundle can be handed on without leaking the token that
  built it.
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
  depend on, is not something packmule detects. A lockfile narrows this but
  does not close it: it pins the hashes as of the moment you locked, so
  tampering *after* that is caught, while anything already compromised when the
  lock was written is faithfully reproduced.
- **It does not verify RPM GPG signatures.** `install_rpm.sh` deliberately
  leaves dnf's `gpgcheck` setting alone so the target's own policy applies.
  **Leave `gpgcheck` enabled on the target.**
- **`SHA256SUMS` is unsigned.** It gives you integrity, not authenticity: it
  proves the bundle was not corrupted, not that it came from you. Anyone who
  can modify the bundle can regenerate it. Sign the `.tar.gz` out of band and
  verify that signature before installing.
- **It does not inspect package contents.** A package that is malicious but
  correctly published will be bundled and installed faithfully. packmule is a
  transport, not a scanner.
- **Installation runs with the operator's privileges.** `install.sh` executes
  pip, npm, or dnf on the target. Read it before running it as root.
- **Local input is trusted.** Manifests, lockfiles, and existing bundle
  directories are treated as coming from the operator.

### Recommended practice

For an air-gapped deployment:

1. Bundle from a **private mirror** you control rather than the public index.
2. **Sign the resulting `.tar.gz`** and verify the signature on the target.
   This is the gap `SHA256SUMS` does not close.
3. Run `packmule verify <dir>` after extracting, before installing.
4. Keep **`gpgcheck` enabled** for RPM installs.
5. Read `install.sh` before running it with elevated privileges.

---

## License

MIT — see [LICENSE](LICENSE).
