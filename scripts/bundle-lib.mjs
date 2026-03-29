import { existsSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { join } from 'node:path';

const root = join(import.meta.dirname, '..');
const outDir = join(root, 'build', 'Release');
const nodeFile = join(outDir, 'qpdf_compress.node');

if (!existsSync(nodeFile)) {
  console.error('qpdf_compress.node not found — run node-gyp rebuild first');
  process.exit(1);
}

/** Strip debug symbols from a binary. */
function strip(file) {
  if (process.platform === 'darwin') {
    // macOS strip can corrupt .node binaries — skip
    console.log(`Skipping strip on macOS (known compatibility issue)`);
    return;
  }
  try {
    execFileSync('strip', ['-s', file], { stdio: 'inherit' });
    console.log(`Stripped ${file.split('/').pop()}`);
  } catch {
    console.warn(`strip failed for ${file.split('/').pop()} — continuing`);
  }
}

strip(nodeFile);
console.log('Bundle complete.');
