# AI Coding Agent Guide

## Repository Overview

**Type**: Native C++ Node.js addon for PDF compression  
**Languages**: C++ (N-API addon), TypeScript (ESM wrapper + types)  
**QPDF**: 12.3.2 (compiled from source, Apache 2.0)  
**Dependencies**: mozjpeg (JPEG optimization + lossy encoding), zlib (Flate compression)  
**Package**: `qpdf-compress`, published to npmjs  
**Size**: ~6 C++ source files, ~2 TS files, 22 tests

Trust these instructions. Only perform additional exploration if information is incomplete.

---

## CRITICAL: Build Commands

### Prerequisites

The native addon requires QPDF compiled from source before building.

```bash
# 1. Install system dependencies
# macOS: brew install cmake nasm
# Ubuntu: sudo apt-get install cmake g++ zlib1g-dev nasm
# Windows: vcpkg install zlib --triplet x64-windows-static

# 2. Install dependencies
npm ci

# 3. Download and compile QPDF
node scripts/download-qpdf.mjs

# 4. Build native addon + strip/bundle
npm run build
```

### Validation Commands

```bash
# Format check (Prettier)
npm run format:check

# Lint check (ESLint with TypeScript)
npm run lint:check

# Tests (Vitest, 20 tests)
npm test
```

### Auto-fix

```bash
npm run format      # Auto-fix formatting
npm run lint        # Auto-fix lint issues
```

---

## Project Structure

| Path                             | Purpose                                                               |
| -------------------------------- | --------------------------------------------------------------------- |
| `src/qpdf_addon.cc`              | C++ addon entry: CompressWorker, N-API bindings                       |
| `src/jpeg.h` / `src/jpeg.cc`     | JPEG helpers: lossless Huffman optimization, lossy encoding (mozjpeg) |
| `src/images.h` / `src/images.cc` | Image processing: dedup, lossy recompress, lossless optimize          |
| `lib/index.ts`                   | Main export, `compress()` function                                    |
| `lib/types.ts`                   | TypeScript interfaces (`CompressOptions`)                             |
| `dist/`                          | Compiled JS + declarations (generated, gitignored)                    |
| `build/Release/`                 | Compiled .node binary (generated, gitignored)                         |
| `deps/qpdf/`                     | Compiled QPDF headers + static lib (gitignored)                       |
| `scripts/download-mozjpeg.mjs`   | Downloads and compiles mozjpeg from source                            |
| `scripts/download-qpdf.mjs`      | Downloads and compiles QPDF from source                               |
| `scripts/install.mjs`            | Prebuild downloader with source fallback                              |
| `scripts/bundle-lib.mjs`         | Strips debug symbols from built addon                                 |
| `test/index.test.ts`             | Vitest tests (lossless, lossy, file output, repair)                   |
| `test/fixtures/`                 | PDF fixtures (minimal, with-image, damaged)                           |
| `binding.gyp`                    | node-gyp build config (macOS/Linux/Windows)                           |

### Generated Files (DO NOT EDIT)

- `dist/` — TypeScript compiler output
- `build/` — node-gyp compiler output
- `deps/` — Compiled QPDF and mozjpeg libraries

---

## Architecture

### C++ Layer

Single async worker exposed to JS via N-API:

- **CompressWorker** (`src/qpdf_addon.cc`): Accepts PDF buffer or file path + options. Runs QPDF pipeline off main thread. Applies Flate 9 compression, object streams, image dedup, JPEG Huffman optimization (lossless) or JPEG recompression via mozjpeg at specified quality (lossy). Returns compressed buffer or writes to output path.

Key patterns:

- **File split**: JPEG helpers in `jpeg.cc`, image processing in `images.cc`, N-API layer in `qpdf_addon.cc`
- **forEachImage helper**: Template in `images.h` that iterates all image XObjects across pages
- **Async execution**: The compress operation runs via `Napi::AsyncWorker` to avoid blocking the event loop
- **Static linking**: QPDF, zlib, and mozjpeg are statically linked into the addon
- **RAII file handles**: Uses `std::unique_ptr<FILE>` with custom deleter for safe file management
- **Integer overflow guards**: Buffer sizes validated before allocation

### TypeScript Layer

- `lib/index.ts` — Single `compress()` export with two overloads (buffer return or file output)
- `lib/types.ts` — `CompressOptions` interface (`mode`, `quality`, `output`)

### API

```typescript
import { compress } from 'qpdf-compress';

// lossless — returns Buffer
const result = await compress(pdfBuffer, { mode: 'lossless' });

// lossy — recompress images at quality 60
const result = await compress(pdfBuffer, { mode: 'lossy', quality: 60 });

// write to file
await compress(pdfBuffer, { mode: 'lossless', output: '/tmp/out.pdf' });
```

---

## Platform Support

Prebuilt binaries for 10 platform/arch combinations:

| Platform            | x64 | arm64 | arm |
| ------------------- | --- | ----- | --- |
| macOS               | ✓   | ✓     |     |
| Linux (glibc)       | ✓   | ✓     | ✓   |
| Linux (musl/Alpine) | ✓   | ✓     |     |
| Windows             | ✓   | ✓     |     |

Falls back to source compilation when no prebuild is available.

---

## CI/CD

- **CI** (`ci.yml`): Runs on push/PR to main. Tests format, lint, build, and test across 3 OS × 3 Node versions
- **Release** (`release.yml`): Triggered by `v*` tags. Builds prebuilds for all platforms, publishes to npm with provenance, creates GitHub release with tarballs
- **Cross-compilation**: QEMU Docker for Linux arm64/arm/musl. vcpkg for Windows static deps
