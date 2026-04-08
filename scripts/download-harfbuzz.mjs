import { execFileSync } from 'node:child_process';
import { createWriteStream, mkdirSync, existsSync, rmSync, cpSync, readdirSync } from 'node:fs';
import { pipeline } from 'node:stream/promises';
import { Readable } from 'node:stream';
import { join } from 'node:path';

const HARFBUZZ_VERSION = '14.1.0';
const BASE_URL = 'https://github.com/harfbuzz/harfbuzz/archive/refs/tags';

const root = join(import.meta.dirname, '..');
const depsDir = join(root, 'deps', 'harfbuzz');

if (existsSync(join(depsDir, 'include', 'harfbuzz', 'hb-subset.h'))) {
  console.log('HarfBuzz already built, skipping.');
  process.exit(0);
}

// validate version to prevent SSRF
if (!/^\d+\.\d+\.\d+$/.test(HARFBUZZ_VERSION)) {
  console.error(`Invalid HarfBuzz version: ${HARFBUZZ_VERSION}`);
  process.exit(1);
}

const url = `${BASE_URL}/${HARFBUZZ_VERSION}.tar.gz`;
const tarball = join(root, `harfbuzz-${HARFBUZZ_VERSION}.tar.gz`);
const srcDir = join(root, `harfbuzz-${HARFBUZZ_VERSION}`);
const buildDir = join(root, 'build-harfbuzz');

// step 1: download
console.log(`Downloading HarfBuzz ${HARFBUZZ_VERSION}...`);
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

// GitHub archive tarballs extract to `harfbuzz-{tag}` — find it
if (!existsSync(srcDir)) {
  const candidates = readdirSync(root).filter(
    (d) => d.startsWith('harfbuzz-') && !d.endsWith('.tar.gz'),
  );
  const match = candidates.find((d) => d.includes(HARFBUZZ_VERSION));
  if (match) {
    const { renameSync } = await import('node:fs');
    renameSync(join(root, match), srcDir);
    console.log(`Renamed ${match} → harfbuzz-${HARFBUZZ_VERSION}`);
  } else {
    console.error(
      `Could not find extracted HarfBuzz source directory. Found: ${candidates.join(', ')}`,
    );
    process.exit(1);
  }
}

// step 2: build with CMake (minimal static build — subset library only)
console.log('Building HarfBuzz...');
mkdirSync(buildDir, { recursive: true });

const cmakeArgs = [
  '-S',
  srcDir,
  '-B',
  buildDir,
  '-DCMAKE_BUILD_TYPE=Release',
  '-DCMAKE_POSITION_INDEPENDENT_CODE=ON',
  '-DBUILD_SHARED_LIBS=OFF',
  '-DHB_BUILD_SUBSET=ON',
  '-DHB_HAVE_FREETYPE=OFF',
  '-DHB_HAVE_GLIB=OFF',
  '-DHB_HAVE_ICU=OFF',
  '-DHB_HAVE_GOBJECT=OFF',
  '-DHB_BUILD_TESTS=OFF',
  `-DCMAKE_INSTALL_PREFIX=${depsDir}`,
];

if (process.platform === 'linux') {
  cmakeArgs.push('-DCMAKE_C_FLAGS=-fPIC', '-DCMAKE_CXX_FLAGS=-fPIC');
}

if (process.platform === 'win32') {
  const vcpkgRoot = process.env.VCPKG_ROOT || join(process.env.GITHUB_WORKSPACE || '', 'vcpkg');
  if (existsSync(join(vcpkgRoot, 'scripts', 'buildsystems', 'vcpkg.cmake'))) {
    const triplet = process.env.VCPKG_TARGET_TRIPLET || `${process.arch}-windows-static`;
    cmakeArgs.push(
      `-DCMAKE_TOOLCHAIN_FILE=${join(vcpkgRoot, 'scripts', 'buildsystems', 'vcpkg.cmake')}`,
      `-DVCPKG_TARGET_TRIPLET=${triplet}`,
    );
    if (triplet.startsWith('arm64')) {
      cmakeArgs.push('-A', 'arm64');
    }
  }
  cmakeArgs.push('-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded');
  cmakeArgs.splice(cmakeArgs.indexOf('-DCMAKE_BUILD_TYPE=Release'), 1);
}

execFileSync('cmake', cmakeArgs, { stdio: 'inherit' });

const buildArgs = ['--build', buildDir, '--parallel'];
if (process.platform === 'win32') {
  buildArgs.push('--config', 'Release');
}
execFileSync('cmake', buildArgs, { stdio: 'inherit' });

// step 3: install headers and libraries
console.log('Installing to deps/harfbuzz...');
mkdirSync(join(depsDir, 'lib'), { recursive: true });
mkdirSync(join(depsDir, 'include', 'harfbuzz'), { recursive: true });

// copy public headers from source
const srcHeaders = join(srcDir, 'src');
const headerFiles = readdirSync(srcHeaders).filter((f) => f.startsWith('hb') && f.endsWith('.h'));
for (const h of headerFiles) {
  cpSync(join(srcHeaders, h), join(depsDir, 'include', 'harfbuzz', h));
}

// also copy generated config header
const generatedHeader = join(buildDir, 'src', 'config.h');
if (existsSync(generatedHeader)) {
  cpSync(generatedHeader, join(depsDir, 'include', 'harfbuzz', 'config.h'));
}

// copy static libraries — CMake may place them in the build root or src/
const isLibFile = (f) => (f.endsWith('.a') || f.endsWith('.lib')) && f.includes('harfbuzz');

let libsCopied = 0;
const libSearchDirs = [
  buildDir,
  join(buildDir, 'src'),
  join(buildDir, 'Release'),
  join(buildDir, 'src', 'Release'),
];
for (const dir of libSearchDirs) {
  if (!existsSync(dir)) continue;
  const libs = readdirSync(dir).filter(isLibFile);
  for (const lib of libs) {
    cpSync(join(dir, lib), join(depsDir, 'lib', lib));
    console.log(`Copied ${lib} from ${dir}`);
    libsCopied++;
  }
}
if (libsCopied === 0) {
  console.error('No HarfBuzz static libraries found!');
  console.error('Searched:', libSearchDirs.join(', '));
  process.exit(1);
}

// step 4: cleanup
console.log('Cleaning up...');
rmSync(srcDir, { recursive: true, force: true });
rmSync(buildDir, { recursive: true, force: true });

console.log(`HarfBuzz ${HARFBUZZ_VERSION} installed to deps/harfbuzz.`);
