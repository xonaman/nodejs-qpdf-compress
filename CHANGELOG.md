# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/), and this project adheres to [Semantic Versioning](https://semver.org/).

## [0.6.0] - 2026-04-04

### Changed

- **mozjpeg**: replaced libjpeg-turbo with vendored mozjpeg 4.1.1 for 5–15% smaller JPEGs via trellis quantization, overshoot deringing, and optimized progressive scan ordering — applies to both lossy recompression and lossless Huffman optimization

## [0.5.0] - 2026-04-04

### Added

- `concurrency()` — get/set max concurrent compress operations dispatched to the libuv thread pool (default: CPU cores, powered by p-limit)
- Husky + lint-staged pre-commit hook (Prettier + ESLint on staged files)

## [0.4.0] - 2026-03-31

### Added

- **Grayscale detection**: automatically converts RGB images where R==G==B to DeviceGray (3× raw data reduction)
- **Bitonal conversion**: converts 8-bit grayscale images that are effectively B&W to 1-bit (8× raw data reduction)
- **TrueType font subsetting**: strips unused glyph outlines from `/FontFile2` font programs via custom binary parser (cmap format 0/4, composite glyph dependency resolution, glyf/loca/hmtx table rebuilding)
- **Soft mask optimization**: losslessly optimizes JPEG `/SMask` transparency streams
- **ICC profile stripping**: replaces ICCBased color spaces with Device equivalents on images and page resources
- **Form flattening**: stamps widget annotation appearances into page content and removes `/AcroForm`
- **Page tree flattening**: pushes inherited attributes to individual pages for optimal QPDFWriter output
- **Content stream coalescing**: merges multiple content streams per page into one
- **Content stream minification**: normalizes whitespace and trims numeric formatting (trailing/leading zeros)
- **Non-image stream deduplication**: deduplicates identical font, ICC, and other non-image streams via FNV-1a hash + full byte comparison
- **Embedded file stripping**: removes `/EmbeddedFiles` from the document name tree
- **JavaScript removal**: strips `/OpenAction`, `/AA`, and `/JavaScript` from catalog, pages, and annotations
- **Accurate DPI calculation**: content stream CTM matrix parser (~170 lines) tracks `q`/`Q`/`cm`/`Do` operators to find actual rendered image dimensions, with MediaBox fallback

### Fixed

- **Grayscale JPEG inflation**: `convertGrayscaleImages` now skips DCTDecode images — previously replacing JPEG with raw gray + Flate inflated photographic images 2–4× in lossless mode
- **Font subset size comparison**: compares uncompressed sizes instead of uncompressed subset vs Flate-compressed original
- **Stream dedup safety**: `deduplicateStreams` now checks `/DecodeParms` equality — identical raw bytes with different decode parameters produce different content
- **Content stream decoding**: decoded once per page in `subsetFonts` instead of redundantly per font

### Changed

- Split `images.cc` into `images.cc` (image operations) and `optimize.cc` (structural optimizations)
- Added `font_subset.cc`/`font_subset.h` for TrueType binary parsing
- Source: 5 `.cc` files, 4 headers, ~3000 lines total

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
