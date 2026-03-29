import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const require = createRequire(import.meta.url);
const __dirname = dirname(fileURLToPath(import.meta.url));
const root = join(__dirname, '..');

const { compress, repair } = require(join(root, 'build/Release/qpdf_native.node'));

const img = readFileSync(join(root, 'test/fixtures/with-image.pdf'));
console.log('with-image.pdf input:', img.length, 'bytes');

const lossless = await compress(img, { mode: 'lossless' });
console.log('  lossless:', lossless.length, 'bytes');

const lossy = await compress(img, { mode: 'lossy', quality: 75 });
console.log('  lossy q75:', lossy.length, 'bytes');

const lossy50 = await compress(img, { mode: 'lossy', quality: 50 });
console.log('  lossy q50:', lossy50.length, 'bytes');

const damaged = readFileSync(join(root, 'test/fixtures/damaged.pdf'));
const repaired = await repair(damaged);
console.log('damaged.pdf:', damaged.length, '-> repaired:', repaired.length, 'bytes');

const fromPath = await compress(join(root, 'test/fixtures/minimal.pdf'), { mode: 'lossless' });
console.log('file path input:', fromPath.length, 'bytes');

console.log('All OK!');
