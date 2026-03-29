# qpdf-compress

Native PDF compression for Node.js, powered by [QPDF](https://qpdf.sourceforge.io/).

## Features

- **Lossless compression** — deduplicates images, optimizes JPEG Huffman tables, recompresses streams with Flate level 9, generates object streams
- **Lossy compression** — additionally recompresses embedded images as JPEG at configurable quality
- **Automatic repair** — reconstructs damaged cross-references and fixes structural issues during compression
- **File output** — write directly to disk to avoid large in-memory buffers
- **Zero runtime dependencies** — QPDF is statically linked
- **Async** — all operations run off the main thread via N-API AsyncWorker

## Installation

```bash
npm install qpdf-compress
```

### Build prerequisites (source fallback)

If no prebuilt binary is available for your platform, the package builds from source. You'll need:

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
```

## Usage

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

## API

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

## How it works

This package embeds [QPDF](https://github.com/qpdf/qpdf) (v12.3.2) as a statically linked C++ library, exposed to Node.js via N-API. Lossless JPEG optimization uses [libjpeg-turbo](https://libjpeg-turbo.org/) at the DCT coefficient level. Image recompression in lossy mode uses [stb_image_write](https://github.com/nothings/stb) for JPEG encoding.

All operations run in a background thread via `Napi::AsyncWorker`, so the event loop is never blocked.

## License

Apache-2.0
