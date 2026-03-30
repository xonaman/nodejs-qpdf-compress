export interface CompressOptions {
  /** Enable lossy compression for more aggressive size reduction. Default: false. */
  readonly lossy?: boolean;
  /** Remove XMP metadata, document info, and thumbnails. Default: true. */
  readonly stripMetadata?: boolean;
  /** Write to this file path instead of returning a Buffer. */
  readonly output?: string;
}

export interface NativeAddon {
  compress(
    input: Buffer | string,
    options: {
      lossy?: boolean;
      stripMetadata?: boolean;
      output?: string;
    },
  ): Promise<Buffer | undefined>;
}
