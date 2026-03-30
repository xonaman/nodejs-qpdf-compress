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

// lossless — optimize without touching image quality
const optimized = await compress(pdfBuffer, { mode: 'lossless' });

// lossy — recompress images as JPEG for maximum savings
const smaller = await compress(pdfBuffer, { mode: 'lossy', quality: 50 });
```

## 💡 Why qpdf-compress?

**⚡ Performance**

- Native C++ — no WASM overhead, no shell-out to CLI tools
- Non-blocking — all operations run off the main thread via N-API AsyncWorker
- Multi-pass optimization — image dedup, JPEG Huffman optimization, Flate level 9

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

|                           | **qpdf-compress**       | qpdf CLI          | Ghostscript       |
| ------------------------- | ----------------------- | ----------------- | ----------------- |
| Integration               | Native Node.js addon    | Shell exec        | Shell exec        |
| Async I/O                 | ✅ Non-blocking         | ❌ Blocks on exec | ❌ Blocks on exec |
| Image deduplication       | ✅                      | ❌                | ❌                |
| JPEG Huffman optimization | ✅ Lossless (libjpeg)   | ❌                | ❌                |
| Lossy image compression   | ✅ Configurable quality | ❌                | ✅                |
| PDF repair                | ✅ Automatic            | ✅ Manual flag    | ⚠️ Partial        |
| License                   | Apache-2.0              | Apache-2.0        | AGPL-3.0 ⚠️       |
| Dependencies              | None¹                   | System binary     | System binary     |

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

// lossless — optimize streams without touching image quality
const optimized = await compress(pdfBuffer, { mode: 'lossless' });

// lossy — recompress images as JPEG (default quality: 75)
const smaller = await compress(pdfBuffer, { mode: 'lossy' });

// lossy with custom quality (1–100)
const tiny = await compress(pdfBuffer, { mode: 'lossy', quality: 50 });

// file path input (avoids copying into memory twice)
const result = await compress('/path/to/file.pdf', { mode: 'lossless' });

// write directly to file instead of returning a Buffer
await compress(pdfBuffer, { mode: 'lossless', output: '/path/to/output.pdf' });

// damaged PDFs are automatically repaired during compression
const fixed = await compress(damagedBuffer, { mode: 'lossless' });
```

## 📖 API

### `compress(input, options): Promise<Buffer>`

### `compress(input, options & { output: string }): Promise<void>`

Compresses a PDF document. Automatically repairs damaged PDFs.

| Parameter         | Type                    | Description                                        |
| ----------------- | ----------------------- | -------------------------------------------------- |
| `input`           | `Buffer \| string`      | PDF data or file path                              |
| `options.mode`    | `'lossy' \| 'lossless'` | Compression mode                                   |
| `options.quality` | `number`                | JPEG quality 1–100 (lossy only, default: 75)       |
| `options.output`  | `string`                | Write to file path instead of returning a `Buffer` |

**Lossless mode:**

- Deduplicates identical images across pages
- Optimizes embedded JPEG Huffman tables (2–15% savings, zero quality loss)
- Recompresses all decodable streams with Flate level 9
- Generates object streams for smaller metadata overhead
- Removes unreferenced objects

**Lossy mode** (in addition to lossless optimizations):

- Extracts 8-bit RGB and grayscale images
- Recompresses as JPEG at the specified quality
- Only replaces images where JPEG is actually smaller
- Skips tiny images (< 50×50 px), CMYK, and indexed color

## ⚙️ How it works

This package embeds [QPDF](https://github.com/qpdf/qpdf) (v12.3.2) as a statically linked C++ library, exposed to Node.js via N-API. Lossless JPEG optimization uses [libjpeg-turbo](https://libjpeg-turbo.org/) at the DCT coefficient level. Image recompression in lossy mode also uses libjpeg-turbo for JPEG encoding.

All operations run in a background thread via `Napi::AsyncWorker`, so the event loop is never blocked.

## License

Apache-2.0
