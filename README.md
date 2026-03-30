# qpdf-compress

[![npm version](https://img.shields.io/npm/v/qpdf-compress)](https://www.npmjs.com/package/qpdf-compress)
[![Node.js](https://img.shields.io/node/v/qpdf-compress)](https://nodejs.org)
[![License](https://img.shields.io/npm/l/qpdf-compress)](https://github.com/xonaman/nodejs-qpdf-compress/blob/main/LICENSE)
[![Platform](https://img.shields.io/badge/platform-macOS%20%7C%20Linux%20%7C%20Windows-blue)]()

Native PDF compression for Node.js — powered by [QPDF](https://qpdf.sourceforge.io/), the industry-standard PDF transformation library. Built as a C++ addon with N-API for ABI stability across Node.js versions.

> Designed for server-side workloads. Non-blocking, fast, and production-ready.

## 🚀 Quick Start

```typescript
import { compress } from 'qpdf-compress';

// lossless (default)
const optimized = await compress(pdfBuffer);

// lossy — aggressive quality + downscale to 72 DPI
const smaller = await compress(pdfBuffer, { lossy: true });
```

## 💡 Why qpdf-compress?

**⚡ Performance**

- Native C++ — no WASM overhead, no shell-out to CLI tools
- Non-blocking — all operations run off the main thread via N-API AsyncWorker
- Multi-pass optimization — image dedup, JPEG Huffman optimization, Flate level 9
- Smart defaults — DPI downscaling, metadata stripping, adaptive JPEG quality

**🛠️ Developer experience**

- Single function API — `compress()` handles everything including repair
- Prebuilt binaries — no compile step on supported platforms
- Full TypeScript support with types included
- File path input/output to avoid unnecessary memory copies

**🔒 Reliability**

- Built on QPDF — the most widely used PDF structural transformation library
- ABI-stable via N-API — works across Node.js 18–24 without recompilation
- Automatic repair — damaged PDFs are reconstructed during compression
- Apache-2.0 licensed — no AGPL/copyleft concerns

### 🎯 Use cases

- 📉 Reduce PDF storage costs in S3/cloud storage
- 📤 Shrink PDFs before email delivery or API responses
- 🔧 Repair damaged PDFs from third-party sources
- ⚙️ Build server-side document processing pipelines

### 📊 How it compares

|                           | **qpdf-compress**     | qpdf CLI          | Ghostscript       |
| ------------------------- | --------------------- | ----------------- | ----------------- |
| Integration               | Native Node.js addon  | Shell exec        | Shell exec        |
| Async I/O                 | ✅ Non-blocking       | ❌ Blocks on exec | ❌ Blocks on exec |
| Image deduplication       | ✅                    | ❌                | ❌                |
| JPEG Huffman optimization | ✅ Lossless (libjpeg) | ❌                | ❌                |
| Lossy image compression   | ✅ Auto quality       | ❌                | ✅                |
| CMYK → RGB conversion     | ✅ Automatic          | ❌                | ✅                |
| DPI downscaling           | ✅ Automatic          | ❌                | ✅                |
| Metadata stripping        | ✅ Default on         | ✅ Manual flag    | ✅                |
| Unused font removal       | ✅ Automatic          | ❌                | ❌                |
| PDF repair                | ✅ Automatic          | ✅ Manual flag    | ⚠️ Partial        |
| License                   | Apache-2.0            | Apache-2.0        | AGPL-3.0 ⚠️       |
| Dependencies              | None¹                 | System binary     | System binary     |

¹ QPDF is statically linked — no runtime dependencies. Prebuilt binaries downloaded at install.

## 📦 Install

```bash
npm install qpdf-compress
```

Prebuilt binaries are available for all [supported platforms](#-supported-platforms) — most installs require no compiler. If no prebuilt is available, the package falls back to compiling from source.

### Build prerequisites (source fallback)

- CMake ≥ 3.16
- C++20 compiler (GCC 10+, Clang 13+, MSVC 2019+)
- zlib development headers
- libjpeg-turbo (or libjpeg) development headers

```bash
# macOS
brew install cmake jpeg-turbo

# Ubuntu / Debian
sudo apt install cmake g++ zlib1g-dev libjpeg-turbo8-dev

# Amazon Linux / RHEL
sudo yum install cmake3 gcc-c++ zlib-devel libjpeg-turbo-devel

# Windows (using vcpkg)
vcpkg install zlib libjpeg-turbo --triplet x64-windows-static
```

## 🌍 Supported Platforms

| OS            | Architectures   |
| ------------- | --------------- |
| macOS         | arm64, x64      |
| Linux (glibc) | x64, arm64, arm |
| Linux (musl)  | x64, arm64      |
| Windows       | x64, arm64      |

## 📚 Usage

```typescript
import { compress } from 'qpdf-compress';

// lossless (default) — re-encodes very high quality JPEGs (q91+) at q85, 150 DPI
const optimized = await compress(pdfBuffer);

// lossy — aggressive quality + downscale to 72 DPI
const smaller = await compress(pdfBuffer, { lossy: true });

// keep metadata (stripped by default)
const withMeta = await compress(pdfBuffer, { stripMetadata: false });

// file path input (avoids copying into memory twice)
const result = await compress('/path/to/file.pdf');

// write directly to file instead of returning a Buffer
await compress(pdfBuffer, { output: '/path/to/output.pdf' });

// damaged PDFs are automatically repaired during compression
const fixed = await compress(damagedBuffer);
```

## 📖 API

### `compress(input, options?): Promise<Buffer>`

### `compress(input, options & { output: string }): Promise<void>`

Compresses a PDF document. Automatically repairs damaged PDFs.

| Parameter               | Type               | Description                                                         |
| ----------------------- | ------------------ | ------------------------------------------------------------------- |
| `input`                 | `Buffer \| string` | PDF data or file path                                               |
| `options.lossy`         | `boolean`          | Enable lossy compression. Default: `false`                          |
| `options.stripMetadata` | `boolean`          | Remove XMP metadata, document info, and thumbnails. Default: `true` |
| `options.output`        | `string`           | Write to file path instead of returning a `Buffer`                  |

**Both modes:**

- Deduplicates identical images across pages
- Re-encodes images with auto quality thresholds (see below)
- Optimizes embedded JPEG Huffman tables (2–15% savings, zero quality loss)
- Recompresses all decodable streams with Flate level 9
- Generates object streams for smaller metadata overhead
- Removes unreferenced objects and unused fonts
- Strips XMP metadata, document info, and thumbnails (default: on)
- Converts CMYK and ICCBased color spaces to RGB
- Automatically repairs damaged PDFs

**Lossless (default):**

- Conservative image re-encoding: skips existing JPEGs at q ≤ 90, re-encodes q91+ at q85
- Downscales images to 150 DPI
- Visually indistinguishable from the original

**Lossy** (`lossy: true`):

- Aggressive image re-encoding: skips existing JPEGs at q ≤ 65, encodes the rest at q75
- Downscales images to 72 DPI
- Only replaces images where JPEG is actually smaller
- Skips tiny images (< 50×50 px)

## ⚙️ How it works

This package embeds [QPDF](https://github.com/qpdf/qpdf) (v12.3.2) as a statically linked C++ library, exposed to Node.js via N-API. Lossless JPEG optimization uses [libjpeg-turbo](https://libjpeg-turbo.org/) at the DCT coefficient level. Image recompression in lossy mode also uses libjpeg-turbo for JPEG encoding.

All operations run in a background thread via `Napi::AsyncWorker`, so the event loop is never blocked.

## License

Apache-2.0
