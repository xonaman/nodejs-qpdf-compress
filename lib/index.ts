import { createRequire } from 'node:module';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import type { CompressOptions, NativeAddon } from './types.js';

const require = createRequire(import.meta.url);
const __dirname = dirname(fileURLToPath(import.meta.url));

const addonDir = resolve(__dirname, '..', 'build', 'Release');
if (process.platform === 'win32') {
  process.env.PATH = `${addonDir};${process.env.PATH ?? ''}`;
}

const addon: NativeAddon = require('../build/Release/qpdf_compress.node');

type PdfInput = Buffer | string;

/**
 * Compresses a PDF document. Automatically repairs damaged PDFs.
 *
 * In lossless mode, deduplicates images, optimizes embedded JPEG Huffman
 * tables, recompresses all streams with Flate level 9, generates object
 * streams, and removes unreferenced objects.
 *
 * In lossy mode, additionally recompresses embedded images as JPEG at the
 * specified quality. Text, vectors, and fonts are preserved.
 */
export function compress(
  input: PdfInput,
  options: CompressOptions & { output: string },
): Promise<void>;
export function compress(input: PdfInput, options: CompressOptions): Promise<Buffer>;
export async function compress(input: PdfInput, options: CompressOptions): Promise<Buffer | void> {
  if (Buffer.isBuffer(input)) {
    if (input.length === 0) {
      throw new TypeError('Input buffer cannot be empty');
    }
  } else if (typeof input === 'string') {
    if (input.length === 0) {
      throw new TypeError('Input path cannot be empty');
    }
  } else {
    throw new TypeError('Input must be a Buffer or file path string');
  }
  const mode = options.mode;
  if (mode !== 'lossy' && mode !== 'lossless') {
    throw new TypeError("Mode must be 'lossy' or 'lossless'");
  }
  const quality = options.quality ?? 75;
  if (quality < 1 || quality > 100) {
    throw new RangeError('Quality must be between 1 and 100');
  }
  return addon.compress(input, {
    mode,
    quality,
    ...(options.output ? { output: options.output } : {}),
  }) as Promise<Buffer | void>;
}

export type { CompressOptions } from './types.js';
