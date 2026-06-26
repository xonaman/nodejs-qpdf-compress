/** Base error for all qpdf-compress operations. */
export class QpdfError extends Error {
  readonly code: string;
  constructor(code: string, message: string) {
    super(message);
    this.name = 'QpdfError';
    this.code = code;
  }
}

/** Thrown when an input or output file cannot be read or written. */
export class QpdfFileError extends QpdfError {
  constructor(message: string) {
    super('FILE', message);
    this.name = 'QpdfFileError';
  }
}

/** Thrown when the input is not a valid PDF or is too damaged to recover. */
export class QpdfFormatError extends QpdfError {
  constructor(message: string) {
    super('FORMAT', message);
    this.name = 'QpdfFormatError';
  }
}

/** Thrown when the PDF is encrypted and cannot be opened without a password. */
export class QpdfPasswordError extends QpdfError {
  constructor(message: string) {
    super('PASSWORD', message);
    this.name = 'QpdfPasswordError';
  }
}

/** Decode the native "CODE:message" convention into a typed error. */
export function parseNativeError(err: unknown): QpdfError {
  const msg = err instanceof Error ? err.message : String(err);
  const colonIdx = msg.indexOf(':');
  if (colonIdx === -1) return new QpdfError('UNKNOWN', msg);

  const code = msg.slice(0, colonIdx);
  const text = msg.slice(colonIdx + 1);

  switch (code) {
    case 'FILE':
      return new QpdfFileError(text);
    case 'FORMAT':
      return new QpdfFormatError(text);
    case 'PASSWORD':
      return new QpdfPasswordError(text);
    default:
      return new QpdfError(code, text);
  }
}
