import { execFileSync } from 'node:child_process';
import { createWriteStream, mkdirSync, existsSync, rmSync, cpSync, readdirSync } from 'node:fs';
import { pipeline } from 'node:stream/promises';
import { Readable } from 'node:stream';
import { join } from 'node:path';

const QPDF_VERSION = '12.3.2';
const BASE_URL = 'https://github.com/qpdf/qpdf/archive/refs/tags';

const root = join(import.meta.dirname, '..');
const depsDir = join(root, 'deps', 'qpdf');

if (existsSync(join(depsDir, 'include', 'qpdf', 'QPDF.hh'))) {
  console.log('QPDF already built, skipping.');
  process.exit(0);
}

// validate version to prevent SSRF
if (!/^\d+\.\d+\.\d+$/.test(QPDF_VERSION)) {
  console.error(`Invalid QPDF version: ${QPDF_VERSION}`);
  process.exit(1);
}

const url = `${BASE_URL}/v${QPDF_VERSION}.tar.gz`;
const tarball = join(root, `qpdf-${QPDF_VERSION}.tar.gz`);
const srcDir = join(root, `qpdf-${QPDF_VERSION}`);
const buildDir = join(root, 'build-qpdf');

// step 1: download
console.log(`Downloading QPDF ${QPDF_VERSION}...`);
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

// GitHub archive tarballs extract to `qpdf-{tag}` (without 'v' prefix)
// but may also be `qpdf-qpdf-{ver}` depending on repo naming — find it
if (!existsSync(srcDir)) {
  // try common GitHub archive naming patterns
  const candidates = readdirSync(root).filter(
    (d) => d.startsWith('qpdf-') && !d.endsWith('.tar.gz'),
  );
  const match = candidates.find((d) => d.includes(QPDF_VERSION));
  if (match) {
    const { renameSync } = await import('node:fs');
    renameSync(join(root, match), srcDir);
    console.log(`Renamed ${match} → qpdf-${QPDF_VERSION}`);
  } else {
    console.error(
      `Could not find extracted QPDF source directory. Found: ${candidates.join(', ')}`,
    );
    process.exit(1);
  }
}

// step 2: build with CMake
console.log('Building QPDF...');
mkdirSync(buildDir, { recursive: true });

const cmakeArgs = [
  '-S',
  srcDir,
  '-B',
  buildDir,
  '-DCMAKE_BUILD_TYPE=Release',
  '-DCMAKE_POSITION_INDEPENDENT_CODE=ON',
  '-DBUILD_SHARED_LIBS=OFF',
  '-DBUILD_STATIC_LIBS=ON',
  '-DREQUIRE_CRYPTO_NATIVE=ON',
  '-DUSE_IMPLICIT_CRYPTO=OFF',
  '-DBUILD_DOC=OFF',
  `-DCMAKE_INSTALL_PREFIX=${depsDir}`,
];

// on macOS, help CMake find Homebrew libjpeg-turbo
if (process.platform === 'darwin') {
  const brewPrefixes = ['/opt/homebrew', '/usr/local'];
  for (const prefix of brewPrefixes) {
    const jpegDir = join(prefix, 'opt', 'jpeg-turbo');
    if (existsSync(jpegDir)) {
      cmakeArgs.push(`-DCMAKE_PREFIX_PATH=${jpegDir}`);
      break;
    }
    const jpegDir2 = join(prefix, 'opt', 'jpeg');
    if (existsSync(jpegDir2)) {
      cmakeArgs.push(`-DCMAKE_PREFIX_PATH=${jpegDir2}`);
      break;
    }
  }
}

execFileSync('cmake', cmakeArgs, { stdio: 'inherit' });
execFileSync('cmake', ['--build', buildDir, '--parallel', '--target', 'libqpdf'], {
  stdio: 'inherit',
});

// step 3: install headers and library
console.log('Installing to deps/qpdf...');
mkdirSync(join(depsDir, 'lib'), { recursive: true });
mkdirSync(join(depsDir, 'include'), { recursive: true });

// copy headers
cpSync(join(srcDir, 'include', 'qpdf'), join(depsDir, 'include', 'qpdf'), { recursive: true });

// also copy generated config header
const generatedInclude = join(buildDir, 'include', 'qpdf');
if (existsSync(generatedInclude)) {
  cpSync(generatedInclude, join(depsDir, 'include', 'qpdf'), { recursive: true, force: true });
}

// copy static library
const libqpdfDir = join(buildDir, 'libqpdf');
const staticLibs = readdirSync(libqpdfDir).filter(
  (f) => f.startsWith('libqpdf') && (f.endsWith('.a') || f.endsWith('.lib')),
);

if (staticLibs.length === 0) {
  // try Release subdirectory (multi-config generators)
  const releaseDir = join(libqpdfDir, 'Release');
  if (existsSync(releaseDir)) {
    const releaseLibs = readdirSync(releaseDir).filter(
      (f) => f.startsWith('libqpdf') && (f.endsWith('.a') || f.endsWith('.lib')),
    );
    for (const lib of releaseLibs) {
      cpSync(join(releaseDir, lib), join(depsDir, 'lib', lib));
      console.log(`Copied ${lib}`);
    }
  }
} else {
  for (const lib of staticLibs) {
    cpSync(join(libqpdfDir, lib), join(depsDir, 'lib', lib));
    console.log(`Copied ${lib}`);
  }
}

// step 4: clean up source and build dirs
rmSync(srcDir, { recursive: true, force: true });
rmSync(buildDir, { recursive: true, force: true });

console.log(`QPDF ${QPDF_VERSION} installed to ${depsDir}`);
