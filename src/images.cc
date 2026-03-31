#include "images.h"
#include "jpeg.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>

#include <qpdf/Buffer.hh>

// ---------------------------------------------------------------------------
// Color space helpers
// ---------------------------------------------------------------------------

// resolves /ColorSpace to component count and whether it's CMYK.
// returns 0 on unsupported color spaces.
static int resolveColorSpace(QPDFObjectHandle cs, bool &isCMYK) {
  isCMYK = false;

  if (cs.isName()) {
    auto name = cs.getName();
    if (name == "/DeviceRGB")
      return 3;
    if (name == "/DeviceGray")
      return 1;
    if (name == "/DeviceCMYK") {
      isCMYK = true;
      return 4;
    }
    return 0;
  }

  // ICCBased: [/ICCBased <stream>] — get /N from the ICC profile stream dict
  if (cs.isArray() && cs.getArrayNItems() >= 2) {
    auto csName = cs.getArrayItem(0);
    if (csName.isName() && csName.getName() == "/ICCBased") {
      auto profile = cs.getArrayItem(1);
      if (profile.isStream()) {
        auto n = profile.getDict().getKey("/N");
        if (n.isInteger()) {
          int components = static_cast<int>(n.getIntValue());
          if (components == 4)
            isCMYK = true;
          if (components == 1 || components == 3 || components == 4)
            return components;
        }
      }
    }
  }

  return 0;
}

// naive CMYK → RGB conversion (without ICC profile).
// uses the standard formula: R = 255 * (1-C) * (1-K), etc.
static void cmykToRgb(const unsigned char *cmyk, unsigned char *rgb,
                      size_t pixelCount) {
  for (size_t i = 0; i < pixelCount; ++i) {
    double c = cmyk[i * 4 + 0] / 255.0;
    double m = cmyk[i * 4 + 1] / 255.0;
    double y = cmyk[i * 4 + 2] / 255.0;
    double k = cmyk[i * 4 + 3] / 255.0;
    rgb[i * 3 + 0] =
        static_cast<unsigned char>(255.0 * (1.0 - c) * (1.0 - k) + 0.5);
    rgb[i * 3 + 1] =
        static_cast<unsigned char>(255.0 * (1.0 - m) * (1.0 - k) + 0.5);
    rgb[i * 3 + 2] =
        static_cast<unsigned char>(255.0 * (1.0 - y) * (1.0 - k) + 0.5);
  }
}

// ---------------------------------------------------------------------------
// Content stream CTM parser — find rendered image dimensions in points
// ---------------------------------------------------------------------------

// 3x3 affine matrix stored as [a b c d e f] (PDF standard order)
struct Matrix {
  double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
};

static Matrix multiply(const Matrix &m1, const Matrix &m2) {
  return {m1.a * m2.a + m1.b * m2.c,        m1.a * m2.b + m1.b * m2.d,
          m1.c * m2.a + m1.d * m2.c,        m1.c * m2.b + m1.d * m2.d,
          m1.e * m2.a + m1.f * m2.c + m2.e, m1.e * m2.b + m1.f * m2.d + m2.f};
}

// returns the rendered width and height in points for each image XObject name.
// falls back to pageW/pageH if parsing fails or the image isn't found.
static std::map<std::string, std::pair<double, double>>
getImageRenderedSizes(QPDFPageObjectHelper &page) {
  std::map<std::string, std::pair<double, double>> result;

  auto pageObj = page.getObjectHandle();
  auto contents = pageObj.getKey("/Contents");

  std::string contentStr;
  try {
    if (contents.isStream()) {
      auto buf = contents.getStreamData(qpdf_dl_generalized);
      contentStr.assign(reinterpret_cast<const char *>(buf->getBuffer()),
                        buf->getSize());
    } else if (contents.isArray()) {
      for (int i = 0; i < contents.getArrayNItems(); ++i) {
        auto stream = contents.getArrayItem(i);
        if (stream.isStream()) {
          auto buf = stream.getStreamData(qpdf_dl_generalized);
          contentStr.append(reinterpret_cast<const char *>(buf->getBuffer()),
                            buf->getSize());
          contentStr += '\n';
        }
      }
    }
  } catch (...) {
    return result;
  }

  // simple tokenizer: split on whitespace, handle names (/Xxx)
  std::vector<std::string> tokens;
  size_t pos = 0;
  while (pos < contentStr.size()) {
    // skip whitespace
    while (pos < contentStr.size() &&
           (contentStr[pos] == ' ' || contentStr[pos] == '\n' ||
            contentStr[pos] == '\r' || contentStr[pos] == '\t'))
      ++pos;
    if (pos >= contentStr.size())
      break;

    // skip comments
    if (contentStr[pos] == '%') {
      while (pos < contentStr.size() && contentStr[pos] != '\n')
        ++pos;
      continue;
    }

    // skip strings (we don't need them)
    if (contentStr[pos] == '(') {
      int depth = 1;
      ++pos;
      while (pos < contentStr.size() && depth > 0) {
        if (contentStr[pos] == '\\') {
          ++pos;
          if (pos < contentStr.size())
            ++pos;
        } else {
          if (contentStr[pos] == '(')
            ++depth;
          else if (contentStr[pos] == ')')
            --depth;
          ++pos;
        }
      }
      continue;
    }
    if (contentStr[pos] == '<' && pos + 1 < contentStr.size() &&
        contentStr[pos + 1] != '<') {
      ++pos;
      while (pos < contentStr.size() && contentStr[pos] != '>')
        ++pos;
      if (pos < contentStr.size())
        ++pos;
      continue;
    }

    // skip inline images (BI ... EI)
    // handled after tokenization

    // read token
    size_t start = pos;
    if (contentStr[pos] == '/') {
      ++pos;
      while (pos < contentStr.size() && contentStr[pos] != ' ' &&
             contentStr[pos] != '\n' && contentStr[pos] != '\r' &&
             contentStr[pos] != '\t' && contentStr[pos] != '/' &&
             contentStr[pos] != '[' && contentStr[pos] != ']' &&
             contentStr[pos] != '<' && contentStr[pos] != '>' &&
             contentStr[pos] != '(' && contentStr[pos] != ')')
        ++pos;
    } else if (contentStr[pos] == '[' || contentStr[pos] == ']') {
      ++pos;
    } else if (contentStr[pos] == '<' && pos + 1 < contentStr.size() &&
               contentStr[pos + 1] == '<') {
      pos += 2; // <<
    } else if (contentStr[pos] == '>' && pos + 1 < contentStr.size() &&
               contentStr[pos + 1] == '>') {
      pos += 2; // >>
    } else {
      while (pos < contentStr.size() && contentStr[pos] != ' ' &&
             contentStr[pos] != '\n' && contentStr[pos] != '\r' &&
             contentStr[pos] != '\t' && contentStr[pos] != '/' &&
             contentStr[pos] != '[' && contentStr[pos] != ']' &&
             contentStr[pos] != '<' && contentStr[pos] != '>' &&
             contentStr[pos] != '(' && contentStr[pos] != ')')
        ++pos;
    }

    if (pos > start)
      tokens.emplace_back(contentStr.substr(start, pos - start));
  }

  // walk tokens tracking CTM
  Matrix ctm;
  std::vector<Matrix> stack;
  std::vector<std::string> operandStack;

  for (size_t i = 0; i < tokens.size(); ++i) {
    auto &tok = tokens[i];

    if (tok == "q") {
      stack.push_back(ctm);
      operandStack.clear();
    } else if (tok == "Q") {
      if (!stack.empty()) {
        ctm = stack.back();
        stack.pop_back();
      }
      operandStack.clear();
    } else if (tok == "cm" && operandStack.size() >= 6) {
      // cm operator: a b c d e f cm
      try {
        Matrix m;
        m.a = std::stod(operandStack[operandStack.size() - 6]);
        m.b = std::stod(operandStack[operandStack.size() - 5]);
        m.c = std::stod(operandStack[operandStack.size() - 4]);
        m.d = std::stod(operandStack[operandStack.size() - 3]);
        m.e = std::stod(operandStack[operandStack.size() - 2]);
        m.f = std::stod(operandStack[operandStack.size() - 1]);
        ctm = multiply(m, ctm);
      } catch (...) {
      }
      operandStack.clear();
    } else if (tok == "Do" && !operandStack.empty()) {
      // Do operator draws an XObject
      auto &name = operandStack.back();
      if (!name.empty() && name[0] == '/') {
        // rendered width = sqrt(a^2 + c^2), height = sqrt(b^2 + d^2)
        // (these are the lengths of the column vectors of the CTM)
        double rw = std::sqrt(ctm.a * ctm.a + ctm.c * ctm.c);
        double rh = std::sqrt(ctm.b * ctm.b + ctm.d * ctm.d);
        // keep the largest rendered size if an image is drawn multiple times
        auto it = result.find(name);
        if (it == result.end() ||
            rw * rh > it->second.first * it->second.second)
          result[name] = {rw, rh};
      }
      operandStack.clear();
    } else {
      // it's an operand — check if it's an operator we don't track
      // (PDF operators are always non-numeric alpha)
      bool isOperator = !tok.empty() && tok[0] != '/' && tok[0] != '-' &&
                        tok[0] != '+' && tok[0] != '.' &&
                        !(tok[0] >= '0' && tok[0] <= '9');
      if (isOperator) {
        operandStack.clear();
      } else {
        operandStack.push_back(tok);
      }
    }
  }

  return result;
}

// ---------------------------------------------------------------------------
// Bilinear downscaling
// ---------------------------------------------------------------------------

static std::vector<uint8_t> bilinearDownscale(const unsigned char *src,
                                              int srcW, int srcH,
                                              int components, int dstW,
                                              int dstH) {
  std::vector<uint8_t> dst(static_cast<size_t>(dstW) * dstH * components);

  double xRatio = static_cast<double>(srcW) / dstW;
  double yRatio = static_cast<double>(srcH) / dstH;

  for (int y = 0; y < dstH; ++y) {
    double srcY = y * yRatio;
    int y0 = static_cast<int>(srcY);
    int y1 = std::min(y0 + 1, srcH - 1);
    double fy = srcY - y0;

    for (int x = 0; x < dstW; ++x) {
      double srcX = x * xRatio;
      int x0 = static_cast<int>(srcX);
      int x1 = std::min(x0 + 1, srcW - 1);
      double fx = srcX - x0;

      for (int c = 0; c < components; ++c) {
        double v00 = src[(y0 * srcW + x0) * components + c];
        double v10 = src[(y0 * srcW + x1) * components + c];
        double v01 = src[(y1 * srcW + x0) * components + c];
        double v11 = src[(y1 * srcW + x1) * components + c];

        double val = v00 * (1 - fx) * (1 - fy) + v10 * fx * (1 - fy) +
                     v01 * (1 - fx) * fy + v11 * fx * fy;
        dst[(y * dstW + x) * components + c] =
            static_cast<uint8_t>(std::min(std::max(val + 0.5, 0.0), 255.0));
      }
    }
  }

  return dst;
}

// ---------------------------------------------------------------------------
// Image recompression for lossy mode
// ---------------------------------------------------------------------------

// Image recompression thresholds are passed via CompressOptions:
//   - lossless mode: skipThreshold=90, targetQuality=85  (conservative)
//   - lossy mode:    skipThreshold=65, targetQuality=75  (aggressive)

void optimizeImages(QPDF &qpdf, const CompressOptions &opts) {
  forEachImage(qpdf, [&](const std::string &, QPDFObjectHandle xobj,
                         QPDFObjectHandle, QPDFPageObjectHelper &) {
    auto dict = xobj.getDict();

    // only handle 8-bit images
    if (!dict.getKey("/BitsPerComponent").isInteger() ||
        dict.getKey("/BitsPerComponent").getIntValue() != 8)
      return;

    int width = 0, height = 0;
    if (dict.getKey("/Width").isInteger())
      width = static_cast<int>(dict.getKey("/Width").getIntValue());
    if (dict.getKey("/Height").isInteger())
      height = static_cast<int>(dict.getKey("/Height").getIntValue());

    if (width <= 0 || height <= 0 || width > 16384 || height > 16384)
      return;

    // determine color components via color space resolution
    bool isCMYK = false;
    int components = resolveColorSpace(dict.getKey("/ColorSpace"), isCMYK);
    if (components == 0)
      return;

    // skip tiny images (logos, icons)
    if (width * height < 2500)
      return;

    // get fully decoded stream data (raw pixels)
    std::shared_ptr<Buffer> streamData;
    try {
      streamData = xobj.getStreamData(qpdf_dl_all);
    } catch (...) {
      return;
    }

    // overflow-safe size calculation
    auto w = static_cast<size_t>(width);
    auto h = static_cast<size_t>(height);
    auto c = static_cast<size_t>(components);
    if (h > 0 && w > std::numeric_limits<size_t>::max() / h)
      return;
    if (c > 0 && (w * h) > std::numeric_limits<size_t>::max() / c)
      return;
    size_t expectedSize = w * h * c;
    if (streamData->getSize() != expectedSize)
      return;

    // convert CMYK → RGB for JPEG encoding (JPEG doesn't support CMYK
    // natively in most decoders)
    const unsigned char *pixels = streamData->getBuffer();
    std::vector<uint8_t> rgbBuf;
    int encodeComponents = components;
    if (isCMYK) {
      size_t pixelCount = w * h;
      rgbBuf.resize(pixelCount * 3);
      cmykToRgb(pixels, rgbBuf.data(), pixelCount);
      pixels = rgbBuf.data();
      encodeComponents = 3;
    }

    auto currentFilter = dict.getKey("/Filter");
    bool isCurrentlyJpeg =
        currentFilter.isName() && currentFilter.getName() == "/DCTDecode";

    // skip existing JPEGs that are already at or below the threshold —
    // re-encoding them adds generation loss for negligible savings
    if (isCurrentlyJpeg && !isCMYK) {
      auto rawData = xobj.getRawStreamData();
      int existingQ =
          estimateJpegQuality(rawData->getBuffer(), rawData->getSize());
      if (existingQ > 0 && existingQ <= opts.skipThreshold)
        return;
    }

    // encode as JPEG via libjpeg-turbo
    std::vector<uint8_t> jpegData;
    if (!encodeJpeg(pixels, width, height, encodeComponents, opts.targetQuality,
                    jpegData))
      return;

    // only replace if we actually reduced size
    auto rawData = xobj.getRawStreamData();
    if (jpegData.size() >= rawData->getSize())
      return;

    // replace stream data with JPEG
    std::string jpegStr(reinterpret_cast<char *>(jpegData.data()),
                        jpegData.size());
    xobj.replaceStreamData(jpegStr, QPDFObjectHandle::newName("/DCTDecode"),
                           QPDFObjectHandle::newNull());

    // update color space to DeviceRGB if converted from CMYK/ICCBased
    if (isCMYK || !dict.getKey("/ColorSpace").isName() ||
        dict.getKey("/ColorSpace").getName() != "/DeviceRGB") {
      if (encodeComponents == 3)
        dict.replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceRGB"));
      else if (encodeComponents == 1)
        dict.replaceKey("/ColorSpace",
                        QPDFObjectHandle::newName("/DeviceGray"));
    }

    // remove FlateDecode-specific params
    if (dict.hasKey("/DecodeParms"))
      dict.removeKey("/DecodeParms");
    if (dict.hasKey("/Predictor"))
      dict.removeKey("/Predictor");
  });
}

// ---------------------------------------------------------------------------
// Duplicate image detection
// ---------------------------------------------------------------------------

void deduplicateImages(QPDF &qpdf) {
  struct ImageEntry {
    QPDFObjGen og;
    size_t dataSize;
    QPDFObjectHandle handle;
  };

  std::unordered_map<uint64_t, std::vector<ImageEntry>> hashGroups;
  std::set<QPDFObjGen> seen;

  // first pass: collect image objects and hash their raw data
  forEachImage(qpdf, [&](const std::string & /*key*/, QPDFObjectHandle xobj,
                         QPDFObjectHandle /*xobjects*/,
                         QPDFPageObjectHelper & /*page*/) {
    auto og = xobj.getObjGen();
    if (seen.count(og))
      return;
    seen.insert(og);

    try {
      auto rawData = xobj.getRawStreamData();
      size_t size = rawData->getSize();

      // FNV-1a hash
      uint64_t hash = 14695981039346656037ULL;
      auto *p = rawData->getBuffer();
      for (size_t i = 0; i < size; ++i) {
        hash ^= static_cast<uint64_t>(p[i]);
        hash *= 1099511628211ULL;
      }

      hashGroups[hash].push_back({og, size, xobj});
    } catch (...) {
    }
  });

  // second pass: verify hash collisions with full byte comparison
  std::map<QPDFObjGen, QPDFObjectHandle> replacements;

  for (auto &[hash, group] : hashGroups) {
    if (group.size() < 2)
      continue;

    for (size_t i = 0; i < group.size(); ++i) {
      if (replacements.count(group[i].og))
        continue;

      auto rawI = group[i].handle.getRawStreamData();
      for (size_t j = i + 1; j < group.size(); ++j) {
        if (replacements.count(group[j].og))
          continue;

        auto rawJ = group[j].handle.getRawStreamData();
        if (rawI->getSize() != rawJ->getSize())
          continue;

        if (memcmp(rawI->getBuffer(), rawJ->getBuffer(), rawI->getSize()) == 0)
          replacements[group[j].og] = group[i].handle;
      }
    }
  }

  if (replacements.empty())
    return;

  // third pass: rewrite XObject references to point to canonical objects
  forEachImage(qpdf,
               [&](const std::string &key, QPDFObjectHandle xobj,
                   QPDFObjectHandle xobjects, QPDFPageObjectHelper & /*page*/) {
                 auto it = replacements.find(xobj.getObjGen());
                 if (it != replacements.end())
                   xobjects.replaceKey(key, it->second);
               });
}

// ---------------------------------------------------------------------------
// Lossless optimization of existing embedded JPEG images
// ---------------------------------------------------------------------------

void optimizeExistingJpegs(QPDF &qpdf) {
  std::set<QPDFObjGen> processed;

  forEachImage(qpdf, [&](const std::string & /*key*/, QPDFObjectHandle xobj,
                         QPDFObjectHandle /*xobjects*/,
                         QPDFPageObjectHelper & /*page*/) {
    auto og = xobj.getObjGen();
    if (processed.count(og))
      return;
    processed.insert(og);

    auto filter = xobj.getDict().getKey("/Filter");
    if (!filter.isName() || filter.getName() != "/DCTDecode")
      return;

    try {
      auto rawData = xobj.getRawStreamData();

      std::vector<uint8_t> optimized;
      if (!losslessJpegOptimize(rawData->getBuffer(), rawData->getSize(),
                                optimized))
        return;

      // only replace if strictly smaller
      if (optimized.size() >= rawData->getSize())
        return;

      std::string jpegStr(reinterpret_cast<char *>(optimized.data()),
                          optimized.size());
      xobj.replaceStreamData(jpegStr, QPDFObjectHandle::newName("/DCTDecode"),
                             QPDFObjectHandle::newNull());
    } catch (...) {
    }
  });
}

// ---------------------------------------------------------------------------
// DPI-based image downscaling
// ---------------------------------------------------------------------------

void downscaleImages(QPDF &qpdf, int maxDpi, int quality) {
  if (maxDpi <= 0)
    return;

  std::set<QPDFObjGen> processed;
  // cache rendered sizes per page object (keyed by page objgen)
  std::map<QPDFObjGen, std::map<std::string, std::pair<double, double>>>
      pageSizesCache;

  forEachImage(qpdf, [&](const std::string &key, QPDFObjectHandle xobj,
                         QPDFObjectHandle /*xobjects*/,
                         QPDFPageObjectHelper &page) {
    auto og = xobj.getObjGen();
    if (processed.count(og))
      return;
    processed.insert(og);

    auto dict = xobj.getDict();

    if (!dict.getKey("/BitsPerComponent").isInteger() ||
        dict.getKey("/BitsPerComponent").getIntValue() != 8)
      return;

    int imgW = 0, imgH = 0;
    if (dict.getKey("/Width").isInteger())
      imgW = static_cast<int>(dict.getKey("/Width").getIntValue());
    if (dict.getKey("/Height").isInteger())
      imgH = static_cast<int>(dict.getKey("/Height").getIntValue());

    if (imgW <= 0 || imgH <= 0)
      return;

    bool isCMYK = false;
    int components = resolveColorSpace(dict.getKey("/ColorSpace"), isCMYK);
    if (components == 0)
      return;

    // get rendered size from CTM parsing, with MediaBox fallback
    auto pageOg = page.getObjectHandle().getObjGen();
    auto cacheIt = pageSizesCache.find(pageOg);
    if (cacheIt == pageSizesCache.end()) {
      pageSizesCache[pageOg] = getImageRenderedSizes(page);
      cacheIt = pageSizesCache.find(pageOg);
    }

    double renderedW = 0, renderedH = 0;
    auto sizeIt = cacheIt->second.find(key);
    if (sizeIt != cacheIt->second.end() && sizeIt->second.first > 0 &&
        sizeIt->second.second > 0) {
      // CTM gives rendered size in points
      renderedW = sizeIt->second.first;
      renderedH = sizeIt->second.second;
    } else {
      // fallback: assume image fills page
      auto mediaBox = page.getAttribute("/MediaBox", false);
      if (!mediaBox.isArray() || mediaBox.getArrayNItems() < 4)
        return;
      try {
        renderedW = mediaBox.getArrayItem(2).getNumericValue() -
                    mediaBox.getArrayItem(0).getNumericValue();
        renderedH = mediaBox.getArrayItem(3).getNumericValue() -
                    mediaBox.getArrayItem(1).getNumericValue();
      } catch (...) {
        return;
      }
    }

    if (renderedW <= 0 || renderedH <= 0)
      return;

    // calculate effective DPI from rendered size in points (72 points/inch)
    double dpiX = imgW / (renderedW / 72.0);
    double dpiY = imgH / (renderedH / 72.0);
    double effectiveDpi = std::max(dpiX, dpiY);

    if (effectiveDpi <= maxDpi)
      return;

    // calculate target dimensions
    double scale = static_cast<double>(maxDpi) / effectiveDpi;
    int newW = std::max(1, static_cast<int>(imgW * scale + 0.5));
    int newH = std::max(1, static_cast<int>(imgH * scale + 0.5));

    // not worth downscaling if the reduction is minimal
    if (newW >= imgW - 1 && newH >= imgH - 1)
      return;

    // decode pixels
    std::shared_ptr<Buffer> streamData;
    try {
      streamData = xobj.getStreamData(qpdf_dl_all);
    } catch (...) {
      return;
    }

    auto w = static_cast<size_t>(imgW);
    auto h = static_cast<size_t>(imgH);
    auto c = static_cast<size_t>(components);
    if (h > 0 && w > std::numeric_limits<size_t>::max() / h)
      return;
    if (c > 0 && (w * h) > std::numeric_limits<size_t>::max() / c)
      return;
    if (streamData->getSize() != w * h * c)
      return;

    const unsigned char *pixels = streamData->getBuffer();
    int downscaleComponents = components;

    // convert CMYK → RGB before downscaling
    std::vector<uint8_t> rgbBuf;
    if (isCMYK) {
      size_t pixelCount = w * h;
      rgbBuf.resize(pixelCount * 3);
      cmykToRgb(pixels, rgbBuf.data(), pixelCount);
      pixels = rgbBuf.data();
      downscaleComponents = 3;
    }

    auto scaled =
        bilinearDownscale(pixels, imgW, imgH, downscaleComponents, newW, newH);

    // re-encode downscaled pixels as JPEG
    std::vector<uint8_t> jpegData;
    if (!encodeJpeg(scaled.data(), newW, newH, downscaleComponents, quality,
                    jpegData))
      return;

    // only replace if the JPEG is actually smaller than the original stream
    auto rawData = xobj.getRawStreamData();
    if (jpegData.size() >= rawData->getSize())
      return;

    std::string jpegStr(reinterpret_cast<char *>(jpegData.data()),
                        jpegData.size());
    xobj.replaceStreamData(jpegStr, QPDFObjectHandle::newName("/DCTDecode"),
                           QPDFObjectHandle::newNull());

    dict.replaceKey("/Width", QPDFObjectHandle::newInteger(newW));
    dict.replaceKey("/Height", QPDFObjectHandle::newInteger(newH));

    if (isCMYK) {
      dict.replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceRGB"));
    }

    // remove predictor params from previous encoding
    if (dict.hasKey("/DecodeParms"))
      dict.removeKey("/DecodeParms");
    if (dict.hasKey("/Predictor"))
      dict.removeKey("/Predictor");
  });
}

// ---------------------------------------------------------------------------
// Grayscale detection — convert RGB images that are actually grayscale to
// DeviceGray (1/3 the raw data size)
// ---------------------------------------------------------------------------

void convertGrayscaleImages(QPDF &qpdf) {
  std::set<QPDFObjGen> processed;

  forEachImage(qpdf, [&](const std::string & /*key*/, QPDFObjectHandle xobj,
                         QPDFObjectHandle /*xobjects*/,
                         QPDFPageObjectHelper & /*page*/) {
    auto og = xobj.getObjGen();
    if (processed.count(og))
      return;
    processed.insert(og);

    auto dict = xobj.getDict();

    // only handle 8-bit RGB images
    if (!dict.getKey("/BitsPerComponent").isInteger() ||
        dict.getKey("/BitsPerComponent").getIntValue() != 8)
      return;

    auto cs = dict.getKey("/ColorSpace");
    if (!cs.isName() || cs.getName() != "/DeviceRGB")
      return;

    int width = 0, height = 0;
    if (dict.getKey("/Width").isInteger())
      width = static_cast<int>(dict.getKey("/Width").getIntValue());
    if (dict.getKey("/Height").isInteger())
      height = static_cast<int>(dict.getKey("/Height").getIntValue());
    if (width <= 0 || height <= 0)
      return;

    // decode pixels
    std::shared_ptr<Buffer> streamData;
    try {
      streamData = xobj.getStreamData(qpdf_dl_all);
    } catch (...) {
      return;
    }

    auto w = static_cast<size_t>(width);
    auto h = static_cast<size_t>(height);
    size_t expectedSize = w * h * 3;
    if (streamData->getSize() != expectedSize)
      return;

    const auto *pixels = streamData->getBuffer();
    size_t pixelCount = w * h;

    // check if all RGB triples have equal channels (R == G == B)
    bool isGray = true;
    for (size_t i = 0; i < pixelCount; ++i) {
      auto r = pixels[i * 3 + 0];
      auto g = pixels[i * 3 + 1];
      auto b = pixels[i * 3 + 2];
      if (r != g || g != b) {
        isGray = false;
        break;
      }
    }

    if (!isGray)
      return;

    // build grayscale pixel buffer
    std::vector<uint8_t> grayPixels(pixelCount);
    for (size_t i = 0; i < pixelCount; ++i)
      grayPixels[i] = pixels[i * 3];

    // replace stream with raw grayscale data (Flate-compressed by QPDFWriter)
    std::string grayStr(reinterpret_cast<char *>(grayPixels.data()),
                        grayPixels.size());
    xobj.replaceStreamData(grayStr, QPDFObjectHandle::newNull(),
                           QPDFObjectHandle::newNull());
    dict.replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceGray"));

    // remove JPEG-specific params
    if (dict.hasKey("/DecodeParms"))
      dict.removeKey("/DecodeParms");
    if (dict.hasKey("/Predictor"))
      dict.removeKey("/Predictor");
  });
}

// ---------------------------------------------------------------------------
// Bitonal conversion — convert 8-bit grayscale images that are effectively
// black & white (all pixels near 0 or 255) to 1-bit, saving ~8x raw data
// ---------------------------------------------------------------------------

void convertBitonalImages(QPDF &qpdf) {
  std::set<QPDFObjGen> processed;

  forEachImage(qpdf, [&](const std::string & /*key*/, QPDFObjectHandle xobj,
                         QPDFObjectHandle /*xobjects*/,
                         QPDFPageObjectHelper & /*page*/) {
    auto og = xobj.getObjGen();
    if (processed.count(og))
      return;
    processed.insert(og);

    auto dict = xobj.getDict();

    // only handle 8-bit grayscale images
    if (!dict.getKey("/BitsPerComponent").isInteger() ||
        dict.getKey("/BitsPerComponent").getIntValue() != 8)
      return;

    auto cs = dict.getKey("/ColorSpace");
    if (!cs.isName() || cs.getName() != "/DeviceGray")
      return;

    // skip images with masks (bitonal conversion may not be safe)
    if (dict.hasKey("/SMask") || dict.hasKey("/Mask"))
      return;

    int width = 0, height = 0;
    if (dict.getKey("/Width").isInteger())
      width = static_cast<int>(dict.getKey("/Width").getIntValue());
    if (dict.getKey("/Height").isInteger())
      height = static_cast<int>(dict.getKey("/Height").getIntValue());
    if (width <= 0 || height <= 0)
      return;

    // decode pixels
    std::shared_ptr<Buffer> streamData;
    try {
      streamData = xobj.getStreamData(qpdf_dl_all);
    } catch (...) {
      return;
    }

    auto w = static_cast<size_t>(width);
    auto h = static_cast<size_t>(height);
    if (streamData->getSize() != w * h)
      return;

    const auto *pixels = streamData->getBuffer();
    size_t pixelCount = w * h;

    // check if all pixels are near black (<=32) or near white (>=224)
    bool isBitonal = true;
    for (size_t i = 0; i < pixelCount; ++i) {
      if (pixels[i] > 32 && pixels[i] < 224) {
        isBitonal = false;
        break;
      }
    }

    if (!isBitonal)
      return;

    // pack into 1-bit: 0 = black, 1 = white
    // each row padded to byte boundary
    size_t rowBytes = (w + 7) / 8;
    std::vector<uint8_t> bitonalData(rowBytes * h, 0);

    for (size_t y = 0; y < h; ++y) {
      for (size_t x = 0; x < w; ++x) {
        bool white = pixels[y * w + x] >= 224;
        if (white)
          bitonalData[y * rowBytes + x / 8] |=
              static_cast<uint8_t>(0x80 >> (x % 8));
      }
    }

    // only replace if 1-bit data is smaller than original raw stream
    auto rawData = xobj.getRawStreamData();
    if (bitonalData.size() >= rawData->getSize())
      return;

    std::string bitStr(reinterpret_cast<char *>(bitonalData.data()),
                       bitonalData.size());
    xobj.replaceStreamData(bitStr, QPDFObjectHandle::newNull(),
                           QPDFObjectHandle::newNull());
    dict.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(1));

    if (dict.hasKey("/DecodeParms"))
      dict.removeKey("/DecodeParms");
    if (dict.hasKey("/Predictor"))
      dict.removeKey("/Predictor");
  });
}

// ---------------------------------------------------------------------------
// Soft mask optimization — losslessly optimize /SMask JPEG streams
// ---------------------------------------------------------------------------

void optimizeSoftMasks(QPDF &qpdf) {
  std::set<QPDFObjGen> processed;

  forEachImage(qpdf, [&](const std::string & /*key*/, QPDFObjectHandle xobj,
                         QPDFObjectHandle /*xobjects*/,
                         QPDFPageObjectHelper & /*page*/) {
    auto dict = xobj.getDict();
    if (!dict.hasKey("/SMask"))
      return;

    auto smask = dict.getKey("/SMask");
    if (!smask.isStream())
      return;

    auto og = smask.getObjGen();
    if (processed.count(og))
      return;
    processed.insert(og);

    auto smaskDict = smask.getDict();
    auto filter = smaskDict.getKey("/Filter");
    if (!filter.isName() || filter.getName() != "/DCTDecode")
      return;

    try {
      auto rawData = smask.getRawStreamData();

      std::vector<uint8_t> optimized;
      if (!losslessJpegOptimize(rawData->getBuffer(), rawData->getSize(),
                                optimized))
        return;

      if (optimized.size() >= rawData->getSize())
        return;

      std::string jpegStr(reinterpret_cast<char *>(optimized.data()),
                          optimized.size());
      smask.replaceStreamData(jpegStr, QPDFObjectHandle::newName("/DCTDecode"),
                              QPDFObjectHandle::newNull());
    } catch (...) {
    }
  });
}
