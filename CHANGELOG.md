# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/), and this project adheres to [Semantic Versioning](https://semver.org/).

## [0.1.2] - 2026-03-30

### Fixed

- Fixed Linux cross-build tarball structure (prebuilds now install correctly)
- Fixed Debian Bookworm cross-builds (use `libjpeg62-turbo-dev` package)
- Moved `setjmp` before `jpeg_create_*` calls to prevent UB on allocation failure
- Used `uint64_t` for FNV-1a image dedup hash (correct on 32-bit ARM)
- Added component count validation in `encodeJpeg`
- Wrapped web `ReadableStream` with `Readable.fromWeb()` in install script
- Made `strerror` calls thread-safe by copying to `std::string`

### Changed

- Bumped minimum Node.js version to 20.11.0 (`import.meta.dirname` requirement)
- Updated CI matrix to Node 22 and 24

## [0.1.1] - 2026-03-30

### Changed

- Replaced stb_image_write with libjpeg-turbo for lossy JPEG encoding (better quality per byte)
- Split monolithic `qpdf_addon.cc` into focused modules: `jpeg.cc`, `images.cc`, `qpdf_addon.cc`
- Extracted shared `forEachImage()` template helper
- Bumped minimum Node.js version to 20.11.0

### Fixed

- Removed global mutex that serialized concurrent compress() calls
- Isolated setjmp/longjmp in C-style functions to avoid UB with C++ destructors
- Fixed prebuild tarball structure so install script finds the .node file
- Added mode validation (reject invalid mode strings)

## [0.1.0] - 2026-03-30

### Added

- Native PDF compression via QPDF 12.3.2 with single `compress()` API
- Flate compression (level 9) for all streams
- Lossless JPEG optimization via libjpeg-turbo
- Image deduplication (content-hash based)
- PDF repair merged into compress pipeline
- Prebuilt binaries for macOS (arm64, x64), Linux glibc (x64, arm64, arm), Linux musl (x64, arm64), Windows (x64, arm64)
- Automatic prebuild download with source compilation fallback
- N-API addon for ABI stability across Node.js versions

### Infrastructure

- CI matrix: 3 OS × 3 Node versions (20, 22, 24)
- Release pipeline with prebuild generation and npm provenance
- QEMU-based cross-compilation for Linux arm64/arm and musl variants
- vcpkg integration for Windows static linking
