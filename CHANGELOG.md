# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/), and this project adheres to [Semantic Versioning](https://semver.org/).

## [Unreleased]

## [0.7.0] - 2026-06-27

### Added

- Typed error hierarchy: `QpdfError` with `QpdfFileError`, `QpdfFormatError`, and `QpdfPasswordError` subclasses for actionable compression failures
- `.nvmrc`, `.editorconfig`, and a `packageManager` field for reproducible toolchains
- `typecheck` CI step and a weekly `windows-latest` canary job that re-tests the `windows-2022` pin
- `./package.json` to the package `exports` map

### Changed

- Raised the Node.js `engines` floor to `>=22.0.0`, dropping end-of-life Node 20
- Modernized `tsconfig`: `nodenext` module resolution, `es2023` target, and stricter type-checking (`verbatimModuleSyntax`, `exactOptionalPropertyTypes`, `noUnusedLocals`/`noUnusedParameters`, `noImplicitOverride`, `noFallthroughCasesInSwitch`)
- Reworked ESLint to type-aware linting for `lib/`, adopted the `globals` package, and dropped `eslint-plugin-prettier`; switched coverage to Vitest's v8 provider
- Bumped the native build to C++20 and Node-API 9 (C++ standard defined once via a gyp variable)
- Hardened and sped up CI/CD: pinned all GitHub Actions to commit SHAs, added npm and native-dependency caching, a concurrency group, job timeouts, and top-level least-privilege permissions
- Migrated npm publish to OIDC trusted publishing (dropped `NPM_TOKEN`)
- Switched QPDF's source download to the official byte-stable release tarball; HarfBuzz uses the SHA-256-pinned GitHub git-archive `.tar.gz` (its only release asset is `.tar.xz`, which hangs Windows `tar` during extraction)

### Security

- Pin and verify the SHA-256 of every downloaded native dependency (`scripts/native-deps.json`) via a hardened downloader (timeout, retry, atomic writes, origin pinning), with an `npm run verify:checksums` tripwire
- Added OpenSSF Scorecard scanning, Dependabot updates, and a `SECURITY.md` policy (CodeQL code scanning runs via GitHub's default setup)

### Fixed

- **Windows build**: upgraded node-gyp to 13 and dropped Node 20 from the CI matrix; pinned Windows CI to `windows-2022` to avoid the VS 2026 MSVC internal compiler error (C1001); fixed Windows zlib detection and addon linking

## [0.6.4] - 2026-06-26

### Fixed

- **Double-free on teardown**: fixed a double free of the addon instance data during Node.js environment teardown

## [0.6.3] - 2026-04-26

### Changed

- Build TypeScript on `npm install` via `prepare` script (`husky && tsc`) so `dist/` is always rebuilt from `lib/` after install and on `npm pack`/publish, preventing stale `dist/` drift

## [0.6.2] - 2026-04-11

### Fixed

- **Shared FontFile2 corruption**: fonts sharing the same `/FontDescriptor` and `/FontFile2` are now subset with merged glyph IDs across all referencing font objects, preventing glyph removal needed by sibling fonts
- **Already-subset font corruption**: fonts with `ABCDEF+` subset prefix are now skipped in width zeroing, TrueType/CID subsetting, and `/W` optimization to avoid double-processing
- **Nested XObject font usage**: font usage collection now recursively scans Form XObjects at all nesting levels instead of only the first level
- Removed unreliable unused font removal from `optimizeFonts` (already handled by `removeUnusedResources`)

## [0.6.1] - 2026-04-06

### Fixed

- Statically link libstdc++ on Linux to fix `GLIBCXX_3.4.31 not found` errors on systems with older GCC

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

[Unreleased]: https://github.com/xonaman/nodejs-qpdf-compress/compare/v0.7.0...HEAD
[0.7.0]: https://github.com/xonaman/nodejs-qpdf-compress/compare/v0.6.4...v0.7.0
[0.6.4]: https://github.com/xonaman/nodejs-qpdf-compress/compare/v0.6.3...v0.6.4
[0.6.3]: https://github.com/xonaman/nodejs-qpdf-compress/compare/v0.6.2...v0.6.3
[0.6.2]: https://github.com/xonaman/nodejs-qpdf-compress/compare/v0.6.1...v0.6.2
[0.6.1]: https://github.com/xonaman/nodejs-qpdf-compress/compare/v0.6.0...v0.6.1
[0.6.0]: https://github.com/xonaman/nodejs-qpdf-compress/compare/v0.5.0...v0.6.0
[0.5.0]: https://github.com/xonaman/nodejs-qpdf-compress/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/xonaman/nodejs-qpdf-compress/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/xonaman/nodejs-qpdf-compress/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/xonaman/nodejs-qpdf-compress/compare/v0.1.3...v0.2.0
[0.1.3]: https://github.com/xonaman/nodejs-qpdf-compress/compare/v0.1.2...v0.1.3
[0.1.2]: https://github.com/xonaman/nodejs-qpdf-compress/compare/v0.1.1...v0.1.2
[0.1.1]: https://github.com/xonaman/nodejs-qpdf-compress/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/xonaman/nodejs-qpdf-compress/releases/tag/v0.1.0
