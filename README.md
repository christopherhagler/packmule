# packmule

[![CI](https://github.com/christopherhagler/packmule/actions/workflows/ci.yml/badge.svg)](https://github.com/christopherhagler/packmule/actions/workflows/ci.yml)

An air-gapped package bundler written in C.

packmule reads a package manifest, queries the appropriate registry to resolve
each entry to a concrete version and download URL, downloads every required
file, verifies each one against its published digest, and optionally compresses
everything into a single `.tar.gz` with a manifest and install script ready for
transport to a network-isolated machine.

Supported registries: **PyPI**, **npm**, **RPM** — all with transitive
dependency resolution and SHA-256/SHA-512 integrity verification.

---

## Quick start

```
$ packmule -r requirements.txt -o ./vendor -a x86_64
packmule: backend   : pypi
packmule: arch      : x86_64
packmule: manifest  : requirements.txt (3 package(s))
packmule: output dir: ./vendor

  [1/3] requests==2.31.0
         -> ./vendor/requests-2.31.0-py3-none-any.whl
  [====================] 100%  61.1 KB / 61.1 KB  489.0 KB/s
            sha256: OK

  [2/8] click==8.1.7
         -> ./vendor/click-8.1.7-py3-none-any.whl
  [====================] 100%  95.6 KB / 95.6 KB  716.9 KB/s
            sha256: OK

  [3/10] certifi (resolved: 2026.5.20)
         -> ./vendor/certifi-2026.5.20-py3-none-any.whl
  [====================] 100%  131.0 KB / 131.0 KB  859.5 KB/s
            sha256: OK

  ...

packmule: 10/10 package(s) downloaded to ./vendor
```

The progress bar updates in-place on a terminal (TTY). When stdout is piped or
redirected the bar is suppressed automatically, keeping log output clean.

The counter grows as transitive dependencies are discovered — `[2/8]` means
8 packages are now queued after resolving the first. Each file is verified
against its registry-published digest before being kept; mismatches are
removed and reported on standard error.

---

## Requirements

| Dependency | Version | Purpose |
|---|---|---|
| libcurl | ≥ 7.68 | Registry API queries and file downloads |
| OpenSSL | ≥ 1.1 | SHA-256 and SHA-512 file verification |
| libarchive | ≥ 3.4 | RPM repository metadata decompression; bundle `.tar.gz` creation |
| cJSON | ≥ 1.7 | JSON registry response parsing |
| CMake | ≥ 3.15 | Build system |
| C compiler | C11 | GCC ≥ 7, Clang ≥ 6, Apple Clang |

cJSON must be installed as a system package; the build does **not** fetch it
from the internet.

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
packmule -r <manifest> [-o <dir>] [-t <type>] [-a <arch>] [-u <url>] [-b] [-n]
packmule -V | --version
packmule -h | --help
```

| Flag | Description |
|---|---|
| `-r <file>` | Path to the package manifest (**required**) |
| `-o <dir>` | Output directory (default: `.`); created if absent |
| `-t <type>`, `--type` | Registry backend: `pypi` (default), `npm`, `rpm` |
| `-a <arch>`, `--arch` | Target CPU architecture (default: current machine). Use `any` for universal/source only |
| `-u <url>`, `--repo-url` | Repository base URL — required for `rpm`; optional for `pypi`/`npm` (overrides public endpoint) |
| `-b`, `--bundle` | Write `manifest.json` and `install.sh`, then compress output to `<dir>.tar.gz` |
| `-n`, `--dry-run` | Resolve and print what would be downloaded — no files written |
| `-V`, `--version` | Print version and exit |
| `-h`, `--help` | Print usage and exit |

### Architecture selection

packmule detects the current machine's architecture via `uname(2)` and uses it
as the default target. Override with `-a`:

```bash
packmule -r requirements.txt -o ./vendor -a x86_64    # Linux x86-64
packmule -r requirements.txt -o ./vendor -a aarch64   # Linux ARM64
packmule -r requirements.txt -o ./vendor -a any       # universal/source only
```

`aarch64` and `arm64` are treated as equivalent (same CPU, different OS naming
convention). Package selection priority (highest first):

1. Architecture-specific package matching the `-a` target
2. Universal package (e.g. pure-Python `none-any` wheel, npm tarball)
3. Source archive

### Dry run

Use `-n` to audit what would be fetched before committing to a download:

```
$ packmule -n -r requirements.txt -a x86_64
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

### Bundling for transport

Add `--bundle` to compress the output directory into a single `.tar.gz` that
can be carried to the air-gapped machine in one step:

```
$ packmule -r requirements.txt -o ./vendor -a x86_64 --bundle
packmule: backend   : pypi
...
packmule: 10/10 package(s) downloaded to ./vendor
packmule: writing manifest.json ...
packmule: writing install.sh ...
packmule: creating ./vendor.tar.gz ...
packmule: bundle ready: ./vendor.tar.gz
```

`vendor.tar.gz` extracts to a directory containing:

| File | Description |
|---|---|
| `*.whl` / `*.tgz` / `*.rpm` | Downloaded package files |
| `manifest.json` | Name, version, filename, and SHA-256 for every package |
| `install.sh` | Pre-configured install script for the active backend |
| `requirements.txt` | *(pypi only)* pip-compatible requirements file used by `install.sh` |

On the air-gapped machine:

```bash
tar -xzf vendor.tar.gz
cd vendor
./install.sh
```

Install scripts by backend:

| Backend | Command |
|---|---|
| `pypi` | `pip install --no-index --find-links="$DIR" -r "$DIR/requirements.txt"` |
| `npm` | loops `npm install` over each `.tgz` in the directory |
| `rpm` | `dnf install --disablerepo='*' *.rpm`, falling back to `rpm -Uvh` |

Bundling is skipped if any package failed to download. The output directory
is always left intact alongside the archive.

### Private registries

All three backends accept a custom base URL via `-u`. Use this to point
packmule at a JFrog Artifactory or Sonatype Nexus instance instead of the
public endpoints:

```bash
# Artifactory PyPI proxy
packmule -r requirements.txt -o ./vendor \
  -u https://artifactory.example.com/artifactory/api/pypi/pypi-virtual/pypi

# Nexus npm proxy
packmule -r package.json -o ./vendor -t npm \
  -u https://nexus.example.com/repository/npm-proxy

# Any DNF/YUM-compatible RPM repository
packmule -r packages.txt -o ./vendor -t rpm -a x86_64 \
  -u https://repo.example.com/fedora/40/x86_64/os
```

Artifactory and Nexus both expose the same JSON API as their respective public
registries, so no response parsing changes are needed.

### Exit codes

| Code | Meaning |
|---|---|
| `0` | All packages resolved and downloaded successfully |
| `1` | One or more packages failed; successful ones are still written to disk |

---

## Backends

### pypi — Python Package Index

Reads a `requirements.txt` file. Supported syntax:

```
requests              # unpinned — resolved to latest
requests==2.31.0      # exact pin
requests[security]    # extras stripped; do not affect downloads
# comments and blank lines are ignored
```

Lines starting with `-` (e.g. `-r`, `-c`) are skipped with a warning.

Transitive dependencies are resolved automatically by following
`requires_dist` from each package's registry metadata. The queue grows
breadth-first and deduplicates by name (case-insensitive, with `-`, `_`, and
`.` treated as equivalent per PEP 503).

Environment markers (e.g. `; python_version > "3.8"`) are stripped without
evaluation — all declared dependencies are queued regardless of conditions.

Wheel selection priority: architecture-specific wheel → universal wheel
(`none-any`) → sdist.

Integrity: SHA-256 hex digest from the PyPI JSON API (`urls[].digests.sha256`).

### npm — Node.js packages

Reads the `"dependencies"` object from a `package.json` file.
`devDependencies`, `peerDependencies`, and `optionalDependencies` are ignored.

Exact versions (`4.18.2`) are queried directly; all range specifiers
(`^4.18.2`, `~1.2.3`, `>=1.0.0`, `*`) resolve to the current `latest` tag.

Transitive dependencies are resolved from the `dependencies` field of each
resolved version document. Scoped packages (`@scope/name`) are supported.

Integrity: SHA-512 SRI from `dist.integrity` (`sha512-<base64>`). Packages
published before ~2017 that lack `dist.integrity` are refused rather than
silently falling back to the weaker SHA-1 `dist.shasum`.

```bash
packmule -r package.json -o ./vendor -t npm
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
packmule -r packages.txt -o ./rpms -t rpm -a x86_64 \
  -u https://dl.fedoraproject.org/pub/fedora/linux/releases/40/Everything/x86_64/os
```

Resolution pipeline: fetch `repodata/repomd.xml` → locate `primary.xml.gz` →
download and decompress with libarchive → scan XML for the package by name and
arch (also matches `noarch` packages regardless of `-a`).

Integrity: SHA-256 hex digest from the `<checksum type="sha256">` element in
the primary database.

> **Note:** The RPM backend resolves packages individually as listed in the
> manifest. Unlike PyPI and npm, it does not follow transitive dependencies
> automatically — list every required package explicitly. On the target machine,
> `install.sh` uses `dnf install --disablerepo='*'`, which handles dependency
> ordering within the bundle.

---

## Running tests

```bash
ctest --test-dir build --output-on-failure
```

All seven test suites are fully offline — no network access required:

| Suite | Coverage |
|---|---|
| `test_package` | Package lifecycle, PackageList grow/contains/dedup, PEP 503 name normalisation |
| `test_registry` | Registry dispatch table, name lookup, vtable integrity |
| `test_registry_pypi` | PyPI manifest parsing: pins, extras, markers, broader specifiers, error paths |
| `test_registry_npm` | npm manifest parsing: deps, scoped packages, dev/peer dep exclusion |
| `test_registry_rpm` | RPM manifest parsing: name-only, name-version, hyphenated names |
| `test_hash` | `sha256_file`, `verify_file` — SHA-256 hex and SHA-512 SRI paths |
| `test_bundle` | Bundle creation for all three backends; skips packages missing from disk |

---

## Installing

```bash
cmake --install build --prefix /usr/local
# binary   → /usr/local/bin/packmule
# man page → /usr/local/share/man/man1/packmule.1.gz
```

### Building a .deb or .rpm

```bash
cd build
cpack -G DEB   # → packmule_0.1.0_<arch>.deb
cpack -G RPM   # → packmule-0.1.0-<arch>.rpm
cpack          # all three: DEB, RPM, TGZ
```

---

## Architecture

```
src/
  main.c              CLI argument parsing; resolve/download/bundle loop
  network.c           libcurl wrappers — fetch_json(), download_file()
  hash.c              SHA-256 and SHA-512 file digest via OpenSSL EVP
  registry.c          Registry dispatch table and name lookup
  registry_pypi.c     PyPI backend — manifest parser, wheel/sdist selection
  registry_npm.c      npm backend — package.json parser, tarball resolution
  registry_rpm.c      RPM backend — packages.txt parser, repomd/primary.xml resolve
  package.c           Package and PackageList data structures
  bundle.c            manifest.json, install.sh, and .tar.gz bundle creation
  utils.c             Abort-on-OOM allocators (pm_malloc, pm_free, …)
include/
  network.h
  hash.h
  registry.h          Registry vtable (name, manifest_name, parse_manifest,
                      resolve, ctx, repo_url, destroy)
  package.h           Package / PackageList types
  bundle.h            BundleOptions struct and bundle_create()
  version.h           PACKMULE_VERSION constant
  utils.h             Allocator wrappers
tests/
  test_package.c
  test_registry.c
  test_registry_pypi.c
  test_registry_npm.c
  test_registry_rpm.c
  test_hash.c
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
    const char  *manifest_name;  /* shown in --help */
    PackageList *(*parse_manifest)(const Registry *self, const char *path);
    int          (*resolve)       (const Registry *self, Package *pkg);
    void        *ctx;            /* arch string, injected by main.c */
    const char  *repo_url;       /* base URL, set from -u flag by main.c */
    void        (*destroy)        (Registry *self);
};
```

1. Create `src/registry_<name>.c` with a `const Registry <name>_registry` instance.
2. Add an `extern` declaration and pointer entry in `src/registry.c`.
3. The new backend appears automatically in `--help` and `--type`.

### Memory convention

`pm_*` allocators in `utils.h` abort on OOM — intentional for a CLI tool
where there is no meaningful recovery path. All heap-returning functions
document ownership; the caller is responsible for freeing via `pm_free()`.

---

## Roadmap

- [x] SHA-256 and SHA-512 SRI verification of every downloaded file
- [x] Dry-run mode (`-n`)
- [x] Registry vtable — pluggable backends
- [x] PyPI backend: manifest parsing, resolve, download, SHA-256 verify
- [x] Transitive dependency resolution (PyPI `requires_dist`, npm `dependencies`)
- [x] Architecture-aware package selection (`-a <arch>`)
- [x] npm backend: manifest parsing, resolve, SHA-512 SRI verify
- [x] RPM backend: manifest parsing, resolve via repomd.xml/primary.xml, SHA-256 verify
- [x] Private/corporate registry support (`-u` for all backends)
- [x] Bundle output — `manifest.json`, `install.sh`, and `.tar.gz` (`--bundle`)
- [x] Progress bar / download speed display (TTY-only, 10 Hz, `\r` in-place)
