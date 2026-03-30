export interface CompressOptions {
  /** Compression mode. */
  readonly mode: 'lossy' | 'lossless';
  /** JPEG quality for lossy mode (1–100). When omitted, automatically determines optimal quality per image (capped at 85). */
  readonly quality?: number;
  /** Maximum image DPI. Images exceeding this are downscaled. 0 = no limit. Default: 75. */
  readonly maxDpi?: number;
  /** Remove XMP metadata, document info, and thumbnails. Default: true. */
  readonly stripMetadata?: boolean;
  /** Write to this file path instead of returning a Buffer. */
  readonly output?: string;
}

export interface NativeAddon {
  compress(
    input: Buffer | string,
    options: {
      mode: 'lossy' | 'lossless';
      quality: number; // 0 = auto, 1–100 = fixed
      maxDpi?: number;
      stripMetadata?: boolean;
      output?: string;
    },
  ): Promise<Buffer | undefined>;
}
