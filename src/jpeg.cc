#include "jpeg.h"

#include <cstdlib>
#include <limits>

void jpegErrorExit(j_common_ptr cinfo) {
  auto *myerr = reinterpret_cast<JpegErrorMgr *>(cinfo->err);
  std::longjmp(myerr->jmpbuf, 1);
}

// ---------------------------------------------------------------------------
// Lossless JPEG optimization
// ---------------------------------------------------------------------------

// isolated setjmp scope — no C++ objects with non-trivial destructors
static bool losslessJpegOptimizeImpl(const unsigned char *data, size_t size,
                                     unsigned char **outbuf,
                                     unsigned long *outsize) {
  struct jpeg_decompress_struct srcinfo = {};
  struct jpeg_compress_struct dstinfo = {};
  JpegErrorMgr jerr = {};

  srcinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = jpegErrorExit;
  dstinfo.err = &jerr.pub;

  if (setjmp(jerr.jmpbuf)) {
    jpeg_destroy_decompress(&srcinfo);
    jpeg_destroy_compress(&dstinfo);
    return false;
  }

  jpeg_create_decompress(&srcinfo);
  jpeg_create_compress(&dstinfo);

  jpeg_mem_src(&srcinfo, data, static_cast<unsigned long>(size));

  if (jpeg_read_header(&srcinfo, TRUE) != JPEG_HEADER_OK) {
    jpeg_destroy_decompress(&srcinfo);
    jpeg_destroy_compress(&dstinfo);
    return false;
  }

  // read DCT coefficients — zero quality loss
  jvirt_barray_ptr *coef_arrays = jpeg_read_coefficients(&srcinfo);
  if (!coef_arrays) {
    jpeg_destroy_decompress(&srcinfo);
    jpeg_destroy_compress(&dstinfo);
    return false;
  }

  *outsize = 0;
  jpeg_mem_dest(&dstinfo, outbuf, outsize);

  jpeg_copy_critical_parameters(&srcinfo, &dstinfo);
  dstinfo.optimize_coding = TRUE;

  // mozjpeg progressive scan optimization — lossless, reorders existing DCT
  // coefficients into optimized progressive scans for better entropy coding
  jpeg_simple_progression(&dstinfo);

  jpeg_write_coefficients(&dstinfo, coef_arrays);
  jpeg_finish_compress(&dstinfo);
  jpeg_finish_decompress(&srcinfo);

  jpeg_destroy_compress(&dstinfo);
  jpeg_destroy_decompress(&srcinfo);

  return *outbuf != nullptr && *outsize > 0;
}

bool losslessJpegOptimize(const unsigned char *data, size_t size,
                          std::vector<uint8_t> &out) {
  unsigned char *outbuf = nullptr;
  unsigned long outsize = 0;

  bool ok = losslessJpegOptimizeImpl(data, size, &outbuf, &outsize);
  if (ok && outbuf && outsize > 0) {
    out.assign(outbuf, outbuf + outsize);
  }
  free(outbuf);
  return ok;
}

// ---------------------------------------------------------------------------
// Lossy JPEG encoding via mozjpeg
// ---------------------------------------------------------------------------

// isolated setjmp scope
static bool encodeJpegImpl(const unsigned char *pixels, int width, int height,
                           int components, int quality, unsigned char **outbuf,
                           unsigned long *outsize) {
  struct jpeg_compress_struct cinfo = {};
  JpegErrorMgr jerr = {};

  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = jpegErrorExit;

  if (setjmp(jerr.jmpbuf)) {
    jpeg_destroy_compress(&cinfo);
    return false;
  }

  jpeg_create_compress(&cinfo);

  *outsize = 0;
  jpeg_mem_dest(&cinfo, outbuf, outsize);

  cinfo.image_width = static_cast<JDIMENSION>(width);
  cinfo.image_height = static_cast<JDIMENSION>(height);
  cinfo.input_components = components;
  if (components != 1 && components != 3) {
    jpeg_destroy_compress(&cinfo);
    return false;
  }
  cinfo.in_color_space = (components == 3) ? JCS_RGB : JCS_GRAYSCALE;

  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, quality, TRUE);
  cinfo.optimize_coding = TRUE;

  // mozjpeg trellis quantization — 5–15% smaller at same perceptual quality
  jpeg_c_set_bool_param(&cinfo, JBOOLEAN_TRELLIS_QUANT, TRUE);
  jpeg_c_set_bool_param(&cinfo, JBOOLEAN_TRELLIS_QUANT_DC, TRUE);
  jpeg_c_set_bool_param(&cinfo, JBOOLEAN_OVERSHOOT_DERINGING, TRUE);
  jpeg_c_set_bool_param(&cinfo, JBOOLEAN_USE_SCANS_IN_TRELLIS, TRUE);
  jpeg_c_set_bool_param(&cinfo, JBOOLEAN_USE_LAMBDA_WEIGHT_TBL, TRUE);

  // mozjpeg progressive scan optimization
  jpeg_simple_progression(&cinfo);

  jpeg_start_compress(&cinfo, TRUE);

  int row_stride = width * components;
  while (cinfo.next_scanline < cinfo.image_height) {
    JSAMPROW row =
        const_cast<JSAMPROW>(pixels + cinfo.next_scanline * row_stride);
    jpeg_write_scanlines(&cinfo, &row, 1);
  }

  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);

  return *outbuf != nullptr && *outsize > 0;
}

bool encodeJpeg(const unsigned char *pixels, int width, int height,
                int components, int quality, std::vector<uint8_t> &out) {
  unsigned char *outbuf = nullptr;
  unsigned long outsize = 0;

  bool ok = encodeJpegImpl(pixels, width, height, components, quality, &outbuf,
                           &outsize);
  if (ok && outbuf && outsize > 0) {
    out.assign(outbuf, outbuf + outsize);
  }
  free(outbuf);
  return ok;
}

// ---------------------------------------------------------------------------
// JPEG quality estimation from quantization tables
// ---------------------------------------------------------------------------

// standard IJG luminance quantization table (quality 50 baseline)
static const unsigned int std_luminance_qt[64] = {
    16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
    14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
    18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};

// isolated setjmp scope for reading JPEG header
static int estimateJpegQualityImpl(const unsigned char *data, size_t size) {
  struct jpeg_decompress_struct cinfo = {};
  JpegErrorMgr jerr = {};

  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = jpegErrorExit;

  if (setjmp(jerr.jmpbuf)) {
    jpeg_destroy_decompress(&cinfo);
    return -1;
  }

  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, data, static_cast<unsigned long>(size));

  if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
    jpeg_destroy_decompress(&cinfo);
    return -1;
  }

  // need at least the luminance table (slot 0)
  if (!cinfo.quant_tbl_ptrs[0]) {
    jpeg_destroy_decompress(&cinfo);
    return -1;
  }

  // reverse-engineer the IJG quality from the luminance table.
  // for each quality q, IJG computes: scale = (q < 50) ? 5000/q : 200-2*q
  // then each table value = clamp(floor((base * scale + 50) / 100), 1, 255)
  // we find q that minimizes the sum of absolute differences.
  JQUANT_TBL *tbl = cinfo.quant_tbl_ptrs[0];
  int bestQ = -1;
  long bestError = std::numeric_limits<long>::max();

  for (int q = 1; q <= 100; q++) {
    long scale = (q < 50) ? 5000L / q : 200L - 2L * q;
    long error = 0;
    for (int i = 0; i < 64; i++) {
      long expected =
          (static_cast<long>(std_luminance_qt[i]) * scale + 50L) / 100L;
      if (expected < 1)
        expected = 1;
      if (expected > 255)
        expected = 255;
      long diff = static_cast<long>(tbl->quantval[i]) - expected;
      error += (diff < 0) ? -diff : diff;
    }
    if (error < bestError) {
      bestError = error;
      bestQ = q;
    }
    // perfect match — stop early
    if (error == 0)
      break;
  }

  jpeg_destroy_decompress(&cinfo);

  // if the best match is poor (avg > 2 per coefficient), the tables are
  // non-standard — return -1 to signal we can't reliably estimate
  if (bestError > 128)
    return -1;

  return bestQ;
}

int estimateJpegQuality(const unsigned char *data, size_t size) {
  return estimateJpegQualityImpl(data, size);
}
