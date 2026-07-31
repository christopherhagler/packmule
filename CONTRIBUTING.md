# Contributing to packmule

Thanks for helping out. This document covers how to build the project, the
checks your change has to pass, and the conventions the codebase follows.

For what packmule does and how the pipeline fits together, see the
[README](README.md). This file does not repeat it.

## Development setup

Install the dependencies listed under [Requirements](README.md#requirements),
then:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Minimum dependency versions are enforced by CMake, so a too-old library fails
at configure time with a readable message instead of at link time. The floors
are set by RHEL/CentOS 8, the oldest supported target.

## The checks CI runs

Run these before opening a pull request. Every one of them gates merge.

**1. Build and test.** Warnings are errors (`PACKMULE_WERROR`, on by default).

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

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

LeakSanitizer does not work on Apple silicon. On macOS, add
`ASAN_OPTIONS=detect_leaks=0` and check for leaks with the platform tool
instead:

```sh
leaks --atExit -- ./build/packmule -n -f tests/fixtures/requirements.txt
```

CI runs LeakSanitizer on Ubuntu, so leaks are caught there regardless.

**3. Static analysis.** The generated header must exist first.

```sh
cmake --build build --target bundle_scripts
clang-tidy -p build --warnings-as-errors='*' src/*.c
```

On macOS, add `-extra-arg="-isysroot$(xcrun --show-sdk-path)"`.

Fix what clang-tidy reports rather than adding suppressions. If a report is a
genuine false positive, scope the suppression as narrowly as possible — a
`NOLINTBEGIN`/`NOLINTEND` pair around the one function — and write down why.
Do not disable a check globally in `.clang-tidy` to silence a single site.

**4. Shell.** The install scripts ship inside every bundle; the e2e scripts are
the only thing exercising the pipeline end to end. Both are held to the same
standard as the C.

```sh
shellcheck -s sh scripts/*.sh tests/e2e/*.sh
```

**5. End-to-end (optional, needs network).** These hit real registries, so they
are opt-in and run nightly rather than per-push.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DPACKMULE_E2E_TESTS=ON
cmake --build build --parallel
ctest --test-dir build -L e2e --output-on-failure
```

They exit 77 (ctest's skip code) when a prerequisite such as `npm` or `python3`
is missing, rather than failing.

## Platform differences that bite

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
  version can reproduce on macOS.

## Tests

Tests link `packmule_core`, a CMake `OBJECT` library, so they reach internal
functions without a second `main()`. They are compiled with `-UNDEBUG` so
`assert()` stays live — which means **no side effects inside `assert()`**.

Add tests in `tests/`, register them in `tests/CMakeLists.txt`. End-to-end
tests go under `tests/e2e/` and must carry the `e2e` label.

## Code style

- C11. Four-space indent, no tabs. Lines around 80 columns.
- `snake_case` for functions and variables; `PascalCase` for types.
- Static by default; non-static only when a test or another module needs it.
- The `pm_*` allocators abort on out-of-memory, so **do not write
  allocation-failure branches** — they are unreachable by construction.
- Errors propagate as return values. Library code does not call `exit()`;
  only `main.c` decides the process exit status.
- Comments explain *why*. The code already says what.

## Invariants reviewers will check

These are properties the compiler cannot enforce and the tests only sample.
Changes that touch resolution should be read against them:

- **`resolve` is idempotent.** Running it twice over the same input produces
  the same set. The fixpoint loop re-invokes `get_deps` for every dirty
  package, so anything with a side effect there runs more than once — see the
  one-shot warning set in `src/registry_rpm.c`.
- **`version` holds one concrete version, never a range.** Ranges live in
  constraints.
- **`user_pinned` is never overridden** by a transitively discovered
  constraint.
- **Merging never overwrites.** Re-queuing a package narrows it through
  `package_set_constraint()` / `package_add_extras()`.
- **Name comparison is per-registry.** Each backend supplies `name_equal`;
  there is deliberately no default, because PyPI, npm and RPM disagree about
  what makes two names the same.
- **An unset digest is a failure, not a pass.** `digest_verify_file()` refuses
  a file it has nothing to check against.

## Commits and pull requests

Write commit messages that explain the reasoning, not just the change. State
what was wrong, why the fix is right, and anything you verified or could not
verify. Keep the subject line short and in the imperative mood.

Pull requests should say how you tested the change, and call out anything you
could not check on your own platform.

## Security

Do not open a public issue for a vulnerability. See [SECURITY.md](SECURITY.md).
