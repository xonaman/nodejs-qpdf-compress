import { execFileSync } from 'node:child_process';
import { createWriteStream, mkdirSync, existsSync, rmSync, readdirSync } from 'node:fs';
import { pipeline } from 'node:stream/promises';
import { Readable } from 'node:stream';
import { join } from 'node:path';

const MOZJPEG_VERSION = '4.1.1';
const BASE_URL = 'https://github.com/mozilla/mozjpeg/archive/refs/tags';

const root = join(import.meta.dirname, '..');
const depsDir = join(root, 'deps', 'mozjpeg');

if (existsSync(join(depsDir, 'include', 'jpeglib.h'))) {
  console.log('mozjpeg already built, skipping.');
  process.exit(0);
}

// validate version to prevent SSRF
if (!/^\d+\.\d+\.\d+$/.test(MOZJPEG_VERSION)) {
  console.error(`Invalid mozjpeg version: ${MOZJPEG_VERSION}`);
  process.exit(1);
}

const url = `${BASE_URL}/v${MOZJPEG_VERSION}.tar.gz`;
const tarball = join(root, `mozjpeg-${MOZJPEG_VERSION}.tar.gz`);
const srcDir = join(root, `mozjpeg-${MOZJPEG_VERSION}`);
const buildDir = join(root, 'build-mozjpeg');

// step 1: download
console.log(`Downloading mozjpeg ${MOZJPEG_VERSION}...`);
console.log(`URL: ${url}`);

const response = await fetch(url, { redirect: 'follow' });
if (!response.ok) {
  console.error(`Download failed: ${response.status} ${response.statusText}`);
  process.exit(1);
}

mkdirSync(join(root, 'deps'), { recursive: true });
await pipeline(Readable.fromWeb(response.body), createWriteStream(tarball));

console.log('Extracting...');
execFileSync('tar', ['-xzf', tarball, '-C', root], { stdio: 'inherit' });
rmSync(tarball);

// GitHub archive tarballs extract to `mozjpeg-{tag}` — find the directory
if (!existsSync(srcDir)) {
  const candidates = readdirSync(root).filter(
    (d) => d.startsWith('mozjpeg-') && !d.endsWith('.tar.gz'),
  );
  const match = candidates.find((d) => d.includes(MOZJPEG_VERSION));
  if (match) {
    const { renameSync } = await import('node:fs');
    renameSync(join(root, match), srcDir);
    console.log(`Renamed ${match} → mozjpeg-${MOZJPEG_VERSION}`);
  } else {
    console.error(
      `Could not find extracted mozjpeg source directory. Found: ${candidates.join(', ')}`,
    );
    process.exit(1);
  }
}

// step 2: build with CMake
console.log('Building mozjpeg...');
mkdirSync(buildDir, { recursive: true });

const cmakeArgs = [
  '-S',
  srcDir,
  '-B',
  buildDir,
  '-DCMAKE_BUILD_TYPE=Release',
  '-DCMAKE_POSITION_INDEPENDENT_CODE=ON',
  '-DCMAKE_POLICY_VERSION_MINIMUM=3.5',
  '-DENABLE_STATIC=ON',
  '-DENABLE_SHARED=OFF',
  '-DPNG_SUPPORTED=OFF',
  '-DWITH_TURBOJPEG=OFF',
  `-DCMAKE_INSTALL_PREFIX=${depsDir}`,
];

// force -fPIC on Linux
if (process.platform === 'linux') {
  cmakeArgs.push('-DCMAKE_C_FLAGS=-fPIC', '-DCMAKE_CXX_FLAGS=-fPIC');
}

// match node-gyp's deployment target on macOS to avoid linker warnings
if (process.platform === 'darwin') {
  cmakeArgs.push('-DCMAKE_OSX_DEPLOYMENT_TARGET=11.0');
}

// Windows multi-config generator
if (process.platform === 'win32') {
  cmakeArgs.push('-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded');
  cmakeArgs.splice(cmakeArgs.indexOf('-DCMAKE_BUILD_TYPE=Release'), 1);
}

execFileSync('cmake', cmakeArgs, { stdio: 'inherit' });

const buildArgs = ['--build', buildDir, '--parallel'];
if (process.platform === 'win32') {
  buildArgs.push('--config', 'Release');
}
execFileSync('cmake', buildArgs, { stdio: 'inherit' });

// step 3: install
console.log('Installing to deps/mozjpeg...');
const installArgs = ['--install', buildDir];
if (process.platform === 'win32') {
  installArgs.push('--config', 'Release');
}
execFileSync('cmake', installArgs, { stdio: 'inherit' });

// verify installation
if (!existsSync(join(depsDir, 'include', 'jpeglib.h'))) {
  console.error('mozjpeg install failed: jpeglib.h not found');
  process.exit(1);
}

// find the static library (varies by platform and install layout)
const libDirs = ['lib', 'lib64'].map((d) => join(depsDir, d));
const libNames = ['libjpeg.a', 'jpeg-static.lib', 'jpeg.lib'];
let foundLib = false;
for (const dir of libDirs) {
  if (!existsSync(dir)) continue;
  for (const name of libNames) {
    if (existsSync(join(dir, name))) {
      foundLib = true;
      console.log(`Found static library: ${join(dir, name)}`);
      break;
    }
  }
  if (foundLib) break;
}

if (!foundLib) {
  // list what we have for debugging
  for (const dir of libDirs) {
    if (existsSync(dir)) {
      console.log(`Contents of ${dir}:`, readdirSync(dir));
    }
  }
  console.error('mozjpeg install failed: static library not found');
  process.exit(1);
}

// step 4: clean up source and build dirs
rmSync(srcDir, { recursive: true, force: true });
rmSync(buildDir, { recursive: true, force: true });

console.log(`mozjpeg ${MOZJPEG_VERSION} installed to ${depsDir}`);
