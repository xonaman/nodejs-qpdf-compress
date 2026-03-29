export interface CompressOptions {
  /** Compression mode. */
  readonly mode: 'lossy' | 'lossless';
  /** JPEG quality for lossy mode (1–100). Default: 75. */
  readonly quality?: number;
  /** Write to this file path instead of returning a Buffer. */
  readonly output?: string;
}

export interface NativeAddon {
  compress(
    input: Buffer | string,
    options: { mode: 'lossy' | 'lossless'; quality: number; output?: string },
  ): Promise<Buffer | undefined>;
}
