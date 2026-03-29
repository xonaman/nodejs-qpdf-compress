/**
 * Creates test PDF fixtures for the test suite.
 * Run with: node test/create-fixtures.mjs
 */
import { writeFileSync, mkdirSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = dirname(fileURLToPath(import.meta.url));
const fixturesDir = join(__dirname, 'fixtures');
mkdirSync(fixturesDir, { recursive: true });

// ---------------------------------------------------------------------------
// 1. Minimal valid PDF (text only)
// ---------------------------------------------------------------------------

function createMinimalPdf() {
  const objects = [];

  // catalog
  objects.push('1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj');
  // pages
  objects.push('2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj');

  // font
  objects.push('4 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj');

  // content stream (draw text)
  const content = 'BT /F1 12 Tf 100 700 Td (Hello, World!) Tj ET';
  objects.push(`5 0 obj\n<< /Length ${content.length} >>\nstream\n${content}\nendstream\nendobj`);

  // page
  objects.push(
    '3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792]\n' +
      '   /Resources << /Font << /F1 4 0 R >> >>\n' +
      '   /Contents 5 0 R >>\nendobj',
  );

  // build the file
  let body = '';
  const offsets = [];
  const header = '%PDF-1.4\n';

  for (const obj of objects) {
    offsets.push(header.length + body.length);
    body += obj + '\n';
  }

  // xref
  const xrefOffset = header.length + body.length;
  let xref = `xref\n0 ${objects.length + 1}\n`;
  xref += '0000000000 65535 f \n';
  for (const off of offsets) {
    xref += String(off).padStart(10, '0') + ' 00000 n \n';
  }

  // trailer
  const trailer = `trailer\n<< /Size ${objects.length + 1} /Root 1 0 R >>\nstartxref\n${xrefOffset}\n%%EOF\n`;

  return Buffer.from(header + body + xref + trailer);
}

// ---------------------------------------------------------------------------
// 2. PDF with an embedded uncompressed RGB image
// ---------------------------------------------------------------------------

function createPdfWithImage() {
  const imgW = 100;
  const imgH = 100;
  const components = 3;
  const pixels = Buffer.alloc(imgW * imgH * components);

  // create a gradient pattern
  for (let y = 0; y < imgH; y++) {
    for (let x = 0; x < imgW; x++) {
      const idx = (y * imgW + x) * 3;
      pixels[idx] = Math.floor((x / imgW) * 255); // R
      pixels[idx + 1] = Math.floor((y / imgH) * 255); // G
      pixels[idx + 2] = 128; // B
    }
  }

  const objects = [];

  objects.push('1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj');
  objects.push('2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj');

  // image XObject (raw uncompressed RGB)
  const imgStream = pixels;
  objects.push(
    `4 0 obj\n<< /Type /XObject /Subtype /Image /Width ${imgW} /Height ${imgH}\n` +
      `   /ColorSpace /DeviceRGB /BitsPerComponent 8 /Length ${imgStream.length} >>\n` +
      `stream\n`,
  );
  // we'll handle stream data separately for binary content

  // content stream (draw image)
  const content = `q ${imgW} 0 0 ${imgH} 100 600 cm /Img1 Do Q`;
  objects.push(`5 0 obj\n<< /Length ${content.length} >>\nstream\n${content}\nendstream\nendobj`);

  // page
  objects.push(
    '3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792]\n' +
      '   /Resources << /XObject << /Img1 4 0 R >> >>\n' +
      '   /Contents 5 0 R >>\nendobj',
  );

  // build manually because we have binary image data
  const header = Buffer.from('%PDF-1.4\n');
  const parts = [];
  const offsets = [];
  let currentOffset = header.length;

  // object 1 — catalog
  const obj1 = Buffer.from('1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n');
  offsets.push(currentOffset);
  parts.push(obj1);
  currentOffset += obj1.length;

  // object 2 — pages
  const obj2 = Buffer.from('2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n');
  offsets.push(currentOffset);
  parts.push(obj2);
  currentOffset += obj2.length;

  // object 4 — image xobject
  const imgHeader = Buffer.from(
    `4 0 obj\n<< /Type /XObject /Subtype /Image /Width ${imgW} /Height ${imgH}\n` +
      `   /ColorSpace /DeviceRGB /BitsPerComponent 8 /Length ${imgStream.length} >>\nstream\n`,
  );
  const imgFooter = Buffer.from('\nendstream\nendobj\n');
  offsets.push(currentOffset);
  parts.push(imgHeader, imgStream, imgFooter);
  currentOffset += imgHeader.length + imgStream.length + imgFooter.length;

  // object 5 — content stream
  const obj5 = Buffer.from(
    `5 0 obj\n<< /Length ${content.length} >>\nstream\n${content}\nendstream\nendobj\n`,
  );
  offsets.push(currentOffset);
  parts.push(obj5);
  currentOffset += obj5.length;

  // object 3 — page
  const obj3 = Buffer.from(
    '3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792]\n' +
      '   /Resources << /XObject << /Img1 4 0 R >> >>\n' +
      '   /Contents 5 0 R >>\nendobj\n',
  );
  offsets.push(currentOffset);
  parts.push(obj3);
  currentOffset += obj3.length;

  // xref
  const xrefOffset = currentOffset;
  // objects are 1,2,4,5,3 → object numbers 1-5
  // we need entries in order 0,1,2,3,4,5
  const objectOffsets = new Array(6).fill(0);
  objectOffsets[1] = offsets[0]; // obj 1
  objectOffsets[2] = offsets[1]; // obj 2
  objectOffsets[4] = offsets[2]; // obj 4
  objectOffsets[5] = offsets[3]; // obj 5
  objectOffsets[3] = offsets[4]; // obj 3

  let xref = `xref\n0 6\n`;
  xref += '0000000000 65535 f \n';
  for (let i = 1; i <= 5; i++) {
    xref += String(objectOffsets[i]).padStart(10, '0') + ' 00000 n \n';
  }

  const trailer = `trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n${xrefOffset}\n%%EOF\n`;
  parts.push(Buffer.from(xref + trailer));

  return Buffer.concat([header, ...parts]);
}

// ---------------------------------------------------------------------------
// 3. Damaged PDF (truncated / bad xref)
// ---------------------------------------------------------------------------

function createDamagedPdf() {
  // take a valid PDF and corrupt the xref
  const valid = createMinimalPdf();
  const str = valid.toString();
  // corrupt the startxref value
  const corrupted = str.replace(/startxref\n\d+/, 'startxref\n999999');
  return Buffer.from(corrupted);
}

// ---------------------------------------------------------------------------
// Write fixtures
// ---------------------------------------------------------------------------

writeFileSync(join(fixturesDir, 'minimal.pdf'), createMinimalPdf());
console.log('Created minimal.pdf');

writeFileSync(join(fixturesDir, 'with-image.pdf'), createPdfWithImage());
console.log('Created with-image.pdf');

writeFileSync(join(fixturesDir, 'damaged.pdf'), createDamagedPdf());
console.log('Created damaged.pdf');

console.log('All fixtures created.');
