# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/), and this project adheres to [Semantic Versioning](https://semver.org/).

## [0.3.0] - 2026-03-30

### Changed

- **BREAKING**: Replaced `mode: 'lossy' | 'lossless'` with `lossy?: boolean` (default `false`)
- **BREAKING**: Removed `quality` parameter — quality is now automatically determined per mode
- **BREAKING**: Removed `maxDpi` parameter — DPI is now automatically determined per mode
- **BREAKING**: Options parameter is now optional — `compress(input)` defaults to lossless
- Lossless: pure structural optimization — no image re-encoding or downscaling
- Lossy: skip images at q ≤ 65, re-encode at q75, downscale to 72 DPI

## [0.2.0] - 2026-03-30

### Added

- CMYK → RGB conversion for JPEG images
- ICCBased color space support (extracts and converts embedded ICC profiles)
- PNG optimization (re-encodes as JPEG when beneficial)
- DPI downscaling (configurable max DPI, default 75)
- Metadata stripping (XMP, document info, thumbnails) — enabled by default
- Unused font removal
- Auto quality mode — estimates existing JPEG quality and skips re-encoding when already below target

## [0.1.3] - 2026-03-30

### Fixed

- Included `<cstdio>` before `<jpeglib.h>` for Linux compatibility
- Restored Node 20 to CI matrix (matching engines `>=20.11.0`)
- Used static import for `Readable` instead of dynamic `import()`

### Added

- Build provenance attestation for prebuilt binaries via `actions/attest-build-provenance`

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
