#include "jpeg.h"

#include <cstdlib>

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

  jpeg_create_decompress(&srcinfo);
  jpeg_create_compress(&dstinfo);

  if (setjmp(jerr.jmpbuf)) {
    jpeg_destroy_decompress(&srcinfo);
    jpeg_destroy_compress(&dstinfo);
    return false;
  }

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
// Lossy JPEG encoding via libjpeg-turbo
// ---------------------------------------------------------------------------

// isolated setjmp scope
static bool encodeJpegImpl(const unsigned char *pixels, int width, int height,
                           int components, int quality, unsigned char **outbuf,
                           unsigned long *outsize) {
  struct jpeg_compress_struct cinfo = {};
  JpegErrorMgr jerr = {};

  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = jpegErrorExit;
  jpeg_create_compress(&cinfo);

  if (setjmp(jerr.jmpbuf)) {
    jpeg_destroy_compress(&cinfo);
    return false;
  }

  *outsize = 0;
  jpeg_mem_dest(&cinfo, outbuf, outsize);

  cinfo.image_width = static_cast<JDIMENSION>(width);
  cinfo.image_height = static_cast<JDIMENSION>(height);
  cinfo.input_components = components;
  cinfo.in_color_space = (components == 3) ? JCS_RGB : JCS_GRAYSCALE;

  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, quality, TRUE);
  cinfo.optimize_coding = TRUE;

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
