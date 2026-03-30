#include "images.h"
#include "jpeg.h"

#include <algorithm>
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

// auto-mode thresholds — only re-encode existing JPEGs whose estimated
// quality exceeds kAutoSkipThreshold (avoids pointless re-encoding where
// generation loss outweighs size savings).  Non-JPEG images and high-quality
// JPEGs are (re-)encoded at kAutoTargetQuality.
static constexpr int kAutoSkipThreshold = 90;
static constexpr int kAutoTargetQuality = 85;

void optimizeImages(QPDF &qpdf, const CompressOptions &opts) {
  const bool autoQuality = (opts.quality == 0);
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

    // determine per-image target quality
    int targetQuality = autoQuality ? kAutoTargetQuality : opts.quality;

    // in auto mode, skip existing JPEGs unless their quality is very high
    // (> 90) — re-encoding a q86 JPEG at q85 saves almost nothing but adds
    // artifacts.  Only high-quality originals (92, 95, 100…) benefit from
    // re-encoding down to 85.
    if (isCurrentlyJpeg && !isCMYK) {
      auto rawData = xobj.getRawStreamData();
      int existingQ =
          estimateJpegQuality(rawData->getBuffer(), rawData->getSize());
      if (autoQuality) {
        if (existingQ > 0 && existingQ <= kAutoSkipThreshold)
          return;
      } else {
        // explicit quality: use existing ceiling logic
        if (existingQ > 0 && existingQ <= targetQuality)
          return;
      }
    }

    // encode as JPEG via libjpeg-turbo
    std::vector<uint8_t> jpegData;
    if (!encodeJpeg(pixels, width, height, encodeComponents, targetQuality,
                    jpegData))
      return;

    // only replace if we actually reduced size (for non-CMYK images)
    if (isCurrentlyJpeg && !isCMYK) {
      auto rawData = xobj.getRawStreamData();
      if (jpegData.size() >= rawData->getSize())
        return;
    }

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

void downscaleImages(QPDF &qpdf, int maxDpi) {
  if (maxDpi <= 0)
    return;

  std::set<QPDFObjGen> processed;

  forEachImage(qpdf, [&](const std::string & /*key*/, QPDFObjectHandle xobj,
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

    // get page dimensions from MediaBox (in points, 72 per inch)
    auto mediaBox = page.getAttribute("/MediaBox", false);
    if (!mediaBox.isArray() || mediaBox.getArrayNItems() < 4)
      return;

    double pageW = 0, pageH = 0;
    try {
      pageW = mediaBox.getArrayItem(2).getNumericValue() -
              mediaBox.getArrayItem(0).getNumericValue();
      pageH = mediaBox.getArrayItem(3).getNumericValue() -
              mediaBox.getArrayItem(1).getNumericValue();
    } catch (...) {
      return;
    }

    if (pageW <= 0 || pageH <= 0)
      return;

    // estimate effective DPI (assumes image fills page — conservative)
    double dpiX = imgW / (pageW / 72.0);
    double dpiY = imgH / (pageH / 72.0);
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

    // re-encode as Flate-compressed raw pixels
    auto newSize = static_cast<size_t>(newW) * newH * downscaleComponents;
    if (scaled.size() != newSize)
      return;

    std::string rawStr(reinterpret_cast<char *>(scaled.data()), scaled.size());
    xobj.replaceStreamData(rawStr, QPDFObjectHandle::newName("/FlateDecode"),
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
// Metadata stripping
// ---------------------------------------------------------------------------

void stripMetadata(QPDF &qpdf) {
  auto root = qpdf.getRoot();

  // remove XMP metadata stream
  if (root.hasKey("/Metadata"))
    root.removeKey("/Metadata");

  // remove document info dictionary
  auto trailer = qpdf.getTrailer();
  if (trailer.hasKey("/Info"))
    trailer.removeKey("/Info");

  // remove page-level metadata and PieceInfo
  for (auto &page : QPDFPageDocumentHelper(qpdf).getAllPages()) {
    auto pageObj = page.getObjectHandle();
    if (pageObj.hasKey("/Metadata"))
      pageObj.removeKey("/Metadata");
    if (pageObj.hasKey("/PieceInfo"))
      pageObj.removeKey("/PieceInfo");
  }

  // remove embedded thumbnails
  for (auto &page : QPDFPageDocumentHelper(qpdf).getAllPages()) {
    auto pageObj = page.getObjectHandle();
    if (pageObj.hasKey("/Thumb"))
      pageObj.removeKey("/Thumb");
  }

  // remove MarkInfo and page labels (optional metadata)
  if (root.hasKey("/MarkInfo"))
    root.removeKey("/MarkInfo");
}

// ---------------------------------------------------------------------------
// Remove unused font resources
// ---------------------------------------------------------------------------

void removeUnusedFonts(QPDF &qpdf) {
  for (auto &page : QPDFPageDocumentHelper(qpdf).getAllPages()) {
    auto pageObj = page.getObjectHandle();
    auto resources = pageObj.getKey("/Resources");
    if (!resources.isDictionary())
      continue;
    auto fonts = resources.getKey("/Font");
    if (!fonts.isDictionary())
      continue;

    // collect all font names referenced in this page's content stream(s)
    std::set<std::string> usedFonts;

    try {
      // get unparsed content stream data
      auto contents = pageObj.getKey("/Contents");
      std::string contentStr;

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

      // scan for /FontName references — Tf operator uses font name
      // pattern: /FontName <size> Tf
      for (auto &fontKey : fonts.getKeys()) {
        // fontKey includes the leading '/', e.g. "/F1"
        if (contentStr.find(fontKey) != std::string::npos)
          usedFonts.insert(fontKey);
      }
    } catch (...) {
      continue; // skip this page if content can't be read
    }

    // remove fonts that are not referenced in the content stream
    auto allFontKeys = fonts.getKeys();
    for (auto &fontKey : allFontKeys) {
      if (usedFonts.find(fontKey) == usedFonts.end())
        fonts.removeKey(fontKey);
    }
  }
}
