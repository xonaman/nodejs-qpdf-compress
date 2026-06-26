import { createRequire } from 'node:module';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { withConcurrency } from './concurrency.js';
import { parseNativeError } from './errors.js';
import type { CompressOptions, NativeAddon } from './types.js';

const require = createRequire(import.meta.url);
const __dirname = dirname(fileURLToPath(import.meta.url));

const addonDir = resolve(__dirname, '..', 'build', 'Release');
if (process.platform === 'win32') {
  process.env.PATH = `${addonDir};${process.env.PATH ?? ''}`;
}

// The native addon is loaded through createRequire, so its export is `any` at
// the module boundary; assert it to the typed surface once, here.
const addon = require('../build/Release/qpdf_compress.node') as NativeAddon;

type PdfInput = Buffer | string;

/**
 * Compresses a PDF document. Automatically repairs damaged PDFs.
 *
 * By default (lossless), deduplicates images, re-encodes very high quality
 * JPEGs (q91+) at q85, downscales images to 150 DPI, optimizes embedded
 * JPEG Huffman tables, recompresses all streams with Flate level 9,
 * generates object streams, and removes unreferenced objects.
 *
 * With `lossy: true`, uses more aggressive image re-encoding (skips JPEGs
 * at q65 or below, re-encodes the rest at q75) and downscales to 72 DPI.
 * Text, vectors, and fonts are preserved.
 */
export function compress(
  input: PdfInput,
  options: CompressOptions & { output: string },
): Promise<void>;
export function compress(input: PdfInput, options?: CompressOptions): Promise<Buffer>;
export async function compress(input: PdfInput, options?: CompressOptions): Promise<Buffer | void> {
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
  const stripMetadata = options?.stripMetadata ?? true;
  try {
    return await withConcurrency(() =>
      addon.compress(input, {
        ...(options?.lossy ? { lossy: true } : {}),
        ...(stripMetadata ? { stripMetadata: true } : {}),
        ...(options?.output ? { output: options.output } : {}),
      }),
    );
  } catch (err) {
    throw parseNativeError(err);
  }
}

export { concurrency } from './concurrency.js';
export { QpdfError, QpdfFileError, QpdfFormatError, QpdfPasswordError } from './errors.js';
export type { CompressOptions } from './types.js';
