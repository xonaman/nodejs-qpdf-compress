#pragma once

#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <jpeglib.h>

// custom error manager — longjmp on fatal errors instead of exit()
struct JpegErrorMgr {
  struct jpeg_error_mgr pub;
  std::jmp_buf jmpbuf;
};

void jpegErrorExit(j_common_ptr cinfo);
void jpegSilenceOutput(j_common_ptr cinfo);

// rewrites Huffman tables at DCT coefficient level (2–15% savings, zero quality
// loss)
bool losslessJpegOptimize(const unsigned char *data, size_t size,
                          std::vector<uint8_t> &out);

// encodes raw pixels as JPEG at the given quality (1–100) via mozjpeg
bool encodeJpeg(const unsigned char *pixels, int width, int height,
                int components, int quality, std::vector<uint8_t> &out);

// estimates the IJG quality factor (1–100) from a JPEG's quantization tables.
// returns -1 if the quality cannot be determined (corrupt, non-standard tables)
int estimateJpegQuality(const unsigned char *data, size_t size);
