/**
 * Try to download a prebuilt binary from GitHub releases.
 * Falls back to compiling from source if no prebuilt is available.
 *
 * Prebuilt tarball naming: qpdf-compress-v{version}-{platform}-{arch}.tar.gz
 * Musl (Alpine) naming:    qpdf-compress-v{version}-linux-musl-{arch}.tar.gz
 * Contents: build/Release/qpdf_compress.node
 */
import { execFileSync, execSync } from 'node:child_process';
import { createWriteStream, existsSync, mkdirSync, readFileSync, unlinkSync } from 'node:fs';
import { join } from 'node:path';
import { Readable } from 'node:stream';
import { pipeline } from 'node:stream/promises';

const root = join(import.meta.dirname, '..');
const pkg = JSON.parse(readFileSync(join(root, 'package.json'), 'utf8'));
const version = pkg.version;

// validate version to prevent SSRF via malicious package.json
if (!/^\d+\.\d+\.\d+(-[a-zA-Z0-9.]+)?$/.test(version)) {
  console.error(`Invalid version format: ${version}`);
  process.exit(1);
}

// detect musl libc (Alpine, Void, etc.)
function isMusl() {
  if (process.platform !== 'linux') return false;
  try {
    const ldd = execSync('ldd --version 2>&1 || true', { encoding: 'utf8' });
    return ldd.toLowerCase().includes('musl');
  } catch {
    return (
      existsSync('/lib/ld-musl-x86_64.so.1') ||
      existsSync('/lib/ld-musl-aarch64.so.1') ||
      existsSync('/lib/ld-musl-armhf.so.1')
    );
  }
}

const platform = process.platform;
const arch = process.arch;
const musl = isMusl();
const platformKey = musl ? `${platform}-musl` : platform;
const tarName = `qpdf-compress-v${version}-${platformKey}-${arch}.tar.gz`;

// hardcoded origin ensures the URL can never point to an attacker-controlled host
const releaseOrigin = 'https://github.com';
const releasePath = `/xonaman/nodejs-qpdf-compress/releases/download/v${version}/${tarName}`;
const releaseUrl = new URL(releasePath, releaseOrigin);

const outDir = join(root, 'build', 'Release');

async function tryDownload() {
  console.log(`Checking for prebuilt binary: ${tarName}`);

  try {
    if (releaseUrl.origin !== releaseOrigin) {
      throw new Error(`Unexpected URL origin: ${releaseUrl.origin}`);
    }
    const res = await fetch(releaseUrl, { redirect: 'follow' });
    if (!res.ok) {
      console.log(`No prebuilt binary found (HTTP ${res.status}), will compile from source.`);
      return false;
    }

    mkdirSync(outDir, { recursive: true });
    const tmpTar = join(root, tarName);

    // download to temp file
    const fileStream = createWriteStream(tmpTar);
    const body = res.body;
    if (!body) return false;

    await pipeline(Readable.fromWeb(body), fileStream);

    // extract tar.gz into project root
    execFileSync('tar', ['xzf', tmpTar, '-C', root], { stdio: 'inherit' });
    unlinkSync(tmpTar);

    // verify the .node file exists
    const nodeFile = join(outDir, 'qpdf_compress.node');
    if (existsSync(nodeFile)) {
      console.log('Prebuilt binary installed successfully.');
      return true;
    }

    console.log(
      'Prebuilt archive extracted but qpdf_compress.node not found, will compile from source.',
    );
    return false;
  } catch (err) {
    console.log(`Failed to download prebuilt binary: ${err.message}`);
    return false;
  }
}

async function buildFromSource() {
  console.log('Building from source...');
  execSync('node scripts/download-mozjpeg.mjs', { stdio: 'inherit', cwd: root });
  execSync('node scripts/download-qpdf.mjs', { stdio: 'inherit', cwd: root });
  execSync('node scripts/download-harfbuzz.mjs', { stdio: 'inherit', cwd: root });
  execSync('npx node-gyp rebuild', { stdio: 'inherit', cwd: root });
  execSync('node scripts/bundle-lib.mjs', { stdio: 'inherit', cwd: root });
}

const nodeFile = join(outDir, 'qpdf_compress.node');
if (existsSync(nodeFile)) {
  console.log('qpdf_compress.node already exists, skipping install.');
  process.exit(0);
}

const downloaded = await tryDownload();
if (!downloaded) {
  await buildFromSource();
}
