#!/usr/bin/env node
/**
 * Validates the PDF/A conformance of compress() output against a recorded baseline.
 *
 * veraPDF is the reference PDF/A validator. It is GPL-3.0 / MPL-2.0, so it is neither
 * vendored nor linked — it runs as an external tool in its own container, pinned by
 * digest so a verdict cannot drift under us when the image is rebuilt.
 *
 * The baseline records which clauses each variant fails today. Any difference, in
 * either direction, fails the run: a regression and an improvement both need the
 * baseline updated deliberately (`--update`) rather than silently absorbed.
 *
 * Usage: npm run verify:pdfa [-- --update]
 */
import { execFileSync } from 'node:child_process';
import { mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import { compress } from '../dist/index.js';

const __dirname = dirname(fileURLToPath(import.meta.url));
const root = resolve(__dirname, '..');
const baselinePath = join(__dirname, 'pdfa-baseline.json');
const baseline = JSON.parse(readFileSync(baselinePath, 'utf8'));
const update = process.argv.includes('--update');

// the work directory must sit inside the repo: Docker Desktop shares the user's
// own tree, not every host temp path
const workDir = join(root, '.cache', 'pdfa');

const variants = {
  original: null,
  default: {},
  'keep-attachments': { stripAttachments: false },
  'keep-both': { stripAttachments: false, stripMetadata: false },
};

function veraPdf(name) {
  const args = [
    'run',
    '--rm',
    '--platform',
    'linux/amd64',
    '-v',
    `${workDir}:/data`,
    baseline.image,
    '--flavour',
    baseline.flavour,
    '--format',
    'mrr',
    `/data/${name}.pdf`,
  ];
  let xml;
  try {
    xml = execFileSync('docker', args, { encoding: 'utf8', maxBuffer: 256 * 1024 * 1024 });
  } catch (err) {
    // veraPDF exits non-zero for a non-compliant file; the report is still on stdout
    if (typeof err.stdout !== 'string' || !err.stdout.includes('<validationReport')) throw err;
    xml = err.stdout;
  }
  const compliant = /<validationReport[^>]*isCompliant="true"/.test(xml);
  const failed = [
    ...new Set(
      [
        ...xml.matchAll(
          /<rule\b[^>]*\bclause="([^"]+)"[^>]*\btestNumber="([^"]+)"[^>]*\bstatus="failed"/g,
        ),
      ].map((m) => `${m[1]}-${m[2]}`),
    ),
  ].sort();
  return { compliant, failedRules: failed };
}

try {
  execFileSync('docker', ['version', '--format', '{{.Server.Version}}'], { stdio: 'ignore' });
} catch {
  console.error('Docker is required to run veraPDF. Start Docker and try again.');
  process.exit(1);
}

rmSync(workDir, { recursive: true, force: true });
mkdirSync(workDir, { recursive: true });

const fixture = readFileSync(join(root, 'test', 'fixtures', baseline.fixture));
writeFileSync(join(workDir, 'original.pdf'), fixture);
for (const [name, options] of Object.entries(variants)) {
  if (!options) continue;
  await compress(fixture, { ...options, output: join(workDir, `${name}.pdf`) });
}

const results = {};
let failures = 0;

for (const name of Object.keys(variants)) {
  const actual = veraPdf(name);
  results[name] = actual;

  const expected = baseline.variants[name];
  const same =
    expected &&
    expected.compliant === actual.compliant &&
    expected.failedRules.join() === actual.failedRules.join();

  const verdict = actual.compliant ? 'PASS' : `FAIL (${actual.failedRules.length})`;
  console.log(
    `${same || update ? ' ok ' : 'DIFF'}  ${name.padEnd(17)} ${verdict.padEnd(9)} ${actual.failedRules.join(' ')}`,
  );

  if (!same && !update) {
    failures++;
    console.log(
      `      expected: ${expected ? `${expected.compliant ? 'PASS' : 'FAIL'} ${expected.failedRules.join(' ')}` : '(no baseline entry)'}`,
    );
  }
}

if (update) {
  writeFileSync(baselinePath, `${JSON.stringify({ ...baseline, variants: results }, null, 2)}\n`);
  // keep the committed file canonical so the pre-commit hook has nothing to say
  try {
    execFileSync('npx', ['prettier', '--write', baselinePath], { stdio: 'ignore' });
  } catch {
    // prettier is a devDependency; a missing one is not worth failing the run over
  }
  console.log(`\nBaseline updated: ${baselinePath}`);
  process.exit(0);
}

if (failures) {
  console.error(
    `\n${failures} variant(s) differ from the baseline. Re-run with --update once the change is intended.`,
  );
  process.exit(1);
}
console.log('\nAll variants match the recorded baseline.');
