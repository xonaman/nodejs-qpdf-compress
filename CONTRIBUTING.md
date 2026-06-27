# Contributing to qpdf-compress

Thanks for your interest in improving `qpdf-compress`! This package is a native
Node.js addon: TypeScript glue in `lib/` wraps a C++ N-API addon in `src/` that
statically links [QPDF](https://github.com/qpdf/qpdf),
[mozjpeg](https://github.com/mozilla/mozjpeg), and
[HarfBuzz](https://github.com/harfbuzz/harfbuzz) (plus zlib on Windows).

## Code of Conduct

This project is governed by our [Code of Conduct](./CODE_OF_CONDUCT.md). By
participating, you are expected to uphold it.

## Reporting bugs and requesting features

Please use the GitHub issue templates. Security vulnerabilities must **not** be
filed as public issues — see [SECURITY.md](./SECURITY.md) for private reporting.

## Prerequisites

Building from source needs a C/C++ toolchain in addition to Node.js ≥ 22:

- CMake ≥ 3.16
- A C++20 compiler (GCC 10+, Clang 13+, or MSVC 2019+)
- zlib development headers
- nasm (optional, enables mozjpeg SIMD acceleration)

```bash
# macOS
brew install cmake nasm

# Ubuntu / Debian
sudo apt install cmake g++ zlib1g-dev nasm

# Amazon Linux / RHEL
sudo yum install cmake3 gcc-c++ zlib-devel nasm

# Windows (using vcpkg)
vcpkg install zlib --triplet x64-windows-static
```

## Native development loop

```bash
# 1. Install JS dependencies without triggering the native install script
npm ci --ignore-scripts

# 2. Download and build the native dependencies (QPDF, mozjpeg, HarfBuzz)
npm run download
#    or individually:
#    npm run download:mozjpeg
#    npm run download:qpdf
#    npm run download:harfbuzz

# 3. Compile the C++ N-API addon
npx node-gyp rebuild

# 4. Strip/bundle the built .node binary into build/Release
node scripts/bundle-lib.mjs

# 5. Run the test suite
npm test
```

Steps 2–4 are the same ones the `install` script runs as its source-compilation
fallback (`scripts/install.mjs`): on `npm install`, the package first tries to
download a prebuilt binary from GitHub Releases for your platform/arch, and only
builds from source when no prebuilt is available.

The downloaded native dependencies are pinned and integrity-checked against
`scripts/native-deps.json`. After changing any download script or the manifest,
run the checksum tripwire:

```bash
npm run verify:checksums
```

## Formatting, linting, and type-checking

Before opening a pull request, make sure the following pass:

```bash
npm run format:check   # Prettier (use `npm run format` to apply)
npm run lint:check     # ESLint   (use `npm run lint` to apply fixes)
npm run typecheck      # tsc --noEmit
```

A Husky pre-commit hook runs Prettier and ESLint on staged files via
lint-staged, so most issues are caught automatically.

## Pull requests

- Branch off `main` and keep changes focused.
- Add or update tests for behavior changes.
- Update `README.md` and add a `CHANGELOG.md` entry under `## [Unreleased]`
  when your change is user-visible.
- Use [Conventional Commit](https://www.conventionalcommits.org/) style commit
  messages where practical (`fix:`, `feat:`, `build:`, `ci:`, `docs:`).

By contributing, you agree that your contributions are licensed under the
project's [Apache-2.0](./LICENSE) license.
