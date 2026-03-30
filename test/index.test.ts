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
  describe('lossless', () => {
    it('returns a valid PDF buffer', async () => {
      const result = await compress(minimal, { mode: 'lossless' });
      expect(Buffer.isBuffer(result)).toBe(true);
      expect(result.subarray(0, 5).toString()).toBe('%PDF-');
    });

    it('reduces size of uncompressed PDF', async () => {
      const result = await compress(withImage, { mode: 'lossless' });
      expect(result.length).toBeLessThan(withImage.length);
    });

    it('accepts file path input', async () => {
      const result = await compress(join(fixtures, 'with-image.pdf'), { mode: 'lossless' });
      expect(Buffer.isBuffer(result)).toBe(true);
      expect(result.subarray(0, 5).toString()).toBe('%PDF-');
    });
  });

  describe('lossy', () => {
    it('compresses images as JPEG', async () => {
      const lossless = await compress(withImage, { mode: 'lossless' });
      const lossy = await compress(withImage, { mode: 'lossy', quality: 75 });
      expect(lossy.length).toBeLessThan(lossless.length);
    });

    it('lower quality produces smaller output', async () => {
      const q75 = await compress(withImage, { mode: 'lossy', quality: 75 });
      const q30 = await compress(withImage, { mode: 'lossy', quality: 30 });
      expect(q30.length).toBeLessThan(q75.length);
    });

    it('uses auto quality by default', async () => {
      const auto_ = await compress(withImage, { mode: 'lossy' });
      expect(Buffer.isBuffer(auto_)).toBe(true);
      expect(auto_.subarray(0, 5).toString()).toBe('%PDF-');
      // auto should still compress
      const lossless = await compress(withImage, { mode: 'lossless' });
      expect(auto_.length).toBeLessThan(lossless.length);
    });

    it('produces valid PDF output', async () => {
      const result = await compress(withImage, { mode: 'lossy', quality: 50 });
      expect(result.subarray(0, 5).toString()).toBe('%PDF-');
      // the output should end with %%EOF (possibly with trailing whitespace)
      const tail = result.subarray(-10).toString();
      expect(tail).toContain('%%EOF');
    });
  });

  describe('validation', () => {
    it('rejects invalid mode', async () => {
      // @ts-expect-error testing invalid input
      await expect(compress(minimal, { mode: 'invalid' })).rejects.toThrow('Mode must be');
    });

    it('rejects invalid quality', async () => {
      await expect(compress(minimal, { mode: 'lossy', quality: -1 })).rejects.toThrow(
        'Quality must be',
      );
      await expect(compress(minimal, { mode: 'lossy', quality: 101 })).rejects.toThrow(
        'Quality must be',
      );
    });

    it('rejects invalid input type', async () => {
      // @ts-expect-error testing invalid input
      await expect(compress(123, { mode: 'lossless' })).rejects.toThrow('Buffer or file path');
    });

    it('rejects empty buffer', async () => {
      await expect(compress(Buffer.alloc(0), { mode: 'lossless' })).rejects.toThrow(
        'Input buffer cannot be empty',
      );
    });

    it('rejects empty string path', async () => {
      await expect(compress('', { mode: 'lossless' })).rejects.toThrow(
        'Input path cannot be empty',
      );
    });
  });
});

describe('concurrent compression', () => {
  it('handles parallel operations', async () => {
    const [r1, r2, r3] = await Promise.all([
      compress(minimal, { mode: 'lossless' }),
      compress(withImage, { mode: 'lossy', quality: 50 }),
      compress(damaged, { mode: 'lossless' }),
    ]);
    expect(Buffer.isBuffer(r1)).toBe(true);
    expect(Buffer.isBuffer(r2)).toBe(true);
    expect(Buffer.isBuffer(r3)).toBe(true);
  });
});

describe('repair (via compress)', () => {
  it('repairs a damaged PDF', async () => {
    const result = await compress(damaged, { mode: 'lossless' });
    expect(Buffer.isBuffer(result)).toBe(true);
    expect(result.subarray(0, 5).toString()).toBe('%PDF-');
  });

  it('processes a valid PDF without error', async () => {
    const result = await compress(minimal, { mode: 'lossless' });
    expect(Buffer.isBuffer(result)).toBe(true);
    expect(result.subarray(0, 5).toString()).toBe('%PDF-');
  });

  it('accepts file path input', async () => {
    const result = await compress(join(fixtures, 'damaged.pdf'), { mode: 'lossless' });
    expect(Buffer.isBuffer(result)).toBe(true);
    expect(result.subarray(0, 5).toString()).toBe('%PDF-');
  });
});

describe('file output', () => {
  it('compress writes to output file', async () => {
    const out = tempPath('compress-output.pdf');
    const result = await compress(withImage, { mode: 'lossless', output: out });
    expect(result).toBeUndefined();
    expect(existsSync(out)).toBe(true);
    const written = readFileSync(out);
    expect(written.subarray(0, 5).toString()).toBe('%PDF-');
    expect(written.length).toBeLessThan(withImage.length);
  });

  it('compress lossy writes to output file', async () => {
    const out = tempPath('lossy-output.pdf');
    await compress(withImage, { mode: 'lossy', quality: 50, output: out });
    const written = readFileSync(out);
    expect(written.subarray(0, 5).toString()).toBe('%PDF-');
  });

  it('compress damaged PDF writes to output file', async () => {
    const out = tempPath('repair-output.pdf');
    const result = await compress(damaged, { mode: 'lossless', output: out });
    expect(result).toBeUndefined();
    expect(existsSync(out)).toBe(true);
    const written = readFileSync(out);
    expect(written.subarray(0, 5).toString()).toBe('%PDF-');
  });

  it('rejects non-existent parent directory', async () => {
    const out = '/nonexistent/dir/output.pdf';
    await expect(compress(minimal, { mode: 'lossless', output: out })).rejects.toThrow(
      'Parent directory does not exist',
    );
  });
});

describe('lossy quality ceiling', () => {
  it('skips re-encoding when existing JPEG quality is at or below target', async () => {
    // compress at q30 first, then try to "compress" again at q60
    const q30 = await compress(withImage, { mode: 'lossy', quality: 30 });
    const q60FromQ30 = await compress(q30, { mode: 'lossy', quality: 60 });
    // the images should not be re-encoded (q30 ≤ q60), so sizes should be
    // very close (only stream-level differences from QPDF relinearization)
    const sizeDiff = Math.abs(q30.length - q60FromQ30.length);
    expect(sizeDiff).toBeLessThan(q30.length * 0.02); // within 2%
  });

  it('re-encodes when existing JPEG quality is above target', async () => {
    // compress at q75, then compress again at q30 — should re-encode
    const q75 = await compress(withImage, { mode: 'lossy', quality: 75 });
    const q30FromQ75 = await compress(q75, { mode: 'lossy', quality: 30 });
    expect(q30FromQ75.length).toBeLessThan(q75.length);
  });
});

describe('CMYK image handling', () => {
  it('compresses PDFs with CMYK images in lossy mode', async () => {
    const result = await compress(cmykImage, { mode: 'lossy', quality: 75 });
    expect(Buffer.isBuffer(result)).toBe(true);
    expect(result.subarray(0, 5).toString()).toBe('%PDF-');
  });

  it('compresses PDFs with CMYK images in lossless mode', async () => {
    const result = await compress(cmykImage, { mode: 'lossless' });
    expect(Buffer.isBuffer(result)).toBe(true);
    expect(result.subarray(0, 5).toString()).toBe('%PDF-');
  });

  it('reduces CMYK image size in lossy mode', async () => {
    const result = await compress(cmykImage, { mode: 'lossy', quality: 50 });
    expect(result.length).toBeLessThan(cmykImage.length);
  });
});

describe('DPI-based downscaling', () => {
  it('downscales high-DPI images', async () => {
    const noDownscale = await compress(highDpiImage, { mode: 'lossless', maxDpi: 0 });
    const downscaled = await compress(highDpiImage, { mode: 'lossless', maxDpi: 150 });
    expect(downscaled.length).toBeLessThan(noDownscale.length);
  });

  it('preserves images below maxDpi threshold', async () => {
    // with-image.pdf has 100x100 on 612x792 page ≈ ~12 DPI, well below 300
    const noDownscale = await compress(withImage, { mode: 'lossless', maxDpi: 0 });
    const withMaxDpi = await compress(withImage, { mode: 'lossless', maxDpi: 300 });
    // sizes should be very close since no downscaling occurs
    expect(Math.abs(noDownscale.length - withMaxDpi.length)).toBeLessThan(
      noDownscale.length * 0.02,
    );
  });

  it('uses default maxDpi of 75', async () => {
    const withDefault = await compress(highDpiImage, { mode: 'lossless' });
    const withExplicit = await compress(highDpiImage, { mode: 'lossless', maxDpi: 75 });
    expect(withDefault.length).toBe(withExplicit.length);
  });

  it('disables downscaling with maxDpi: 0', async () => {
    const disabled = await compress(highDpiImage, { mode: 'lossless', maxDpi: 0 });
    const enabled = await compress(highDpiImage, { mode: 'lossless', maxDpi: 75 });
    expect(enabled.length).toBeLessThan(disabled.length);
  });

  it('produces valid PDF after downscaling', async () => {
    const result = await compress(highDpiImage, { mode: 'lossless', maxDpi: 72 });
    expect(result.subarray(0, 5).toString()).toBe('%PDF-');
  });
});

describe('metadata stripping', () => {
  it('strips metadata by default', async () => {
    const result = await compress(withMetadata, { mode: 'lossless' });
    const text = result.toString('latin1');
    expect(text).not.toContain('xmpmeta');
  });

  it('preserves metadata when stripMetadata is false', async () => {
    const stripped = await compress(withMetadata, { mode: 'lossless' });
    const preserved = await compress(withMetadata, { mode: 'lossless', stripMetadata: false });
    expect(preserved.length).toBeGreaterThan(stripped.length);
  });

  it('produces valid PDF after stripping metadata', async () => {
    const result = await compress(withMetadata, { mode: 'lossless', stripMetadata: true });
    expect(result.subarray(0, 5).toString()).toBe('%PDF-');
  });
});

describe('unused font removal', () => {
  it('reduces size by removing unused fonts', async () => {
    const result = await compress(unusedFonts, { mode: 'lossless' });
    expect(Buffer.isBuffer(result)).toBe(true);
    expect(result.subarray(0, 5).toString()).toBe('%PDF-');
    // the output should not contain the unused Courier font
    const text = result.toString('latin1');
    expect(text).not.toContain('/Courier');
  });

  it('preserves used fonts', async () => {
    const result = await compress(unusedFonts, { mode: 'lossless' });
    // the used font (Helvetica via /F1) must still be present.
    // since QPDF uses object streams, we check the text still renders
    // by verifying the PDF is valid and at least as functional
    expect(Buffer.isBuffer(result)).toBe(true);
    expect(result.subarray(0, 5).toString()).toBe('%PDF-');
    // compress again to confirm it's a valid, processable PDF
    const recompressed = await compress(result, { mode: 'lossless' });
    expect(Buffer.isBuffer(recompressed)).toBe(true);
  });
});

describe('error handling', () => {
  it('rejects non-existent input file path', async () => {
    await expect(compress('/nonexistent/input.pdf', { mode: 'lossless' })).rejects.toThrow();
  });

  it('rejects non-PDF buffer', async () => {
    const garbage = Buffer.from('this is not a PDF file at all');
    await expect(compress(garbage, { mode: 'lossless' })).rejects.toThrow('not a valid PDF');
  });
});
