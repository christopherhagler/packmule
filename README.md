# packmule

[![CI](https://github.com/christopherhagler/packmule/actions/workflows/ci.yml/badge.svg)](https://github.com/christopherhagler/packmule/actions/workflows/ci.yml)
[![RPM packaging](https://github.com/christopherhagler/packmule/actions/workflows/rpm.yml/badge.svg)](https://github.com/christopherhagler/packmule/actions/workflows/rpm.yml)

An air-gapped package bundler written in C.

packmule reads a package manifest, queries the appropriate registry to resolve
each entry to a concrete version and download URL, downloads every required
file, verifies each one against its published digest, and optionally compresses
everything into a single `.tar.gz` with a manifest and install script ready for
transport to a network-isolated machine.

Supported registries: **PyPI**, **npm**, **RPM** — with SHA-256/SHA-512
integrity verification. PyPI and npm follow transitive dependencies
automatically; RPM requires an explicit manifest.

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
packmule -f <manifest> [-o <dir>] [-t <type>] [-a <arch>] [-u <url>] [-b] [-n]
packmule -V | --version
packmule -h | --help
```

| Flag | Description |
|---|---|
| `-f <file>`, `--manifest` | Path to the package manifest (**required**) |
| `-o <dir>` | Output directory (default: `.`); created if absent |
| `-t <type>`, `--type` | Registry backend: `pypi`, `npm`, `rpm`. Auto-detected from the manifest filename when omitted (`requirements*.txt` → `pypi`, `package.json` / `package-lock.json` / `npm-shrinkwrap.json` → `npm`, `packages.txt` → `rpm`); falls back to `pypi` for unrecognised names |
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
| `pypi` | `python3 -m pip install --no-index --find-links="$DIR" --no-build-isolation -r "$DIR/requirements.txt"` (installs bundled setuptools/wheel first when the bundle contains an sdist) |
| `npm` | lockfile bundle: `npm ci --offline --omit=dev` replaying the bundled `package-lock.json` against the bundled tarballs; flat bundle: one `npm install --offline --omit=dev --no-save` over every `.tgz` (devDependencies hidden from npm for the duration and the manifest restored afterwards) |
| `rpm` | `dnf install -y --disablerepo='*' *.rpm`, falling back to `rpm -Uvh` |

Bundling is skipped if any package failed to download. The output directory
is always left intact alongside the archive.

After a bundle is created, packmule verifies it end-to-end so a bundle that
cannot install offline fails **here**, not on the air-gapped machine:

- **pypi** — `pip install --dry-run --no-index` against only the bundled
  files (when this machine matches the bundle's target os/arch/python and
  pip is ≥ 22.2).
- **npm** — runs the generated `install.sh` in a scratch project with the
  registry pointed at an unroutable address and an empty npm cache, with
  scripts disabled.

### Private registries

All three backends accept a custom base URL via `-u`. Use this to point
packmule at a JFrog Artifactory or Sonatype Nexus instance instead of the
public endpoints:

```bash
# Artifactory PyPI proxy
packmule -f requirements.txt -o ./vendor \
  -u https://artifactory.example.com/artifactory/api/pypi/pypi-virtual/pypi

# Nexus npm proxy
packmule -f package.json -o ./vendor -t npm \
  -u https://nexus.example.com/repository/npm-proxy

# Any DNF/YUM-compatible RPM repository
packmule -f packages.txt -o ./vendor -t rpm -a x86_64 \
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
version ranges (`>=`, `<`, `~=`, `!=`, `==x.*`) so the bundle is mutually
installable. The queue grows breadth-first and deduplicates by name
(case-insensitive, with `-`, `_`, and `.` treated as equivalent per PEP 503);
ranges from multiple dependents are intersected.

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

Integrity: SHA-256 hex digest from the PyPI JSON API (`urls[].digests.sha256`).

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

The unit suites are fully offline — no network access required:

| Suite | Coverage |
|---|---|
| `test_package` | Package lifecycle, PackageList grow, name-based dedup, PEP 503 name normalisation |
| `test_registry` | Registry dispatch table, name lookup, vtable integrity, `get_deps` slot |
| `test_semver` | node-semver ranges: `^`, `~`, comparators, x-ranges, hyphen ranges, `\|\|`, prereleases |
| `test_pep440` | PEP 440 ordering (dev/pre/post/epoch) and specifiers (`==`, `!=`, `<=`, `>=`, `~=`, `==x.*`) |
| `test_pep508` | Dependency-spec parsing (name/extras/pin/constraint) and environment-marker evaluation |
| `test_wheeltag` | Wheel filename classification, platform/arch/python tag matching, manylinux glibc floors |
| `test_registry_pypi` | requirements.txt parsing (includes, extras, constraints) and `get_deps`; wheel selection via `pypi_parse_response` |
| `test_registry_npm` | npm manifest/lockfile parsing and `get_deps`: aliases, peers, range intersection, dedup |
| `test_registry_rpm` | RPM manifest parsing: name-only, name-version, hyphenated names |
| `test_hash` | `sha256_file`, `verify_file` — SHA-256 hex and SHA-512 SRI paths |
| `test_bundle` | Bundle creation for all three backends; skips packages missing from disk |

### End-to-end tests

Network-dependent full-pipeline tests live in `tests/e2e/` and are opt-in:

```bash
cmake -B build -DPACKMULE_E2E_TESTS=ON
ctest --test-dir build -L e2e --output-on-failure
```

| Test | What it proves |
|---|---|
| `e2e_pypi` | manifest → resolve → download → bundle → extract → **offline install into a fresh venv** (`--no-index`) → import check |
| `e2e_npm` | two full cycles — flat (ranges, scoped package, devDeps excluded) and lockfile (a tree needing two `debug` versions) — bundle → extract → install with the registry pointed at an **unroutable address** → `require()` and nesting checks |
| `e2e_rpm` | repomd.xml → primary.xml decompress → package match → sha256/version extraction against a live DNF repo (dry run) |

A missing prerequisite (`python3`, `npm`) skips the test rather than failing it,
so the suite is safe in minimal CI images.

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
  main.c              CLI argument parsing; resolve/download/bundle loop;
                      post-bundle offline install check
  network.c           libcurl wrappers — fetch_json(), download_file()
  hash.c              SHA-256 and SHA-512 file digest via OpenSSL EVP
  registry.c          Registry dispatch table and name lookup
  registry_pypi.c     PyPI backend — requirements.txt parser, distribution
                      selection, constraint-aware resolver, get_deps
  registry_npm.c      npm backend — package.json parser, tarball resolution
  registry_rpm.c      RPM backend — packages.txt parser, repomd/primary.xml resolve
  semver.c            node-semver comparison and range matching (npm)
  pep440.c            PEP 440 version ordering and specifier matching (pypi)
  pep508.c            PEP 508 dependency-spec parsing and environment markers (pypi)
  wheeltag.c          Wheel/sdist filename classification and platform-tag
                      matching (pypi)
  package.c           Package and PackageList data structures
  bundle.c            manifest.json + requirements.txt + install.sh + .tar.gz;
                      install.sh is an embedded scripts/install_<name>.sh
  utils.c             Abort-on-OOM allocators (pm_malloc, pm_free, …) and string
                      helpers (pm_strtrim, pm_asprintf, pm_human_size)
include/
  network.h
  hash.h
  registry.h          Registry vtable (name, manifest_name, parse_manifest,
                      resolve, get_deps, ctx, repo_url)
  registry_internal.h Backend internals exposed to the test suite only
  semver.h / pep440.h / pep508.h / wheeltag.h
  package.h           Package / PackageList types
  bundle.h            BundleOptions struct and bundle_create()
  version.h           PACKMULE_VERSION constant
  utils.h             Allocator wrappers and string helpers
scripts/
  install_pypi.sh     Per-backend offline install scripts (real, shellcheck-able);
  install_npm.sh        embedded into the binary at build time as byte arrays
  install_rpm.sh        (build/generated/bundle_scripts.h) and written verbatim
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
    PackageList *(*parse_manifest)(const Registry *self, const char *path);
    int          (*detect)        (const char *basename);
    int          (*resolve)       (const Registry *self, Package *pkg);
    int          (*get_deps)      (const Registry *self, const Package *pkg,
                                   const PackageList *seen, PackageList *out);
    void        *ctx;            /* arch string, injected by main.c */
    const char  *repo_url;       /* base URL, set from -u flag by main.c */
};
```

1. Create `src/registry_<name>.c` with a `const Registry <name>_registry` instance.
2. Implement `parse_manifest` and `resolve`. Optionally implement `detect` (for
   filename auto-detection) and `get_deps` (transitive resolution); set either to
   `NULL` if not needed.
3. Add an `extern` declaration and pointer entry in `src/registry.c`.
4. The new backend — and its `manifest_name` — appears automatically in `--help`
   and `--type`; `registry_names()` is derived from the dispatch table.
5. To support `--bundle`, add `scripts/install_<name>.sh` and a one-line entry in
   `script_for()` (`src/bundle.c`). The script is embedded into the binary at
   build time, so no runtime resource files are needed.

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
- [x] Per-file download progress bar (percent + speed) with permanent ✓/✗ lines;
      auto-suppressed to plain output when stdout is not a terminal
