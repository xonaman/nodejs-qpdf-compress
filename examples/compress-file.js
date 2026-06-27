import { readFileSync, statSync } from 'node:fs';
import { compress } from 'qpdf-compress';

// compress a PDF on disk and write the result to a new file
await compress('input.pdf', { output: 'compressed.pdf' });
const before = statSync('input.pdf').size;
const after = statSync('compressed.pdf').size;
console.log(
  `Lossless: ${before} → ${after} bytes (${((1 - after / before) * 100).toFixed(1)}% smaller)`,
);

// compress a Buffer in-memory and get a Buffer back
const input = readFileSync('input.pdf');
const out = await compress(input);
console.log(`In-memory: ${input.length} → ${out.length} bytes`);
