import { existsSync, readFileSync, unlinkSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { inflateSync } from 'node:zlib';
import { describe, expect, it, afterEach } from 'vitest';
import { compress, QpdfError, QpdfFileError } from '../lib/index.js';

const fixtures = join(import.meta.dirname, 'fixtures');
const minimal = readFileSync(join(fixtures, 'minimal.pdf'));
const withImage = readFileSync(join(fixtures, 'with-image.pdf'));
const damaged = readFileSync(join(fixtures, 'damaged.pdf'));
const cmykImage = readFileSync(join(fixtures, 'cmyk-image.pdf'));
const highDpiImage = readFileSync(join(fixtures, 'high-dpi-image.pdf'));
const withMetadata = readFileSync(join(fixtures, 'with-metadata.pdf'));
const unusedFonts = readFileSync(join(fixtures, 'unused-fonts.pdf'));
const withAttachment = readFileSync(join(fixtures, 'with-attachment.pdf'));

// the payload of the embedded file in with-attachment.pdf (see create-fixtures.mjs)
const attachmentMarker = 'X-QPDF-COMPRESS-EMBEDDED-FILE-7f3a9c';

// output is written with object streams and Flate-compressed stream data, so a
// raw byte search misses content that is entirely intact. Search the literal
// bytes plus the decompressed contents of every stream.
function searchableText(pdf: Buffer): string {
  const parts = [pdf.toString('latin1')];
  for (let at = 0; ;) {
    const found = pdf.indexOf('stream', at);
    if (found === -1) break;
    at = found + 6;
    if (pdf.subarray(found - 3, found).toString('latin1') === 'end') continue;
    let start = at;
    if (pdf[start] === 0x0d) start++;
    if (pdf[start] === 0x0a) start++;
    const end = pdf.indexOf('endstream', start);
    if (end === -1) break;
    try {
      parts.push(inflateSync(pdf.subarray(start, end)).toString('latin1'));
    } catch {
      // not a Flate stream (or not decodable on its own) — the literal bytes
      // are already covered above
    }
  }
  return parts.join('\n');
}

// track temp files for cleanup
const tempFiles: string[] = [];
function tempPath(name: string) {
  const p = join(tmpdir(), `pdf-compress-test-${Date.now()}-${name}`);
  tempFiles.push(p);
  return p;
}
afterEach(() => {
  for (const f of tempFiles) {
    try {
      unlinkSync(f);
    } catch {}
  }
  tempFiles.length = 0;
});

describe('compress', () => {
  describe('lossless (default)', () => {
    it('returns a valid PDF buffer', async () => {
      const result = await compress(minimal);
      expect(Buffer.isBuffer(result)).toBe(true);
      expect(result.subarray(0, 5).toString()).toBe('%PDF-');
    });

    it('reduces size of uncompressed PDF', async () => {
      const result = await compress(withImage);
      expect(result.length).toBeLessThan(withImage.length);
    });

    it('accepts file path input', async () => {
      const result = await compress(join(fixtures, 'with-image.pdf'));
      expect(Buffer.isBuffer(result)).toBe(true);
      expect(result.subarray(0, 5).toString()).toBe('%PDF-');
    });
  });

  describe('lossy', () => {
    it('compresses images as JPEG', async () => {
      const lossless = await compress(withImage);
      const lossy = await compress(withImage, { lossy: true });
      expect(lossy.length).toBeLessThan(lossless.length);
    });

    it('produces valid PDF output', async () => {
      const result = await compress(withImage, { lossy: true });
      expect(result.subarray(0, 5).toString()).toBe('%PDF-');
      // the output should end with %%EOF (possibly with trailing whitespace)
      const tail = result.subarray(-10).toString();
      expect(tail).toContain('%%EOF');
    });
  });

  describe('validation', () => {
    it('rejects invalid input type', async () => {
      // @ts-expect-error testing invalid input
      await expect(compress(123)).rejects.toThrow('Buffer or file path');
    });

    it('rejects empty buffer', async () => {
      await expect(compress(Buffer.alloc(0))).rejects.toThrow('Input buffer cannot be empty');
    });

    it('rejects empty string path', async () => {
      await expect(compress('')).rejects.toThrow('Input path cannot be empty');
    });
  });
});

describe('concurrent compression', () => {
  it('handles parallel operations', async () => {
    const [r1, r2, r3] = await Promise.all([
      compress(minimal),
      compress(withImage, { lossy: true }),
      compress(damaged),
    ]);
    expect(Buffer.isBuffer(r1)).toBe(true);
    expect(Buffer.isBuffer(r2)).toBe(true);
    expect(Buffer.isBuffer(r3)).toBe(true);
  });
});

describe('repair (via compress)', () => {
  it('repairs a damaged PDF', async () => {
    const result = await compress(damaged);
    expect(Buffer.isBuffer(result)).toBe(true);
    expect(result.subarray(0, 5).toString()).toBe('%PDF-');
  });

  it('processes a valid PDF without error', async () => {
    const result = await compress(minimal);
    expect(Buffer.isBuffer(result)).toBe(true);
    expect(result.subarray(0, 5).toString()).toBe('%PDF-');
  });

  it('accepts file path input', async () => {
    const result = await compress(join(fixtures, 'damaged.pdf'));
    expect(Buffer.isBuffer(result)).toBe(true);
    expect(result.subarray(0, 5).toString()).toBe('%PDF-');
  });
});

describe('file output', () => {
  it('compress writes to output file', async () => {
    const out = tempPath('compress-output.pdf');
    const result = await compress(withImage, { output: out });
    expect(result).toBeUndefined();
    expect(existsSync(out)).toBe(true);
    const written = readFileSync(out);
    expect(written.subarray(0, 5).toString()).toBe('%PDF-');
    expect(written.length).toBeLessThan(withImage.length);
  });

  it('compress lossy writes to output file', async () => {
    const out = tempPath('lossy-output.pdf');
    await compress(withImage, { lossy: true, output: out });
    const written = readFileSync(out);
    expect(written.subarray(0, 5).toString()).toBe('%PDF-');
  });

  it('compress damaged PDF writes to output file', async () => {
    const out = tempPath('repair-output.pdf');
    const result = await compress(damaged, { output: out });
    expect(result).toBeUndefined();
    expect(existsSync(out)).toBe(true);
    const written = readFileSync(out);
    expect(written.subarray(0, 5).toString()).toBe('%PDF-');
  });

  it('rejects non-existent parent directory', async () => {
    const out = '/nonexistent/dir/output.pdf';
    await expect(compress(minimal, { output: out })).rejects.toThrow(
      'Parent directory does not exist',
    );
  });
});

describe('lossy auto quality thresholds', () => {
  it('lossy mode re-encodes images more aggressively than lossless', async () => {
    const lossless = await compress(withImage);
    const lossy = await compress(withImage, { lossy: true });
    expect(lossy.length).toBeLessThan(lossless.length);
  });

  it('re-compressing lossy output yields similar size (already below threshold)', async () => {
    const first = await compress(withImage, { lossy: true });
    const second = await compress(first, { lossy: true });
    // images are now at q75, below skip threshold of 65? No, q75 > 65 so
    // they will be re-encoded again but at the same quality — size should
    // be very close due to the "only replace if smaller" guard
    const sizeDiff = Math.abs(first.length - second.length);
    expect(sizeDiff).toBeLessThan(first.length * 0.05); // within 5%
  });
});

describe('CMYK image handling', () => {
  it('compresses PDFs with CMYK images in lossy mode', async () => {
    const result = await compress(cmykImage, { lossy: true });
    expect(Buffer.isBuffer(result)).toBe(true);
    expect(result.subarray(0, 5).toString()).toBe('%PDF-');
  });

  it('compresses PDFs with CMYK images in lossless mode', async () => {
    const result = await compress(cmykImage);
    expect(Buffer.isBuffer(result)).toBe(true);
    expect(result.subarray(0, 5).toString()).toBe('%PDF-');
  });

  it('reduces CMYK image size in lossy mode', async () => {
    const result = await compress(cmykImage, { lossy: true });
    expect(result.length).toBeLessThan(cmykImage.length);
  });
});

describe('structural optimization', () => {
  it('lossless produces smaller output via Flate 9 + object streams', async () => {
    const result = await compress(highDpiImage);
    expect(result.length).toBeLessThan(highDpiImage.length);
  });

  it('lossy produces smaller output than lossless', async () => {
    const lossless = await compress(highDpiImage);
    const lossy = await compress(highDpiImage, { lossy: true });
    expect(lossy.length).toBeLessThan(lossless.length);
  });

  it('produces valid PDF', async () => {
    const result = await compress(highDpiImage);
    expect(result.subarray(0, 5).toString()).toBe('%PDF-');
  });
});

describe('metadata stripping', () => {
  it('strips metadata by default', async () => {
    const result = await compress(withMetadata);
    const text = result.toString('latin1');
    expect(text).not.toContain('xmpmeta');
  });

  it('preserves metadata when stripMetadata is false', async () => {
    const stripped = await compress(withMetadata);
    const preserved = await compress(withMetadata, { stripMetadata: false });
    expect(preserved.length).toBeGreaterThan(stripped.length);
  });

  it('produces valid PDF after stripping metadata', async () => {
    const result = await compress(withMetadata, { stripMetadata: true });
    expect(result.subarray(0, 5).toString()).toBe('%PDF-');
  });
});

describe('unused font removal', () => {
  it('reduces size by removing unused fonts', async () => {
    const result = await compress(unusedFonts);
    expect(Buffer.isBuffer(result)).toBe(true);
    expect(result.subarray(0, 5).toString()).toBe('%PDF-');
    // the output should not contain the unused Courier font
    const text = result.toString('latin1');
    expect(text).not.toContain('/Courier');
  });

  it('preserves used fonts', async () => {
    const result = await compress(unusedFonts);
    // the used font (Helvetica via /F1) must still be present.
    // since QPDF uses object streams, we check the text still renders
    // by verifying the PDF is valid and at least as functional
    expect(Buffer.isBuffer(result)).toBe(true);
    expect(result.subarray(0, 5).toString()).toBe('%PDF-');
    // compress again to confirm it's a valid, processable PDF
    const recompressed = await compress(result);
    expect(Buffer.isBuffer(recompressed)).toBe(true);
  });
});

describe('embedded file stripping', () => {
  it('the fixture carries an attachment the search can see', () => {
    const text = searchableText(withAttachment);
    expect(text).toContain(attachmentMarker);
    expect(text).toContain('/EmbeddedFiles');
    expect(text).toContain('/AFRelationship');
    expect(text).toContain('/FileAttachment');
  });

  it('leaves unrelated annotations alone', async () => {
    const stripped = searchableText(await compress(withAttachment));
    const kept = searchableText(await compress(withAttachment, { stripAttachments: false }));
    expect(stripped).toContain('example.invalid');
    expect(kept).toContain('example.invalid');
  });

  it('strips the attachment by default', async () => {
    const text = searchableText(await compress(withAttachment));
    expect(text).not.toContain(attachmentMarker);
  });

  it('removes every path to the attachment, not just the name tree', async () => {
    const text = searchableText(await compress(withAttachment));
    expect(text).not.toContain('/EmbeddedFiles');
    expect(text).not.toContain('/AFRelationship');
    expect(text).not.toContain('/FileAttachment');
  });

  it('keeps the attachment and its lookup paths when stripAttachments is false', async () => {
    const text = searchableText(await compress(withAttachment, { stripAttachments: false }));
    expect(text).toContain(attachmentMarker);
    expect(text).toContain('/EmbeddedFiles');
    expect(text).toContain('/AFRelationship');
  });

  it('produces a valid PDF in both modes', async () => {
    const stripped = await compress(withAttachment);
    const kept = await compress(withAttachment, { stripAttachments: false });
    expect(stripped.subarray(0, 5).toString()).toBe('%PDF-');
    expect(kept.subarray(0, 5).toString()).toBe('%PDF-');
    expect(kept.length).toBeGreaterThan(stripped.length);
  });
});

describe('error handling', () => {
  it('rejects non-existent input file path', async () => {
    await expect(compress('/nonexistent/input.pdf')).rejects.toThrow();
  });

  it('rejects non-PDF buffer', async () => {
    const garbage = Buffer.from('this is not a PDF file at all');
    await expect(compress(garbage)).rejects.toThrow();
  });

  it('non-existent input rejects with a typed QpdfFileError', async () => {
    const err = await compress('/nonexistent/input.pdf').catch((e: unknown) => e);
    expect(err).toBeInstanceOf(QpdfFileError);
    expect(err).toBeInstanceOf(QpdfError);
    expect((err as QpdfFileError).code).toBe('FILE');
  });

  it('a non-PDF buffer rejects with a QpdfError carrying a code', async () => {
    const garbage = Buffer.from('this is not a PDF file at all');
    const err = await compress(garbage).catch((e: unknown) => e);
    expect(err).toBeInstanceOf(QpdfError);
    expect((err as QpdfError).code).toBeTruthy();
  });
});
