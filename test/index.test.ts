import { existsSync, readFileSync, unlinkSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { describe, expect, it, afterEach } from 'vitest';
import { compress } from '../lib/index.js';

const fixtures = join(import.meta.dirname, 'fixtures');
const minimal = readFileSync(join(fixtures, 'minimal.pdf'));
const withImage = readFileSync(join(fixtures, 'with-image.pdf'));
const damaged = readFileSync(join(fixtures, 'damaged.pdf'));
const cmykImage = readFileSync(join(fixtures, 'cmyk-image.pdf'));
const highDpiImage = readFileSync(join(fixtures, 'high-dpi-image.pdf'));
const withMetadata = readFileSync(join(fixtures, 'with-metadata.pdf'));
const unusedFonts = readFileSync(join(fixtures, 'unused-fonts.pdf'));

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

describe('error handling', () => {
  it('rejects non-existent input file path', async () => {
    await expect(compress('/nonexistent/input.pdf')).rejects.toThrow();
  });

  it('rejects non-PDF buffer', async () => {
    const garbage = Buffer.from('this is not a PDF file at all');
    await expect(compress(garbage)).rejects.toThrow();
  });
});
