# Third-Party Notices

`qpdf-compress` is distributed under the [Apache-2.0](./LICENSE) license. The
published package and its prebuilt native binaries statically link the
third-party libraries listed below. Each remains under its own license, and the
full license text of each is available from the upstream project linked.

| Library  | Version            | License                                                   | Source                                 |
| -------- | ------------------ | --------------------------------------------------------- | -------------------------------------- |
| QPDF     | 12.3.2             | Apache-2.0                                                | <https://github.com/qpdf/qpdf>         |
| mozjpeg  | 4.1.1              | BSD-3-Clause and IJG (with zlib-licensed SIMD components) | <https://github.com/mozilla/mozjpeg>   |
| HarfBuzz | 14.2.1             | MIT ("Old MIT")                                           | <https://github.com/harfbuzz/harfbuzz> |
| zlib     | bundled on Windows | zlib License                                              | <https://github.com/madler/zlib>       |

## Details

### QPDF

Copyright Jay Berkenbilt and QPDF contributors. Licensed under the Apache
License, Version 2.0. QPDF performs the structural PDF transformation and
compression. See <https://github.com/qpdf/qpdf/blob/main/LICENSE.txt>.

### mozjpeg

Copyright the Mozilla Foundation and contributors; derived from libjpeg-turbo
and the Independent JPEG Group (IJG) library. mozjpeg is covered by three
licenses: the IJG License, a BSD-3-Clause-style license for libjpeg-turbo
modifications, and the zlib License for the SIMD extensions. mozjpeg is used for
lossless JPEG Huffman optimization and lossy JPEG re-encoding. See
<https://github.com/mozilla/mozjpeg/blob/master/LICENSE.md>.

### HarfBuzz

Copyright the HarfBuzz contributors. Licensed under the "Old MIT" license. Used
for TrueType/OpenType font handling and subsetting (`harfbuzz` and
`harfbuzz-subset`). See
<https://github.com/harfbuzz/harfbuzz/blob/main/COPYING>.

### zlib

Copyright Jean-loup Gailly and Mark Adler. Licensed under the zlib License.
zlib provides DEFLATE compression and is statically linked into the Windows
build (via vcpkg); on macOS and Linux the system zlib is used at build time.
See <https://github.com/madler/zlib/blob/master/LICENSE>.
