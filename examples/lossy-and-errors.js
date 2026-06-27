import { readFileSync } from 'node:fs';
import { compress, QpdfError } from 'qpdf-compress';

const input = readFileSync('scanned.pdf');

// lossless (default): dedupe images, optimize streams, keep visual fidelity
const lossless = await compress(input);

// lossy: more aggressive image re-encoding + downscaling to 72 DPI
const lossy = await compress(input, { lossy: true });

console.log(`Original: ${input.length} bytes`);
console.log(`Lossless: ${lossless.length} bytes`);
console.log(`Lossy:    ${lossy.length} bytes`);

// typed error handling — distinguish a corrupt PDF from a missing file, etc.
try {
  await compress(Buffer.from('not a pdf'));
} catch (err) {
  if (err instanceof QpdfError) {
    console.log(`Compression failed [${err.code}]: ${err.message}`);
  }
}
