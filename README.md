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

// lossless (default) — pure structural optimization
const optimized = await compress(pdfBuffer);

// lossy — image recompression + downscale to 72 DPI
const smaller = await compress(pdfBuffer, { lossy: true });
```

## 💡 Why qpdf-compress?

**⚡ Performance**

- Native C++ — no WASM overhead, no shell-out to CLI tools
- Non-blocking — all operations run off the main thread via N-API AsyncWorker
- Multi-pass optimization — image dedup, JPEG Huffman optimization, Flate level 9
- Smart defaults — metadata stripping, image dedup, lossless JPEG Huffman optimization

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
| JPEG Huffman optimization | ✅ Lossless (mozjpeg) | ❌                | ❌                |
| Lossy image compression   | ✅ Auto quality       | ❌                | ✅                |
| CMYK → RGB conversion     | ✅ Automatic          | ❌                | ✅                |
| DPI downscaling           | ✅ Lossy mode         | ❌                | ✅                |
| Grayscale detection       | ✅ Automatic          | ❌                | ❌                |
| Bitonal conversion        | ✅ Automatic          | ❌                | ❌                |
| Font subsetting           | ✅ TrueType glyph     | ❌                | ❌                |
| Unused font removal       | ✅ Automatic          | ❌                | ❌                |
| ICC profile stripping     | ✅ Automatic          | ❌                | ❌                |
| Form flattening           | ✅ Automatic          | ❌                | ❌                |
| Stream deduplication      | ✅ Automatic          | ❌                | ❌                |
| Content minification      | ✅ Automatic          | ❌                | ❌                |
| JS/embedded file removal  | ✅ Automatic          | ❌                | ❌                |
| Metadata stripping        | ✅ Default on         | ✅ Manual flag    | ✅                |
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
- nasm (optional, for mozjpeg SIMD acceleration)

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

// lossless (default) — pure structural optimization, no image re-encoding
const optimized = await compress(pdfBuffer);

// lossy — image recompression + downscale to 72 DPI
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

- Deduplicates identical images and non-image streams across pages
- Detects and converts RGB images that are actually grayscale (3× raw data reduction)
- Converts effectively black-and-white grayscale images to 1-bit (8× raw data reduction)
- Optimizes embedded JPEG Huffman tables (2–15% savings, zero quality loss)
- Optimizes soft mask (transparency) JPEG streams
- Removes unused font resources from pages
- Subsets TrueType fonts — strips unused glyph outlines from font programs
- Strips ICC color profiles, replacing with Device equivalents
- Flattens interactive forms (AcroForm) into page content
- Flattens page tree (pushes inherited attributes to pages)
- Coalesces multiple content streams per page into one
- Minifies content streams (whitespace normalization, numeric formatting)
- Strips embedded files and JavaScript actions
- Recompresses all decodable streams with Flate level 9
- Generates object streams for smaller metadata overhead
- Removes unreferenced objects
- Strips XMP metadata, document info, and thumbnails (default: on)
- Automatically repairs damaged PDFs

**Lossless (default):**

- Pure structural optimization — no image re-encoding or downscaling
- Visually identical to the original

**Lossy** (`lossy: true`):

- Re-encodes JPEGs above q65 at q75 (skips images already at or below target)
- Downscales images exceeding 72 DPI using CTM-based rendered size detection
- Converts CMYK and ICCBased color spaces to RGB
- Only replaces images where the result is actually smaller
- Skips tiny images (< 50×50 px)

### `concurrency(value?): number`

Gets or sets the maximum number of concurrent compress operations dispatched to the thread pool.

The default is the number of CPU cores (`os.availableParallelism()`). A value of `0` resets to the default.

Excess calls are queued in JavaScript, preventing libuv thread pool starvation.

```typescript
import { compress, concurrency } from 'qpdf-compress';

concurrency(); // 8 (CPU cores)
concurrency(2); // limit to 2 concurrent operations
concurrency(0); // reset to default
```

---

## ⚙️ How it works

This package embeds [QPDF](https://github.com/qpdf/qpdf) (v12.3.2) and [mozjpeg](https://github.com/mozilla/mozjpeg) (v4.1.1) as statically linked C++ libraries, exposed to Node.js via N-API. Lossless JPEG optimization uses mozjpeg at the DCT coefficient level with progressive scan optimization. Image recompression in lossy mode uses mozjpeg's trellis quantization for 5–15% smaller JPEGs at the same perceptual quality. TrueType font subsetting is handled by a custom binary parser that reads cmap tables, resolves composite glyph dependencies, and rebuilds glyf/loca/hmtx tables with only the used glyphs.

All operations run in a background thread via `Napi::AsyncWorker`, so the event loop is never blocked.

### Compression pipeline (execution order)

1. Deduplicate identical images
2. Convert grayscale RGB images to DeviceGray
3. Convert bitonal grayscale images to 1-bit
4. Flatten page tree (push inherited attributes)
5. _(lossy only)_ Re-encode high-quality JPEGs at q75
6. _(lossy only)_ Downscale images above 72 DPI
7. Optimize existing JPEG Huffman tables
8. Optimize soft mask JPEG streams
9. Remove unused font resources
10. Subset TrueType fonts (strip unused glyphs)
11. Strip ICC color profiles
12. Flatten interactive forms into page content
13. Coalesce multiple content streams per page
14. Minify content streams
15. Deduplicate identical non-image streams
16. Strip embedded files and JavaScript
17. _(optional)_ Strip metadata
18. QPDFWriter: Flate 9, object streams, unreferenced object removal

## License

Apache-2.0
